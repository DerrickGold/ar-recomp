#include "action_obj_interpolation.h"

#include <stddef.h>
#include <string.h>

#include "snes/ppu.h"

typedef struct ActionObjEmitterIdentity {
  bool valid;
  uint16_t object_address;
  uint16_t object_signature;
  int16_t anchor_x, anchor_y;
} ActionObjEmitterIdentity;

typedef struct ActionObjSyntheticPart {
  PpuObjPart part;
  ActionObjEmitterIdentity identity;
} ActionObjSyntheticPart;

static bool s_emitter_enabled;
static ActionObjEmitterIdentity s_oam_identity[128];
static ActionObjSyntheticPart
    s_synthetic[kActionObjInterpolationMaxParts - 128];
static int s_synthetic_count;

uint32_t g_action_obj_interpolation_atlas_pixels[
    kActionObjInterpolationAtlasWidth *
    kActionObjInterpolationAtlasHeight];

static int s_atlas_dirty_width;
static int s_atlas_dirty_height;
static uint8_t s_claimed[kPpuSurfaceWidth * kPpuBufHeight];
static uint32_t s_raster[64 * 64];
static uint32_t s_masked[64 * 64];

static int16_t ClampInt16(int value) {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return (int16_t)value;
}

void ActionObjInterpolation_BeginFrame(bool enabled) {
  s_emitter_enabled = enabled;
  memset(s_oam_identity, 0, sizeof(s_oam_identity));
  s_synthetic_count = 0;
}

void ActionObjInterpolation_RecordOamPart(
    uint8_t slot, uint16_t object_address, uint16_t object_signature,
    int screen_anchor_x, int screen_anchor_y) {
  if (!s_emitter_enabled || slot >= 128 || !object_address) return;
  s_oam_identity[slot] = (ActionObjEmitterIdentity){
    .valid = true,
    .object_address = object_address,
    .object_signature = object_signature,
    .anchor_x = ClampInt16(screen_anchor_x),
    .anchor_y = ClampInt16(screen_anchor_y),
  };
}

void ActionObjInterpolation_RecordSyntheticPart(
    int x, int y, uint16_t tile_attr, uint8_t size,
    uint16_t object_address, uint16_t object_signature,
    int screen_anchor_x, int screen_anchor_y) {
  if (!s_emitter_enabled || !size || !object_address ||
      s_synthetic_count >=
          (int)(sizeof(s_synthetic) / sizeof(s_synthetic[0])))
    return;
  ActionObjSyntheticPart *part = &s_synthetic[s_synthetic_count++];
  part->part = (PpuObjPart){
    .x = ClampInt16(x),
    .y = ClampInt16(y),
    .tile_attr = tile_attr,
    .size = size,
  };
  part->identity = (ActionObjEmitterIdentity){
    .valid = true,
    .object_address = object_address,
    .object_signature = object_signature,
    .anchor_x = ClampInt16(screen_anchor_x),
    .anchor_y = ClampInt16(screen_anchor_y),
  };
}

static void ClearAtlas(void) {
  for (int y = 0; y < s_atlas_dirty_height; y++)
    memset(&g_action_obj_interpolation_atlas_pixels[
               (size_t)y * kActionObjInterpolationAtlasWidth],
           0, (size_t)s_atlas_dirty_width * sizeof(uint32_t));
  s_atlas_dirty_width = 0;
  s_atlas_dirty_height = 0;
}

static bool PartIntersectsSurface(const PpuObjPart *part,
                                  int surface_width, int surface_height,
                                  int screen_x_origin,
                                  int screen_y_origin) {
  return part && part->size &&
      part->x + part->size > -screen_x_origin &&
      part->x < surface_width - screen_x_origin &&
      part->y + part->size > -screen_y_origin &&
      part->y < surface_height - screen_y_origin;
}

typedef struct AtlasPacker {
  int x, y, row_height;
  int used_width, used_height;
} AtlasPacker;

static bool PackVisiblePart(
    Ppu *ppu, ActionObjInterpolationFrame *out, AtlasPacker *packer,
    const PpuObjPart *part, uint8_t oam_slot,
    bool synthetic,
    const ActionObjEmitterIdentity *identity,
    const uint8_t *priority_pixels[4], uint8_t priority_content_mask,
    int surface_width, int surface_height,
    int screen_x_origin, int screen_y_origin) {
  const int priority = (part->tile_attr >> 12) & 3;
  if (!(priority_content_mask & (1u << priority)) ||
      !priority_pixels[priority] ||
      !PartIntersectsSurface(part, surface_width, surface_height,
                             screen_x_origin, screen_y_origin))
    return true;

  const int size = part->size;
  if (size <= 0 || size > 64) return false;
  const PpuObjRangeBounds bounds = {
    part->x, part->y,
    (int16_t)(part->x + size), (int16_t)(part->y + size),
  };
  if (!PpuRasterizeParts(ppu, part, 1, &bounds, s_raster,
                         size, size, (size_t)size * sizeof(uint32_t)))
    return false;

  int visible_pixels = 0;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const size_t local = (size_t)y * size + x;
      s_masked[local] = 0;
      const uint32_t raster = s_raster[local];
      if (!raster) continue;
      const int screen_x = part->x + x;
      const PpuOverlayCapture *capture =
          &ppu->overlayCaptures[kPpuOverlaySource_Obj];
      if (capture->x1 > capture->x0) {
        const bool in_scanout =
            screen_x >= capture->x0 && screen_x < capture->x1;
        /* Real OAM fills the configured scanout rectangle; the synthetic
         * channel fills only its two outside apron bands. Keeping those
         * domains disjoint prevents a same-colour straddler from borrowing a
         * synthetic pixel (or vice versa) and assigning it the wrong object. */
        if (in_scanout == synthetic) continue;
      }
      const int surface_x = part->x + x + screen_x_origin;
      const int surface_y = part->y + y + screen_y_origin;
      if (surface_x < 0 || surface_x >= surface_width ||
          surface_y < 0 || surface_y >= surface_height)
        continue;
      const size_t surface = (size_t)surface_y * surface_width + surface_x;
      if (s_claimed[surface]) continue;
      const uint32_t captured =
          ((const uint32_t *)priority_pixels[priority])[surface];
      /* Capture may encode OBJ colour-math eligibility in alpha. RGB remains
       * the rasterizer's authoritative color, so compare it and retain the
       * captured word verbatim. */
      if (!captured ||
          (captured & 0x00ffffffu) != (raster & 0x00ffffffu))
        continue;
      s_claimed[surface] = 1;
      s_masked[local] = captured;
      visible_pixels++;
    }
  }
  if (!visible_pixels) return true;
  if (out->part_count >= kActionObjInterpolationMaxParts) return false;

  enum { kPadding = 1 };
  if (packer->x + size + kPadding > kActionObjInterpolationAtlasWidth) {
    packer->x = kPadding;
    packer->y += packer->row_height + kPadding;
    packer->row_height = 0;
  }
  if (packer->y + size + kPadding > kActionObjInterpolationAtlasHeight)
    return false;

  ActionObjInterpolationPart *dst = &out->parts[out->part_count++];
  *dst = (ActionObjInterpolationPart){
    .x = part->x,
    .y = part->y,
    .tile_attr = part->tile_attr,
    .atlas_x = (uint16_t)packer->x,
    .atlas_y = (uint16_t)packer->y,
    .size = part->size,
    .priority = (uint8_t)priority,
    .oam_slot = oam_slot,
  };
  if (identity && identity->valid) {
    dst->anchor_x = identity->anchor_x;
    dst->anchor_y = identity->anchor_y;
    dst->object_address = identity->object_address;
    dst->object_signature = identity->object_signature;
  }

  for (int y = 0; y < size; y++)
    memcpy(&g_action_obj_interpolation_atlas_pixels[
               (size_t)(packer->y + y) *
                   kActionObjInterpolationAtlasWidth + packer->x],
           &s_masked[(size_t)y * size],
           (size_t)size * sizeof(uint32_t));

  const int right = packer->x + size;
  const int bottom = packer->y + size;
  if (right > packer->used_width) packer->used_width = right;
  if (bottom > packer->used_height) packer->used_height = bottom;
  if (size > packer->row_height) packer->row_height = size;
  packer->x = right + kPadding;
  return true;
}

bool ActionObjInterpolation_BuildFrame(
    Ppu *ppu, ActionObjInterpolationFrame *out,
    const uint8_t *priority_pixels[4], uint8_t priority_content_mask,
    int surface_width, int surface_height,
    int screen_x_origin, int screen_y_origin,
    uint8_t excluded_oam_first, uint8_t excluded_oam_count,
    uint64_t timestamp_ns) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->timestamp_ns = timestamp_ns;
  ClearAtlas();
  if (!s_emitter_enabled || !ppu || !priority_pixels ||
      surface_width <= 0 || surface_width > kPpuSurfaceWidth ||
      surface_height <= 0 || surface_height > kPpuBufHeight)
    return false;

  for (int y = 0; y < surface_height; y++)
    memset(&s_claimed[(size_t)y * surface_width], 0,
           (size_t)surface_width);

  AtlasPacker packer = {.x = 1, .y = 1};
  uint8_t index = PPU_objPriority(ppu) ? (uint8_t)(ppu->oamaddl & 0xfe) : 0;
  for (int evaluated = 0; evaluated < 128;
       evaluated++, index = (uint8_t)(index + 2)) {
    const uint8_t slot = index >> 1;
    if (excluded_oam_count && slot >= excluded_oam_first &&
        slot < excluded_oam_first + excluded_oam_count)
      continue;
    PpuObjPart part;
    if (!PpuResolveObjSlot(ppu, slot, &part) ||
        !PackVisiblePart(
            ppu, out, &packer, &part, slot, false, &s_oam_identity[slot],
            priority_pixels, priority_content_mask,
            surface_width, surface_height,
            screen_x_origin, screen_y_origin)) {
      s_atlas_dirty_width = packer.used_width;
      s_atlas_dirty_height = packer.used_height;
      out->part_count = 0;
      return false;
    }
  }

  for (int i = 0; i < s_synthetic_count; i++) {
    if (!PackVisiblePart(
            ppu, out, &packer, &s_synthetic[i].part,
            kActionObjInterpolationSyntheticSlot, true,
            &s_synthetic[i].identity,
            priority_pixels, priority_content_mask,
            surface_width, surface_height,
            screen_x_origin, screen_y_origin)) {
      s_atlas_dirty_width = packer.used_width;
      s_atlas_dirty_height = packer.used_height;
      out->part_count = 0;
      return false;
    }
  }

  /* Completeness is the proof-of-concept's safety boundary. Endpoint PPU
   * state can differ from scanline-fetched state under HDMA or streaming; in
   * that case a rasterized part will fail to claim some captured pixels. Do
   * not guess: retain the existing whole-plane renderer for that frame. */
  for (int priority = 0; priority < 4; priority++) {
    if (!(priority_content_mask & (1u << priority)) ||
        !priority_pixels[priority])
      continue;
    const uint32_t *pixels = (const uint32_t *)priority_pixels[priority];
    for (int y = 0; y < surface_height; y++) {
      for (int x = 0; x < surface_width; x++) {
        const size_t at = (size_t)y * surface_width + x;
        if (pixels[at] && !s_claimed[at]) {
          s_atlas_dirty_width = packer.used_width;
          s_atlas_dirty_height = packer.used_height;
          out->part_count = 0;
          return false;
        }
      }
    }
  }

  s_atlas_dirty_width = packer.used_width;
  s_atlas_dirty_height = packer.used_height;
  out->atlas_used_width = (uint16_t)packer.used_width;
  out->atlas_used_height = (uint16_t)packer.used_height;
  out->valid = true;
  return true;
}

void ActionObjInterpolation_PartPosition(
    const ActionObjInterpolationFrame *previous,
    const ActionObjInterpolationPart *current,
    float pair_phase, int maximum_delta,
    float *out_x, float *out_y) {
  if (!out_x || !out_y) return;
  *out_x = current ? (float)current->x : 0.0f;
  *out_y = current ? (float)current->y : 0.0f;
  if (!previous || !previous->valid || !current ||
      !current->object_address || maximum_delta < 0)
    return;

  const ActionObjInterpolationPart *match = NULL;
  for (uint16_t i = 0; i < previous->part_count; i++) {
    const ActionObjInterpolationPart *candidate = &previous->parts[i];
    if (candidate->object_address == current->object_address &&
        candidate->object_signature == current->object_signature) {
      match = candidate;
      break;
    }
  }
  if (!match) return;

  const int dx = (int)current->anchor_x - (int)match->anchor_x;
  const int dy = (int)current->anchor_y - (int)match->anchor_y;
  if (dx < -maximum_delta || dx > maximum_delta ||
      dy < -maximum_delta || dy > maximum_delta)
    return;
  if (pair_phase < 0.0f) pair_phase = 0.0f;
  if (pair_phase > 1.0f) pair_phase = 1.0f;
  *out_x += (pair_phase - 1.0f) * (float)dx;
  *out_y += (pair_phase - 1.0f) * (float)dy;
}
