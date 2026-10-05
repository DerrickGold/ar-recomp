#ifndef AR_SIM_COMPLETION_CHERUBS_H
#define AR_SIM_COMPLETION_CHERUBS_H
/* Immutable SIM angel artwork used by the completion sky's cherub sprites. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kSimCompletionCherubFrames = 3,
  kSimCompletionCherubWidth = 16,
  kSimCompletionCherubHeight = 20,
  kSimCompletionCherubAtlasWidth = kSimCompletionCherubFrames * kSimCompletionCherubWidth,
  kSimCompletionCherubAtlasHeight = kSimCompletionCherubHeight,
};

/* Immutable ROM art, initialized/retired with the game session. Decode only
 * coverage: the completion presenter supplies the backlit silhouette tint.
 * Never borrows live OBJ VRAM, runs a game script or changes emulated state. */
bool SimCompletionCherubs_Init(const uint8_t *rom, size_t bytes);
bool SimCompletionCherubs_Copy(uint32_t *pixels, uint32_t *revision);

#endif
