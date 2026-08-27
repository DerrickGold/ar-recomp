#include "sim_render_atlas.h"

#include <string.h>

#include "sim_render_metadata.h"

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

/* The packed rectangle the previous build wrote into. Only that region can hold
 * stale pixels: the atlas starts zeroed and nothing has ever written outside
 * the union of the rectangles cleared here, so the transparent padding the
 * presenter's LINEAR sampling relies on is preserved without memsetting all
 * 1 MB every frame. */
static int s_dirty_width, s_dirty_height;

static void ClearPackedRegion(void) {
  for (int y = 0; y < s_dirty_height; y++)
    memset(&g_sim_obj_atlas_pixels[(size_t)y * kSimObjAtlasWidth], 0,
           (size_t)s_dirty_width * sizeof(uint32_t));
  s_dirty_width = s_dirty_height = 0;
}

static bool PartBounds(const SrPpuObjPart *parts, uint32_t part_count,
                       SrPpuObjResolveResult *bounds) {
  int x0 = INT16_MAX;
  int y0 = INT16_MAX;
  int x1 = INT16_MIN;
  int y1 = INT16_MIN;
  if (!parts || !part_count || !bounds) return false;
  for (uint32_t i = 0; i < part_count; i++) {
    const SrPpuObjPart *part = &parts[i];
    if (part->reserved || !part->size || part->size > 64u ||
        (part->size & 7u))
      return false;
    if (part->x < x0) x0 = part->x;
    if (part->y < y0) y0 = part->y;
    if (part->x + part->size > x1) x1 = part->x + part->size;
    if (part->y + part->size > y1) y1 = part->y + part->size;
  }
  if (x1 <= x0 || y1 <= y0 || x1 > INT16_MAX || y1 > INT16_MAX)
    return false;
  bounds->part_count = part_count;
  bounds->x0 = x0;
  bounds->y0 = y0;
  bounds->x1 = x1;
  bounds->y1 = y1;
  return true;
}

bool SimRenderAtlas_Build(SrRunnerHandle *runner,
                          uint16 camera_x, uint16 camera_y) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrGenerationSnapshot generations = {
      .struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE,
  };
  SimAtlasBuildInput input;
  if (!SimRenderMetadata_CopyAtlasInput(&input))
    return false;

  ClearPackedRegion();
  uint32_t failure = !api || !runner ||
      api->struct_size < SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE ||
      (api->capabilities &
       (SR_RUNNER_CAP_GENERATION_COUNTERS | SR_RUNNER_CAP_PPU_OBJ_RASTER)) !=
          (SR_RUNNER_CAP_GENERATION_COUNTERS | SR_RUNNER_CAP_PPU_OBJ_RASTER) ||
      api->query_generations(runner, &generations) != SR_RESULT_OK
      ? kSimMetadataIntegrity_AtlasRasterFailure : 0;
  int cursor_x = kAtlasPadding;
  int cursor_y = kAtlasPadding;
  int row_height = 0;
  int used_width = 0;
  int used_height = 0;
  int written_width = 0;
  int written_height = 0;

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
    SrPpuObjPart oam_parts[128];
    const SrPpuObjPart *parts = NULL;
    uint32_t part_count = 0;
    SrPpuObjResolveResult bounds = {
        .struct_size = SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE,
    };
    bool explicit_complete = object->part_count > 0 &&
        object->part_count ==
            object->oam_count + object->synthetic_part_count &&
        object->part_first + object->part_count <= input.part_count;
    if (explicit_complete) {
      parts = &input.parts[object->part_first];
      part_count = object->part_count;
      if (!PartBounds(parts, part_count, &bounds)) parts = NULL;
    } else if (object->oam_count) {
      SrPpuObjResolveRequest request = {
          .struct_size = SR_PPU_OBJ_RESOLVE_REQUEST_V2_SIZE,
          .lifetime_generation = generations.lifetime_generation,
          .first_sprite = object->oam_first,
          .sprite_count = object->oam_count,
          .priority = object->priority,
          .part_capacity =
              (uint32_t)(sizeof(oam_parts) / sizeof(oam_parts[0])),
          .parts = oam_parts,
      };
      if (api->resolve_ppu_obj_range(runner, &request, &bounds) ==
          SR_RESULT_OK) {
        parts = oam_parts;
        part_count = bounds.part_count;
      }
    }
    if (!parts || !part_count) {
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

    /* Recorded BEFORE the raster, unlike used_*: a rasterizer that fails
     * partway has still dirtied this rectangle, and next frame's clear has to
     * cover it even though the object gets purged below. */
    if (cursor_x + width > written_width) written_width = cursor_x + width;
    if (cursor_y + height > written_height) written_height = cursor_y + height;

    uint32_t *destination =
        &g_sim_obj_atlas_pixels[cursor_y * kSimObjAtlasWidth + cursor_x];
    SrPpuObjPartsRasterRequest raster = {
        .struct_size = SR_PPU_OBJ_PARTS_RASTER_REQUEST_V2_SIZE,
        .lifetime_generation = generations.lifetime_generation,
        .parts = parts,
        .part_count = part_count,
        .x0 = bounds.x0,
        .y0 = bounds.y0,
        .x1 = bounds.x1,
        .y1 = bounds.y1,
        .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels = destination,
        .pixel_byte_size =
            sizeof(g_sim_obj_atlas_pixels) -
            (uint64_t)(destination - g_sim_obj_atlas_pixels) *
                sizeof(*destination),
        .pitch_bytes = kSimObjAtlasPitch,
    };
    SrPpuObjRasterResult raster_result = {
        .struct_size = SR_PPU_OBJ_RASTER_RESULT_V2_SIZE,
    };
    if (api->rasterize_ppu_obj_parts(
            runner, &raster, &raster_result) != SR_RESULT_OK ||
        raster_result.width != (uint32_t)width ||
        raster_result.height != (uint32_t)height) {
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

  s_dirty_width = written_width;
  s_dirty_height = written_height;

  bool valid = failure == 0;
  if (!valid) {
    /* Discard this frame's packing entirely -- and clear it now rather than
     * leaving it for the next build, so a broken contract cannot leave
     * half-rasterized pixels in a buffer the presenter may still upload. */
    ClearPackedRegion();
    used_width = used_height = 0;
    for (uint16_t i = 0; i < input.object_count; i++)
      ClearObjectAtlasFields(&input.objects[i]);
  }
  return SimRenderMetadata_CommitAtlas(
      input.build_serial, input.objects, input.object_count, valid,
      kSimObjAtlasWidth, kSimObjAtlasHeight,
      (uint16_t)used_width, (uint16_t)used_height, failure) && valid;
}
