#ifndef AR_ACTRAISER_WORLD_LOCATIONS_H
#define AR_ACTRAISER_WORLD_LOCATIONS_H

#include <stdbool.h>
#include <stdint.h>

/* ROM $01:B73C: top-left source pixels of the native 256x256 selection
 * regions. Shared by gameplay focus restoration and presentation. IDs are
 * one-based; Death Heim is a location but is not a SIM town. */
typedef struct ActRaiserWorldRegion {
  uint16_t x, y;
} ActRaiserWorldRegion;

static inline bool ActRaiserWorldLocation_Region(unsigned location,
                                                ActRaiserWorldRegion *out) {
  static const ActRaiserWorldRegion regions[] = {
      {640, 384}, /* Fillmore */
      {384, 384}, /* Bloodpool */
      {128, 512}, /* Kasandora */
      {128, 256}, /* Aitos */
      {512, 768}, /* Marahna */
      {256,   0}, /* Northwall */
      {640,   0}, /* Death Heim */
  };
  if (!out || location < 1 || location > 7) return false;
  *out = regions[location - 1];
  return true;
}

#endif
