#include "sim/church/present_church.h"
#include "sim/church/church_scene.h"
#include "sim/church/church_art.h"
#include "actraiser_game.h"
#include "present/presentation_surface.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static SimBackgroundVoxelScene *s_town;
static uint32_t *s_ground;
static ChurchLandscape *s_landscape;
static uint32_t s_serial;
static uint64_t s_reset_generation, s_load_generation;
static ChurchSceneOptions s_options;
static ArRenderTexture s_people, s_panels;
static bool s_foreground_valid, s_failed;
static uint32_t s_sprite_pixels[256 * 512];
static uint32_t s_panel_pixels[256 * 224];

static bool UploadForeground(ArRenderDevice *device, const FrameSlot *slot) {
  const SrPpuSurfaceView *objects =
      PresentationSurface_Bound(&slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0]);
  const SrPpuSurfaceView *panels =
      PresentationSurface_Bound(&slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0]);
  const uint8_t *obj = PresentationSurface_Region(objects, objects ? objects->origin_x : -1,
                                                  objects ? objects->origin_y : -1, 256, 224);
  const uint8_t *bg = PresentationSurface_Region(panels, panels ? panels->origin_x : -1,
                                                 panels ? panels->origin_y : -1, 256, 224);
  const uint32_t *columns = ChurchArt_ColumnPixels();
  if (!obj || !bg || !columns) return false;
  memset(s_sprite_pixels, 0, sizeof(s_sprite_pixels));
  for (int y = 0; y < 224; y++)
    memcpy(s_sprite_pixels + y * 256, obj + y * objects->pitch_bytes, 256 * sizeof(uint32_t));
  /* The temple's original 32-square actor cells. The source is the separate
   * OBJ plane: no floor, spotlight, or menu frame becomes part of a sprite. */
  for (int actor = 0; actor < 2; actor++) {
    int left = 32, top = 32, right = 0, bottom = 0;
    for (int y = 0; y < 32; y++) {
      uint32_t *source = s_sprite_pixels + (72 + y) * 256 + 96 + actor * 32;
      for (int x = 0; x < 32; x++) {
        if (!(source[x] >> 24)) continue;
        if (x < left) left = x;
        if (y < top) top = y;
        if (x + 1 > right) right = x + 1;
        if (y + 1 > bottom) bottom = y + 1;
      }
      memcpy(s_sprite_pixels + (224 + y) * 256 + actor * 32, source, 32 * sizeof(uint32_t));
      memset(source, 0, 32 * sizeof(uint32_t));
    }
    s_options.actor_bounds[actor] =
        right > left ? (ArRenderRectI){left, top, right - left, bottom - top} : (ArRenderRectI){0};
  }
  for (int y = 0; y < 224; y++) {
    for (int x = 0; x < 256; x++) {
      uint32_t panel;
      memcpy(&panel, bg + y * panels->pitch_bytes + x * 4, 4);
      const uint32_t sprite = s_sprite_pixels[y * 256 + x];
      s_panel_pixels[y * 256 + x] = (sprite >> 24) ? sprite : panel;
    }
  }
  for (int y = 0; y < 144; y++)
    memcpy(s_sprite_pixels + (272 + y) * 256, columns + y * 28, 28 * 4);
  ArRenderTextureDesc desc = {.width = 256,
                              .height = 512,
                              .format = kArRenderPixelFormat_Argb8888,
                              .usage = kArRenderTextureUsage_Streaming,
                              .filter = kArRenderFilter_Nearest,
                              .blend = kArRenderBlendMode_Alpha};
  if (!ArRenderTexture_IsValid(s_people) && !ArRenderDevice_CreateTexture(device, &desc, &s_people))
    return false;
  desc.height = 224;
  if (!ArRenderTexture_IsValid(s_panels) && !ArRenderDevice_CreateTexture(device, &desc, &s_panels))
    return false;
  return ArRenderDevice_UpdateTexture(device, s_people, NULL, s_sprite_pixels, 256 * 4) &&
         ArRenderDevice_UpdateTexture(device, s_panels, NULL, s_panel_pixels, 256 * 4);
}

void PresentChurch_ResetResources(ArRenderDevice *device) {
  ChurchScene_Reset(device);
  ArRenderDevice_DestroyTexture(device, s_people);
  ArRenderDevice_DestroyTexture(device, s_panels);
  s_people = s_panels = ArRenderTexture_Invalid();
  s_foreground_valid = false;
  s_failed = false;
}

static void ResetTown(ArRenderDevice *device) {
  PresentChurch_ResetResources(device);
  free(s_town);
  free(s_ground);
  free(s_landscape);
  s_landscape = NULL;
  s_options.landscape = NULL;
  s_town = NULL;
  s_ground = NULL;
  s_serial = 0;
}

void PresentChurch_Reset(ArRenderDevice *device) {
  ResetTown(device);
  s_reset_generation = s_load_generation = 0;
}

void PresentChurch_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  /* Reset/load generations retire the old town even
   * when the loaded state happens to be in the same region and has the same
   * source serial. Device resets only release GPU resources, preserving the
   * owned CPU snapshot needed to reconstruct an already-open church. */
  if (slot->church_reset_generation != s_reset_generation ||
      slot->church_load_generation != s_load_generation) {
    ResetTown(device);
    s_reset_generation = slot->church_reset_generation;
    s_load_generation = slot->church_load_generation;
  }
  if (!slot->church_enabled) {
    PresentChurch_Reset(device);
    return;
  }
  s_foreground_valid = slot->church_town && UploadForeground(device, slot);
  if (slot->sim.view != kSimView_Enhanced) {
    /* Only an adjacent church visit may borrow this town. A title, palace,
     * another mode, or an authentic town retires the prototype snapshot. */
    if (!slot->church_town) ResetTown(device);
    return;
  }
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const uint32_t *ground = SimBackgroundVoxels_GroundPixels();
  if (!scene || !ground || scene->town != slot->sim.town || (slot->inidisp & 0x8f) != 15 ||
      !slot->sim.background_voxel_serial)
    return;
  /* A complete town frame starts a fresh visit after an optional failure. */
  if (s_failed) PresentChurch_ResetResources(device);
  if (!s_town) s_town = malloc(sizeof(*s_town));
  if (!s_ground) s_ground = malloc(512 * 512 * sizeof(*s_ground));
  if (!s_landscape) s_landscape = calloc(1, sizeof(*s_landscape));
  if (!s_town || !s_ground || !s_landscape) {
    PresentChurch_Reset(device);
    return;
  }
  if (s_serial != slot->sim.background_voxel_serial) {
    if (!ChurchLandscape_Capture(s_landscape, scene)) {
      ResetTown(device);
      return;
    }
    s_options.landscape = s_landscape;
    *s_town = *scene;
    memcpy(s_ground, ground, 512 * 512 * sizeof(*s_ground));
    s_serial = slot->sim.background_voxel_serial;
  }
  s_options.landscape_height_pct = slot->sim.landscape_height_pct;
  s_options.shading = slot->sim.background_voxel_shading;
  s_options.style = slot->sim.background_voxel_style;
  s_options.light_azimuth_deg = slot->sim.light_azimuth_deg;
  s_options.light_elevation_deg = slot->sim.light_elevation_deg;
  s_options.town_serial = s_serial;
}

bool PresentChurch_Active(const FrameSlot *slot) {
  if (!slot || !slot->church_enabled || !slot->church_town || !s_foreground_valid || s_failed ||
      !s_serial || !s_town || !s_ground || s_town->town != slot->church_town ||
      s_town->object_count > kSimBackgroundMaxObjects)
    return false;
  for (unsigned i = 0; i < s_town->object_count; i++)
    if (s_town->objects[i].kind == kSimBackgroundVoxel_Cathedral) return true;
  return false;
}

PresentationOutcome PresentChurch_Draw(ArRenderDevice *device, const FrameSlot *slot,
                                       ArRenderRectI viewport) {
  ChurchSceneOptions options = s_options;
  options.brightness = slot->inidisp & 0x80 ? 0.0f : (slot->inidisp & 15) / 15.0f;
  options.seconds = slot->timestamp_ns / 1000000000.0;
  options.reference_altar = slot->church_reference_altar;
  options.people = s_people;
  if (!PresentChurch_Active(slot)) return kPresentationOutcome_OptionalOmitted;
  ArRenderTargetState saved;
  if (!ArRenderDevice_CaptureTargetState(device, &saved)) {
    s_failed = true;
    return kPresentationOutcome_OptionalOmitted;
  }
  const bool drawn = ChurchScene_Draw(device, viewport, s_town, s_ground, &options);
  /* Even a failed nested target operation must restore the original owner
   * before native fallback can overwrite a partial scene. Failure to restore
   * is a core renderer error, never a recoverable missing enhancement. */
  if (!ArRenderDevice_EndTarget(device, &saved)) return kPresentationOutcome_CoreFailure;
  if (drawn) return kPresentationOutcome_Complete;
  fprintf(stderr, "[church] scene unavailable; using native church for this visit (%s)\n",
          ArRenderDevice_LastError(device));
  PresentChurch_ResetResources(device);
  s_failed = true;
  return kPresentationOutcome_OptionalOmitted;
}

ArRenderTexture PresentChurch_UiBackdrop(void) { return s_panels; }


void PresentChurch_PlaceComparison(const FrameSlot *slot, ArRenderRectI viewport,
                                   int margin, int frame_size, ArRenderRectF *destination) {
  if (!slot->church_enabled || !slot->church_town) return;
  destination->x = viewport.x + margin;
  destination->y = viewport.y + viewport.h * kActRaiserSimulationHudHeight /
                                    kFrameSlotAuthenticHeight + margin + frame_size;
}
