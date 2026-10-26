#ifndef AR_AR2_WORLD_ATLAS_H
#define AR_AR2_WORLD_ATLAS_H
/* ROM-derived geography and destination records, independent of execution,
 * progression policy and presentation. Currently the verified USA revision. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kAr2WorldAtlasPixels = 1024,
  kAr2WorldDestinationCapacity = 22,
};

typedef struct Ar2WorldDestination {
  uint16_t x_min, x_max, y_min, y_max;
  uint16_t flags_and_scene, progress_list, unlock_gate, completion_gate;
  /* Stable native record identity: co-located progression variants are distinct. */
  unsigned record_index;
  char label[24];
} Ar2WorldDestination;

typedef struct Ar2WorldAtlas {
  bool available;
  size_t destination_count;
  Ar2WorldDestination destinations[kAr2WorldDestinationCapacity];
  /* ARGB8888; native direct-color texels. Deep ocean remains opaque here:
   * the presentation decides how to join continents to its shared ocean. */
  uint32_t pixels[kAr2WorldAtlasPixels * kAr2WorldAtlasPixels];
  /* Native direct-color indices preserve material identity for presentation.
   * In this map $80 is deep ocean, including every texel of CHR tile $00. */
  uint8_t indices[kAr2WorldAtlasPixels * kAr2WorldAtlasPixels];
} Ar2WorldAtlas;

/* Caller owns the large output (prefer heap allocation). On failure it is
 * unavailable and must not be consumed. No assets or progression are cached. */
bool Ar2WorldAtlas_Decode(Ar2WorldAtlas *out, const uint8_t *rom, size_t size);

#endif
