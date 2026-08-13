#include "ini_upgrade.h"

#include <stdio.h>
#include <string.h>

/* One line of an INI file, classified. The merge only needs to know three
 * things: where sections begin, which lines carry a key, and everything else
 * (comments, blanks, continuation junk) which is passed through untouched. */
enum {
  /* Longest section or key NAME compared when deciding whether the live file
   * already has a key. Measured against the real files: the longest shipped name
   * is "[replace:title-swirl]" at 21 characters, so this is a ~6x margin.
   *
   * THE LIMIT, stated plainly: names are truncated to this length for COMPARISON,
   * so two keys in one section differing ONLY past character 127 look identical
   * and the second would not be appended. Verified reachable only with a
   * synthetic 200-character key. The line itself is always emitted in full, so
   * the failure mode is a setting that does not arrive -- never data loss, and
   * never a corrupted file. Raising this is a one-line change if a format ever
   * grows names that long. */
  kIniNameMax = 128,
};

typedef enum {
  kIniLine_Other = 0,   /* comment, blank, or anything unparseable */
  kIniLine_Section,     /* [name] */
  kIniLine_Key,         /* name = value */
} IniLineKind;

/* Length of the line starting at `at`, including its newline if present. */
static size_t IniLineLength(const char *at) {
  const char *newline = strchr(at, '\n');
  return newline ? (size_t)(newline - at) + 1 : strlen(at);
}

static bool IniIsSpace(char c) { return c == ' ' || c == '\t'; }

/* Classify a line, and for sections/keys copy the name into `name`.
 *
 * Names are compared case-insensitively later, so they are stored as written --
 * preserving the user's capitalisation matters when we echo a section name into
 * an appended block. */
static IniLineKind IniClassify(const char *line, size_t len, char *name,
                               size_t name_size) {
  size_t start = 0;
  while (start < len && IniIsSpace(line[start])) start++;
  if (start >= len) return kIniLine_Other;

  char first = line[start];
  if (first == '\r' || first == '\n') return kIniLine_Other;
  if (first == ';' || first == '#') return kIniLine_Other;

  if (first == '[') {
    size_t close = start + 1;
    while (close < len && line[close] != ']') close++;
    if (close >= len) return kIniLine_Other;   /* unterminated: not a section */
    size_t n = 0;
    for (size_t i = start + 1; i < close && n + 1 < name_size; i++)
      name[n++] = line[i];
    name[n] = '\0';
    return kIniLine_Section;
  }

  /* A key line is `name = value`, or `name=value`. Anything before the first
   * '=' with no '=' at all is not a key -- passed through as Other, which is the
   * safe default: an unrecognised line is never rewritten. */
  size_t equals = start;
  while (equals < len && line[equals] != '=' && line[equals] != '\n') equals++;
  if (equals >= len || line[equals] != '=') return kIniLine_Other;
  size_t end = equals;
  while (end > start && IniIsSpace(line[end - 1])) end--;
  if (end == start) return kIniLine_Other;   /* "= value" with no name */
  size_t n = 0;
  for (size_t i = start; i < end && n + 1 < name_size; i++) name[n++] = line[i];
  name[n] = '\0';
  return kIniLine_Key;
}

static bool IniNamesEqual(const char *a, const char *b) {
  for (;; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    if (!ca) return true;
  }
}

/* Does `text` contain `key` inside section `section`?
 *
 * Section-scoped on purpose: config.ini carries `Fullscreen` in both [Graphics]
 * and [KeyMap] with unrelated meanings, so a global search would see one and
 * wrongly conclude the other is already present. An empty `section` means the
 * implicit leading section before any [header].
 */
static bool IniHasKeyInSection(const char *text, const char *section,
                               const char *key) {
  char current[kIniNameMax] = {0};
  char name[kIniNameMax];
  for (const char *at = text; *at;) {
    size_t len = IniLineLength(at);
    IniLineKind kind = IniClassify(at, len, name, sizeof(name));
    if (kind == kIniLine_Section) {
      snprintf(current, sizeof(current), "%s", name);
    } else if (kind == kIniLine_Key && IniNamesEqual(current, section) &&
               IniNamesEqual(name, key)) {
      return true;
    }
    at += len;
  }
  return false;
}

/* Does `text` contain the section `section` at all? */
static bool IniHasSection(const char *text, const char *section) {
  char name[kIniNameMax];
  for (const char *at = text; *at;) {
    size_t len = IniLineLength(at);
    if (IniClassify(at, len, name, sizeof(name)) == kIniLine_Section &&
        IniNamesEqual(name, section))
      return true;
    at += len;
  }
  return false;
}

/* Shared core. `buffer` may be NULL (sizing pass); `out_added` may be NULL. */
static size_t IniMergeInto(const char *live, const char *shipped,
                           IniUpgradeSectionKind kind, char *buffer,
                           size_t size, int *out_added) {
  size_t total = 0;
  int added = 0;

#define EMIT(ptr, count)                                                    \
  do {                                                                      \
    size_t emit_n = (count);                                                 \
    if (buffer && total < size) {                                           \
      size_t emit_room = size - total;                                       \
      size_t emit_copy = emit_n < emit_room                                  \
          ? emit_n : (emit_room ? emit_room - 1 : 0);                        \
      memcpy(buffer + total, (ptr), emit_copy);                              \
    }                                                                       \
    total += emit_n;                                                        \
  } while (0)
/* Prefix macro locals so they cannot shadow the walk loops' `at` cursor. Without
 * the prefix, an EMIT_FMT argument named `at` would silently bind to the output
 * pointer instead of the input line. */
#define EMIT_FMT(...)                                                       \
  do {                                                                      \
    size_t emit_room = (total < size) ? size - total : 0;                     \
    char *emit_at = buffer ? buffer + (total < size ? total : size) : NULL;   \
    int emit_wrote = snprintf(emit_at, emit_room, __VA_ARGS__);               \
    if (emit_wrote > 0) total += (size_t)emit_wrote;                          \
  } while (0)

  if (live == NULL) live = "";
  if (shipped == NULL) shipped = "";

  /* No live file yet: the result is the shipped template verbatim. This is the
   * first-run seed, and it must not be counted as "keys added" -- there was
   * nothing to upgrade. */
  if (!*live) {
    EMIT(shipped, strlen(shipped));
    if (buffer && size) buffer[size - 1] = '\0';
    if (out_added) *out_added = 0;
    return total;
  }

  /* PASS 1: the user's file, verbatim. Nothing here is ever rewritten -- the
   * merge only appends -- so their comments, ordering and values all survive.
   *
   * No newline is forced after it, deliberately: every append below is introduced
   * by the banner, which begins with its own "\n", so a live file whose last line
   * lacks a terminator is still separated from whatever follows. An explicit
   * `needs_newline` guard here was dead code -- a probe deleting it changed
   * nothing observable, because the banner already covers the case. The test
   * asserts the PROPERTY (the user's last line is followed by a line break or
   * end-of-file) rather than this implementation of it. */
  EMIT(live, strlen(live));

  /* PASS 2: walk the shipped default and append whatever the live file lacks.
   * Two sub-cases, kept separate because they read very differently in the
   * output: a wholly new section is appended as a block with its header, while a
   * new key in an EXISTING section has to be introduced with a header of its own
   * (we cannot insert into the middle without rewriting the user's file, which
   * this deliberately never does). */
  char section[kIniNameMax] = {0};
  char name[kIniNameMax];

  /* 2a: keys missing from sections the live file already has.
   *
   * SKIPPED ENTIRELY for a record file. There, a section the user has is a
   * record they authored, and a key missing from it means they removed it --
   * clearing a plane in the layer editor, pruning a manifest field. Appending it
   * would undo that on the next launch, and would do so by re-stating the
   * header, which both record parsers read as the start of a NEW entry: the
   * result is a one-key stub that fails validation and is dropped with a warning
   * on every launch forever, plus a duplicate header per cycle. Whole new
   * records still arrive via 2b. See ini_upgrade.h. */
  bool wrote_added_banner = false;
  for (const char *at = (kind == kIniUpgrade_Namespaces) ? shipped : ""; *at;) {
    size_t len = IniLineLength(at);
    IniLineKind line_kind = IniClassify(at, len, name, sizeof(name));
    if (line_kind == kIniLine_Section) {
      snprintf(section, sizeof(section), "%s", name);
      at += len;
      continue;
    }
    if (line_kind == kIniLine_Key && IniHasSection(live, section) &&
        !IniHasKeyInSection(live, section, name)) {
      if (!wrote_added_banner) {
        EMIT_FMT("\n# ---- added by the upgrade: new settings in this version"
                 " ----\n");
        wrote_added_banner = true;
      }
      /* Re-state the section so the appended key lands in the right one when the
       * file is re-read; INI has no other way to say where a key belongs. */
      EMIT_FMT("[%s]\n", section);
      EMIT(at, len);
      if (len > 0 && at[len - 1] != '\n') EMIT_FMT("\n");
      added++;
    }
    at += len;
  }

  /* 2b: sections the live file does not have at all, appended whole (their
   * comments included, so the new block arrives documented). */
  section[0] = '\0';
  bool copying_new_section = false;
  for (const char *at = shipped; *at;) {
    size_t len = IniLineLength(at);
    IniLineKind line_kind = IniClassify(at, len, name, sizeof(name));
    if (line_kind == kIniLine_Section) {
      snprintf(section, sizeof(section), "%s", name);
      copying_new_section = !IniHasSection(live, section);
      if (copying_new_section) {
        if (!wrote_added_banner) {
          EMIT_FMT("\n# ---- added by the upgrade: new settings in this version"
                   " ----\n");
          wrote_added_banner = true;
        }
        EMIT_FMT("\n");
        EMIT(at, len);
        added++;
      }
      at += len;
      continue;
    }
    if (copying_new_section) {
      EMIT(at, len);
      if (line_kind == kIniLine_Key) added++;
    }
    at += len;
  }

#undef EMIT
#undef EMIT_FMT
  if (buffer && size) buffer[size - 1] = '\0';
  if (out_added) *out_added = added;
  return total;
}

size_t IniUpgrade_Merge(const char *live, const char *shipped,
                        IniUpgradeSectionKind kind, char *buffer,
                        size_t size, int *out_added) {
  return IniMergeInto(live, shipped, kind, buffer, size, out_added);
}

bool IniUpgrade_NeedsMerge(const char *live, const char *shipped,
                           IniUpgradeSectionKind kind) {
  /* A missing live file always needs seeding; otherwise it needs a merge only
   * when the sizing pass reports something would be appended. Checking `added`
   * rather than comparing lengths keeps this honest when the only difference is
   * the banner. */
  if (live == NULL || !*live) return shipped != NULL && *shipped != '\0';
  int added = 0;
  (void)IniMergeInto(live, shipped, kind, NULL, 0, &added);
  return added > 0;
}
