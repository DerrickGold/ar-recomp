#include "sim_render_atlas.h"

#include <string.h>

#include "sim_render_metadata.h"
#include "snes/ppu.h"

enum { kAtlasPadding = 1 };

uint32_t g_sim_obj_atlas_pixels[
    kSimObjAtlasWidth * kSimObjAtlasHeight];

static void ClearObjectAtlasFields(SimRenderObject *object) {
  object->local_x0 = object->local_y0 = 0;
  object->local_x1 = object->local_y1 = 0;
  object->atlas_x = object->atlas_y = 0;
  object->atlas_w = object->atlas_h = 0;
  object->atlas_valid = 0;
}

bool SimRenderAtlas_Build(Ppu *ppu, uint16 camera_x, uint16 camera_y) {
  SimAtlasBuildInput input;
  if (!SimRenderMetadata_CopyAtlasInput(&input))
    return false;

  memset(g_sim_obj_atlas_pixels, 0, sizeof(g_sim_obj_atlas_pixels));
  uint32_t failure = ppu ? 0 : kSimMetadataIntegrity_AtlasRasterFailure;
  int cursor_x = kAtlasPadding;
  int cursor_y = kAtlasPadding;
  int row_height = 0;
  int used_width = 0;
  int used_height = 0;

  /* Per-object failures purge that object and keep going; only a broken
   * contract (no PPU, or a source index outside the record array) fails the
   * whole build.
   *
   * This used to be all-or-nothing, on the principle that a partial atlas is
   * not a trustworthy one. In practice "not trustworthy" meant the frame's
   * metadata went invalid, which used to drop the entire view to the flat
   * composite -- a full-screen perspective flash standing in for one sprite
   * that could not be packed. The ROM itself makes actors vanish at the
   * screen edge, so a purged fragment is both proportionate and authentic.
   * Purged objects keep atlas_valid clear and are counted by the D1 census,
   * so the condition is still reported and still fails a checkpoint; it just
   * no longer costs the frame. */
  for (uint16_t i = 0; !failure && i < input.object_count; i++) {
    SimRenderObject *object = &input.objects[i];
    PpuObjRangeBounds bounds;
    if (!PpuGetObjRangeBounds(ppu, (uint8_t)object->oam_first,
                              object->oam_count, object->priority, &bounds)) {
      ClearObjectAtlasFields(object);
      continue;
    }
    int width = bounds.x1 - bounds.x0;
    int height = bounds.y1 - bounds.y0;
    /* Too large to ever fit -- a composition whose parts straddle the OAM X
     * wrap produces a union box hundreds of pixels wide. */
    if (width <= 0 || height <= 0 ||
        width + 2 * kAtlasPadding > kSimObjAtlasWidth ||
        height + 2 * kAtlasPadding > kSimObjAtlasHeight) {
      ClearObjectAtlasFields(object);
      continue;
    }
    if (cursor_x + width + kAtlasPadding > kSimObjAtlasWidth) {
      cursor_x = kAtlasPadding;
      cursor_y += row_height + kAtlasPadding;
      row_height = 0;
    }
    /* The atlas is full for this frame; purge the remainder rather than
     * discard the objects that did pack. */
    if (cursor_y + height + kAtlasPadding > kSimObjAtlasHeight) {
      ClearObjectAtlasFields(object);
      continue;
    }

    int anchor_x = (int)(int16_t)(uint16_t)(object->world_x - camera_x);
    int anchor_y = (int)(int16_t)(uint16_t)(object->world_y - camera_y);
    object->local_x0 = (int16_t)(bounds.x0 - anchor_x);
    object->local_y0 = (int16_t)(bounds.y0 - anchor_y);
    object->local_x1 = (int16_t)(bounds.x1 - anchor_x);
    object->local_y1 = (int16_t)(bounds.y1 - anchor_y);
    object->atlas_x = (uint16_t)cursor_x;
    object->atlas_y = (uint16_t)cursor_y;
    object->atlas_w = (uint16_t)width;
    object->atlas_h = (uint16_t)height;
    object->atlas_valid = 1;

    uint32_t *destination =
        &g_sim_obj_atlas_pixels[cursor_y * kSimObjAtlasWidth + cursor_x];
    if (!PpuRasterizeObjRange(
            ppu, (uint8_t)object->oam_first, object->oam_count,
            object->priority, &bounds, destination, width, height,
            kSimObjAtlasPitch)) {
      ClearObjectAtlasFields(object);
      continue;
    }

    if (cursor_x + width > used_width) used_width = cursor_x + width;
    if (cursor_y + height > used_height) used_height = cursor_y + height;
    if (height > row_height) row_height = height;
    cursor_x += width + kAtlasPadding;
  }

  /* A source record can be split into several priority fragments. Derive one
   * shared bottom-centre foot from the union of those fragments, otherwise a
   * projected multipart actor would hinge each band around a different point
   * and visibly tear while the camera tilts or zooms. local_* is relative to
   * the record origin here; the presenter subtracts this shared foot offset
   * when placing every fragment. */
  if (!failure) {
    int16_t min_x[kSimMaxSourceRecords] = {0};
    int16_t max_x[kSimMaxSourceRecords] = {0};
    int16_t max_y[kSimMaxSourceRecords] = {0};
    bool seen[kSimMaxSourceRecords] = {false};
    for (uint16_t i = 0; i < input.object_count; i++) {
      const SimRenderObject *object = &input.objects[i];
      unsigned source = object->source_index;
      if (source >= kSimMaxSourceRecords) {
        failure = kSimMetadataIntegrity_AtlasRasterFailure;
        break;
      }
      /* A purged fragment has zeroed local bounds; folding those into the
       * union would drag the shared foot toward the record origin and tear
       * the fragments that did pack. */
      if (!object->atlas_valid) continue;
      if (!seen[source]) {
        seen[source] = true;
        min_x[source] = object->local_x0;
        max_x[source] = object->local_x1;
        max_y[source] = object->local_y1;
      } else {
        if (object->local_x0 < min_x[source])
          min_x[source] = object->local_x0;
        if (object->local_x1 > max_x[source])
          max_x[source] = object->local_x1;
        if (object->local_y1 > max_y[source])
          max_y[source] = object->local_y1;
      }
    }
    for (uint16_t i = 0; !failure && i < input.object_count; i++) {
      SimRenderObject *object = &input.objects[i];
      if (object->tier != kSimRecordTier_World) continue;
      unsigned source = object->source_index;
      int foot_dx = (min_x[source] + max_x[source]) / 2;
      int foot_dy = max_y[source];
      object->foot_x = (int16_t)(object->world_x + foot_dx);
      object->foot_y = (int16_t)(object->world_y + foot_dy);
    }
  }

  bool valid = failure == 0;
  if (!valid) {
    memset(g_sim_obj_atlas_pixels, 0, sizeof(g_sim_obj_atlas_pixels));
    used_width = used_height = 0;
    for (uint16_t i = 0; i < input.object_count; i++)
      ClearObjectAtlasFields(&input.objects[i]);
  }
  return SimRenderMetadata_CommitAtlas(
      input.build_serial, input.objects, input.object_count, valid,
      kSimObjAtlasWidth, kSimObjAtlasHeight,
      (uint16_t)used_width, (uint16_t)used_height, failure) && valid;
}
