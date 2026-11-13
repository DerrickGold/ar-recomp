#include "diorama/diorama_trunk_gpu.h"
#include "diorama/diorama_trunk_guard_upload.h"
#include "diorama/diorama_voxel_guard_upload.h"
#include "action/action_trunk.h"
#include "action/action_voxel.h"
#include "action/action_cloud_light.h"
#include "action/action_heat.h"
#include "action/action_storm.h"
#include "diorama/diorama_gpu_policy.h"
#include "diorama/diorama_bg_decode.h"
#include "diorama/diorama_depth_gpu.h"
#include "diorama/diorama_voxel_source_cache.h"
#include "diorama/present_diorama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "action/action_effect_projection.h"
#include "action/action_exposure.h"
#include "action/action_effect_receivers.h"
#include "render/scenery_dimming.h"
#include "action/present_action_effects.h"
#include "app/session_fatal.h"
#include "app/performance_metrics.h"
#include "dev/diorama_snapshot_capture.h"
#include "diorama/diorama.h"
#include "diorama/diorama_astral_rift.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_upload.h"
#include "diorama/diorama_bg_gpu.h"
#include "host/host_clock.h"
#include "present/present.h"
#include "render/render_output.h"
#include "present/presentation_surface.h"
#include "present/presentation_upload_mirror.h"
#include "render/present_hud.h"
#include "sim/world_nav/present_world_nav.h"
#include "sim/world_nav/completion_vista_backend.h"

static ArRenderTexture s_plane_textures[kDioramaPlane_Count];
static ArRenderTexture s_resolved_textures[kDioramaPlane_Count];
static uint32_t *s_bg_fallback[kDioramaPlane_Count + 1];
static uint32_t *s_trunk_fallback[3];
static DioramaTrunkGuardUpload s_trunk_guard;
static DioramaVoxelGuardUpload s_voxel_guard;
static uint32_t *s_voxel_fallback[2];
static DioramaVoxelSourceCache s_voxel_source_cache;
/* Monotonic content identity for the retained split. Never derive this from
 * packet pointers/timestamps: native reloads can reuse both. */
static uint64_t s_voxel_upload_revision;
static ArRenderTexture s_voxel_sprite_atlas;
static PresentationUploadMirror s_voxel_sprite_mirror;
static ActionVoxelSpriteFrame s_voxel_sprites_previous, s_voxel_sprites_current;
static uint64_t s_voxel_sprite_timestamp, s_voxel_sprite_revision;
static bool s_voxel_sprite_pair;
static uint32_t *s_voxel_obj_pixels[4], *s_voxel_obj_attributes[4];
static void ResetVoxelSprites(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_voxel_sprite_atlas);
  s_voxel_sprite_atlas = ArRenderTexture_Invalid();
  PresentationUploadMirror_Reset(&s_voxel_sprite_mirror);
  s_voxel_sprite_timestamp = s_voxel_sprite_revision = 0;
  s_voxel_sprites_current.count = s_voxel_sprites_previous.count = 0;
  s_voxel_sprite_pair = false;
  for(unsigned i=0;i<4;++i) {
    free(s_voxel_obj_pixels[i]);s_voxel_obj_pixels[i]=NULL;
    free(s_voxel_obj_attributes[i]);s_voxel_obj_attributes[i]=NULL;
  }
}

/* Keep the captured/native fallback untouched. Successful modeled props own
 * their exact original winners; unrelated overlapping sprites keep their
 * native priority. Normalize receiver attributes before upload/interpolation,
 * including when capture or atlas upload failed and no prop was handed off. */
static bool MaterializeVoxelActors(const FrameSlot *slot,
    const uint8_t *pixels[kDioramaPlane_Count],size_t pitches[kDioramaPlane_Count]) {
  if(!slot->diorama_obj_owner_tags)return true;
  const int width=slot->snes_width+2*slot->obj_apron;
  const int height=slot->snes_height+slot->ws_extra_top+slot->ws_extra_bottom;
  if(width<=0 || width>(int)SR_PPU_SURFACE_MAX_WIDTH ||
      height<=0 || height>(int)SR_PPU_SURFACE_MAX_HEIGHT)
    return false;
  for(unsigned band=0;band<4;++band) {
    const unsigned plane=DioramaPlaneForObjectPriority(band),attr=kDioramaPlane_ObjAttributes0+band;
    if(!pixels[attr])continue;
    if(pitches[attr]<(size_t)width*4 || (pixels[plane] && pitches[plane]<(size_t)width*4))return false;
    const uint32_t *source_attributes=(const uint32_t *)pixels[attr];
    const size_t attribute_pitch=pitches[attr]/4;
    if(!s_voxel_obj_attributes[band])s_voxel_obj_attributes[band]=calloc(
        SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT,4);
    if(!s_voxel_obj_attributes[band])return false;
    bool copied=false;
    for(unsigned i=0;pixels[plane] && i<s_voxel_sprites_current.count;++i) {
      const ActionVoxelSprite *prop=&s_voxel_sprites_current.sprites[i];
      if(prop->priority!=band || !ActionVoxelSprite_IsModeled(prop) ||
          ActionVoxelSprite_KeepsNativeFace(prop))continue;
      if(!copied) {
        if(!s_voxel_obj_pixels[band])s_voxel_obj_pixels[band]=calloc(
            SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT,4);
        if(!s_voxel_obj_pixels[band])return false;
        for(int y=0;y<height;++y)memcpy(s_voxel_obj_pixels[band]+y*SR_PPU_SURFACE_MAX_WIDTH,
            pixels[plane]+y*pitches[plane],(size_t)width*4);
        pixels[plane]=(const uint8_t *)s_voxel_obj_pixels[band];
        pitches[plane]=SR_PPU_SURFACE_MAX_WIDTH*4;copied=true;
      }
      ActionVoxelSprite_RemoveNative(prop,width,height,s_voxel_obj_pixels[band],
          SR_PPU_SURFACE_MAX_WIDTH,source_attributes,attribute_pitch);
    }
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      const uint32_t a=source_attributes[(size_t)y*attribute_pitch+x];
      const bool removed=copied && !s_voxel_obj_pixels[band][y*SR_PPU_SURFACE_MAX_WIDTH+x];
      s_voxel_obj_attributes[band][y*SR_PPU_SURFACE_MAX_WIDTH+x]=
          (a>>24) && !removed ? a|0xff000000u : 0;
    }
    pixels[attr]=(const uint8_t *)s_voxel_obj_attributes[band];
    pitches[attr]=SR_PPU_SURFACE_MAX_WIDTH*4;
  }
  return true;
}

/* CPU backends and diagnostic/CPU-motion snapshots use the same split as the
 * GPU path. Never read back a texture merely to obtain this representation. */
static bool MaterializeTrunk(const FrameSlot *slot,
    const uint8_t *pixels[kDioramaPlane_Count],size_t pitches[kDioramaPlane_Count]) {
  const unsigned planes[3]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  const uint32_t *input[3];size_t input_pitch[3];
  for(unsigned i=0;i<3;++i) {
    if(!s_trunk_fallback[i])s_trunk_fallback[i]=calloc(SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT,4);
    if(!s_trunk_fallback[i])return false;
    input[i]=(const uint32_t *)pixels[planes[i]];input_pitch[i]=pitches[planes[i]]/4;
  }
  ActionTrunk_Split(slot->diorama_trunk,slot->bg1_camera_x-slot->ws_extra-slot->obj_apron,
      slot->bg1_camera_y-slot->ws_extra_top+1,slot->snes_width+slot->obj_apron*2,
      slot->snes_height+slot->ws_extra_top+slot->ws_extra_bottom,SR_PPU_SURFACE_MAX_WIDTH,
      input,input_pitch,s_trunk_fallback,(slot->inidisp&0x80)?0:slot->inidisp&15);
  for(unsigned i=0;i<3;++i) {
    pixels[planes[i]]=(const uint8_t *)s_trunk_fallback[i];pitches[planes[i]]=SR_PPU_SURFACE_MAX_WIDTH*4;
  }
  return true;
}
static bool MaterializeVoxel(const FrameSlot *slot,
    const uint8_t *pixels[kDioramaPlane_Count], size_t pitches[kDioramaPlane_Count],
    bool reuse, const DioramaVoxelSourceRowPlan *rows) {
  for (unsigned i = 0; i < 2; ++i) {
    if (!s_voxel_fallback[i]) s_voxel_fallback[i] = calloc(
        SR_PPU_SURFACE_MAX_WIDTH * SR_PPU_SURFACE_MAX_HEIGHT, sizeof(uint32_t));
    if (!s_voxel_fallback[i]) return false;
  }
  const ActionRoomSceneFrameState colors = {
    .brightness = (slot->inidisp & 0x80) ? 0 : slot->inidisp & 15,
    .cgwsel = slot->diorama_voxel_cgwsel, .cgadsub = slot->diorama_voxel_cgadsub,
    .fixed_color = slot->diorama_voxel_fixed_color};
  const int height = slot->snes_height+slot->ws_extra_top+slot->ws_extra_bottom;
  const uint32_t *low = (const uint32_t *)pixels[SR_PPU_OVERLAY_BG1];
  const size_t low_pitch = pitches[SR_PPU_OVERLAY_BG1]/4;
  if (!reuse && height <= 0) return false;
  if (!reuse) for (int y = 0; y < height;) {
    if (rows && !rows->changed[y]) { ++y; continue; }
    const int first = y++;
    while (y < height && (!rows || rows->changed[y])) ++y;
    if (!ActionVoxel_Split(slot->diorama_voxel,
        slot->bg1_camera_x-slot->ws_extra-slot->obj_apron,
        slot->bg1_camera_y-slot->ws_extra_top+1+first,
        slot->snes_width+slot->obj_apron*2, y-first,
        SR_PPU_SURFACE_MAX_WIDTH, &colors,
        low ? low+(size_t)first*low_pitch : NULL, low_pitch,
        s_voxel_fallback[0]+(size_t)first*SR_PPU_SURFACE_MAX_WIDTH,
        s_voxel_fallback[1]+(size_t)first*SR_PPU_SURFACE_MAX_WIDTH)) return false;
  }
  pixels[SR_PPU_OVERLAY_BG1] = (const uint8_t *)s_voxel_fallback[0];
  pixels[kDioramaPlane_Bg1Far] = (const uint8_t *)s_voxel_fallback[1];
  pitches[SR_PPU_OVERLAY_BG1] = pitches[kDioramaPlane_Bg1Far] = SR_PPU_SURFACE_MAX_WIDTH * 4;
  return true;
}
static uint32_t s_diorama_uploaded_plane_mask;
static DioramaCoverageMask s_diorama_coverage_masks[kDioramaPlane_Count];
static uint64_t s_diorama_bg2_content_revision;
static DioramaSkyboxView s_diorama_skybox_view;
static ArRenderTexture s_diorama_skybox_texture;
static bool s_diorama_skybox_texture_periodic;
static PresentationUploadMirror s_diorama_skybox_mirror;
static ArRenderTexture s_completion_art;
static int s_completion_art_width, s_completion_art_height;

/* Session history survives retained frames, room changes and GPU resets. */
static DioramaCameraPresenter s_diorama_camera = DIORAMA_CAMERA_PRESENTER_INIT;

void PresentDiorama_DestroyPlanes(ArRenderDevice *device) {
  DioramaTrunkGuardUpload_Reset(&s_trunk_guard, device);
  DioramaVoxelGuardUpload_Reset(&s_voxel_guard, device);
  DioramaVoxelSourceCache_Reset(&s_voxel_source_cache);
  DioramaDepthGpu_Reset(device);
  ResetVoxelSprites(device);
  DioramaAstralRift_Reset(device);
  ArRenderDevice_DestroyTexture(device, s_completion_art);
  s_completion_art = ArRenderTexture_Invalid();
  s_completion_art_width = s_completion_art_height = 0;
  DioramaFrameGeneration_FinishCapture();
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(device, s_plane_textures[i]);
    s_plane_textures[i] = ArRenderTexture_Invalid();
  }
}

void PresentDiorama_CreatePlanes(ArRenderDevice *device) {
  /* The ABI's full render-target size covers every horizontal and vertical
   * margin without a realloc. Only the leading snes_width x
   * (snes_height + ws_extra_top + ws_extra_bottom) region is uploaded
   * each frame; Diorama_Composite's UV window is expressed against these
   * allocated dimensions. Zero the padding because the tilted view can
   * sample outside the captured rectangle. */
  uint8_t *zero_fill =
      calloc(1, (size_t)SR_PPU_SURFACE_MAX_WIDTH *
                    SR_PPU_SURFACE_MAX_HEIGHT * 4);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    if (i == SR_PPU_OVERLAY_BG4)
      continue;
    const ArRenderTextureDesc desc = {
      .width = SR_PPU_SURFACE_MAX_WIDTH,
      .height = SR_PPU_SURFACE_MAX_HEIGHT,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = i == kDioramaPlane_Backdrop
          ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            device, &desc, &s_plane_textures[i]))
      continue;
    if (zero_fill)
      ArRenderDevice_UpdateTexture(
          device, s_plane_textures[i], NULL, zero_fill,
          SR_PPU_SURFACE_MAX_WIDTH * 4);
  }
  free(zero_fill);
}

static const SrPpuSurfaceView *DioramaPpuSurface(
    const FrameSlot *slot, int plane) {
  if (!slot) return NULL;
  if (plane >= SR_PPU_OVERLAY_BG1 && plane <= SR_PPU_OVERLAY_OBJ)
    return &slot->ppu_surfaces.overlays[plane][0];
  if (DioramaPlaneIsObjectAttributes(plane))
    return &slot->diorama_obj_attributes[plane-kDioramaPlane_ObjAttributes0];
  if (plane == kDioramaPlane_Backdrop)
    return &slot->ppu_surfaces.main;
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; i++)
    if (bands[i].plane == plane)
      return &slot->ppu_surfaces.overlays[bands[i].source][bands[i].band];
  return NULL;
}

static void CaptureDioramaPpuSurfaces(
    const FrameSlot *slot,
    const uint8_t *pixels[kDioramaPlane_Count],
    size_t pitch_bytes[kDioramaPlane_Count]) {
  const int width = slot->snes_width + slot->obj_apron * 2;
  const int height = slot->snes_height +
      slot->ws_extra_top + slot->ws_extra_bottom;
  memset(pixels, 0, sizeof(*pixels) * kDioramaPlane_Count);
  if (pitch_bytes)
    memset(pitch_bytes, 0, sizeof(*pitch_bytes) * kDioramaPlane_Count);
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    const SrPpuSurfaceView *surface = DioramaPpuSurface(slot, plane);
    if (DioramaPlanes_GpuOwnedMask(slot->background_packet) & (1u << plane)) {
      if (pitch_bytes) pitch_bytes[plane] = slot->background_packet->words[0] * sizeof(uint32_t);
      continue;
    }
    if (!PresentationSurface_Holds(surface, width, height)) continue;
    pixels[plane] = surface->data;
    if (pitch_bytes)
      pitch_bytes[plane] = (size_t)surface->pitch_bytes;
  }
}

/* Renderer recovery, CPU diagnostics and the Voxel front/rear split consume
 * owned packets here. Unchanged Voxel sources retain the completed split. */
static void MaterializeBackgrounds(const SrPpuBgPacket *packet, uint32_t mask,
    const uint8_t **pixels, size_t *pitches, const DioramaVoxelSourceRowPlan *voxel_rows) {
  if (!packet) return;
  const unsigned planes[2][3] = {{SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far},
      {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far}};
  for (unsigned bg = 0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band) {
    const unsigned plane = planes[bg][band];
    if (!(mask & (1u << plane))) continue;
    if (!s_bg_fallback[plane]) s_bg_fallback[plane] = malloc(
        SR_PPU_BG_PACKET_WIDTH * SR_PPU_BG_PACKET_HEIGHT * sizeof(uint32_t));
    if (!s_bg_fallback[plane]) continue;
    for (unsigned y = 0; y < packet->words[1]; ++y) {
      if (plane == SR_PPU_OVERLAY_BG1 && voxel_rows && !voxel_rows->changed[y]) continue;
      DioramaBgDecode_Row(packet, bg, band, y, packet->words[0],
          s_bg_fallback[plane] + y * packet->words[0]);
    }
    pixels[plane] = (const uint8_t *)s_bg_fallback[plane];
    pitches[plane] = packet->words[0] * sizeof(uint32_t);
  }
}

static void UploadVoxelGuard(ArRenderDevice *device, const FrameSlot *slot,
    const uint8_t *const pixels[kDioramaPlane_Count], const size_t pitches[kDioramaPlane_Count]) {
  const DioramaVoxelGuardSource *source = &slot->diorama_voxel_guard;
  if (!slot->diorama_voxel_mode || slot->diorama_trunk ||
      !ActionVoxel_Active(slot->diorama_voxel, slot->diorama_map_group, slot->diorama_map_number) ||
      !DioramaVoxelGuard_Valid(source)) {
    (void)DioramaVoxelGuardUpload_Update(&s_voxel_guard, device, NULL, NULL, NULL, NULL);
    return;
  }
  const int height = slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom;
  const int first = height - kDioramaVoxelGuardPrefixRows;
  const int width = slot->snes_width + slot->obj_apron * 2;
  const int world_x = slot->bg1_camera_x - slot->ws_extra - slot->obj_apron;
  const int world_y = slot->bg1_camera_y - slot->ws_extra_top + 1;
  if (first < 0 || width != source->layout.lower.width || height != source->layout.primary_height ||
      world_x != source->layout.lower.world_x || world_y != source->layout.primary_y) {
    (void)DioramaVoxelGuardUpload_Update(&s_voxel_guard, device, NULL, NULL, NULL, NULL);
    return;
  }
  DioramaVoxelGuardPrefix prefix = {.width = width, .capture_generation = source->capture_generation};
  const unsigned planes[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far};
  static uint32_t high_prefix[kDioramaVoxelGuardWidth * kDioramaVoxelGuardPrefixRows];
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned plane = planes[i];
    if (pixels[plane]) {
      prefix.pixels[i] = (const uint32_t *)(pixels[plane] + (size_t)first * pitches[plane]);
      prefix.pitch_pixels[i] = pitches[plane] / sizeof(uint32_t);
    } else if (i == 1 && (DioramaPlanes_GpuOwnedMask(slot->background_packet) & (1u << plane))) {
      /* Only the overlap needs CPU High pixels. Decode these 16 immutable
       * packet rows, not an extra full capture or a GPU readback. */
      const SrPpuBgPacket *packet = slot->background_packet;
      if (!packet || packet->words[0] < (unsigned)width || packet->words[1] < (unsigned)height) {
        (void)DioramaVoxelGuardUpload_Update(&s_voxel_guard, device, NULL, NULL, NULL, NULL);
        return;
      }
      for (unsigned y = 0; y < kDioramaVoxelGuardPrefixRows; ++y)
        DioramaBgDecode_Row(packet, 0, 1, (unsigned)first + y, (unsigned)width,
            high_prefix + y * kDioramaVoxelGuardWidth);
      prefix.pixels[i] = high_prefix;
      prefix.pitch_pixels[i] = kDioramaVoxelGuardWidth;
    }
  }
  const ActionRoomSceneFrameState frame = {
    .brightness = (slot->inidisp & 0x80) ? 0 : slot->inidisp & 15,
    .cgwsel = slot->diorama_voxel_cgwsel, .cgadsub = slot->diorama_voxel_cgadsub,
    .fixed_color = slot->diorama_voxel_fixed_color};
  const uint64_t previous = s_voxel_guard.view.capture_generation;
  if (DioramaVoxelGuardUpload_Update(&s_voxel_guard, device, slot->diorama_voxel,
      source, &frame, &prefix) && !previous) {
    fprintf(stderr, "[voxel-guard] ready map=%02x/%02x source=%d,%d size=%dx%d finite=%d,%d,%d,%d generation=%llu\n",
        slot->diorama_map_group, slot->diorama_map_number,
        source->layout.lower.world_x, source->layout.lower.world_y,
        source->layout.lower.width, kDioramaVoxelGuardRows,
        source->layout.lower.valid.x, source->layout.lower.valid.y,
        source->layout.lower.valid.width, source->layout.lower.valid.height,
        (unsigned long long)source->capture_generation);
  }
}

/* Coarse mesh coverage reads bitplanes in eight-pixel groups. It never expands
 * colors or retains a CPU image, and keeps sparse far/high layers cheap. */
static DioramaCoverageMask BackgroundPacketCoverage(const SrPpuBgPacket *packet,
    unsigned source, unsigned band, unsigned apron) {
  const unsigned width = packet->words[0] - apron * 2, height = packet->words[1];
  DioramaCoverageMask occupied = 0;
  for (unsigned gy = 0; gy < kDioramaCoverageRows; ++gy) {
    const unsigned y0 = (gy * height + kDioramaCoverageRows - 1) / kDioramaCoverageRows;
    const unsigned y1 = ((gy + 1) * height + kDioramaCoverageRows - 1) / kDioramaCoverageRows;
    for (unsigned gx = 0; gx < kDioramaCoverageColumns; ++gx) {
      const unsigned x0 =
          apron + (gx * width + kDioramaCoverageColumns - 1) / kDioramaCoverageColumns;
      const unsigned x1 =
          apron + ((gx + 1) * width + kDioramaCoverageColumns - 1) / kDioramaCoverageColumns;
      bool found = false;
      for (unsigned y = y0; y < y1 && !found; ++y) {
        const unsigned row = SrPpuBgPacket_Row(source, y);
        const uint32_t *meta =
            packet->words + SR_PPU_BG_PACKET_HEADER_WORDS + row * SR_PPU_BG_PACKET_ROW_WORDS;
        if (meta[0] >= 3 && meta[7]) {
          /* Conservative tile coverage is sufficient for mesh culling; avoid
           * expanding the GPU-owned image just to find occupied grid cells. */
          if (band == 0 && (meta[1] >> 24) && x0 < meta[3] && x1 > meta[2]) found = true;
          for (unsigned x = x0; x < x1 && !found;) {
            const unsigned column = x + meta[4];
            const uint32_t *tile = packet->words + meta[7] + (column / 8) * 2;
            found = tile[0] && (tile[1] & 0xff00u) && ((tile[1] >> 3) & 3u) == band;
            x += 8u - (column & 7u);
          }
          if (meta[0] == 4 && meta[5])
            for (unsigned x = x0; x < x1 && !found; x = (x | 7u) + 1u) {
              const uint32_t *edit = packet->words + meta[5] + (x / 8u) * 9u + band * 3u;
              found = edit[0] || (edit[2] & 0xffffu);
            }
          continue;
        }
        if (meta[0] != 2 || !meta[7]) {
          for (unsigned x = x0; x < x1 && !found; ++x)
            found = (SrPpuBgPacket_Color(packet, source, band, x, y) >> 24) != 0;
          continue;
        }
        const uint32_t *pixels = packet->words + meta[7];
        for (unsigned x = x0; x < x1 && !found;) {
          unsigned end = (x | 7u) + 1;
          if (end > x1) end = x1;
          const unsigned mask = ((1u << (end - x)) - 1u) << (x & 7u);
          const uint32_t *cell = pixels + (x / 8) * 9 + band * 3;
          unsigned covered = cell[0] | (cell[0] >> 8) | (cell[0] >> 16) | (cell[0] >> 24) | cell[2];
          if (meta[1] >> 24) {
            covered |= cell[2] >> 8;
            if (band == 0) for (unsigned at = x; at < end; ++at)
              if (at >= meta[2] && at < meta[3] && !(cell[2] & (1u << ((at & 7u) + 16))))
                covered |= 1u << (at & 7u);
          }
          found = (covered & mask) != 0;
          x = end;
        }
      }
      if (found) occupied |= UINT64_C(1) << (gy * kDioramaCoverageColumns + gx);
    }
  }
  return occupied ? DioramaCoverage_Dilate(occupied) : DioramaCoverage_FullMask();
}

/* AR_PLANESTAT=1: report the alpha-bearing fraction and content bounding box
 * of each plane that actually synchronized for presentation. This runs while
 * the borrowed producer surfaces are still owned by PresentUpload; retained
 * re-presents must not rescan pointers that a later game tick can rewrite. */
static void PlaneStatCensus(
    const uint8_t *pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    int width, int height, uint32_t plane_mask) {
  static int enabled = -1;
  static unsigned long frames;
  static double covered_sum[kDioramaPlane_Count];
  static double bbox_sum[kDioramaPlane_Count];
  static unsigned long present_count[kDioramaPlane_Count];
  if (enabled < 0) {
    const char *value = getenv("AR_PLANESTAT");
    enabled = value && value[0] && value[0] != '0';
  }
  if (!enabled || width <= 0 || height <= 0) return;
  frames++;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (!(plane_mask & (1u << plane)) ||
        !pixels[plane] || !pitch_bytes[plane])
      continue;
    long covered = 0;
    int x0 = width, x1 = -1, y0 = height, y1 = -1;
    for (int y = 0; y < height; y++) {
      const uint32_t *row = (const uint32_t *)(
          pixels[plane] + (size_t)y * pitch_bytes[plane]);
      for (int x = 0; x < width; x++) {
        if ((row[x] >> 24) == 0u) continue;
        covered++;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
    present_count[plane]++;
    const double area = (double)width * (double)height;
    covered_sum[plane] += (double)covered / area;
    bbox_sum[plane] += x1 < 0 ? 0.0
        : (double)(x1 - x0 + 1) * (double)(y1 - y0 + 1) / area;
  }
  if (frames % 300u != 0u) return;
  fprintf(stderr, "[planestat] after %lu frames (%dx%d)\n",
          frames, width, height);
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (!present_count[plane]) continue;
    fprintf(stderr,
            "  plane %2d: present %5.1f%% covered %5.1f%% bbox %5.1f%%\n",
            plane, 100.0 * (double)present_count[plane] / (double)frames,
            100.0 * covered_sum[plane] / (double)present_count[plane],
            100.0 * bbox_sum[plane] / (double)present_count[plane]);
  }
}

/* Every scene recipe uses source-space projection. Hardware/resource failures
 * are handled by full-frame recovery, never by a room/effect whitelist. */
static bool SourceProjectionQualified(const FrameSlot *slot) {
  return DioramaGpuPolicy_ForRoom(slot->diorama_map_group, slot->diorama_map_number).resident;
}

void PresentDiorama_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  const bool collect_coverage = DioramaUpload_UsesCoverage(slot->interp_setting_enabled);
  /* GPU-owned and CPU-uploaded masks share this consumer policy. Forget GPU
   * masks while unused, including failed device preparation, so an unchanged
   * endpoint recomputes them when interpolation is turned off again. */
  if (!collect_coverage)
    memset(s_diorama_coverage_masks, 0, sizeof(s_diorama_coverage_masks));
  PresentActionEffects_InvalidateSourcePackets();
  DioramaFrameGeneration_FinishCapture();
  DioramaFrameGeneration_AllowSourceProjection(SourceProjectionQualified(slot));
  s_diorama_skybox_view.texture = ArRenderTexture_Invalid();
  if (!ArRenderDevice_IsReady(device)) return;
  /* Prepare during the Voxel mode's menu/SIM packets as well as action upload.
   * Off and ordinary Diorama do not allocate the extra visibility targets.
   * Begin checks preparation before drawing if this optional warm-up failed. */
  if (slot->diorama_voxel_mode) (void)DioramaDepthGpu_Prepare(device);
  const uint64_t revision = ActionVoxel_Revision(slot->diorama_voxel);
  s_voxel_sprite_pair = revision && revision == s_voxel_sprite_revision &&
      slot->timestamp_ns > s_voxel_sprite_timestamp &&
      slot->timestamp_ns-s_voxel_sprite_timestamp < UINT64_C(100000000) &&
      slot->capture_ticks > 0 && slot->capture_ticks <= 4;
  s_voxel_sprites_previous = s_voxel_sprites_current;
  s_voxel_sprites_current = slot->voxel_sprites;
  s_voxel_sprite_timestamp = slot->timestamp_ns;
  s_voxel_sprite_revision = revision;
  if (s_voxel_sprites_current.count) {
    const ArRenderTextureDesc desc = {.width = kVoxelSpriteAtlasWidth,
        .height = kVoxelSpriteAtlasHeight, .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming, .filter = kArRenderFilter_Nearest,
        .blend = kArRenderBlendMode_Alpha};
    PresentationUploadResult uploaded;
    if ((!ArRenderTexture_IsValid(s_voxel_sprite_atlas) &&
         !ArRenderDevice_CreateTexture(device, &desc, &s_voxel_sprite_atlas)) ||
        !slot->voxel_sprite_atlas.data ||
        !PresentationUploadMirror_UploadArgb8888(&s_voxel_sprite_mirror, device,
            s_voxel_sprite_atlas, slot->voxel_sprite_atlas.data,
            slot->voxel_sprite_atlas.width_pixels, slot->voxel_sprite_atlas.height_pixels,
            slot->voxel_sprite_atlas.pitch_bytes, 0, 0, &uploaded))
      s_voxel_sprites_current.count = 0;
  }
  if (!slot->diorama_active) {
    s_voxel_source_cache.valid = false;
    s_diorama_uploaded_plane_mask = 0;
    memset(s_diorama_coverage_masks, 0, sizeof(s_diorama_coverage_masks));
    return;
  }
  bool skybox_changed = false;
  const uint32_t castle_planes = slot->diorama_voxel
      ? (1u << SR_PPU_OVERLAY_BG1) | (1u << kDioramaPlane_Bg1Far) : 0;
  const uint32_t upload_mask = slot->diorama_plane_request_mask &
                         slot->diorama_plane_content_mask;
  memcpy(s_resolved_textures, s_plane_textures, sizeof(s_resolved_textures));
  uint32_t gpu_changed = 0;
  const DioramaPerformanceScope gpu_capture_scope =
      DioramaPerformance_Begin(kDioramaPerformance_Upload);
  ArRenderTexture gpu_skybox = ArRenderTexture_Invalid();
  uint32_t gpu_mask = DioramaBgGpu_Resolve(device,
      slot->background_packet, upload_mask & ~castle_planes, s_resolved_textures, &gpu_skybox, &gpu_changed);
  if (ArRenderTexture_IsValid(gpu_skybox)) {
    skybox_changed = (gpu_changed & (1u << kDioramaPlane_Count)) != 0;
    if (skybox_changed) ++s_diorama_skybox_view.revision;
    s_diorama_skybox_view.texture = gpu_skybox;
    s_diorama_skybox_view.width = (int)slot->background_packet->words[4];
    s_diorama_skybox_view.periodic = slot->diorama_skybox_periodic;
  }
  DioramaPerformance_End(gpu_capture_scope);
  for (unsigned plane = 0; plane < kDioramaPlane_Count; ++plane)
    if (!(gpu_mask & (1u << plane))) s_resolved_textures[plane] = s_plane_textures[plane];
  SrPpuSurfaceView skybox_storage = slot->diorama_skybox_surface;
  const bool owned_skybox =
      slot->background_packet && (slot->background_packet->owned_sources & 4u);
  const bool skybox_diagnostic = getenv("AR_DIORAMA_SNAPSHOT") || getenv("AR_PLANESTAT");
  if (owned_skybox && (!ArRenderTexture_IsValid(gpu_skybox) || skybox_diagnostic)) {
    const SrPpuBgPacket *packet = slot->background_packet;
    if (!s_bg_fallback[kDioramaPlane_Count]) s_bg_fallback[kDioramaPlane_Count] = malloc(
        SR_PPU_BG_PACKET_WIDTH * SR_PPU_BG_PACKET_HEIGHT * sizeof(uint32_t));
    if (s_bg_fallback[kDioramaPlane_Count]) {
      for (unsigned y = 0; y < packet->words[5]; ++y)
        for (unsigned x = 0; x < packet->words[4]; ++x)
          s_bg_fallback[kDioramaPlane_Count][y * packet->words[4] + x] =
              SrPpuBgPacket_Color(packet, 2, 0, x, y);
      skybox_storage.data = (uint8_t *)s_bg_fallback[kDioramaPlane_Count];
    }
  }
  if (ArRenderTexture_IsValid(gpu_skybox) && !skybox_diagnostic) skybox_storage.data = NULL;
  const SrPpuSurfaceView *skybox = PresentationSurface_Bound(&skybox_storage);
  const uint8_t *skybox_pixels = PresentationSurface_Region(skybox, 0, 0,
      skybox ? (int)skybox->width_pixels : 0,
      skybox ? (int)skybox->height_pixels : 0);
  if (skybox_pixels && !ArRenderTexture_IsValid(gpu_skybox)) {
    const bool periodic = slot->diorama_skybox_periodic;
    if (periodic != s_diorama_skybox_texture_periodic) {
      ArRenderDevice_DestroyTexture(device, s_diorama_skybox_texture);
      s_diorama_skybox_texture = ArRenderTexture_Invalid();
      PresentationUploadMirror_Reset(&s_diorama_skybox_mirror);
      s_diorama_skybox_texture_periodic = periodic;
    }
    if (!ArRenderTexture_IsValid(s_diorama_skybox_texture)) {
      const ArRenderTextureDesc desc = {
        .width = periodic ? 256 : kFrameSlotLayerTextureWidth,
        .height = periodic ? 256 : kFrameSlotLayerTextureHeight,
        .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Opaque,
      };
      (void)ArRenderDevice_CreateTexture(
          device, &desc, &s_diorama_skybox_texture);
    }
    /* The blur prefilter visits the fixed allocation, not just the view.
     * Initialize its padding, including after a capture extent shrinks. */
    if (!periodic && ArRenderTexture_IsValid(s_diorama_skybox_texture) &&
        (!s_diorama_skybox_mirror.valid ||
         s_diorama_skybox_mirror.width != (int)skybox->width_pixels ||
         s_diorama_skybox_mirror.height != (int)skybox->height_pixels)) {
      uint32_t *empty = calloc(kFrameSlotLayerTextureWidth *
          kFrameSlotLayerTextureHeight, sizeof(uint32_t));
      const bool cleared = empty && ArRenderDevice_UpdateTexture(
          device, s_diorama_skybox_texture, NULL, empty,
          kFrameSlotLayerTextureWidth * sizeof(uint32_t));
      free(empty);
      if (!cleared) {
        ArRenderDevice_DestroyTexture(device, s_diorama_skybox_texture);
        s_diorama_skybox_texture = ArRenderTexture_Invalid();
      }
      PresentationUploadMirror_Reset(&s_diorama_skybox_mirror);
    }
    PresentationUploadResult result;
    if (ArRenderTexture_IsValid(s_diorama_skybox_texture) &&
        PresentationUploadMirror_UploadArgb8888(
            &s_diorama_skybox_mirror, device,
            s_diorama_skybox_texture, skybox_pixels,
            (int)skybox->width_pixels, (int)skybox->height_pixels,
            (int)skybox->pitch_bytes, 0, 0, &result)) {
      skybox_changed = result.changed;
      if (result.changed) s_diorama_skybox_view.revision++;
      s_diorama_skybox_view.texture = s_diorama_skybox_texture;
      s_diorama_skybox_view.width = (int)skybox->width_pixels;
      s_diorama_skybox_view.periodic = periodic;
    }
  }
  const uint8_t *pixels[kDioramaPlane_Count];
  size_t pitch_bytes[kDioramaPlane_Count];
  CaptureDioramaPpuSurfaces(slot, pixels, pitch_bytes);
  if(!MaterializeVoxelActors(slot,pixels,pitch_bytes)) {
    SessionFatal_Request("Cannot prepare owned Voxel sprite surfaces");return;
  }
  const uint32_t owned_mask = DioramaPlanes_GpuOwnedMask(slot->background_packet);
  const bool needs_cpu_analysis = slot->interp_setting_enabled &&
      !DioramaFrameGeneration_UsesGpuAnalysis(device, slot);
  const bool needs_cpu_diagnostic = getenv("AR_PLANESTAT") || getenv("AR_DIORAMA_SNAPSHOT");
  const bool castle=slot->diorama_voxel != NULL;
  const bool trunk=slot->diorama_trunk&&slot->diorama_trunk_settings.enabled;
  const bool gpu_trunk=trunk&&DioramaTrunkGpu_Prepare(device);
  const DioramaVoxelSourceKey voxel_source_key = {
    .geometry = slot->diorama_voxel, .revision = revision,
    .world_x = slot->bg1_camera_x-slot->ws_extra-slot->obj_apron,
    .world_y = slot->bg1_camera_y-slot->ws_extra_top+1,
    .width = slot->snes_width+slot->obj_apron*2,
    .height = slot->snes_height+slot->ws_extra_top+slot->ws_extra_bottom,
    .output_pitch = SR_PPU_SURFACE_MAX_WIDTH,
    .brightness = (slot->inidisp&0x80) ? 0 : slot->inidisp&15,
    .cgwsel = slot->diorama_voxel_cgwsel, .cgadsub = slot->diorama_voxel_cgadsub,
    .fixed_color = slot->diorama_voxel_fixed_color};
  const bool cache_voxel_source = slot->diorama_voxel_mode && castle && !trunk &&
      (owned_mask & upload_mask & (1u << SR_PPU_OVERLAY_BG1));
  const bool retained_voxel_source = cache_voxel_source &&
      s_voxel_fallback[0] && s_voxel_fallback[1];
  DioramaVoxelSourceRowPlan voxel_rows;
  const bool plan_voxel_rows = retained_voxel_source &&
      DioramaVoxelSourceCache_PlanRows(&s_voxel_source_cache,
          slot->background_packet, &voxel_source_key, &voxel_rows);
  /* The planner covers both ordinary and full-add packets. A rejected plan
   * cannot reuse the source; don't repeat its validation and full comparison. */
  const bool reuse_voxel_source = plan_voxel_rows && !voxel_rows.count;
  const DioramaVoxelSourceRowPlan *changed_voxel_rows =
      plan_voxel_rows && voxel_rows.count ? &voxel_rows : NULL;
  static int trace_voxel_source = -1;
  if (trace_voxel_source < 0)
    trace_voxel_source = getenv("AR_VOXEL_SOURCE_CACHE_TRACE") != NULL;
  if (trace_voxel_source && cache_voxel_source)
    fprintf(stderr, "[voxel-source-cache] source-ns=%llu reuse=%u revision=%llu world=%d,%d size=%d,%d changed-rows=%u row-reuse=%u\n",
        (unsigned long long)slot->timestamp_ns, reuse_voxel_source,
        (unsigned long long)revision, voxel_source_key.world_x, voxel_source_key.world_y,
        voxel_source_key.width, voxel_source_key.height,
        reuse_voxel_source ? 0u : plan_voxel_rows ? voxel_rows.count : (unsigned)voxel_source_key.height,
        plan_voxel_rows);
  /* Every uncached split writes these same surfaces. Invalidate before any
   * recovery path can replace them, including hidden/missing BG1 sources. */
  if (!reuse_voxel_source) s_voxel_source_cache.valid = false;
  MaterializeBackgrounds(slot->background_packet, owned_mask & upload_mask &
      ~(castle ? 1u << kDioramaPlane_Bg1Far : 0) &
      ~(reuse_voxel_source ? 1u << SR_PPU_OVERLAY_BG1 : 0) &
      (needs_cpu_analysis || needs_cpu_diagnostic || (trunk&&!gpu_trunk) ? UINT32_MAX : ~gpu_mask),
      pixels, pitch_bytes, changed_voxel_rows);
  if(trunk&&!gpu_trunk&&!MaterializeTrunk(slot,pixels,pitch_bytes)) {
    SessionFatal_Request("Cannot allocate trunk enclosure surfaces");return;
  }
  if(trunk&&!gpu_trunk)for(unsigned i=0;i<3;++i) {
    const unsigned plane=i==0?SR_PPU_OVERLAY_BG1:i==1?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far;
    gpu_mask&=~(1u<<plane);s_resolved_textures[plane]=s_plane_textures[plane];
  }
  DioramaUploadChanges voxel_changes = {0};
  if (castle) {
    const bool source_materialized = pixels[SR_PPU_OVERLAY_BG1] != NULL;
    /* A failed optional decode must never publish a partially updated split.
     * Preserve the complete recovery split when no low source was produced. */
    if (!MaterializeVoxel(slot,pixels,pitch_bytes,reuse_voxel_source,
        source_materialized ? changed_voxel_rows : NULL)) {
      SessionFatal_Request("Cannot allocate castle enclosure surfaces"); return;
    }
    uint64_t previous_revision = s_voxel_upload_revision;
    if (!reuse_voxel_source && ++s_voxel_upload_revision == 0) {
      s_voxel_upload_revision = 1;
      previous_revision = 0;
    }
    voxel_changes = (DioramaUploadChanges){
      .plane_mask = cache_voxel_source ? castle_planes : 0,
      .source = {.previous_revision = previous_revision, .revision = s_voxel_upload_revision,
        .changed_rows = source_materialized && changed_voxel_rows ? changed_voxel_rows->changed : NULL,
        .row_count = (unsigned)voxel_source_key.height}};
    if (!reuse_voxel_source && cache_voxel_source && source_materialized)
      DioramaVoxelSourceCache_Retain(&s_voxel_source_cache,
          slot->background_packet, &voxel_source_key);
    const unsigned planes[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Far};
    for (unsigned i=0;i<2;++i) {
      gpu_mask &= ~(1u << planes[i]);
      s_resolved_textures[planes[i]] = s_plane_textures[planes[i]];
    }
  }
  /* Row 0 is the top of the captured world band. Upload both sides; the
   * authentic frame begins at ws_extra_top and the lower band follows it. */
  const DioramaUploadResult upload = Diorama_UploadResolvedWithChanges(
      device, s_resolved_textures, pixels, pitch_bytes,
      slot->snes_width + slot->obj_apron * 2,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      slot->obj_apron, slot->diorama_bg_apron_mask, upload_mask, gpu_mask, gpu_changed,
      collect_coverage, &voxel_changes);
  s_diorama_uploaded_plane_mask = upload.synchronized_plane_mask;
  uint32_t trunk_changed=upload.changed_plane_mask;
  if(gpu_trunk&&!DioramaTrunkGpu_Upload(device,slot,s_resolved_textures,upload.changed_plane_mask,&trunk_changed)) {
    SessionFatal_Request("Trunk enclosure split failed: %s",ArRenderDevice_LastError(device));return;
  }
  if(slot->diorama_trunk) s_diorama_uploaded_plane_mask|=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Bg1Far);
  if (!DioramaTrunkGuardUpload_Update(&s_trunk_guard, device,
      trunk ? slot->diorama_trunk : NULL, trunk ? &slot->diorama_trunk_guard : NULL,
      (slot->inidisp & 0x80) ? 0 : slot->inidisp & 15)) {
    SessionFatal_Request("Cannot upload native tree source guards"); return;
  }
  UploadVoxelGuard(device, slot, pixels, pitch_bytes);
  if(gpu_trunk&&(needs_cpu_analysis||needs_cpu_diagnostic)&&!MaterializeTrunk(slot,pixels,pitch_bytes)) {
    SessionFatal_Request("Cannot allocate trunk enclosure snapshot");return;
  }
  if (upload.changed_plane_mask &
      ((UINT32_C(1) << SR_PPU_OVERLAY_BG2) | (UINT32_C(1) << kDioramaPlane_Bg2Far)))
    s_diorama_bg2_content_revision++;
  const PerformanceScope coverage = PerformanceMetrics_Begin(kPerformance_BgCoverage);
  for (unsigned plane = 0; collect_coverage && plane < kDioramaPlane_Count; ++plane) {
    if ((gpu_mask & (1u << plane)) && DioramaPlaneUsesSparseCoverage((int)plane)) {
      if ((gpu_changed & (1u << plane)) || !s_diorama_coverage_masks[plane]) {
        const bool bg2 = plane == kDioramaPlane_Bg2Hi || plane == kDioramaPlane_Bg2Far;
        const unsigned band =
            plane == kDioramaPlane_Bg1Hi || plane == kDioramaPlane_Bg2Hi ? 1u : 2u;
        s_diorama_coverage_masks[plane] = BackgroundPacketCoverage(
            slot->background_packet, bg2 ? 1u : 0u, band, (unsigned)slot->obj_apron);
      }
    } else s_diorama_coverage_masks[plane] = upload.coverage_masks[plane];
  }
  PerformanceMetrics_End(coverage);
  if(collect_coverage && slot->diorama_trunk) {
    s_diorama_coverage_masks[kDioramaPlane_Bg1Hi]=UINT64_MAX;
    s_diorama_coverage_masks[kDioramaPlane_Bg1Far]=UINT64_MAX;
  }
  PlaneStatCensus(
      pixels, pitch_bytes,
      slot->snes_width + slot->obj_apron * 2,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      s_diorama_uploaded_plane_mask);
  /* A failed raw upload cannot be a valid endpoint: exclude it before
   * retaining/analyzing the pair so generation never interpolates from an
   * image that was not actually presentable. */
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    if (!(s_diorama_uploaded_plane_mask & (1u << plane)))
      pixels[plane] = NULL;
  DioramaSnapshotCapture_Retain(slot, pixels, pitch_bytes,
      s_diorama_uploaded_plane_mask,
      skybox_pixels && ArRenderTexture_IsValid(s_diorama_skybox_view.texture) ? skybox : NULL);
  DioramaPerformanceScope frame_analysis =
      DioramaPerformance_Begin(kDioramaPerformance_FrameAnalysis);
  DioramaFrameGeneration_CaptureWithSkybox(
      device, slot, s_resolved_textures, pixels, pitch_bytes,
      s_diorama_uploaded_plane_mask, trunk_changed,
      s_diorama_skybox_view.texture, skybox_changed);
  DioramaPerformance_End(frame_analysis);
}

static ArRenderPointF CompletionPlayer(const FrameSlot *slot,
                                      const DioramaProjection *projection) {
  ArRenderPointF player = {.5f, .72f};
  if (!projection->valid || projection->output_width <= 0 || projection->output_height <= 0)
    return player;
  for (unsigned i = 0; i < slot->action_scene_effects.actor_count; i++) {
    const ActionEffectActor *actor = &slot->action_scene_effects.actors[i];
    if (!actor->player) continue;
    ArRenderPointF point;
    const float x = actor->x - slot->bg1_camera_x + slot->ws_extra;
    const float y = actor->y - slot->bg1_camera_y + slot->ws_extra_top - 14;
    if (Diorama_ProjectCapturedPoint(projection, x, y, actor->priority, &point, NULL, NULL))
      player = (ArRenderPointF){(point.x - projection->output_x) / projection->output_width,
                                (point.y - projection->output_y) / projection->output_height};
    break;
  }
  return player;
}

typedef struct CompletionBackdropContext {
  const FrameSlot *slot;
  const DioramaProjection *projection;
  const DioramaCapture *capture;
  uint32_t visible_planes;
  ArRenderDevice *device;
  DeathHeimCompletionArt art;
} CompletionBackdropContext;

static PresentationOutcome CaptureCompletionArt(CompletionBackdropContext *context,
                                                ArRenderRectI viewport) {
  ArRenderDevice *device = context->device;
  context->art = (DeathHeimCompletionArt){0};
  const FrameSlot *slot = context->slot;
  if (!(slot->sim.death_heim_completion_flags &
        (kDeathHeimCompletion_Platform | kDeathHeimCompletion_Rim)) ||
      !CompletionVistaBackend_IsAvailable(device))
    return kPresentationOutcome_Complete;
  if (s_completion_art_width != viewport.w || s_completion_art_height != viewport.h) {
    ArRenderDevice_DestroyTexture(device, s_completion_art);
    s_completion_art = ArRenderTexture_Invalid();
  }
  if (!ArRenderTexture_IsValid(s_completion_art)) {
    const ArRenderTextureDesc desc = {viewport.w, viewport.h, kArRenderPixelFormat_Argb8888,
        kArRenderTextureUsage_Target, kArRenderFilter_Nearest,
        kArRenderBlendMode_AlphaPremultiplied};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_completion_art))
      return kPresentationOutcome_OptionalOmitted;
    s_completion_art_width = viewport.w;
    s_completion_art_height = viewport.h;
  }
  ArRenderTargetState saved;
  const ArRenderTargetBeginResult begun =
      ArRenderDevice_BeginTarget(device, s_completion_art, &saved);
  if (begun != kArRenderTargetBegin_Ready)
    return begun == kArRenderTargetBegin_StateLost ? kPresentationOutcome_CoreFailure
                                                  : kPresentationOutcome_OptionalOmitted;
  bool ready = ArRenderDevice_Clear(device, (ArRenderColorF){0});
  DioramaProjection projection = *context->projection;
  projection.output_x = projection.output_y = 0;
  enum { kColumns = 8, kRows = 12, kVertices = (kColumns + 1) * (kRows + 1) };
  ArRenderVertex2D vertices[kVertices];
  int32_t indices[kColumns * kRows * 6];
  int used = 0;
  for (int y = 0; y < kRows; y++) for (int x = 0; x < kColumns; x++) {
    const int a = y * (kColumns + 1) + x;
    const int32_t cell[6] = {a, a+1, a+kColumns+2, a, a+kColumns+2, a+kColumns+1};
    memcpy(indices + used, cell, sizeof(cell));
    used += 6;
  }
  const int planes[] = {kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi,
      SR_PPU_OVERLAY_OBJ, kDioramaPlane_Obj1, kDioramaPlane_Obj2, kDioramaPlane_Obj3};
  for (unsigned p = 0; ready && p < sizeof(planes)/sizeof(planes[0]); p++) {
    const int plane = planes[p], priority = DioramaPlaneObjectPriority(plane);
    const ArRenderTexture texture = context->capture->textures[plane];
    if (!(context->visible_planes & (1u << plane)) ||
        !context->capture->pixels[plane] || !ArRenderTexture_IsValid(texture)) continue;
    const DioramaPlaneProjection *shape = priority >= 0 ? &projection.object_planes[priority]
        : plane == SR_PPU_OVERLAY_BG1 ? &projection.bg1_plane
        : plane == kDioramaPlane_Bg1Far ? &projection.bg1_far_plane : &projection.bg1_high_plane;
    if (!shape->valid) continue;
    bool projected = true;
    for (int y = 0; projected && y <= kRows; y++) for (int x = 0; x <= kColumns; x++) {
      const float sx = shape->u0 + (shape->u1-shape->u0)*x/kColumns;
      const float sy = shape->v0 + (shape->v1-shape->v0)*y/kRows;
      const float cx =
          sx * projection.texture_width - projection.texture_x_origin - shape->capture_offset.x;
      const float cy = sy*projection.texture_height - shape->capture_offset.y;
      ArRenderVertex2D *v = &vertices[y*(kColumns+1)+x];
      projected = priority >= 0
          ? Diorama_ProjectCapturedPoint(&projection,cx,cy,priority,&v->position,NULL,NULL)
          : plane == SR_PPU_OVERLAY_BG1
              ? Diorama_ProjectCapturedBg1Point(&projection,cx,cy,&v->position,NULL,NULL)
              : plane == kDioramaPlane_Bg1Far
                  ? Diorama_ProjectCapturedBg1FarPoint(&projection,cx,cy,&v->position,NULL,NULL)
                  : Diorama_ProjectCapturedBg1HighPoint(&projection,cx,cy,&v->position,NULL,NULL);
      if (!projected) break;
      v->color = (ArRenderColorF){1,1,1,1}; v->tex_coord = (ArRenderPointF){sx,sy};
    }
    if (projected)
      ready = ArRenderDevice_DrawGeometry(device, texture, vertices, kVertices, indices, used);
  }
  if (!ArRenderDevice_EndTarget(device, &saved)) return kPresentationOutcome_CoreFailure;
  if (!ready) return kPresentationOutcome_OptionalOmitted;
  ArRenderPointF waterline = CompletionPlayer(slot, &projection);
  waterline.y += .24f;
  for (unsigned i = 0; i < slot->action_scene_effects.actor_count; i++) {
    const ActionEffectActor *actor = &slot->action_scene_effects.actors[i];
    if (!actor->player) continue;
    ArRenderPointF point;
    /* The native completion plinth extends 36 original pixels below the
     * standing actor. Project that contact through BG1, including camera tilt. */
    if (Diorama_ProjectCapturedBg1Point(&projection,
        actor->x-slot->bg1_camera_x+slot->ws_extra,
        actor->y-slot->bg1_camera_y+slot->ws_extra_top+36,&point,NULL,NULL))
      waterline = (ArRenderPointF){point.x/viewport.w,point.y/viewport.h};
    break;
  }
  context->art = (DeathHeimCompletionArt){s_completion_art, waterline};
  return kPresentationOutcome_Complete;
}

static PresentationOutcome DrawCompletionBackdrop(void *userdata, ArRenderRectI viewport) {
  CompletionBackdropContext *context = userdata;
  const PresentationOutcome art = CaptureCompletionArt(context,viewport);
  if (!PresentationOutcome_IsUsable(art)) return art;
  return PresentationOutcome_Combine(art,PresentDeathHeimCompletionBackdrop(
      context->slot, viewport, CompletionPlayer(context->slot, context->projection), context->art));
}

void PresentDiorama_Draw(ArRenderDevice *device, const FrameSlot *slot, float alpha) {
  DioramaPerformanceScope presentation_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Total);
  /* Compositing uses only presence, never CPU texels. Upload has already
   * built textures, coverage masks, snapshot copies and motion endpoints.
   * Use a real presence token, as the portable compositor tests do, instead
   * of carrying a pointer into the producer's next scanout. */
  static const uint8_t uploaded_plane = 0;
  const uint8_t *pixels[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    pixels[plane] = (s_diorama_uploaded_plane_mask & (1u << plane))
        ? &uploaded_plane : NULL;
  ArRenderTexture current_textures[kDioramaPlane_Count];
  ArRenderTexture scene_textures[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    current_textures[plane] = s_resolved_textures[plane];
  const DioramaCameraView camera = DioramaCamera_Present(
      &s_diorama_camera, &slot->diorama_camera,
      slot->timestamp_ns, HostClock_Nanoseconds());
  bool retried_projection = false;
retry_projection:;
  DioramaPerformanceScope frame_synthesis =
      DioramaPerformance_Begin(kDioramaPerformance_FrameSynthesis);
  DioramaSkyboxView skybox_view = s_diorama_skybox_view;
  const uint32_t generated_plane_mask = DioramaFrameGeneration_PrepareWithSkybox(
      device, slot, alpha, current_textures,
      s_diorama_uploaded_plane_mask, scene_textures,
      skybox_view.texture, &skybox_view.texture);
  skybox_view.dynamic =
      (generated_plane_mask & (1u << kDioramaFrameGenerationSkybox)) != 0;
  skybox_view.capture_offset =
      DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox);
  /* Canonical source mapping belongs to the captured pixels; frame generation
   * contributes only its presentation translation. */
  if (!skybox_view.periodic && ArRenderTexture_IsValid(skybox_view.texture)) {
    skybox_view.capture_offset.x += slot->bg2_camera_x - slot->ws_extra -
        slot->diorama_skybox_world_x;
    skybox_view.subpixel_x = slot->diorama_skybox_world_x_fraction;
  }
  DioramaPerformance_End(frame_synthesis);
  /* The existing graphics setting now selects frame-space generation.
   * Prepare fails individual planes closed when either endpoint or pair
   * continuity is unavailable, leaving their current raw textures intact. */

  /* Resolve BG2's row-banded valid capture spans from the slot
   * alone (the presenter only reads the captured frame). ws_extra, not
   * extra_left_right, is the offset: the capture pitch and Diorama_Upload's
   * rect are both derived from ws_extra, so it is what texture column 0
   * corresponds to. Keep the two concepts distinct even when their values are
   * equal, so either can change without altering the other's meaning. */
  DioramaBgValidSpanPlan bg2_valid_spans;
  /* + obj_apron: each span is in SURFACE columns, and screen x = 0 sits at
   * column obj_apron + ws_extra now that the surfaces carry resolve headroom
   * on both sides. Without it the skybox would crop its sky an apron early. */
  DioramaBgValidSpanPlan_Build(
      slot->ws_extra + slot->obj_apron,
      slot->extra_left_right,
      slot->extra_left_cur, slot->extra_right_cur,
      slot->bg_capture_pad_to_budget,
      &slot->action_bg_plan.layer[kActionBgPlanLayerCount - 1],
      &slot->diorama_bg2_source_bounds,
      slot->ws_extra_top,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      kFrameSlotLayerTextureWidth, &bg2_valid_spans);
  ArRenderRectI output_viewport;
  if (!Present_ResolveOutputViewport(device, slot, &output_viewport)) {
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    SessionFatal_Request(
        "The renderer could not resolve the Diorama output viewport (%s). "
        "Restart the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(device));
    return;
  }
  DioramaProjection action_projection;
  const uint8_t required_effect_obj_priorities =
      (slot->action_effect_lighting || slot->action_effect_particles)
          ? ActionEffectProjection_RequiredObjPriorityMask(
                &slot->action_effects, &slot->action_scene_effects)
          : 0;
  const uint8_t effect_obj_priority_mask =
      Diorama_FilterObjEffectProjectionMask(
          required_effect_obj_priorities,
          slot->diorama_plane_request_mask,
          slot->diorama_plane_content_mask,
          s_diorama_uploaded_plane_mask);
  const uint32_t required_effect_bg_planes =
      (slot->action_effect_lighting || slot->action_effect_particles ||
       slot->action_environmental_effects)
          ? ActionEffectProjection_RequiredBgPlaneMask(
                &slot->action_effects, &slot->action_scene_effects)
          : 0;
  const uint32_t effect_bg_plane_mask =
      Diorama_FilterBgEffectProjectionMask(
          required_effect_bg_planes,
          slot->diorama_plane_request_mask,
          slot->diorama_plane_content_mask,
          s_diorama_uploaded_plane_mask);
  (void)PresentActionHeat_Begin(device, slot, output_viewport);
  const ArRenderRectI viewport = PresentActionHeat_SceneViewport(output_viewport);
  const PresentActionSourceDraw source_draw = DioramaFrameGeneration_SourceProjectionActive()
      ? DioramaFrameGeneration_DrawSource : NULL;
  PresentActionPlaneEffectContext plane_effect = {
    device, slot, viewport, source_draw,
  };
  ArRenderPointF plane_offsets[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    plane_offsets[plane] = DioramaFrameGeneration_PlaneOffset(plane);
  const int capture_height =
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom;
  DioramaVerticalBounds vertical_bounds = {0};
  DioramaHorizontalBounds horizontal_bounds = {0};
  const int primary_layer = ActionBgPlan_PlayfieldLayer(&slot->action_bg_plan);
  if (primary_layer >= 0) {
    const ActionBgLayerPlan *primary = &slot->action_bg_plan.layer[primary_layer];
    const int plane = primary_layer == 0 ? SR_PPU_OVERLAY_BG1 : SR_PPU_OVERLAY_BG2;
    const int world_y0 = slot->diorama_world_height > 0
        ? slot->diorama_world_y0 : 0;
    const int world_height = slot->diorama_world_height > 0
        ? slot->diorama_world_height : primary->world_height;
    vertical_bounds = DioramaVerticalBounds_Resolve(
        plane, primary->camera_y - world_y0, world_height,
        slot->ws_extra_top, capture_height);
    if (slot->diorama_world_width > 0)
      horizontal_bounds = DioramaHorizontalBounds_Resolve(
          plane, primary_layer == 0 ? slot->bg1_camera_x : slot->bg2_camera_x,
          slot->diorama_world_x0,
          slot->diorama_world_width, slot->snes_width,
          (slot->diorama_bg_apron_mask & (1u << primary_layer))
              ? slot->obj_apron : 0);
  }
  const ActionRoomSceneFrameState voxel_colors={
      .brightness=(slot->inidisp&0x80)?0:slot->inidisp&15,
      .cgwsel=slot->diorama_voxel_cgwsel,.cgadsub=slot->diorama_voxel_cgadsub,
      .fixed_color=slot->diorama_voxel_fixed_color};
  const DioramaCapture capture = {
      .voxel_frame=&voxel_colors,
      .plane_capture_offsets = plane_offsets,
      .voxel_map_plane_mask = DioramaFrameGeneration_VoxelMapPlaneMask(),
      .width = slot->snes_width,
      .height = capture_height,
      .authentic_y0 = slot->ws_extra_top,
      .vertical_bounds = vertical_bounds,
      .horizontal_bounds = horizontal_bounds,
      .framing_x = slot->diorama_framing_x,
      .framing_y = slot->diorama_framing_y,
      .bg_apron_mask = slot->diorama_bg_apron_mask,
      .camera_y = slot->bg1_camera_y,
      .bg2_camera_x = slot->bg2_camera_x,
      .bg2_camera_y = slot->bg2_camera_y,
      .bg2_world_height = slot->action_bg_plan.layer[1].world_height,
      .bg2_vertical_ratio = primary_layer == 0
          ? slot->bg2_vertical_ratio : 0,
      .bg2_scroll_valid = primary_layer == 0 &&
          slot->action_bg_plan.layer[1].valid,
      .obj_apron = slot->obj_apron,
      .textures = scene_textures,
      .pixels = pixels,
      .bg_transparent_fill_configured =
          slot->diorama_bg_transparent_fill_configured,
      .bg_transparent_fill_argb = slot->diorama_bg_transparent_fill_argb,
      .coverage_masks =
          DioramaUpload_UsesCoverage(slot->interp_setting_enabled) ? s_diorama_coverage_masks : NULL,
      .bg2_valid_spans = &bg2_valid_spans,
      .bg2_raster_camera_sensitivity = slot->bg2_raster_camera_sensitivity_valid
          ? slot->bg2_raster_camera_sensitivity : NULL,
      .bg2_raster_subtexel = slot->bg2_raster_camera_sensitivity_valid
          ? slot->bg2_raster_subtexel : NULL,
      .bg1_camera_x = slot->bg1_camera_x,
      .bg2_raster_camera_x = slot->bg2_raster_camera_x,
      .skybox = &skybox_view,
      .bg2_revision = s_diorama_bg2_content_revision,
      .bg2_dynamic =
          (generated_plane_mask & ((UINT32_C(1) << SR_PPU_OVERLAY_BG2) |
              (UINT32_C(1) << kDioramaPlane_Bg2Far))) != 0,
  };
  const DioramaView view = {
      .camera = camera.pose,
      .distance_scale = camera.distance_scale,
      .distance_offset = camera.distance_offset,
      .camera_framing_weight = camera.framing_weight,
      .fit_free_camera = camera.free_camera && primary_layer >= 0,
      .pixel_aspect = slot->pixel_aspect,
      .ignore_aspect_ratio = slot->ignore_aspect_ratio,
      .visible_width = slot->visible_width,
      .visible_height = FrameSlot_VisibleHeight(slot),
      .viewport = viewport,
  };
  const float exposure_x=slot->bg1_camera_x-slot->ws_extra-slot->obj_apron;
  const float exposure_y=slot->bg1_camera_y-slot->ws_extra_top;
  ArRenderRectF dimming_ramp = SceneryDimming_ToUv(PresentActionEffects_Bg1DimmingRamp(slot),
      exposure_x,exposure_y,kFrameSlotLayerTextureWidth,kFrameSlotLayerTextureHeight);
  ActionExposureLighting exposure={.origin_x=exposure_x,.origin_y=exposure_y,
      .width=kFrameSlotLayerTextureWidth,.height=kFrameSlotLayerTextureHeight};
  ActionExposure_Prepare(&exposure,slot->action_environmental_effects?&slot->action_scene_effects:NULL);
  DioramaRenderOptions render;
  Diorama_CaptureRenderOptions(&render);
  if (source_draw) {
    render.draw_resident_skybox = DioramaFrameGeneration_DrawSkybox;
    render.source_effect_batch = DioramaFrameGeneration_SourceEffectBatch;
  }
  const bool completion_world = slot->sim.death_heim_completion_world &&
      slot->diorama_layer_section == kDioramaLayerSection_DeathHeimCompletion;
  if (completion_world) {
    /* The vista owns its backdrop and enclosure for this draw. Keep the
     * player's saved skybox/shoebox choices intact for ordinary rooms and
     * for the native completion scene when this enhancement is disabled. */
    render.skybox = kDioramaSky_Off;
    render.shoebox = false;
    render.visible_planes &= ~((1u << SR_PPU_OVERLAY_BG2) |
        (1u << kDioramaPlane_Bg2Hi) | (1u << kDioramaPlane_Bg2Far) |
        (1u << kDioramaPlane_Backdrop));
  }
  /* The compositor publishes the resolved player plane before this callback.
   * Water reflections and foreground shafts therefore share the same anchor. */
  CompletionBackdropContext completion = {
      .slot = slot, .projection = &action_projection, .capture = &capture,
      .visible_planes = render.visible_planes, .device = device};
  ActionVoxelSpriteFrame voxel_sprites = s_voxel_sprites_current;
  if (s_voxel_sprite_pair && slot->interp_setting_enabled && alpha >= 0) {
    const float phase = PresentationFrameGeneration_PairPhase(alpha, slot->capture_ticks);
    for (unsigned i=0; i<voxel_sprites.count; ++i) {
      ActionVoxelSprite *prop = &voxel_sprites.sprites[i];
      if (!(generated_plane_mask & (1u << DioramaPlaneForObjectPriority(prop->priority)))) continue;
      for (unsigned j=0; j<s_voxel_sprites_previous.count; ++j) {
        const ActionVoxelSprite *old = &s_voxel_sprites_previous.sprites[j];
        if (!ActionVoxelSprite_SameComponent(old,prop)) continue;
        const float dx=old->x-prop->x, dy=old->y-prop->y;
        if (dx*dx+dy*dy <= 1024) {
          prop->x += dx*(1-phase);
          prop->y += dy*(1-phase);
        }
        break;
      }
    }
  }
  DioramaScene scene = {
      .render = &render,
      .voxel_mode=slot->diorama_voxel_mode,
      .voxel_geometry=slot->diorama_voxel,
      .voxel_guard=s_voxel_guard.view.capture_generation &&
          s_voxel_guard.view.capture_generation==slot->diorama_voxel_guard.capture_generation &&
          s_voxel_guard.view.source_revision==ActionVoxel_Revision(slot->diorama_voxel)
          ? &s_voxel_guard.view : NULL,
      .voxel_sprites=&voxel_sprites,
      .voxel_sprite_atlas=s_voxel_sprite_atlas,
      .trunk=slot->diorama_trunk,.trunk_settings=slot->diorama_trunk_settings,
      .trunk_guard = s_trunk_guard.view.capture_generation &&
          s_trunk_guard.view.capture_generation == slot->diorama_trunk_guard.capture_generation &&
          slot->diorama_trunk && s_trunk_guard.view.source_revision == slot->diorama_trunk->revision
          ? &s_trunk_guard.view : NULL,
      .trunk_effects=slot->action_environmental_effects?&slot->action_scene_effects:NULL,
      .ground_cover_light=ActionGroundCover_LightSamples,
      .trunk_lighting=slot->action_effect_lighting,.trunk_particles=slot->action_effect_particles,
      .map_group = slot->diorama_map_group,
      .map_number = slot->diorama_map_number,
      .native_bloom_clock = slot->action_scene_clock,
      .native_bloom_dimming = (slot->inidisp&0x80)?1:1-(slot->inidisp&15)/15.f,
      .layer_section = slot->diorama_layer_section,
      .bg1_dimming = PresentActionEffects_Bg1Dimming(slot),
      .bg1_dimming_ramp = dimming_ramp,
      .scenery_exposure = exposure.count?&exposure.field:NULL,
      .scenery_grade = slot->action_environmental_effects ?
          DioramaLayerOrder_SceneryGrade(render.layers,slot->diorama_map_group,
              slot->diorama_map_number,slot->diorama_layer_section) : (DioramaSceneryGrade){0},
      .scenery_light = slot->action_environmental_effects &&
          ActionEffectReceivers_HasSurfaceLights(&slot->action_scene_effects)
          ? PresentActionEffects_DrawSceneryLight : NULL,
      .actor_light = slot->action_environmental_effects ? PresentActionEffects_DrawActorLight : NULL,
      .sky_weather = slot->action_environmental_effects &&
          ActionBackground_HasSourceEffects(&slot->action_scene_effects,
              slot->action_effect_lighting,slot->action_effect_particles) ? PresentActionEffects_DrawSkyWeather : NULL,
      .actor_light_reuse = {
        slot->action_effect_lighting&&ActionEffectReceivers_MapsMatch(&slot->action_scene_effects,1,2)?1:0,
        slot->action_effect_lighting&&ActionEffectReceivers_MapsMatch(&slot->action_scene_effects,1,4)?1:
          ActionEffectReceivers_MapsMatch(&slot->action_scene_effects,2,4)?2:0},
      .cloud_light = ActionCloudLight_Prepare(slot->action_environmental_effects&&slot->action_effect_lighting?&slot->action_scene_effects:NULL,
          slot->bg2_camera_x-slot->ws_extra-slot->obj_apron,slot->bg2_camera_y-slot->ws_extra_top,
          kFrameSlotLayerTextureWidth,kFrameSlotLayerTextureHeight),
      .background_heat = ActionHeat_Prepare(slot->action_environmental_effects&&slot->action_effect_particles?&slot->action_scene_effects:NULL,
          kActionEffectProjectionPlane_Bg2,
          slot->bg2_camera_x-slot->ws_extra-slot->obj_apron,slot->bg2_camera_y-slot->ws_extra_top,
          kFrameSlotLayerTextureWidth,kFrameSlotLayerTextureHeight),
      .scenery_heat = ActionHeat_Prepare(slot->action_environmental_effects&&slot->action_effect_particles?&slot->action_scene_effects:NULL,
          kActionEffectProjectionPlane_Bg1,
          slot->bg1_camera_x-slot->ws_extra-slot->obj_apron,slot->bg1_camera_y-slot->ws_extra_top,
          kFrameSlotLayerTextureWidth,kFrameSlotLayerTextureHeight),
      .additive_plane_mask =
          slot->diorama_plane_additive_mask & s_diorama_uploaded_plane_mask,
      .effect_obj_priority_mask = effect_obj_priority_mask,
      .effect_bg_plane_mask = effect_bg_plane_mask,
      .plane_effect = PresentActionEffects_DrawDioramaPlane,
      .plane_effect_userdata = &plane_effect,
      .backdrop = completion_world ? DrawCompletionBackdrop : NULL,
      .backdrop_userdata = &completion,
  };
  DioramaAstralRift_Attach(slot->action_environmental_effects, device, &scene);
  DioramaSnapshotCapture_Write(slot, &capture, &view, &scene);
  /* Shared visibility follows the ActionVoxel material/object contract.
   * The established curved-tree enclosure has its own split-plane priority
   * policy and no verified modeled OBJ family; retain that compositor. */
  const bool shared_depth=slot->diorama_voxel_mode &&
      ActionVoxel_Active(scene.voxel_geometry,scene.map_group,scene.map_number);
  if (shared_depth) {
    scene.depth_sink=DioramaDepthGpu_Begin(device);
    if (!scene.depth_sink) {
      PresentActionHeat_Cancel(device);
      DioramaPerformance_End(presentation_performance);
      DioramaPerformance_PresentCompleted();
      SessionFatal_Request("Cannot prepare Voxel world visibility (%s).",
          ArRenderDevice_LastError(device));
      return;
    }
  }
  const PresentationOutcome diorama = Diorama_Composite(
      device, &capture, &view, &scene, &action_projection);
  if (PresentationOutcome_IsUsable(diorama))
    Diorama_SetAutoCameraDistance(action_projection.auto_distance);
  if ((!PresentationOutcome_IsUsable(diorama) ||
       DioramaFrameGeneration_SourceProjectionFailed()) && source_draw && !retried_projection) {
    PresentActionHeat_Cancel(device);
    DioramaFrameGeneration_RecoverSourceProjection(device);
    retried_projection = true;
    goto retry_projection;
  }
  if (!PresentationOutcome_IsUsable(diorama)) {
    PresentActionHeat_Cancel(device);
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    SessionFatal_Request(
        "The selected Diorama renderer could not complete its core scene "
        "(%s). Restart the game. If this happens again, update your "
        "graphics driver or disable Diorama mode before entering the room.",
        ArRenderDevice_LastError(device));
    return;
  }
  DioramaPerformanceScope callback_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Callback);
  if (!source_draw || DioramaFrameGeneration_SourceEffectBatch(device, true)) {
    PresentActionEffects_DrawWithSource(device, slot, viewport, &action_projection, source_draw);
    if (source_draw) (void)DioramaFrameGeneration_SourceEffectBatch(device, false);
  }
  DioramaPerformance_End(callback_performance);
  if (source_draw && DioramaFrameGeneration_SourceProjectionFailed() && !retried_projection) {
    PresentActionHeat_Cancel(device);
    DioramaFrameGeneration_RecoverSourceProjection(device);
    retried_projection = true;
    goto retry_projection;
  }
  PresentActionHeat_End(device, slot, output_viewport);
  if (completion_world) {
    const PresentationOutcome foreground =
        PresentDeathHeimCompletionForeground(slot, output_viewport,
                                             CompletionPlayer(slot, &action_projection),
                                             completion.art);
    if (!PresentationOutcome_IsUsable(foreground)) {
      DioramaPerformance_End(presentation_performance);
      DioramaPerformance_PresentCompleted();
      SessionFatal_Request(
          "The completion light effects could not restore their renderer state (%s).",
          ArRenderDevice_LastError(device));
      return;
    }
  }
  /* Count once after recovery, including the first frame that fell back. */
  if (DioramaFrameGeneration_SourceProjectionFallback())
    PerformanceMetrics_Add(kPerformanceCount_EffectProjectionFallbackFrames, 1);
  if (!ArRenderSceneScale_Resolve(device, &output_viewport, kArRenderFilter_Linear)) {
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    SessionFatal_Request("The renderer could not upscale the Diorama scene.");
    return;
  }
  /* Flat HUD mode leaves BG3 in the same RemoveFromGame capture used by flat
   * presentation. Reconstruct its split pieces into one texture before
   * drawing the screen-space overlay; drawing them directly creates seams
   * (see PresentHud_DrawComposited).
   *
   * With diorama_hud_flat off, capture rebinds BG3 into the diorama layer
   * buffer so it renders as the ordinary tilted BG3 plane in
   * Diorama_Composite's own
   * per-layer loop above — skip the anchored overlay entirely here so the
   * two don't both draw a HUD. */
  if (slot->diorama_hud_flat)
    PresentHud_DrawComposited(
        device, slot, (ArRenderRectI){
          output_viewport.x, output_viewport.y,
          output_viewport.w, output_viewport.h,
        });
  DioramaPerformance_End(presentation_performance);
  DioramaPerformance_PresentCompleted();
}

void PresentDiorama_Reset(ArRenderDevice *device) {
  DioramaTrunkGuardUpload_Reset(&s_trunk_guard, device);
  DioramaVoxelGuardUpload_Reset(&s_voxel_guard, device);
  DioramaVoxelSourceCache_Reset(&s_voxel_source_cache);
  DioramaDepthGpu_Reset(device);
  ResetVoxelSprites(device);
  ArRenderDevice_DestroyTexture(device,s_completion_art);
  s_completion_art=ArRenderTexture_Invalid();
  s_completion_art_width=s_completion_art_height=0;
  DioramaFrameGeneration_Reset();
  for (unsigned plane = 0; plane <= kDioramaPlane_Count; ++plane) {
    free(s_bg_fallback[plane]);
    s_bg_fallback[plane] = NULL;
  }
  DioramaTrunkGpu_Reset(device);
  for(unsigned i=0;i<3;++i){free(s_trunk_fallback[i]);s_trunk_fallback[i]=NULL;}
  for(unsigned i=0;i<2;++i){free(s_voxel_fallback[i]);s_voxel_fallback[i]=NULL;}
  DioramaBgGpu_Reset(device);
  DioramaSnapshotCapture_Reset();
  ArRenderDevice_DestroyTexture(device, s_diorama_skybox_texture);
  s_diorama_skybox_texture = ArRenderTexture_Invalid();
  s_diorama_skybox_view = (DioramaSkyboxView){0};
  PresentationUploadMirror_Reset(&s_diorama_skybox_mirror);
}
