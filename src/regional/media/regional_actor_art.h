#ifndef AR_REGIONAL_ACTOR_ART_H
#define AR_REGIONAL_ACTOR_ART_H

#include "regional/media/regional_media.h"

enum {
  kArRegionalActorArt_MaximumEntries = 256,
  kArRegionalActorArt_MaximumPictures = 256,
  kArRegionalActorArt_MaximumParts = 128,
};
typedef enum ArRegionalActorArtKind {
  kArRegionalActorArt_Characters = 1,
  kArRegionalActorArt_Palette = 2,
  kArRegionalActorArt_Pictures = 3,
} ArRegionalActorArtKind;

/* Immutable borrowed storage. Parse once when registering a validated donor;
 * Find is a bounded binary search at asset-load time, not a per-part parser.
 * Keys refer to scene declarations, NOT animation-bank/graphics-bank aliases.
 * Callers own inheritance and coherent activation of all three resource kinds. */
typedef struct ArRegionalActorArtView {
  ArRegionalMediaBytes bytes;
  unsigned count;
} ArRegionalActorArtView;
bool ArRegionalActorArt_Parse(ArRegionalMediaBytes bytes,ArRegionalActorArtView *out);
ArRegionalMediaBytes ArRegionalActorArt_Find(const ArRegionalActorArtView *view,
    uint16_t scene,ArRegionalActorArtKind kind,unsigned slot);

/* Draw coordinates relative to the actor's world anchor, before native draw
 * bias. These are NOT actor extents: never write them to simulation memory.
 * No hitbox, source program, frame timer or host renderer handle crosses here. */
typedef struct ArRegionalActorArtPart {
  int16_t x[2],y[2];
  uint16_t attributes;
  bool large;
} ArRegionalActorArtPart;
typedef struct ArRegionalActorArtPicture {
  ArRegionalMediaBytes parts;
  unsigned count;
} ArRegionalActorArtPicture;
/* The table must come from Find on a successfully parsed immutable view.
 * Missing ordinals fail closed; callers must explicitly map regional ordinal
 * differences instead of clamping/wrapping into an unrelated picture. */
bool ArRegionalActorArt_Picture(ArRegionalMediaBytes table,unsigned ordinal,
    ArRegionalActorArtPicture *out);
bool ArRegionalActorArt_Part(const ArRegionalActorArtPicture *picture,unsigned index,
    ArRegionalActorArtPart *out);

#endif
