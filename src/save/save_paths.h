#ifndef AR_SAVE_PATHS_H
#define AR_SAVE_PATHS_H
/* SavePaths: the layout of the saves folder under the launcher's data root:
 * per-slot files, imports, exports reserved under unique names, backups and
 * recovery copies. No absolute path is ever stored in a slot.
 * Phase: host I/O.
 * Tests: tests/save_paths_test.c */
#include <stdbool.h>
#include <stddef.h>
#include "constants.h"
typedef struct SaveError SaveError;
/* The launcher chooses the data root. These paths only organize its saves
 * subtree; no platform-home fallback or absolute path is persisted in a slot. */
typedef struct SavePaths {
  char root[kHostPathCapacity];
  int slot; /* Zero-based managed slot, or -1 for an external diagnostic save. */
} SavePaths;
bool SavePaths_Init(SavePaths *paths,const char *root,int slot,SaveError *error);
bool SavePaths_EnsureDirectory(const char *path,SaveError *error);
bool SavePaths_Import(const SavePaths *paths,char *out,size_t capacity,SaveError *error);
/* Reserves a unique destination with an exclusive .pending directory. Release
 * after either success or failure; interrupted reservations are never reused. */
bool SavePaths_Export(const SavePaths *paths, const char *extension, char *out, size_t capacity,
                      SaveError *error);
bool SavePaths_Backup(const SavePaths *paths,char *out,size_t capacity,SaveError *error);
void SavePaths_Release(const char *path);
bool SavePaths_Recovery(const SavePaths *paths, const unsigned char id[16], char *out,
                        size_t capacity, SaveError *error);
#endif
