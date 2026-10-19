#ifndef AR_CHURCH_ART_H
#define AR_CHURCH_ART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Immutable native Palace column art, prepared from the session ROM. No
 * renderer or prior Palace visit is needed. Never distributes ROM pixels. */
enum { kChurchColumnWidth = 28, kChurchColumnHeight = 144 };
/* Exact OBJ winners owned by the two native visitors. Each bit identifies a
 * pixel to move out of the UI plane, within the original 32x32 actor cells. */
enum { kChurchActorCount = 2, kChurchActorSize = 32, kChurchActorX = 96, kChurchActorY = 72 };
typedef struct ChurchActorMask {
  uint64_t lifetime_generation;
  bool valid;
  uint32_t rows[kChurchActorCount][kChurchActorSize];
} ChurchActorMask;
bool ChurchArt_Init(const uint8_t *rom, size_t size);
const uint32_t *ChurchArt_ColumnPixels(void);
#endif
