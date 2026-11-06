#ifndef AR_ACTION_VOXEL_SPRITES_H
#define AR_ACTION_VOXEL_SPRITES_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Small, bounded native compositions. Pixels travel in a separate owned
 * surface; this value snapshot contains only identity, placement and alpha. */
enum {
  kVoxelSpriteLimit = 16, kVoxelSpriteWidth = 64, kVoxelSpriteHeight = 32,
  kVoxelSpriteCellWidth = 66, kVoxelSpriteCellHeight = 34,
  kVoxelSpriteAtlasWidth = 264, kVoxelSpriteAtlasHeight = 136,
};
typedef struct ActionVoxelSprite {
  uint16_t owner;
  uint8_t shape, priority, width, height;
  uint8_t owner_tag; /* Capture-only OAM ownership, not stable component identity. */
  int16_t component_x, component_y; /* Relative to the emitted composition origin. */
  uint8_t solid_y, solid_height; /* Other rows use crossed spike profiles. */
  uint32_t join_left, join_right; /* Opaque neighbours across a section boundary. */
  float x, y; /* Capture-surface coordinates, including its apron. */
  float ambient;
  uint64_t rows[kVoxelSpriteHeight];
} ActionVoxelSprite;
typedef struct ActionVoxelSpriteFrame {
  unsigned count;
  ActionVoxelSprite sprites[kVoxelSpriteLimit];
} ActionVoxelSpriteFrame;

/* Hand off only this modeled component's native winning pixels. The caller
 * supplies a copied OBJ surface and the immutable, unfiltered owner attributes
 * from the same capture. Failed/unmodeled props keep their original pixels.
 * Capture coordinates are integral; never use interpolated presentation data. */
static inline void ActionVoxelSprite_RemoveNative(const ActionVoxelSprite *p,
    unsigned width, unsigned height, uint32_t *pixels, size_t pitch,
    const uint32_t *attributes, size_t attribute_pitch) {
  if (!p || !p->owner_tag || p->owner_tag>128 || !pixels || !attributes ||
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
      a->component_x==b->component_x && a->component_y==b->component_y &&
      a->width==b->width && a->height==b->height &&
      a->solid_y==b->solid_y && a->solid_height==b->solid_height &&
      a->join_left==b->join_left && a->join_right==b->join_right &&
      memcmp(a->rows,b->rows,sizeof(a->rows))==0;
}
#endif
