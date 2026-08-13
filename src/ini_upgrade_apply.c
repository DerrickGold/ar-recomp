#include "ini_upgrade_apply.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atomic_replace.h"
#include "constants.h"
#include "ini_upgrade.h"
#include "user_data_dir.h"

/* Applies the shipped defaults to the user's live files at startup.
 *
 * THE UPGRADE PATH. A user extracts a new bundle over their existing install.
 * The bundle ships its copies to `defaults/`, never to the live names, so
 * extraction cannot touch anything the user has edited. This then merges each
 * default forward: values the user changed are kept, keys that are new in this
 * version are appended. See ini_upgrade.h for why the separation is what makes a
 * merge possible at all.
 *
 * Runs in the GAME rather than only in the builder GUI, because `run-game`
 * starts the game directly and is the documented way to play -- a GUI-only
 * upgrade step would never run for those users.
 *
 * Every failure here is non-fatal by design. The game must still start if a
 * default is missing, unreadable, or the disk is full: the worst outcome of a
 * failed upgrade is a setting that does not appear yet, never a game that will
 * not launch.
 */

/* Files carried as shipped defaults. Each lives at `defaults/<leaf>` in the
 * bundle and merges into `<leaf>` beside it.
 *
 * These are exactly the three the old packaging shipped as LIVE files, which is
 * why they were the three an upgrade destroyed. settings.ini and saves/ are
 * absent on purpose: nothing ships them, so nothing can overwrite them, and they
 * have no defaults to merge. */
typedef struct {
  const char *leaf;
  /* Whether this file's sections are key namespaces or records -- see
   * ini_upgrade.h. Getting it wrong on a record file resurrects overrides the
   * user deleted and fabricates broken stub entries, so it is stated per file
   * rather than inferred. */
  IniUpgradeSectionKind kind;
} IniUpgradeLeaf;

static const IniUpgradeLeaf kUpgradeLeaves[] = {
  /* [Graphics]/[KeyMap]/[Sound]/... : a fixed set of namespaces holding
     settings, so a key the user lacks is genuinely new in this version. */
  { "config.ini", kIniUpgrade_Namespaces },
  /* [layers:GG:MM] : one section per authored room. A missing plane line means
     the user CLEARED that plane in the editor, not that it is new. */
  { "diorama-layers.ini", kIniUpgrade_Records },
  /* [replace:*] / [music:*] : one section per replacement. Both parsers start a
     fresh entry at every '[', so a per-key append would fabricate a one-key stub
     that fails validation on every launch. */
  { "game-assets/manifest.ini", kIniUpgrade_Records },
};

enum {
  /* Refuse to slurp anything absurd: these are hand-sized config files (the
   * largest shipped is a few KB), so a multi-megabyte one means the path is
   * wrong or the file is not what we think. Hitting this cap reports
   * NOT-absent, so the caller skips the leaf and leaves the file alone --
   * which is only true because of that distinction. Reporting it as absent
   * would replace the very file we refused to read. */
  kIniUpgradeMaxFileBytes = 4 << 20,
  /* Room for a live path plus the ".tmp" suffix and the "defaults/" prefix. */
  kIniUpgradePathMax = 1088,
};

/* Read a whole file, or NULL. Caller frees.
 *
 * `*out_absent` distinguishes the ONE benign reason for NULL -- there is no such
 * file -- from every other: permission denied, a seek/size failure, over the size
 * cap, out of memory. That distinction is load-bearing. The caller treats a NULL
 * live file as "first run" and writes the shipped default over the path, so
 * conflating "cannot read it" with "it is not there" REPLACES a file it failed to
 * read. A 6 MB config.ini and a chmod-000 one were both destroyed that way.
 *
 * Pass NULL when the answer does not matter (reading the shipped default: any
 * failure there means skip the leaf either way). */
static char *ReadWholeFile(const char *path, bool *out_absent) {
  if (out_absent) *out_absent = false;
  FILE *file = fopen(path, "rb");
  if (!file) {
    /* ENOENT is the only "absent". Anything else (EACCES, EISDIR, ...) is a
     * file we must not touch. */
    if (out_absent && errno == ENOENT) *out_absent = true;
    return NULL;
  }
  /* Past this point the file demonstrably EXISTS, so every remaining failure
   * (over the cap, unseekable, out of memory) reports not-absent -- fopen
   * already proved it is there, and errno from these paths is not meaningful
   * enough to branch on. */
  char *data = NULL;
  if (fseek(file, 0, SEEK_END) == 0) {
    long length = ftell(file);
    if (length >= 0 && length < kIniUpgradeMaxFileBytes) {
      rewind(file);
      data = (char *)malloc((size_t)length + 1);
      if (data) {
        size_t got = fread(data, 1, (size_t)length, file);
        data[got] = '\0';
      }
    }
  }
  fclose(file);
  return data;
}

/* Write `text` to `path` via a temp file + rename, so an interrupted write
 * cannot truncate a file that may hold unreproducible user content. */
static bool WriteWholeFile(const char *path, const char *text, size_t length) {
  char temporary[kIniUpgradePathMax];
  snprintf(temporary, sizeof temporary, "%s.tmp", path);
  FILE *file = fopen(temporary, "wb");
  if (!file) return false;
  size_t put = fwrite(text, 1, length, file);
  if (put != length || fflush(file) != 0) {
    fclose(file);
    remove(temporary);
    return false;
  }
  fclose(file);
  /* One atomic replace on both platforms -- see atomic_replace.h for why a bare
   * rename() is not portable here and why remove-then-rename is worse than the
   * bug it fixes. On failure the live file is untouched, which is what makes the
   * caller's "your file is unchanged" true. */
  if (!AtomicReplaceFile(temporary, path)) {
    remove(temporary);
    return false;
  }
  return true;
}

void IniUpgrade_ApplyShippedDefaults(void) {
  for (size_t i = 0; i < sizeof(kUpgradeLeaves) / sizeof(kUpgradeLeaves[0]);
       i++) {
    const char *leaf = kUpgradeLeaves[i].leaf;
    const IniUpgradeSectionKind kind = kUpgradeLeaves[i].kind;

    char live_path[kHostPathCapacity];
    UserDataFile(live_path, sizeof live_path, leaf);

    char default_path[kIniUpgradePathMax];
    snprintf(default_path, sizeof default_path, "defaults/%s", leaf);

    char *shipped = ReadWholeFile(default_path, NULL);
    if (!shipped) continue;   /* no default for this file: nothing to do */

    bool live_absent = false;
    char *live = ReadWholeFile(live_path, &live_absent);
    if (!live && !live_absent) {
      /* The file EXISTS but could not be read -- denied, a directory, or past
       * the size cap. Merging now would write the shipped default over content
       * we never saw, which is precisely the data loss this whole module exists
       * to prevent. Skip the leaf and say so: a missing new setting is a far
       * better outcome than a replaced config. */
      fprintf(stderr,
              "[upgrade] %s exists but could not be read -- skipped, and your "
              "file is left exactly as it is\n",
              leaf);
      free(shipped);
      continue;
    }

    if (!IniUpgrade_NeedsMerge(live, shipped, kind)) {
      /* The overwhelmingly common case: already up to date. Silent, and the
       * file's mtime is left alone rather than rewritten on every launch. */
      free(live);
      free(shipped);
      continue;
    }

    int added = 0;
    size_t need = IniUpgrade_Merge(live, shipped, kind, NULL, 0, NULL);
    char *merged = (char *)malloc(need + 1);
    if (!merged) {
      free(live);
      free(shipped);
      continue;
    }
    size_t wrote = IniUpgrade_Merge(live, shipped, kind, merged, need + 1, &added);

    if (WriteWholeFile(live_path, merged, wrote)) {
      if (live == NULL || !*live) {
        fprintf(stderr, "[upgrade] created %s from the shipped default\n", leaf);
      } else {
        fprintf(stderr,
                "[upgrade] %s: %d new setting%s added, your values kept\n",
                leaf, added, added == 1 ? "" : "s");
      }
    } else {
      /* Reported rather than silent, but not fatal: the user still has their
       * original file, which is the outcome that matters. */
      fprintf(stderr, "[upgrade] could not update %s -- your file is unchanged\n",
              leaf);
    }
    free(merged);
    free(live);
    free(shipped);
  }
}
