#ifndef AR_DIORAMA_VOXEL_EDIT_H
#define AR_DIORAMA_VOXEL_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Presentation-only authoring. Cell coordinates identify 16px BG1 scenery;
 * neither these records nor their artwork change native collision or priority.
 * Zero terrain_mask means US, otherwise US=1, JP=2, EU/GE=4. The room codec
 * resolves one terrain before consumers use the lookup helpers below. */
/* The bound is on compact records, not the number of covered map cells. */
enum { kDioramaVoxelCellMax = 256, kDioramaVoxelObjectMax = 64 };
enum {
  kDioramaVoxelCell_Front = 1u << 0,
  kDioramaVoxelCell_Back = 1u << 1,
  kDioramaVoxelCell_Width = 1u << 2,
  kDioramaVoxelCell_Offset = 1u << 3,
  kDioramaVoxelCell_Backing = 1u << 4,
  kDioramaVoxelCell_BackArt = 1u << 5,
  kDioramaVoxelCell_TopArt = 1u << 6,
  kDioramaVoxelCell_SideArt = 1u << 7,
  kDioramaVoxelCell_FrontArt = 1u << 8,
  kDioramaVoxelCell_LeftArt = 1u << 9,
  kDioramaVoxelCell_RightArt = 1u << 10,
  kDioramaVoxelCell_BottomArt = 1u << 11,
  kDioramaVoxelCell_Faces = 1u << 12,
  kDioramaVoxelCell_Shape = 1u << 13,
  kDioramaVoxelCell_FrontEnd = 1u << 14,
  kDioramaVoxelCell_BackEnd = 1u << 15,
  kDioramaVoxelCell_Bend = 1u << 16,
  kDioramaVoxelCell_Transform = 1u << 17,
};
enum {
  kDioramaVoxelFaceMask_Front = 1u << 0,
  kDioramaVoxelFaceMask_Back = 1u << 1,
  kDioramaVoxelFaceMask_Top = 1u << 2,
  kDioramaVoxelFaceMask_Bottom = 1u << 3,
  kDioramaVoxelFaceMask_Left = 1u << 4,
  kDioramaVoxelFaceMask_Right = 1u << 5,
  kDioramaVoxelFaceMask_All = 63,
};
typedef enum DioramaVoxelShape {
  kDioramaVoxelShape_Box,
  kDioramaVoxelShape_Taper,
  kDioramaVoxelShape_Curve,
} DioramaVoxelShape;
/* Taper interpolates linearly over shape_y..shape_y+shape_h; Curve uses
 * smoothstep. Front goes from its resolved semantic/authored depth to
 * front_end, or the resolved back when FrontEnd is absent. Back goes from
 * its resolved depth to back_end, or stays fixed when BackEnd is absent.
 * Consumers clamp back >= front at each point. All rectangles belonging to
 * one connected shape retain the same explicit domain when split or copied. */
typedef enum DioramaVoxelBacking {
  kDioramaVoxelBacking_Auto,
  kDioramaVoxelBacking_None,
  kDioramaVoxelBacking_Fill,
  kDioramaVoxelBacking_Replace,
} DioramaVoxelBacking;

/* Frozen displayed native words in the current room, not a fragile metatile
 * number or a donor's viewport coordinates. Palette/flip bits remain intact.
 * Crop coordinates are within the displayed 16px square; size is 8 or 16.
 * set=false explicitly selects the material's automatic artwork. */
typedef struct DioramaVoxelArt {
  uint16_t words[4];
  uint8_t bg, x, y, size;
  bool set;
} DioramaVoxelArt;

enum {
  kDioramaVoxelBend_None,
  kDioramaVoxelBend_X,
  kDioramaVoxelBend_Y,
};
/* An anchored translation, independent of the extrusion's depth profile.
 * The explicit shared domain is in original native artwork coordinates, so
 * splitting a record or changing width never moves its attachment point.
 * Smoothstep(clamp((source[axis]-anchor)/(tip-anchor))) weights all three
 * offsets equally on the front, back and caps, preserving local thickness.
 * Reversing anchor/tip keeps the opposite end fixed. XY offsets are native
 * pixels; positive depth is farther. Native collision/priority never move. */
typedef struct DioramaVoxelBend {
  uint8_t axis;
  int16_t anchor, tip;
  float dx, dy, dz;
} DioramaVoxelBend;

/* Shared affine transform of completed semantic/authored geometry, after an
 * anchored bend. Source cells, artwork UVs and native collision/priority stay
 * fixed. Every member of a group retains this same explicit pivot when a
 * region is split for a local artwork edit. XY use native world pixels; Z is
 * positive recession. Positive scales preserve winding and face ownership.
 * An absent transform has zero scales; a persisted identity uses scale 1. */
typedef struct DioramaVoxelTransform {
  float px, py, pz;
  float dx, dy, dz;
  float sx, sy, sz;
} DioramaVoxelTransform;

typedef struct DioramaVoxelCell {
  int16_t x, y;
  uint16_t w, h; /* Rectangle in 16px cells; zero retains legacy 1x1. */
  uint32_t fields;
  uint8_t terrain_mask, backing;
  float front, back; /* Recession in scene-depth units; positive is farther. */
  float width, offset_x; /* Native pixels; automatic width is 16. */
  DioramaVoxelArt backing_art, top_art, side_art;
  DioramaVoxelArt front_art, left_art, right_art, bottom_art;
  uint8_t faces, shape;
  int16_t shape_y;
  uint16_t shape_h; /* Shared native-pixel domain, preserved when splitting. */
  float front_end, back_end;
  DioramaVoxelBend bend;
  DioramaVoxelTransform transform;
} DioramaVoxelCell;

typedef enum DioramaVoxelObjectMode {
  kDioramaVoxelObject_Auto,
  kDioramaVoxelObject_Flat,
  kDioramaVoxelObject_Extruded,
} DioramaVoxelObjectMode;
enum {
  kDioramaVoxelObject_Parent = 1u << 0,
  kDioramaVoxelObject_Animation = 1u << 1,
};
enum {
  kDioramaVoxelObject_Mode = 1u << 0,
  kDioramaVoxelObject_Offset = 1u << 1,
  kDioramaVoxelObject_Thickness = 1u << 2,
  kDioramaVoxelObject_Width = 1u << 3,
};
typedef struct DioramaVoxelObject {
  /* Retained family identity and optional captured parent/animation filters.
   * Never an OAM index, pool address, generation or transient owner tag. */
  uint16_t source, parent, animation;
  uint8_t selector_fields, fields, terrain_mask, mode;
  float offset, thickness, width_scale;
} DioramaVoxelObject;

typedef struct DioramaVoxelEdits {
  uint16_t cell_count, object_count;
  DioramaVoxelCell cells[kDioramaVoxelCellMax];
  DioramaVoxelObject objects[kDioramaVoxelObjectMax];
} DioramaVoxelEdits;

/* Resolve whole-record ownership before using this mask. Explicit Auto
 * artwork still resets an older record, but does not itself request a new
 * authored surface or convert a native transform into a prism. */
static inline uint32_t DioramaVoxelCell_EffectiveFields(const DioramaVoxelCell *c) {
  if (!c) return 0;
  uint32_t fields = c->fields;
  const DioramaVoxelArt *art[] = {&c->backing_art, &c->top_art, &c->side_art,
      &c->front_art, &c->left_art, &c->right_art, &c->bottom_art};
  const uint32_t flags[] = {kDioramaVoxelCell_BackArt, kDioramaVoxelCell_TopArt,
      kDioramaVoxelCell_SideArt, kDioramaVoxelCell_FrontArt, kDioramaVoxelCell_LeftArt,
      kDioramaVoxelCell_RightArt, kDioramaVoxelCell_BottomArt};
  for (unsigned i = 0; i < sizeof(flags) / sizeof(*flags); ++i)
    if (!art[i]->set) fields &= ~flags[i];
  return fields;
}

static inline unsigned DioramaVoxelCell_Width(const DioramaVoxelCell *c) {
  return c->w ? c->w : 1;
}
static inline unsigned DioramaVoxelCell_Height(const DioramaVoxelCell *c) {
  return c->h ? c->h : 1;
}
static inline bool DioramaVoxelCell_SameRegion(const DioramaVoxelCell *a,
                                              const DioramaVoxelCell *b) {
  return a->x == b->x && a->y == b->y &&
      DioramaVoxelCell_Width(a) == DioramaVoxelCell_Width(b) &&
      DioramaVoxelCell_Height(a) == DioramaVoxelCell_Height(b);
}
static inline bool DioramaVoxelCell_Contains(const DioramaVoxelCell *c, int x, int y) {
  return x >= c->x && y >= c->y &&
      x - c->x < (int)DioramaVoxelCell_Width(c) &&
      y - c->y < (int)DioramaVoxelCell_Height(c);
}

/* Newer complete covering records win, matching regional resolution. Missing
 * fields inherit the semantic material, never an older overlapping record.
 * A zero-field rectangle is an explicit reset that masks older records. Exact
 * same-region replacements move to the end; other overlaps retain order. */
static inline const DioramaVoxelCell *DioramaVoxelEdits_Cell(
    const DioramaVoxelEdits *edits, int x, int y) {
  if (!edits || edits->cell_count > kDioramaVoxelCellMax) return NULL;
  for (unsigned i = edits->cell_count; i-- > 0;)
    if (DioramaVoxelCell_Contains(&edits->cells[i], x, y)) return &edits->cells[i];
  return NULL;
}
static inline bool DioramaVoxelObject_SameSelector(
    const DioramaVoxelObject *a, const DioramaVoxelObject *b) {
  return a->source == b->source && a->selector_fields == b->selector_fields &&
      (!(a->selector_fields & kDioramaVoxelObject_Parent) || a->parent == b->parent) &&
      (!(a->selector_fields & kDioramaVoxelObject_Animation) || a->animation == b->animation);
}
static inline const DioramaVoxelObject *DioramaVoxelEdits_Object(
    const DioramaVoxelEdits *edits, uint16_t source, uint16_t parent, uint16_t animation) {
  if (!edits || !source || edits->object_count > kDioramaVoxelObjectMax) return NULL;
  for (unsigned i = edits->object_count; i-- > 0;) {
    const DioramaVoxelObject *o = &edits->objects[i];
    if (o->source == source &&
        (!(o->selector_fields & kDioramaVoxelObject_Parent) || o->parent == parent) &&
        (!(o->selector_fields & kDioramaVoxelObject_Animation) || o->animation == animation)) return o;
  }
  return NULL;
}

/* Hash only semantic fields, never padding or inactive art. Native caches use
 * this digest so a Preview/Apply change invalidates immutable scene geometry. */
static inline uint64_t DioramaVoxel_HashBytes(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  while (size--) hash = (hash ^ *bytes++) * UINT64_C(1099511628211);
  return hash;
}
static inline uint64_t DioramaVoxelArt_Hash(uint64_t hash, const DioramaVoxelArt *art) {
  hash = DioramaVoxel_HashBytes(hash, &art->set, sizeof(art->set));
  if (!art->set) return hash;
  hash = DioramaVoxel_HashBytes(hash, art->words, sizeof(art->words));
  const uint8_t crop[] = {art->bg, art->x, art->y, art->size};
  return DioramaVoxel_HashBytes(hash, crop, sizeof(crop));
}
static inline uint64_t DioramaVoxelEdits_Hash(uint64_t hash, const DioramaVoxelEdits *edits) {
  if (!edits || edits->cell_count > kDioramaVoxelCellMax ||
      edits->object_count > kDioramaVoxelObjectMax) return hash;
  hash = DioramaVoxel_HashBytes(hash, &edits->cell_count, sizeof(edits->cell_count));
  hash = DioramaVoxel_HashBytes(hash, &edits->object_count, sizeof(edits->object_count));
  for (unsigned i = 0; i < edits->cell_count; ++i) {
    const DioramaVoxelCell *c = &edits->cells[i];
    const int16_t xy[] = {c->x, c->y};
    hash = DioramaVoxel_HashBytes(hash, xy, sizeof(xy));
    const uint16_t wh[] = {DioramaVoxelCell_Width(c), DioramaVoxelCell_Height(c)};
    hash = DioramaVoxel_HashBytes(hash, wh, sizeof(wh));
    hash = DioramaVoxel_HashBytes(hash, &c->fields, sizeof(c->fields));
    hash = DioramaVoxel_HashBytes(hash, &c->terrain_mask, sizeof(c->terrain_mask));
    if (c->fields & kDioramaVoxelCell_Front) hash = DioramaVoxel_HashBytes(hash, &c->front, sizeof(c->front));
    if (c->fields & kDioramaVoxelCell_Back) hash = DioramaVoxel_HashBytes(hash, &c->back, sizeof(c->back));
    if (c->fields & kDioramaVoxelCell_Width) hash = DioramaVoxel_HashBytes(hash, &c->width, sizeof(c->width));
    if (c->fields & kDioramaVoxelCell_Offset) hash = DioramaVoxel_HashBytes(hash, &c->offset_x, sizeof(c->offset_x));
    if (c->fields & kDioramaVoxelCell_Backing) hash = DioramaVoxel_HashBytes(hash, &c->backing, sizeof(c->backing));
    if (c->fields & kDioramaVoxelCell_BackArt) hash = DioramaVoxelArt_Hash(hash, &c->backing_art);
    if (c->fields & kDioramaVoxelCell_TopArt) hash = DioramaVoxelArt_Hash(hash, &c->top_art);
    if (c->fields & kDioramaVoxelCell_SideArt) hash = DioramaVoxelArt_Hash(hash, &c->side_art);
    if (c->fields & kDioramaVoxelCell_FrontArt) hash = DioramaVoxelArt_Hash(hash, &c->front_art);
    if (c->fields & kDioramaVoxelCell_LeftArt) hash = DioramaVoxelArt_Hash(hash, &c->left_art);
    if (c->fields & kDioramaVoxelCell_RightArt) hash = DioramaVoxelArt_Hash(hash, &c->right_art);
    if (c->fields & kDioramaVoxelCell_BottomArt) hash = DioramaVoxelArt_Hash(hash, &c->bottom_art);
    if (c->fields & kDioramaVoxelCell_Faces) hash = DioramaVoxel_HashBytes(hash, &c->faces, sizeof(c->faces));
    if (c->fields & kDioramaVoxelCell_Shape) {
      hash = DioramaVoxel_HashBytes(hash, &c->shape, sizeof(c->shape));
      if (c->shape != kDioramaVoxelShape_Box) {
        hash = DioramaVoxel_HashBytes(hash, &c->shape_y, sizeof(c->shape_y));
        hash = DioramaVoxel_HashBytes(hash, &c->shape_h, sizeof(c->shape_h));
      }
    }
    if (c->fields & kDioramaVoxelCell_FrontEnd) hash = DioramaVoxel_HashBytes(hash, &c->front_end, sizeof(c->front_end));
    if (c->fields & kDioramaVoxelCell_BackEnd) hash = DioramaVoxel_HashBytes(hash, &c->back_end, sizeof(c->back_end));
    if (c->fields & kDioramaVoxelCell_Bend) {
      hash = DioramaVoxel_HashBytes(hash, &c->bend.axis, sizeof(c->bend.axis));
      const int16_t domain[] = {c->bend.anchor, c->bend.tip};
      const float offsets[] = {c->bend.dx, c->bend.dy, c->bend.dz};
      hash = DioramaVoxel_HashBytes(hash, domain, sizeof(domain));
      hash = DioramaVoxel_HashBytes(hash, offsets, sizeof(offsets));
    }
    if (c->fields & kDioramaVoxelCell_Transform) {
      const float transform[] = {c->transform.px, c->transform.py, c->transform.pz,
          c->transform.dx, c->transform.dy, c->transform.dz,
          c->transform.sx, c->transform.sy, c->transform.sz};
      hash = DioramaVoxel_HashBytes(hash, transform, sizeof(transform));
    }
  }
  for (unsigned i = 0; i < edits->object_count; ++i) {
    const DioramaVoxelObject *o = &edits->objects[i];
    hash = DioramaVoxel_HashBytes(hash, &o->source, sizeof(o->source));
    const uint8_t fields[] = {o->selector_fields, o->fields, o->terrain_mask};
    hash = DioramaVoxel_HashBytes(hash, fields, sizeof(fields));
    if (o->selector_fields & kDioramaVoxelObject_Parent) hash = DioramaVoxel_HashBytes(hash, &o->parent, sizeof(o->parent));
    if (o->selector_fields & kDioramaVoxelObject_Animation) hash = DioramaVoxel_HashBytes(hash, &o->animation, sizeof(o->animation));
    if (o->fields & kDioramaVoxelObject_Mode) hash = DioramaVoxel_HashBytes(hash, &o->mode, sizeof(o->mode));
    if (o->fields & kDioramaVoxelObject_Offset) hash = DioramaVoxel_HashBytes(hash, &o->offset, sizeof(o->offset));
    if (o->fields & kDioramaVoxelObject_Thickness) hash = DioramaVoxel_HashBytes(hash, &o->thickness, sizeof(o->thickness));
    if (o->fields & kDioramaVoxelObject_Width) hash = DioramaVoxel_HashBytes(hash, &o->width_scale, sizeof(o->width_scale));
  }
  return hash;
}

#endif
