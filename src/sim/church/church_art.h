#ifndef AR_CHURCH_ART_H
#define AR_CHURCH_ART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Immutable native Palace column art, prepared from the session ROM. No
 * renderer or prior Palace visit is needed. Never distributes ROM pixels. */
enum { kChurchColumnWidth = 28, kChurchColumnHeight = 144 };
bool ChurchArt_Init(const uint8_t *rom, size_t size);
const uint32_t *ChurchArt_ColumnPixels(void);
#endif
