#ifndef AR_SAVE_NAME_H
#define AR_SAVE_NAME_H
/* Player-name codec. New saves journal names with their complete snapshot;
 * .arname is read only when adopting a legacy save. */
#include "save/save_system.h"

enum { kSaveNameCapacity = 257 };
bool SaveName_CopyNative(const uint8_t *image, char *out, size_t capacity);
bool SaveName_Valid(const char *name);
/* Missing is an empty enhanced name. Damaged legacy companions reject the
 * operation; callers may fall back to the native name for display only. */
bool SaveName_ReadLegacy(const char *path, const uint8_t *image,
                         char out[kSaveNameCapacity], SaveError *error);
#endif
