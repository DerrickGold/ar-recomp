#ifndef AR_ACTION_VOXEL_SPRITES_H
#define AR_ACTION_VOXEL_SPRITES_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "diorama/diorama_voxel_edit.h"

/* Small, bounded native compositions. Pixels travel in a separate owned
 * surface; this value snapshot contains only identity, placement and alpha. */
enum {
  kVoxelSpriteLimit = 16, kVoxelSpriteWidth = 64, kVoxelSpriteHeight = 32,
  kVoxelSpriteCellWidth = 66, kVoxelSpriteCellHeight = 34,
  kVoxelSpriteAtlasWidth = 264, kVoxelSpriteAtlasHeight = 136,
  kActionVoxelSpriteGenericShape = 9,
};
typedef struct ActionVoxelSprite {
  uint16_t owner;
  uint8_t shape, priority, width, height;
  uint8_t owner_tag; /* Capture-only OAM ownership, not stable component identity. */
  int16_t component_x, component_y; /* Relative to the emitted composition origin. */
  uint8_t solid_y, solid_height; /* Other rows use crossed spike profiles. */
  uint32_t join_left, join_right; /* Opaque neighbours across a section boundary. */
  uint64_t join_top, join_bottom;
  float x, y; /* Capture-surface coordinates, including its apron. */
  float ambient;
  /* Canonical native family facts. owner/owner_tag remain capture-local and
   * must never be serialized as an authored selector. */
  uint16_t source, parent_source, animation;
  uint32_t generation; /* Immutable actor lifetime; never a saved selector. */
  uint8_t presentation_mode, presentation_fields;
  float depth_offset, thickness, width_scale;
  uint64_t rows[kVoxelSpriteHeight];
} ActionVoxelSprite;
typedef struct ActionVoxelSpriteFrame {
  unsigned count;
  ActionVoxelSprite sprites[kVoxelSpriteLimit];
} ActionVoxelSpriteFrame;

static inline bool ActionVoxelSprite_IsModeled(const ActionVoxelSprite *p) {
  return p && p->presentation_mode != kDioramaVoxelObject_Flat &&
      (p->shape != kActionVoxelSpriteGenericShape ||
       p->presentation_mode == kDioramaVoxelObject_Extruded);
}
/* Re-resolve against the current project, not the rules active when a
 * diagnostic pose was captured. Unauthored generic compositions stay native. */
static inline void ActionVoxelSprite_Resolve(ActionVoxelSprite *p,
    const DioramaVoxelEdits *edits) {
  p->presentation_mode=kDioramaVoxelObject_Auto;p->presentation_fields=0;
  p->depth_offset=p->thickness=0;p->width_scale=1;
  const DioramaVoxelObject *o=DioramaVoxelEdits_Object(edits,p->source,p->parent_source,p->animation);
  if(!o)return;
  p->presentation_fields=o->fields;
  if(o->fields&kDioramaVoxelObject_Mode)p->presentation_mode=o->mode;
  if(o->fields&kDioramaVoxelObject_Offset)p->depth_offset=o->offset;
  if(o->fields&kDioramaVoxelObject_Thickness)p->thickness=o->thickness;
  if(o->fields&kDioramaVoxelObject_Width)p->width_scale=o->width_scale;
}

/* Hand off only this modeled component's native winning pixels. The caller
 * supplies a copied OBJ surface and the immutable, unfiltered owner attributes
 * from the same capture. Failed/unmodeled props keep their original pixels.
 * Capture coordinates are integral; never use interpolated presentation data. */
static inline void ActionVoxelSprite_RemoveNative(const ActionVoxelSprite *p,
    unsigned width, unsigned height, uint32_t *pixels, size_t pitch,
    const uint32_t *attributes, size_t attribute_pitch) {
  if (!ActionVoxelSprite_IsModeled(p) || !p->owner_tag || p->owner_tag>128 || !pixels || !attributes ||
      !p->width || p->width>kVoxelSpriteWidth || !p->height || p->height>kVoxelSpriteHeight ||
      pitch<width || attribute_pitch<width ||
      !(p->x>=-(float)p->width && p->x<(float)width &&
        p->y>=-(float)p->height && p->y<(float)height)) return;
  const int x0=(int)p->x,y0=(int)p->y;
  for (unsigned y=0;y<p->height;++y) {
    const int py=y0+(int)y;
    if (py<0 || (unsigned)py>=height) continue;
    for (unsigned x=0;x<p->width;++x) {
      const int px=x0+(int)x;
      if (px<0 || (unsigned)px>=width || !(p->rows[y]&(UINT64_C(1)<<x))) continue;
      if ((attributes[(size_t)py*attribute_pitch+px]>>24)==p->owner_tag)
        pixels[(size_t)py*pitch+px]=0;
    }
  }
}

static inline bool ActionVoxelSprite_SameComponent(const ActionVoxelSprite *a,
    const ActionVoxelSprite *b) {
  return a->owner==b->owner && a->shape==b->shape && a->priority==b->priority &&
      a->generation==b->generation &&
      (a->shape!=kActionVoxelSpriteGenericShape || a->generation!=0) &&
      a->component_x==b->component_x && a->component_y==b->component_y &&
      a->width==b->width && a->height==b->height &&
      a->solid_y==b->solid_y && a->solid_height==b->solid_height &&
      a->join_left==b->join_left && a->join_right==b->join_right &&
      a->join_top==b->join_top && a->join_bottom==b->join_bottom &&
      a->source==b->source && a->parent_source==b->parent_source && a->animation==b->animation &&
      a->presentation_mode==b->presentation_mode && a->presentation_fields==b->presentation_fields &&
      a->depth_offset==b->depth_offset && a->thickness==b->thickness && a->width_scale==b->width_scale &&
      memcmp(a->rows,b->rows,sizeof(a->rows))==0;
}
#endif
