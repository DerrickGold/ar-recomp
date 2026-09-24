#ifndef ACTRAISER_ACTOR_ART_H
#define ACTRAISER_ACTOR_ART_H

#include "regional/regional_actor_art_residency.h"

/* Boot/shutdown only. The validated donor remains immutable and owned by the
 * media store. This owner registers/detaches the game decompression observer. */
void ActRaiserActorArt_Initialize(const ArRegionalActorArtView *donor);
void ActRaiserActorArt_Shutdown(void);
/* Start of the actual asset VM, including retries. This does not activate a
 * pending setting: inherited/partial room uploads keep the stage's artwork.
 * Only an act-entry script (ordinary animation-bank replacement) activates. */
void ActRaiserActorArt_BeginRoom(uint16_t scene);
/* Cheap draw gate: false includes missing donors. Does not inspect settings
 * or activate anything; lets native rendering avoid extra actor reads. */
bool ActRaiserActorArt_Active(void);
bool ActRaiserActorArt_NeedsUpload(uint16_t scene,ArRegionalActorArtKind kind,
    unsigned slot,uint32_t source);
/* Accepted native upload only. All matching uploads in this invocation share
 * one captured choice, including a settings edit between CHR and palette. */
bool ActRaiserActorArt_Upload(uint16_t scene,ArRegionalActorArtKind kind,
    unsigned slot,uint32_t source,ArRegionalMediaBytes *out);

typedef struct ActRaiserActorArtDraw {
  ArRegionalActorArtPicture picture;
  bool attributes_only; /* Keep native/projected plant geometry and part count. */
} ActRaiserActorArtDraw;
/* O(1) resident-picture lookup, no ROM decode, hashing, settings validation or
 * allocation per actor/part. The caller verifies native actor bank/context. */
bool ActRaiserActorArt_Draw(uint16_t base,uint16_t composition,unsigned visual,
    ActRaiserActorArtDraw *out);

typedef struct ActRaiserActorArtPart {
  int x, y; /* Relative to the native draw origin; may be negative. */
  uint16_t attributes;
  bool large;
} ActRaiserActorArtPart;
/* Native extents are read-only compensation for the native draw origin, not
 * replacement hitboxes. Attribute-only plans retain the supplied geometry. */
void ActRaiserActorArt_ResolvePart(const ActRaiserActorArtDraw *draw,unsigned index,
    bool flip_x,bool flip_y,int16_t left,int16_t top,ActRaiserActorArtPart *part);

#endif
