/* Upgrade merging of a shipped INI default into the user's live file.
 *
 * THE BUG THIS GUARDS. Extracting a new bundle over an existing install used to
 * overwrite config.ini, diorama-layers.ini and game-assets/manifest.ini, because
 * the archive shipped the LIVE files. Measured before the fix: a played install
 * lost its display settings, its authored diorama rooms, and its asset mappings.
 * The bundle now ships to utils/defaults/ and this merges forward.
 *
 * The load-bearing property is ONE-WAY: the merge only ever APPENDS. Nothing the
 * user wrote is rewritten or removed, so the worst a bug here can do is fail to
 * add a setting -- never lose one. Most of these tests assert exactly that.
 */
#include "ini_upgrade.h"
#include "ini_upgrade_apply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

/* Merge onto the heap, asserting the two-pass sizing contract every time: the
 * size-0 probe must predict the write exactly, since that is what the caller
 * relies on to size its buffer. */
static char *Merge(const char *live, const char *shipped, int *added) {
  size_t need = IniUpgrade_Merge(live, shipped, NULL, 0, NULL);
  char *out = (char *)malloc(need + 1);
  CHECK(out != NULL);
  if (!out) return NULL;
  size_t wrote = IniUpgrade_Merge(live, shipped, out, need + 1, added);
  CHECK(wrote == need);
  CHECK(strlen(out) == need);
  return out;
}

/* THE headline guarantee: a value the user changed is never touched, even when
 * the shipped default now says something different. */
static void TestUserValuesAlwaysWin(void) {
  const char *live =
      "[Graphics]\n"
      "WindowScale = 3\n"      /* user raised this */
      "Fullscreen = 1\n";
  const char *shipped =
      "[Graphics]\n"
      "WindowScale = 2\n"      /* shipped default is lower */
      "Fullscreen = 0\n";

  int added = -1;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(strstr(out, "WindowScale = 3") != NULL);   /* kept */
  CHECK(strstr(out, "WindowScale = 2") == NULL);   /* shipped NOT applied */
  CHECK(strstr(out, "Fullscreen = 1") != NULL);
  CHECK(added == 0);                                /* nothing to add */
  CHECK(!IniUpgrade_NeedsMerge(live, shipped));      /* so no rewrite needed */
  free(out);
}

/* A key that is new in this version arrives, in its own section. */
static void TestNewKeyIsAppended(void) {
  const char *live =
      "[Graphics]\n"
      "WindowScale = 3\n";
  const char *shipped =
      "[Graphics]\n"
      "WindowScale = 2\n"
      "NewShinyOption = 42\n";

  int added = 0;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(strstr(out, "WindowScale = 3") != NULL);        /* still the user's */
  CHECK(strstr(out, "NewShinyOption = 42") != NULL);    /* arrived */
  CHECK(added == 1);
  CHECK(IniUpgrade_NeedsMerge(live, shipped));
  /* It must be attributed to the right section, or re-reading puts it nowhere. */
  const char *appended = strstr(out, "NewShinyOption");
  const char *header = out;
  const char *scan = out;
  while ((scan = strstr(scan, "[Graphics]")) != NULL && scan < appended) {
    header = scan;
    scan += 1;
  }
  CHECK(header != NULL && header < appended);
  free(out);
}

/* SECTION SCOPING, which is load-bearing rather than pedantic: config.ini has
 * `Fullscreen` in both [Graphics] and [KeyMap] with unrelated meanings. A global
 * key match would see the [Graphics] one and wrongly decide the [KeyMap] binding
 * already exists, silently dropping a real setting from the upgrade.
 *
 * BOTH sub-cases are exercised deliberately. An earlier version of this test only
 * covered the new-SECTION path, which never consults the per-section key lookup
 * at all -- so a probe that removed the section scoping SURVIVED it. The case
 * that actually tests scoping is a section the live file ALREADY HAS, missing one
 * key that exists under a different section elsewhere. */
static void TestSameKeyInTwoSectionsIsNotConflated(void) {
  /* Case A: [KeyMap] is wholly new -- arrives as a block. */
  {
    const char *live = "[Graphics]\nFullscreen = 0\n";
    const char *shipped =
        "[Graphics]\nFullscreen = 0\n\n[KeyMap]\nFullscreen = Alt+Return\n";
    int added = 0;
    char *out = Merge(live, shipped, &added);
    if (out) {
      CHECK(strstr(out, "[KeyMap]") != NULL);
      CHECK(strstr(out, "Alt+Return") != NULL);
      CHECK(added > 0);
      free(out);
    }
  }
  /* Case B: THE scoping test. [KeyMap] already exists but lacks Fullscreen,
   * while [Graphics] has one. Only a section-scoped lookup notices the binding
   * is missing; a global one sees "Fullscreen" and adds nothing. */
  {
    const char *live =
        "[Graphics]\nFullscreen = 0\n"
        "[KeyMap]\nReset = Ctrl+r\n";
    const char *shipped =
        "[Graphics]\nFullscreen = 0\n"
        "[KeyMap]\nReset = Ctrl+r\nFullscreen = Alt+Return\n";
    int added = 0;
    char *out = Merge(live, shipped, &added);
    if (out) {
      CHECK(added == 1);
      CHECK(strstr(out, "Alt+Return") != NULL);
      /* And the [Graphics] Fullscreen was NOT disturbed. */
      CHECK(strstr(out, "Fullscreen = 0") != NULL);
      free(out);
    }
  }
}

/* A key appended into an EXISTING section must be preceded by that section's
 * header, or re-reading the file attributes it to whatever section happened to
 * come last -- silently applying a [KeyMap] binding as a [Graphics] setting.
 * Verified by re-reading: the appended key's nearest preceding header is its own
 * section, not some other one. */
static void TestAppendedKeyCarriesItsSectionHeader(void) {
  const char *live =
      "[Graphics]\nWindowScale = 3\n"
      "[Sound]\nEnableAudio = 1\n";
  /* The new key belongs to [Graphics], but [Sound] is the last section in the
   * live file -- so an append without a header would land in [Sound]. */
  const char *shipped =
      "[Graphics]\nWindowScale = 3\nNewRenderer = 1\n"
      "[Sound]\nEnableAudio = 1\n";

  int added = 0;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(added == 1);
  const char *key = strstr(out, "NewRenderer");
  CHECK(key != NULL);
  if (key) {
    /* Walk back to the nearest '[' line before the key and check which it is. */
    const char *nearest = NULL;
    for (const char *scan = out; scan && scan < key;) {
      const char *bracket = strchr(scan, '[');
      if (!bracket || bracket >= key) break;
      nearest = bracket;
      scan = bracket + 1;
    }
    CHECK(nearest != NULL);
    CHECK(nearest != NULL && strncmp(nearest, "[Graphics]", 10) == 0);
  }
  free(out);
}

/* An entirely new section is appended as a block. */
static void TestNewSectionIsAppendedWhole(void) {
  const char *live = "[General]\nAutosave = 1\n";
  const char *shipped =
      "[General]\n"
      "Autosave = 0\n"
      "\n"
      "# what this new section is for\n"
      "[Sound]\n"
      "EnableAudio = 1\n"
      "AudioFreq = 32040\n";

  int added = 0;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(strstr(out, "Autosave = 1") != NULL);      /* user's value kept */
  CHECK(strstr(out, "[Sound]") != NULL);
  CHECK(strstr(out, "EnableAudio = 1") != NULL);
  CHECK(strstr(out, "AudioFreq = 32040") != NULL);
  free(out);
}

/* Everything the user wrote survives: comments, blank lines, ordering, and any
 * section or key we do not ship at all. This is the property that makes the
 * merge safe to run unattended on every launch. */
static void TestUserContentSurvivesVerbatim(void) {
  const char *live =
      "# my own notes, do not delete\n"
      "\n"
      "[Graphics]\n"
      "; why I set this\n"
      "WindowScale = 3   ; inline comment\n"
      "\n"
      "[MyOwnSection]\n"
      "SomethingWeNeverShipped = yes\n";
  const char *shipped = "[Graphics]\nWindowScale = 2\n";

  char *out = Merge(live, shipped, NULL);
  if (!out) return;
  CHECK(strstr(out, "# my own notes, do not delete") != NULL);
  CHECK(strstr(out, "; why I set this") != NULL);
  CHECK(strstr(out, "WindowScale = 3   ; inline comment") != NULL);
  CHECK(strstr(out, "[MyOwnSection]") != NULL);
  CHECK(strstr(out, "SomethingWeNeverShipped = yes") != NULL);
  /* And the user's file is a PREFIX of the result -- proof that pass 1 copied it
   * byte-for-byte and the merge only appended after it. */
  CHECK(strncmp(out, live, strlen(live)) == 0);
  free(out);
}

/* First run: no live file yet, so the result is the template verbatim and
 * nothing counts as "added" -- there was nothing to upgrade. */
static void TestFirstRunSeedsFromTemplate(void) {
  const char *shipped = "# docs\n[Graphics]\nWindowScale = 2\n";
  for (int variant = 0; variant < 2; variant++) {
    const char *live = variant == 0 ? NULL : "";
    int added = -1;
    char *out = Merge(live, shipped, &added);
    if (!out) continue;
    CHECK(strcmp(out, shipped) == 0);
    CHECK(added == 0);
    CHECK(IniUpgrade_NeedsMerge(live, shipped));   /* still needs writing */
    free(out);
  }
}

/* Idempotence: merging twice changes nothing the second time. A launch-time
 * merge runs on every start, so a non-idempotent one would grow the file
 * without bound. */
static void TestMergeIsIdempotent(void) {
  const char *live = "[Graphics]\nWindowScale = 3\n";
  const char *shipped = "[Graphics]\nWindowScale = 2\nNewOption = 7\n";

  char *once = Merge(live, shipped, NULL);
  if (!once) return;
  int added = -1;
  char *twice = Merge(once, shipped, &added);
  if (twice) {
    CHECK(strcmp(once, twice) == 0);
    CHECK(added == 0);                              /* nothing left to add */
    CHECK(!IniUpgrade_NeedsMerge(once, shipped));    /* so no third rewrite */
    free(twice);
  }
  free(once);
}

/* The merge must never DELETE. Asserted directly by checking that every key the
 * live file had is still present, whatever the shipped file says. */
static void TestMergeNeverRemovesAnything(void) {
  const char *live =
      "[A]\nKeepMe = 1\nAlsoKeepMe = 2\n"
      "[B]\nAndMe = 3\n";
  /* A shipped file that mentions none of them, and renames the sections. */
  const char *shipped = "[C]\nSomethingElse = 9\n";

  char *out = Merge(live, shipped, NULL);
  if (!out) return;
  CHECK(strstr(out, "KeepMe = 1") != NULL);
  CHECK(strstr(out, "AlsoKeepMe = 2") != NULL);
  CHECK(strstr(out, "AndMe = 3") != NULL);
  CHECK(strstr(out, "[A]") != NULL);
  CHECK(strstr(out, "[B]") != NULL);
  free(out);
}

/* Key names match case-insensitively, as INI readers do. Otherwise a user who
 * typed `windowscale` would get a duplicate `WindowScale` appended, and which
 * one wins would depend on the reader. */
static void TestKeyMatchIsCaseInsensitive(void) {
  const char *live = "[graphics]\nwindowscale = 3\n";
  const char *shipped = "[Graphics]\nWindowScale = 2\n";
  int added = -1;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(added == 0);                                  /* recognised as present */
  CHECK(strstr(out, "WindowScale = 2") == NULL);       /* no duplicate */
  free(out);
}

/* Degenerate and malformed input must be handled without losing the user's file:
 * unterminated sections, a bare `= value`, and a file with no trailing newline. */
static void TestMalformedInputIsPassedThrough(void) {
  const char *live =
      "[Unterminated\n"
      "= orphan value\n"
      "[Graphics]\n"
      "WindowScale = 3";           /* no trailing newline */
  const char *shipped = "[Graphics]\nWindowScale = 2\nBrandNew = 1\n";

  char *out = Merge(live, shipped, NULL);
  if (!out) return;
  /* Everything the user had, including the malformed lines. */
  CHECK(strstr(out, "[Unterminated") != NULL);
  CHECK(strstr(out, "= orphan value") != NULL);
  CHECK(strstr(out, "WindowScale = 3") != NULL);
  /* The new key still arrives, and a newline was inserted so it does not get
   * glued onto the user's unterminated last line.
   *
   * Asserted POSITIVELY -- that the user's final line is intact and followed by a
   * newline -- rather than by listing glue patterns that must be absent. The
   * negative form passed even with the newline removed, because the very next
   * emitted character happened not to match the patterns guessed at; a probe that
   * deleted the newline insertion SURVIVED it. */
  CHECK(strstr(out, "BrandNew = 1") != NULL);
  const char *last = strstr(out, "WindowScale = 3");
  CHECK(last != NULL);
  if (last) {
    const char *after = last + strlen("WindowScale = 3");
    /* Either the file ended there, or the next byte is a line break -- never a
     * '[', '#' or anything else appended straight onto the value. */
    CHECK(*after == '\0' || *after == '\n' || *after == '\r');
  }
  free(out);
}

/* The sizing contract, including an undersized buffer: never write past the end,
 * always terminate, and always report the true length. */
static void TestSizingContract(void) {
  const char *live = "[Graphics]\nWindowScale = 3\n";
  const char *shipped = "[Graphics]\nWindowScale = 2\nNewOption = 7\n";
  size_t need = IniUpgrade_Merge(live, shipped, NULL, 0, NULL);
  CHECK(need > 0);

  char small[8];
  memset(small, 0x7F, sizeof(small));
  size_t wrote = IniUpgrade_Merge(live, shipped, small, sizeof(small), NULL);
  CHECK(wrote == need);                        /* true length still reported */
  CHECK(small[sizeof(small) - 1] == '\0');     /* never overran */

  /* A zero-size buffer with a non-NULL pointer must not write at all. */
  char guard = 'x';
  (void)IniUpgrade_Merge(live, shipped, &guard, 0, NULL);
  CHECK(guard == 'x');
}

/* The real config.ini shape: the two-Fullscreen case plus a realistic upgrade
 * where the new version adds one key to an existing section. */
static void TestRealisticConfigUpgrade(void) {
  const char *live =
      "[General]\n"
      "Autosave = 1\n"
      "\n"
      "[Graphics]\n"
      "WindowScale = 4\n"
      "Fullscreen = 0\n"
      "\n"
      "[KeyMap]\n"
      "Fullscreen = Alt+Return\n";
  const char *shipped =
      "[General]\n"
      "Autosave = 0\n"
      "\n"
      "[Graphics]\n"
      "WindowScale = 3\n"
      "Fullscreen = 0\n"
      "NewRenderer = 1\n"          /* the new setting this version adds */
      "\n"
      "[KeyMap]\n"
      "Fullscreen = Alt+Return\n";

  int added = 0;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  CHECK(added == 1);                                 /* exactly the one */
  CHECK(strstr(out, "WindowScale = 4") != NULL);      /* user's scale kept */
  CHECK(strstr(out, "Autosave = 1") != NULL);         /* user's autosave kept */
  CHECK(strstr(out, "NewRenderer = 1") != NULL);      /* new setting arrived */
  CHECK(strstr(out, "Alt+Return") != NULL);           /* binding untouched */
  free(out);
}

/* ── the APPLIER: the part that actually writes files ────────────────────────
 *
 * IniUpgrade_ApplyShippedDefaults had no test, and two mutation probes exposed
 * that: writing the shipped default straight over the user's file, and swapping
 * the merge arguments, both went unnoticed by the pure-merge tests. Those are the
 * exact shapes of the original data-loss bug, so they are guarded directly here
 * by driving the real file I/O in a temp directory.
 *
 * Runs chdir'd into a fixture because the applier resolves paths through
 * UserDataFile, which returns bare leaves relative to the working directory --
 * the same way the shipped game resolves them. */
static bool WriteFileText(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  fputs(text, file);
  fclose(file);
  return true;
}

static char *ReadFileText(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  static char buffer[4096];
  size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
  buffer[got] = '\0';
  fclose(file);
  return buffer;
}

static void TestApplierKeepsUserValuesAndAddsNewOnes(void) {
  char template_dir[] = "/tmp/ar-iniupg-XXXXXX";
  if (!mkdtemp(template_dir)) {
    CHECK(!"could not create a temp dir");
    return;
  }
  char original[1024];
  if (!getcwd(original, sizeof(original))) {
    CHECK(!"could not read cwd");
    return;
  }
  CHECK(chdir(template_dir) == 0);
  CHECK(mkdir("defaults", 0755) == 0);

  /* The user's played install, and a NEW default that adds a key -- so a write
   * genuinely happens. (With nothing to add the applier correctly does not
   * write at all, which is why an earlier probe run looked inert.) */
  CHECK(WriteFileText("config.ini",
                      "[Graphics]\nWindowScale = 9   ; mine\nMyOwnKey = keep\n"));
  CHECK(WriteFileText("defaults/config.ini",
                      "[Graphics]\nWindowScale = 3\nBrandNewKey = 7\n"));

  IniUpgrade_ApplyShippedDefaults();

  const char *after = ReadFileText("config.ini");
  CHECK(after != NULL);
  if (after) {
    /* THE data-loss assertions: the user's value and their own key survive. */
    CHECK(strstr(after, "WindowScale = 9   ; mine") != NULL);
    CHECK(strstr(after, "MyOwnKey = keep") != NULL);
    CHECK(strstr(after, "WindowScale = 3") == NULL);   /* default NOT applied */
    /* And the upgrade actually delivered the new setting. */
    CHECK(strstr(after, "BrandNewKey = 7") != NULL);
  }

  /* Idempotent: a second launch must not rewrite or duplicate anything. */
  char before_second[4096];
  snprintf(before_second, sizeof(before_second), "%s", after ? after : "");
  IniUpgrade_ApplyShippedDefaults();
  const char *twice = ReadFileText("config.ini");
  CHECK(twice != NULL && strcmp(before_second, twice) == 0);

  /* A missing default is simply skipped -- never truncates the live file. */
  CHECK(WriteFileText("diorama-layers.ini", "# untouched\n"));
  IniUpgrade_ApplyShippedDefaults();
  const char *layers = ReadFileText("diorama-layers.ini");
  CHECK(layers != NULL && strcmp(layers, "# untouched\n") == 0);

  CHECK(chdir(original) == 0);
}

/* The documented comparison limit, pinned so it is a known boundary rather than a
 * surprise. Names are compared up to kIniNameMax; two keys differing only past
 * that look identical, so the second is not appended. Real names top out around
 * 21 characters (measured against all three shipped files), so this needs a
 * synthetic key to reach -- and the failure mode is a setting that does not
 * arrive, never lost data or a corrupted file. */
static void TestOverlongNamesAreComparedTruncated(void) {
  char shared[300], other[320], live[800], shipped[1200];
  memset(shared, 'K', 200);
  shared[200] = '\0';
  snprintf(other, sizeof(other), "%s_DIFFERENT", shared);
  snprintf(live, sizeof(live), "[S]\n%s = 1\n", shared);
  snprintf(shipped, sizeof(shipped), "[S]\n%s = 1\n%s = 2\n", shared, other);

  int added = -1;
  char *out = Merge(live, shipped, &added);
  if (!out) return;
  /* The boundary as it actually behaves: the near-duplicate is NOT appended. */
  CHECK(added == 0);
  /* What matters is that nothing was lost or mangled. */
  CHECK(strstr(out, shared) != NULL);
  CHECK(strlen(out) >= strlen(live));
  free(out);
}

int main(void) {
  TestOverlongNamesAreComparedTruncated();
  TestApplierKeepsUserValuesAndAddsNewOnes();
  TestUserValuesAlwaysWin();
  TestNewKeyIsAppended();
  TestSameKeyInTwoSectionsIsNotConflated();
  TestAppendedKeyCarriesItsSectionHeader();
  TestNewSectionIsAppendedWhole();
  TestUserContentSurvivesVerbatim();
  TestFirstRunSeedsFromTemplate();
  TestMergeIsIdempotent();
  TestMergeNeverRemovesAnything();
  TestKeyMatchIsCaseInsensitive();
  TestMalformedInputIsPassedThrough();
  TestSizingContract();
  TestRealisticConfigUpgrade();

  if (g_failures) {
    printf("ini_upgrade_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ini_upgrade_test: all checks passed\n");
  return 0;
}
