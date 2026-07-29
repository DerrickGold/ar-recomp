#include "ini_upgrade_apply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static const char *const kUpgradeLeaves[] = {
  "config.ini",
  "diorama-layers.ini",
  "game-assets/manifest.ini",
};

enum {
  /* Refuse to slurp anything absurd: these are hand-sized config files (the
   * largest shipped is a few KB), so a multi-megabyte one means the path is
   * wrong or the file is not what we think. Bailing out leaves the user's file
   * untouched, which is the safe outcome. */
  kIniUpgradeMaxFileBytes = 4 << 20,
  /* Room for a live path plus the ".tmp" suffix and the "defaults/" prefix. */
  kIniUpgradePathMax = 1088,
};

/* Read a whole file, or NULL. Caller frees. */
static char *ReadWholeFile(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
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
  if (rename(temporary, path) != 0) {
    remove(temporary);
    return false;
  }
  return true;
}

void IniUpgrade_ApplyShippedDefaults(void) {
  for (size_t i = 0; i < sizeof(kUpgradeLeaves) / sizeof(kUpgradeLeaves[0]);
       i++) {
    const char *leaf = kUpgradeLeaves[i];

    char live_path[1024];
    UserDataFile(live_path, sizeof live_path, leaf);

    char default_path[kIniUpgradePathMax];
    snprintf(default_path, sizeof default_path, "defaults/%s", leaf);

    char *shipped = ReadWholeFile(default_path);
    if (!shipped) continue;   /* no default for this file: nothing to do */

    char *live = ReadWholeFile(live_path);

    if (!IniUpgrade_NeedsMerge(live, shipped)) {
      /* The overwhelmingly common case: already up to date. Silent, and the
       * file's mtime is left alone rather than rewritten on every launch. */
      free(live);
      free(shipped);
      continue;
    }

    int added = 0;
    size_t need = IniUpgrade_Merge(live, shipped, NULL, 0, NULL);
    char *merged = (char *)malloc(need + 1);
    if (!merged) {
      free(live);
      free(shipped);
      continue;
    }
    size_t wrote = IniUpgrade_Merge(live, shipped, merged, need + 1, &added);

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
