#ifndef AR_REGIONAL_ACTOR_ART_RESIDENCY_H
#define AR_REGIONAL_ACTOR_ART_RESIDENCY_H

#include "regional/media/regional_actor_art.h"

typedef struct ArRegionalActorArtBinding {
  uint16_t scene;
  uint8_t kind,slot;
  uint32_t source; /* US ROM file offset, before the compressed size header. */
  uint16_t size,table,pictures;
} ArRegionalActorArtBinding;
const ArRegionalActorArtBinding *ArRegionalActorArt_Binding(uint16_t scene,
    ArRegionalActorArtKind kind,unsigned slot);

/* A picture's native allocation may overlap another animation slot or the
 * graphics workspace. Preserve only those allocations not subsequently
 * overwritten; do not assume that each slot owns an exclusive 4 KiB bank. */
typedef struct ArRegionalActorArtResidentBank {
  const ArRegionalActorArtBinding *binding;
  uint16_t begin[kArRegionalActorArt_MaximumPictures];
  uint16_t end[kArRegionalActorArt_MaximumPictures];
  uint8_t valid[kArRegionalActorArt_MaximumPictures];
} ArRegionalActorArtResidentBank;
typedef struct ArRegionalActorArtResidency {
  ArRegionalActorArtResidentBank banks[2];
} ArRegionalActorArtResidency;

/* Notify after a complete native decode, before its detached output expires.
 * Every write can retire overlapping pictures, even from an unrelated source.
 * True means a recognized, bounded native animation was newly bound. Neither
 * function writes to native RAM or imports donor code/collision metadata. */
bool ArRegionalActorArt_ObserveDecode(ArRegionalActorArtResidency *residency,
    uint32_t source,uint16_t destination,ArRegionalMediaBytes decoded);
const ArRegionalActorArtBinding *ArRegionalActorArt_Resident(
    const ArRegionalActorArtResidency *residency,uint16_t base,
    uint16_t composition,unsigned visual);

#endif
