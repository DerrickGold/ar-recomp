#include "sim3d.h"
#include "sim_background_voxels.h"
#include "sim_background_voxel_preset.h"
#include "sim_town_terrain.h"

#include "sim_town_canvas.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "constants.h"
#include "deterministic_hash.h"
#include "snes_bgr555.h"
#include "snes/ppu.h"

_Static_assert(kSim3DMaxWidth == kPpuBufWidth,
               "SIM capture width must match the PPU buffer ceiling");
_Static_assert(kSim3DPlane_Count <= 16,
               "SIM produced-plane mask must fit its frame payload");

uint32_t *g_sim3d_layer_pixels[kSim3DPlane_Count];
uint32_t g_sim3d_flat_pixels[kSim3DMaxWidth * kSim3DMaxHeight];
uint32_t g_sim3d_difference_pixels[kSim3DMaxWidth * kSim3DMaxHeight];

typedef struct Sim3DCaptureState {
  bool active;
  bool bindings_owned;
  bool inspector_active;
  bool separated_valid;
  bool flat_stage_requested;
  bool billboard_renderer_ready;
  bool raw_obj_planes;
  bool hud_handoff;
  bool hud_bg3;
  bool hud_obj;
  bool object_half_add;
  Sim3DCaptureStatus status;
  int width, height;
  int live_x0, live_x1;
  int hud_obj_priority;
  int hud_obj_mask_width;
  int hud_obj_mask_x0, hud_obj_mask_y0;
  int hud_obj_mask_x1, hud_obj_mask_y1;
  Ppu *ppu;
  PpuOverlayCapture prior_captures[kPpuOverlaySource_Count];
  uint8_t *hud_bg_pixels;
  uint8_t *hud_obj_pixels;
  uint32_t hud_bg_pitch;
  uint32_t hud_obj_pitch;
  uint32_t backdrop_argb;
  /* Colour-math evidence for the view-transition log; see SimFrameData. */
  uint8_t cgwsel, cgadsub;
  uint16_t fixed_color;
  uint8_t screen_main, screen_sub, brightness;
  /* Resolved fixed-colour add: the 5-bit components and the cgadsub layer
   * mask they apply to. Zero mask means the frame has no colour math to
   * reproduce, which is the overwhelmingly common case. */
  uint8_t fixed_add_r, fixed_add_g, fixed_add_b, fixed_add_mask;
  /* The PPU's 5-bit-to-8-bit brightness table, copied at gate time. `ppu` is
   * only retained when the HUD handoff runs, so reading it later is not an
   * option; 32 bytes settles the lifetime question outright. */
  uint8_t brightness_mult[32];
  uint32_t diagnostic_layer_mask;
  uint32_t captured_plane_mask;
  uint32_t mismatch_pixels;
  uint64_t separated_hash;
} Sim3DCaptureState;

static Sim3DCaptureState g_sim3d;
static uint32_t g_sim3d_hud_obj_mask[kSim3DMaxWidth * kSim3DMaxHeight];

static bool DemoArtifactsArmable(void);

enum {
  kSim3DAllPlaneMask = (1u << kSim3DPlane_Count) - 1u,
};

int Sim3D_ObjPlaneForPriority(int priority) {
  static const int planes[] = {
    kSim3DPlane_Obj0, kSim3DPlane_Obj1,
    kSim3DPlane_Obj2, kSim3DPlane_Obj3,
  };
  return priority >= 0 && priority < 4 ? planes[priority] : -1;
}

static uint32_t Sim3D_ObjPlaneMask(void) {
  uint32_t mask = 0;
  for (int priority = 0; priority < 4; priority++)
    mask |= 1u << Sim3D_ObjPlaneForPriority(priority);
  return mask;
}

uint32_t Sim3D_PlaneTextureUploadMask(
    SimRenderFeatureMask effective_features, uint32_t captured_plane_mask) {
  if (!(effective_features & kSimFeature_GroundProjection)) return 0;
  uint32_t mask = captured_plane_mask & kSim3DAllPlaneMask;
  if (effective_features & kSimFeature_ObjectBillboards)
    mask &= ~Sim3D_ObjPlaneMask();
  return mask;
}

/* Shared with actraiser_rtl.c's margin-gap fill (Fix C, SPEC-backdrop-clip.md)
 * rather than duplicated there: both want "the colour the authentic renderer
 * shows for an unrendered pixel", and two copies of the 5-bit expansion would
 * be free to drift. Declared in sim3d.h. */
uint32_t ActRaiser_BackdropArgb(const Ppu *ppu) {
  uint32_t color = ppu->cgram[0];
  int brightness = PPU_brightness(ppu);
  return 0xff000000u |
      (uint32_t)ExpandColor5(color & 0x1f, brightness) << 16 |
      (uint32_t)ExpandColor5((color >> 5) & 0x1f, brightness) << 8 |
      ExpandColor5((color >> 10) & 0x1f, brightness);
}

uint32_t ActRaiser_BackdropArgbFullBrightness(const Ppu *ppu) {
  uint32_t color = ppu->cgram[0];
  return 0xff000000u |
      (uint32_t)ExpandColor5(color & 0x1f, 15) << 16 |
      (uint32_t)ExpandColor5((color >> 5) & 0x1f, 15) << 8 |
      ExpandColor5((color >> 10) & 0x1f, 15);
}

static bool AllocatePlanes(uint32_t plane_mask) {
  const size_t bytes =
      (size_t)kSim3DMaxWidth * kSim3DMaxHeight * sizeof(uint32_t);
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!(plane_mask & (1u << plane))) continue;
    if (!g_sim3d_layer_pixels[plane])
      g_sim3d_layer_pixels[plane] = calloc(1, bytes);
    if (!g_sim3d_layer_pixels[plane]) return false;
  }
  return true;
}

static bool CaptureIsEmpty(const PpuOverlayCapture *capture) {
  return capture->x1 <= capture->x0 && capture->y1 <= capture->y0 &&
      capture->oamCount == 0;
}

static bool StandardTownHudPolicyActive(const Ppu *ppu) {
  return ppu->wsHudSplitHeight == kActRaiserSimulationHudHeight &&
      ppu->wsHudLeftEnd == kActRaiserSimulationHudSplit &&
      ppu->wsHudRightStart == kActRaiserSimulationHudSplit &&
      ppu->wsHudPlayerRowY == kActRaiserSimulationHudHeight &&
      ppu->wsHudLeftOnlyY == kActRaiserSimulationHudHeight;
}

static bool StandardTownHudCapture(const Ppu *ppu, int source,
                                   const PpuOverlayCapture *capture) {
  if (!StandardTownHudPolicyActive(ppu) ||
      (source != kPpuOverlaySource_Bg3 &&
       source != kPpuOverlaySource_Obj) ||
      capture->x0 != 0 || capture->x1 != kActRaiserAuthenticWidth ||
      capture->y0 != 0 || capture->y1 != kActRaiserSimulationHudHeight ||
      capture->flags != kPpuOverlayFlag_RemoveFromGame)
    return false;
  if (source == kPpuOverlaySource_Bg3)
    return capture->oamCount == 0;
  /* The town menu consumes the leading OAM entries, so the fixed-screen
   * hourglass moves from its ordinary slots 0-3 (observed at 11-14 in
   * runs/20260810-231616 and runs/20260811-145909). The widescreen HUD
   * promoter already discovers that range from the complete sprite signature;
   * validate against the same per-frame fact here instead of accidentally
   * treating a fixed screen position as a fixed OAM allocation. */
  enum { kPpuOamSlots = kPpuOamWords / 2 };
  const int hourglass_first = ActRaiser_FindSimulationHourglass(
      ppu->oam, ppu->highOam, kPpuOamSlots);
  return hourglass_first >= 0 && capture->oamFirst == hourglass_first &&
      capture->oamCount == kActRaiserHudObjOamCount;
}

static bool OverlayPolicyConflicts(const Ppu *ppu) {
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    if (!CaptureIsEmpty(capture) &&
        !StandardTownHudCapture(ppu, source, capture))
      return true;
  }
  return false;
}

static void ClearPriorHudObjMask(void) {
  int width = g_sim3d.hud_obj_mask_width;
  int x0 = g_sim3d.hud_obj_mask_x0;
  int y0 = g_sim3d.hud_obj_mask_y0;
  int x1 = g_sim3d.hud_obj_mask_x1;
  int y1 = g_sim3d.hud_obj_mask_y1;
  if (width > 0 && x0 >= 0 && x1 <= width && x1 > x0 &&
      y0 >= 0 && y1 <= kSim3DMaxHeight && y1 > y0) {
    for (int y = y0; y < y1; y++)
      memset(&g_sim3d_hud_obj_mask[
                 (size_t)y * (size_t)width + (size_t)x0], 0,
             (size_t)(x1 - x0) * sizeof(g_sim3d_hud_obj_mask[0]));
  }
  g_sim3d.hud_obj_mask_x0 = g_sim3d.hud_obj_mask_y0 = 0;
  g_sim3d.hud_obj_mask_x1 = g_sim3d.hud_obj_mask_y1 = 0;
}

static bool PrepareHudHandoff(Ppu *ppu, int width) {
  const PpuOverlayCapture *bg3 =
      &ppu->overlayCaptures[kPpuOverlaySource_Bg3];
  const PpuOverlayCapture *obj =
      &ppu->overlayCaptures[kPpuOverlaySource_Obj];
  g_sim3d.hud_bg3 = !CaptureIsEmpty(bg3);
  g_sim3d.hud_obj = !CaptureIsEmpty(obj);
  g_sim3d.hud_handoff = g_sim3d.hud_bg3 || g_sim3d.hud_obj;
  if (!g_sim3d.hud_handoff) return true;

  g_sim3d.ppu = ppu;
  memcpy(g_sim3d.prior_captures, ppu->overlayCaptures,
         sizeof(g_sim3d.prior_captures));
  g_sim3d.hud_bg_pixels =
      ppu->overlayRenderBuffer[kPpuOverlaySource_Bg3];
  g_sim3d.hud_obj_pixels =
      ppu->overlayRenderBuffer[kPpuOverlaySource_Obj];
  g_sim3d.hud_bg_pitch = ppu->overlayRenderPitch[kPpuOverlaySource_Bg3];
  g_sim3d.hud_obj_pitch = ppu->overlayRenderPitch[kPpuOverlaySource_Obj];
  /* Only the prior frame's nontransparent raster can be dirty. Remember the
   * pitch it used because a display-mode change can alter `width` between
   * captures; clearing the entire 448x240 ceiling here used to write 430 KB
   * every enhanced town frame for a HUD icon that is at most 32x32 pixels. */
  ClearPriorHudObjMask();
  g_sim3d.hud_obj_mask_width = width;
  if (!g_sim3d.hud_obj) return true;

  int priority = (ppu->oam[obj->oamFirst * 2 + 1] >> 12) & 3;
  PpuObjRangeBounds bounds;
  if (!PpuGetObjRangeBounds(ppu, obj->oamFirst, obj->oamCount,
                            (uint8_t)priority, &bounds))
    return false;
  int raster_width = bounds.x1 - bounds.x0;
  int raster_height = bounds.y1 - bounds.y0;
  enum { kHudRasterLimit = 128 };
  if (raster_width <= 0 || raster_height <= 0 ||
      raster_width > kHudRasterLimit || raster_height > kHudRasterLimit)
    return false;
  uint32_t raster[kHudRasterLimit * kHudRasterLimit];
  if (!PpuRasterizeObjRange(
          ppu, obj->oamFirst, obj->oamCount, (uint8_t)priority,
          &bounds, raster, raster_width, raster_height,
          (size_t)raster_width * sizeof(uint32_t)))
    return false;

  int extra = (width - kActRaiserAuthenticWidth) / 2;
  int mask_x0 = width, mask_y0 = kSim3DMaxHeight;
  int mask_x1 = 0, mask_y1 = 0;
  for (int y = 0; y < raster_height; y++) {
    int screen_y = bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kSim3DMaxHeight) continue;
    for (int x = 0; x < raster_width; x++) {
      int texture_x = bounds.x0 + x + extra;
      if (texture_x < 0 || texture_x >= width) continue;
      uint32_t pixel = raster[
          (size_t)y * (size_t)raster_width + (size_t)x];
      if (!pixel) continue;
      g_sim3d_hud_obj_mask[
          (size_t)screen_y * (size_t)width + (size_t)texture_x] = pixel;
      if (texture_x < mask_x0) mask_x0 = texture_x;
      if (screen_y < mask_y0) mask_y0 = screen_y;
      if (texture_x + 1 > mask_x1) mask_x1 = texture_x + 1;
      if (screen_y + 1 > mask_y1) mask_y1 = screen_y + 1;
    }
  }
  if (mask_x1 > mask_x0 && mask_y1 > mask_y0) {
    g_sim3d.hud_obj_mask_x0 = mask_x0;
    g_sim3d.hud_obj_mask_y0 = mask_y0;
    g_sim3d.hud_obj_mask_x1 = mask_x1;
    g_sim3d.hud_obj_mask_y1 = mask_y1;
  }
  g_sim3d.hud_obj_priority = priority;
  return true;
}

bool Sim3D_BeginFrame(void) {
  bool restore_bindings = g_sim3d.bindings_owned;
  g_sim3d.active = false;
  g_sim3d.bindings_owned = false;
  g_sim3d.inspector_active = false;
  g_sim3d.separated_valid = false;
  g_sim3d.flat_stage_requested = false;
  g_sim3d.billboard_renderer_ready = false;
  g_sim3d.raw_obj_planes = false;
  g_sim3d.hud_handoff = false;
  g_sim3d.hud_bg3 = false;
  g_sim3d.hud_obj = false;
  g_sim3d.object_half_add = false;
  g_sim3d.ppu = NULL;
  g_sim3d.status = kSim3DCapture_Inactive;
  g_sim3d.mismatch_pixels = 0;
  g_sim3d.separated_hash = 0;
  g_sim3d.captured_plane_mask = 0;
  return restore_bindings;
}

bool Sim3D_PrepareCapture(Ppu *ppu, const Sim3DCaptureRequest *request) {
  if (!ppu || !request || !request->town) return false;
  /* Retained for FinishCapture, which is where the diagnostic-only passes are
   * decided and which does not see the request. */
  g_sim3d.inspector_active = request->inspector_active;
  if (!request->master_enabled) {
    g_sim3d.status = kSim3DCapture_MasterOff;
    return false;
  }
  if (!(request->requested_features & kSimFeature_SeparatedComposite)) {
    g_sim3d.status = kSim3DCapture_NotRequested;
    return false;
  }
  /* The flat texture is a real presentation dependency only when the
   * perspective ground stage is disabled. Retain that request here because
   * FinishCapture runs after scanout and no longer has the settings payload. */
  g_sim3d.flat_stage_requested =
      !(request->requested_features & kSimFeature_GroundProjection);
#if AR_SIM3D_PICKER_TOPDOWN
  if (request->picker_active) {
    g_sim3d.status = kSim3DCapture_Picker;
    return false;
  }
#else
  /* The picker keeps the enhanced capture; see AR_SIM3D_PICKER_TOPDOWN. The
   * request field is still produced so diagnostics and the frame payload keep
   * reporting the live picker state. */
  (void)request->picker_active;
#endif
  if (!request->renderer_ready) {
    g_sim3d.status = kSim3DCapture_NoRenderer;
    return false;
  }
  if (request->diorama_active || OverlayPolicyConflicts(ppu)) {
    g_sim3d.status = kSim3DCapture_OverlayConflict;
    return false;
  }
  /* Recorded before any gate can reject, so a rejection reports the state
   * that caused it rather than the last state that passed. */
  g_sim3d.cgwsel = (uint8_t)ppu->cgwsel;
  g_sim3d.cgadsub = (uint8_t)ppu->cgadsub;
  g_sim3d.fixed_color = (uint16_t)ppu->fixedColor;
  g_sim3d.screen_main = (uint8_t)ppu->screenEnabled[0];
  g_sim3d.screen_sub = (uint8_t)ppu->screenEnabled[1];
  g_sim3d.brightness = (uint8_t)PPU_brightness(ppu);

  bool ordinary_screen =
      (ppu->screenEnabled[0] == 0x15 || ppu->screenEnabled[0] == 0x17) &&
      ppu->screenEnabled[1] == 0;
  bool targeted_miracle_screen = ppu->screenEnabled[0] == 0x15 &&
      ppu->screenEnabled[1] == 0x01;
  if (request->width < kPpuXPixels || request->width > kSim3DMaxWidth ||
      request->height <= 0 || request->height > kSim3DMaxHeight ||
      ppu->bgmode != 9 || PPU_forcedBlank(ppu) ||
      (!ordinary_screen && !targeted_miracle_screen)) {
    g_sim3d.status = kSim3DCapture_UnsupportedPpu;
    return false;
  }
  /* With fixed color zero, no half/subtract, no subscreen, and no color
   * window clipping, the PPU's fast path proves color math is a no-op. */
  bool no_op_color_math = ppu->cgwsel == 0 && ppu->fixedColor == 0 &&
      (ppu->cgadsub & 0xc0) == 0;

  /* The sun miracle: a plain fixed-colour add, ramping, onto BG1 alone
   * (measured cgwsel=$00 cgadsub=$01 fixed=$0001 screen=$15/$00). cgwsel zero
   * is what makes it reproducible -- fixed colour rather than subscreen as the
   * math source, math enabled over the whole screen, no main-screen-black
   * region and no direct colour, so there is no window geometry to recover.
   *
   * Restricted to full brightness, conservatively rather than necessarily:
   * the reproduction inverts the PPU's own brightness table to recover the
   * 5-bit value it must add to, and while that table turns out to stay
   * injective well below 15, only 15 has been verified against hardware
   * output. A miracle running under a screen fade is a second effect on top
   * of this one and deserves its own evidence before being admitted. */
  bool fixed_color_add = ppu->cgwsel == 0 && (ppu->cgadsub & 0xc0) == 0 &&
      ppu->fixedColor != 0 && (ppu->cgadsub & 0x3f) != 0 &&
      PPU_brightness(ppu) == 15;
  /* Targeted miracles keep BG1 on the subscreen and half-add it beneath OBJ.
   * BG1 + the identical BG1 subscreen is unchanged; OBJ palette groups 4-7
   * become a 50/50 mix with BG1 while groups 0-3 (including the selector)
   * stay opaque. Restrict this to the exact full-brightness state observed in
   * all five recorded miracle confirmations so unknown math still fails
   * closed. */
  bool targeted_miracle_half_add = targeted_miracle_screen &&
      ppu->cgwsel == 0x02 && ppu->cgadsub == 0x51 &&
      ppu->fixedColor == 0 && PPU_brightness(ppu) == 15;
  if ((!targeted_miracle_screen && !no_op_color_math && !fixed_color_add) ||
      (targeted_miracle_screen && !targeted_miracle_half_add)) {
    g_sim3d.status = kSim3DCapture_UnsupportedColorMath;
    return false;
  }
  g_sim3d.object_half_add = targeted_miracle_half_add;
  g_sim3d.fixed_add_mask = fixed_color_add ? (ppu->cgadsub & 0x3f) : 0;
  g_sim3d.fixed_add_r = (uint8_t)PPU_fixedColorR(ppu);
  g_sim3d.fixed_add_g = (uint8_t)PPU_fixedColorG(ppu);
  g_sim3d.fixed_add_b = (uint8_t)PPU_fixedColorB(ppu);
  memcpy(g_sim3d.brightness_mult, ppu->brightnessMult,
         sizeof(g_sim3d.brightness_mult));
  /* Raw OBJ is redundant only when this exact frame has both a complete atlas
   * and a renderer that can consume it. Diagnostics retain the raw planes so
   * the inspector, D1 hash and one-shot D2 dump remain exact ten-plane views.
   * Any failed prerequisite falls back before scanout, never after stale
   * surfaces could already have been selected. */
  bool diagnostics_armed = request->inspector_active ||
      DemoArtifactsArmable() || SimRenderMetadata_TraceArmed();
  bool billboards_requested =
      (request->requested_features & kSimFeature_GroundProjection) &&
      (request->requested_features & kSimFeature_ObjectBillboards);
  g_sim3d.billboard_renderer_ready = request->billboard_renderer_ready;
  g_sim3d.raw_obj_planes = diagnostics_armed || !billboards_requested ||
      !request->billboard_atlas_ready || !request->billboard_renderer_ready;
  uint32_t captured_plane_mask = kSim3DAllPlaneMask;
  if (!g_sim3d.raw_obj_planes)
    captured_plane_mask &= ~Sim3D_ObjPlaneMask();
  if (!AllocatePlanes(captured_plane_mask)) {
    g_sim3d.status = kSim3DCapture_AllocationFailure;
    return false;
  }
  if (!PrepareHudHandoff(ppu, request->width)) {
    g_sim3d.status = kSim3DCapture_OverlayConflict;
    return false;
  }

  const size_t pitch = (size_t)request->width * sizeof(uint32_t);
  /* Binding a primary surface replaces that source's complete priority-band
   * family. Remember ownership before the first mutation so a partial bind or
   * capture-policy failure is still repaired at the start of the next frame. */
  g_sim3d.bindings_owned = true;
  bool ok = true;
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg1,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg1Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg1, 1,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg1High]);
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg2,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg2Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg2, 1,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg2High]);
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg3,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg3Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg3, 1,
      (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg3High]);
  if (g_sim3d.raw_obj_planes) {
    ok &= PpuBindOverlaySurface(
        ppu, kPpuOverlaySource_Obj,
        (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Obj0], pitch);
    ok &= PpuBindOverlayPrioSurface(
        ppu, kPpuOverlaySource_Obj, 1,
        (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Obj1]);
    ok &= PpuBindOverlayPrioSurface(
        ppu, kPpuOverlaySource_Obj, 2,
        (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Obj2]);
    ok &= PpuBindOverlayPrioSurface(
        ppu, kPpuOverlaySource_Obj, 3,
        (uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Obj3]);
  } else {
    /* This also clears the source's capture and priority bindings, so the PPU
     * skips its isolated OBJ scratch setup, four row clears and resolve pass. */
    ok &= PpuBindOverlaySurface(
        ppu, kPpuOverlaySource_Obj, NULL, 0);
  }

  int extra = (request->width - kPpuXPixels) / 2;
  for (int source = kPpuOverlaySource_Bg1;
       ok && source <= kPpuOverlaySource_Bg3; source++) {
    ok &= PpuSetOverlayCapture(ppu, (PpuOverlaySource)source,
                               -extra, 0, request->width, request->height, 0);
  }
  if (ok && g_sim3d.raw_obj_planes) {
    ok &= PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj,
                               -extra, 0, request->width, request->height,
                               targeted_miracle_half_add
                                   ? kPpuOverlayFlag_MarkObjColorMath : 0);
    ok &= PpuSetOverlayOamRange(ppu, 0, 128);
  }
  if (!ok) {
    g_sim3d.status = kSim3DCapture_AllocationFailure;
    return false;
  }

  g_sim3d.active = true;
  g_sim3d.status = kSim3DCapture_Capturing;
  g_sim3d.width = request->width;
  g_sim3d.height = request->height;
  g_sim3d.live_x0 = extra - ppu->extraLeftCur;
  g_sim3d.live_x1 = extra + kPpuXPixels + ppu->extraRightCur;
  g_sim3d.backdrop_argb = ActRaiser_BackdropArgb(ppu);
  g_sim3d.diagnostic_layer_mask = request->diagnostic_layer_mask;
  g_sim3d.captured_plane_mask = captured_plane_mask;
  return true;
}

static bool IsObjPlane(int plane) {
  for (int priority = 0; priority < 4; priority++)
    if (plane == Sim3D_ObjPlaneForPriority(priority)) return true;
  return false;
}

static uint32_t HalfAddRgb(uint32_t foreground, uint32_t background) {
  uint32_t result = 0xff000000u;
  for (int shift = 0; shift <= 16; shift += 8) {
    uint32_t a = (foreground >> shift) & 0xff;
    uint32_t b = (background >> shift) & 0xff;
    uint32_t value5 = ((a >> 3) + (b >> 3)) >> 1;
    uint32_t expanded = (value5 << 3) | (value5 >> 2);
    result |= expanded << shift;
  }
  return result;
}

static bool ObjPixelUsesColorMath(uint32_t pixel) {
  return (pixel >> 24) == 0x80;
}

static uint32_t OpaqueArgb(uint32_t pixel) {
  return pixel | 0xff000000u;
}

/* `full_width_rows` are the top rows the promoted town HUD spans edge to edge;
 * below them, columns outside the live area belong to no layer and the
 * hardware leaves them black. */
static void ComposeFlatPixelsPolicy(
    uint32_t *dst, int width, int height, int pitch,
    uint32_t backdrop_argb, int live_x0, int live_x1,
    uint32_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    bool object_half_add, int full_width_rows) {
  if (!dst || width <= 0 || height <= 0 ||
      (size_t)width > INT_MAX / sizeof(*dst) ||
      pitch < width * (int)sizeof(*dst) ||
      pitch % (int)sizeof(*dst) != 0)
    return;
  if (live_x0 < 0) live_x0 = 0;
  if (live_x1 > width) live_x1 = width;
  if (live_x1 < live_x0) live_x1 = live_x0;
  uint32_t enabled = plane_mask ? plane_mask : (1u << kSim3DPlane_Count) - 1;

  for (int y = 0; y < height; y++) {
    uint32_t *out = dst +
        (size_t)y * (size_t)(pitch / (int)sizeof(*dst));
    for (int x = 0; x < width; x++)
      out[x] = x >= live_x0 && x < live_x1 ? backdrop_argb : 0xff000000u;
    /* An OBJ whose X has wrapped negative still rasterizes into the margin
     * columns, but when the camera sits at a map edge the live margin has
     * collapsed and the hardware shows black there. Compositing those pixels
     * anyway cost exactly the 4-8 px that failed the D2 equality gate, and
     * with it a one-frame drop to the authentic view every time an actor left
     * the screen sideways. */
    bool clip_to_live = y >= full_width_rows;
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      if (!(enabled & (1u << plane)) || !planes[plane]) continue;
      const uint32_t *src = planes[plane] + (size_t)y * (size_t)width;
      for (int x = 0; x < width; x++) {
        if (!(src[x] >> 24)) continue;
        if (clip_to_live && (x < live_x0 || x >= live_x1)) continue;
        if (object_half_add && IsObjPlane(plane) &&
            ObjPixelUsesColorMath(src[x])) {
          size_t index =
              (size_t)y * (size_t)width + (size_t)x;
          uint32_t bg1_high = planes[kSim3DPlane_Bg1High][index];
          uint32_t bg1_low = planes[kSim3DPlane_Bg1Low][index];
          uint32_t subscreen = bg1_high ? bg1_high : bg1_low;
          out[x] = subscreen
              ? HalfAddRgb(OpaqueArgb(src[x]), subscreen)
              : OpaqueArgb(src[x]);
        } else {
          out[x] = OpaqueArgb(src[x]);
        }
      }
    }
  }
}

void Sim3D_ComposeFlatPixels(
    uint32_t *dst, int width, int height, int pitch,
    uint32_t backdrop_argb, int live_x0, int live_x1,
    uint32_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    int full_width_rows) {
  ComposeFlatPixelsPolicy(dst, width, height, pitch, backdrop_argb,
                          live_x0, live_x1, planes, plane_mask, false,
                          full_width_rows);
}

static bool WritePpm(const char *path, const uint8_t *pixels,
                     int width, int height, int pitch) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
  for (int y = 0; ok && y < height; y++) {
    const uint8_t *row = pixels + (size_t)y * (size_t)pitch;
    for (int x = 0; x < width; x++) {
      uint32_t color;
      memcpy(&color, row + (size_t)x * sizeof(color), sizeof(color));
      ok &= fputc((color >> 16) & 0xff, file) != EOF;
      ok &= fputc((color >> 8) & 0xff, file) != EOF;
      ok &= fputc(color & 0xff, file) != EOF;
    }
  }
  ok &= fclose(file) == 0;
  return ok;
}

/* D2's deterministic checkpoint asks for one same-frame A/B/difference
 * triplet. This path is dormant unless the runner supplies an absolute
 * prefix; it does not participate in ordinary screenshots or presentation.
 *
 * At file scope rather than inside MaybeDumpDemoArtifacts because
 * Sim3D_FinishCapture has to know whether a dump could still fire BEFORE it
 * decides to build the difference image the dump would write. */
static struct {
  bool initialized;
  bool attempted;
  bool on_mismatch;
  unsigned target_game_frame;
  char prefix[768];
} g_demo_dump;

/* "A dump could still fire", not "fires this frame": the on-mismatch trigger
 * is undecidable until BuildDifference has produced the count, and the
 * frame-number trigger needs the image already built by the time it fires. So
 * the difference image is maintained for as long as the one-shot is unspent. */
static bool DemoArtifactsArmable(void) {
  if (!g_demo_dump.initialized) {
    g_demo_dump.initialized = true;
    const char *value = getenv("AR_SIM3D_D2_DUMP_PREFIX");
    if (value)
      snprintf(g_demo_dump.prefix, sizeof(g_demo_dump.prefix), "%s", value);
    value = getenv("AR_SIM3D_D2_DUMP_AT_GF");
    g_demo_dump.target_game_frame = value && value[0]
        ? (unsigned)strtoul(value, NULL, 0) : 0;
    /* A fidelity failure cannot be dumped by frame number, because nobody
     * knows the number until after it happens. Arming this instead captures
     * the first frame that actually mismatches, which is the only frame worth
     * looking at. */
    value = getenv("AR_SIM3D_DUMP_ON_MISMATCH");
    g_demo_dump.on_mismatch = value && value[0] && value[0] != '0';
  }
  return !g_demo_dump.attempted && g_demo_dump.prefix[0] != '\0';
}

static void MaybeDumpDemoArtifacts(const uint8_t *authentic_pixels,
                                   int authentic_pitch,
                                   uint16_t game_frame) {
  /* Armable first: it owns the one-time environment read, so the trigger
   * fields below are not yet populated on the very first call. */
  if (!DemoArtifactsArmable()) return;
  const char *prefix = g_demo_dump.prefix;
  bool triggered = g_demo_dump.on_mismatch
      ? g_sim3d.mismatch_pixels > 0
      : game_frame >= g_demo_dump.target_game_frame;
  if (!triggered ||
      (g_sim3d.status != kSim3DCapture_Capturing &&
       g_sim3d.status != kSim3DCapture_Ready))
    return;
  g_demo_dump.attempted = true;

  char path_a[kHostPathCapacity], path_b[kHostPathCapacity],
      path_difference[kHostPathCapacity], path_json[kHostPathCapacity];
  snprintf(path_a, sizeof(path_a), "%s-A.ppm", prefix);
  snprintf(path_b, sizeof(path_b), "%s-B.ppm", prefix);
  snprintf(path_difference, sizeof(path_difference), "%s-difference.ppm",
           prefix);
  snprintf(path_json, sizeof(path_json), "%s.json", prefix);
  bool ok = WritePpm(path_a, authentic_pixels, g_sim3d.width,
                     g_sim3d.height, authentic_pitch) &&
      WritePpm(path_b, (const uint8_t *)g_sim3d_flat_pixels,
               g_sim3d.width, g_sim3d.height, g_sim3d.width * 4) &&
      WritePpm(path_difference,
               (const uint8_t *)g_sim3d_difference_pixels,
               g_sim3d.width, g_sim3d.height, g_sim3d.width * 4);
  static const char *const plane_names[kSim3DPlane_Count] = {
    "bg3-low", "obj0", "obj1", "bg2-low", "bg1-low",
    "obj2", "bg2-high", "bg1-high", "obj3", "bg3-high",
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    char plane_path[kHostPathCapacity];
    snprintf(plane_path, sizeof(plane_path), "%s-plane-%02d-%s.ppm",
             prefix, plane, plane_names[plane]);
    ok &= WritePpm(plane_path, (const uint8_t *)g_sim3d_layer_pixels[plane],
                   g_sim3d.width, g_sim3d.height, g_sim3d.width * 4);
  }
  FILE *metadata = fopen(path_json, "w");
  if (metadata) {
    ok &= fprintf(metadata,
                  "{\"schema\":\"actraiser-sim3d-d2-flat-v1\","
                  "\"game_frame\":%u,\"width\":%d,\"height\":%d,"
                  "\"mismatch_pixels\":%u,"
                  "\"separated_hash\":\"%016llx\"}\n",
                  (unsigned)game_frame, g_sim3d.width, g_sim3d.height,
                  (unsigned)g_sim3d.mismatch_pixels,
                  (unsigned long long)g_sim3d.separated_hash) > 0;
    ok &= fclose(metadata) == 0;
  } else {
    ok = false;
  }
  fprintf(stderr, "[sim3d-d2] demo artifacts %s at gf=%u -> %s-*\n",
          ok ? "written" : "failed", (unsigned)game_frame, prefix);
}

/* The mismatch count and image are diagnostic-only now that pixel differences
 * do not veto a frame. `write_image` is reserved for artifact dumps. */
static uint32_t BuildDifference(const uint8_t *authentic_pixels,
                                int authentic_pitch, bool write_image,
                                uint64_t *out_flat_hash) {
  uint32_t mismatch = 0;
  uint64_t hash = UINT64_C(1469598103934665603);
  for (int y = 0; y < g_sim3d.height; y++) {
    const uint8_t *authentic =
        authentic_pixels + (size_t)y * (size_t)authentic_pitch;
    const uint32_t *flat =
        &g_sim3d_flat_pixels[(size_t)y * (size_t)g_sim3d.width];
    uint32_t *difference = write_image
        ? &g_sim3d_difference_pixels[
              (size_t)y * (size_t)g_sim3d.width] : NULL;
    for (int x = 0; x < g_sim3d.width; x++) {
      uint32_t a;
      memcpy(&a, authentic + (size_t)x * sizeof(a), sizeof(a));
      uint32_t b = flat[x];
      if (out_flat_hash) {
        uint32_t color = b & 0xffffff;
        for (int byte = 0; byte < 3; byte++) {
          hash = DeterministicHash_Fnv1a64Byte(
              hash, (uint8_t)(color & 0xff));
          color >>= 8;
        }
      }
      int ar = a >> 16 & 0xff, ag = a >> 8 & 0xff, ab = a & 0xff;
      int br = b >> 16 & 0xff, bg = b >> 8 & 0xff, bb = b & 0xff;
      int dr = ar > br ? ar - br : br - ar;
      int dg = ag > bg ? ag - bg : bg - ag;
      int db = ab > bb ? ab - bb : bb - ab;
      if (difference)
        difference[x] = 0xff000000u | (uint32_t)dr << 16 |
                        (uint32_t)dg << 8 | (uint32_t)db;
      if (dr || dg || db) mismatch++;
    }
  }
  if (out_flat_hash) *out_flat_hash = hash;
  return mismatch;
}

static uint32_t ComposeTownPixelWithoutHud(int x, int y,
                                           bool skip_bg3, bool skip_obj) {
  bool live = x >= g_sim3d.live_x0 && x < g_sim3d.live_x1;
  bool hud_composite_span = g_sim3d.hud_bg3 &&
      y < g_sim3d.prior_captures[kPpuOverlaySource_Bg3].y1;
  uint32_t color = (live || hud_composite_span)
      ? g_sim3d.backdrop_argb : 0xff000000u;

  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!(g_sim3d.captured_plane_mask & (1u << plane))) continue;
    if (skip_bg3 &&
        (plane == kSim3DPlane_Bg3Low || plane == kSim3DPlane_Bg3High))
      continue;
    if (skip_obj &&
        plane == Sim3D_ObjPlaneForPriority(g_sim3d.hud_obj_priority))
      continue;
    /* Same live-area rule the composed side uses: a wrapped-negative OBJ
     * rasterizes into margin columns the hardware leaves black. */
    if (!live && !hud_composite_span) continue;
    size_t index =
        (size_t)y * (size_t)g_sim3d.width + (size_t)x;
    uint32_t pixel = g_sim3d_layer_pixels[plane][index];
    if (!(pixel >> 24)) continue;
    if (g_sim3d.object_half_add && IsObjPlane(plane) &&
        ObjPixelUsesColorMath(pixel)) {
      uint32_t bg1_high =
          g_sim3d_layer_pixels[kSim3DPlane_Bg1High][index];
      uint32_t bg1_low =
          g_sim3d_layer_pixels[kSim3DPlane_Bg1Low][index];
      uint32_t subscreen = bg1_high ? bg1_high : bg1_low;
      color = subscreen
          ? HalfAddRgb(OpaqueArgb(pixel), subscreen)
          : OpaqueArgb(pixel);
    } else {
      color = OpaqueArgb(pixel);
    }
  }
  return color & 0x00ffffffu;
}

static void RestoreTownHudPixel(uint8_t *authentic_pixels,
                                int authentic_pitch, int x, int y,
                                bool skip_bg3, bool skip_obj) {
  size_t index =
      (size_t)y * (size_t)g_sim3d.width + (size_t)x;
  uint32_t color = ComposeTownPixelWithoutHud(x, y, skip_bg3, skip_obj);
  memcpy(authentic_pixels + (size_t)y * (size_t)authentic_pitch +
             (size_t)x * sizeof(color),
         &color, sizeof(color));
  g_sim3d_flat_pixels[index] = color | 0xff000000u;
  if (skip_bg3) {
    g_sim3d_layer_pixels[kSim3DPlane_Bg3Low][index] = 0;
    g_sim3d_layer_pixels[kSim3DPlane_Bg3High][index] = 0;
  }
  if (skip_obj) {
    int plane = Sim3D_ObjPlaneForPriority(g_sim3d.hud_obj_priority);
    if (g_sim3d.captured_plane_mask & (1u << plane))
      g_sim3d_layer_pixels[plane][index] = 0;
  }
}

static void RestoreTownHudPolicy(uint8_t *authentic_pixels,
                                 int authentic_pitch) {
  if (!g_sim3d.hud_handoff || !g_sim3d.ppu) return;

  /* Recreate the two standard host HUD surfaces that the full-plane capture
   * temporarily superseded. BG3's two priority bands are mutually exclusive
   * per source pixel; the exact OAM-range raster was prepared before scanout. */
  if (g_sim3d.hud_bg_pixels && g_sim3d.hud_bg_pitch) {
    memset(g_sim3d.hud_bg_pixels, 0,
           (size_t)g_sim3d.hud_bg_pitch * kActRaiserSimulationHudHeight);
    const PpuOverlayCapture *capture =
        &g_sim3d.prior_captures[kPpuOverlaySource_Bg3];
    for (int y = capture->y0; y < capture->y1; y++) {
      uint8_t *dst = g_sim3d.hud_bg_pixels +
          (size_t)y * (size_t)g_sim3d.hud_bg_pitch;
      for (int x = capture->x0; x < capture->x1; x++) {
        int texture_x = x + (g_sim3d.width - kActRaiserAuthenticWidth) / 2;
        size_t index = (size_t)y * (size_t)g_sim3d.width +
            (size_t)texture_x;
        uint32_t high =
            g_sim3d_layer_pixels[kSim3DPlane_Bg3High][index];
        uint32_t low =
            g_sim3d_layer_pixels[kSim3DPlane_Bg3Low][index];
        const uint32_t color = high ? high : low;
        memcpy(dst + (size_t)texture_x * sizeof(color),
               &color, sizeof(color));
      }
    }
  }
  if (g_sim3d.hud_obj_pixels && g_sim3d.hud_obj_pitch) {
    memset(g_sim3d.hud_obj_pixels, 0,
           (size_t)g_sim3d.hud_obj_pitch * kActRaiserSimulationHudHeight);
    for (int y = 0; y < kActRaiserSimulationHudHeight; y++) {
      uint8_t *dst = g_sim3d.hud_obj_pixels +
          (size_t)y * (size_t)g_sim3d.hud_obj_pitch;
      memcpy(dst, &g_sim3d_hud_obj_mask[
                      (size_t)y * (size_t)g_sim3d.width],
             (size_t)g_sim3d.width * sizeof(uint32_t));
    }
  }

  /* The full-plane capture does not use RemoveFromGame, so the PPU framebuffer
   * entering this function is already correct everywhere except the promoted
   * HUD pixels. Patch only the BG3 capture
   * rectangle and the sparse OBJ raster instead of recompositing all ten planes
   * over the entire frame, then scanning all ten planes over it a second time.
   *
   * Published SIM textures omit the HUD too; PresentSim3D adds the anchored
   * host overlay after either A/B profile has rendered. */
  int bg_x0 = 0, bg_y0 = 0, bg_x1 = 0, bg_y1 = 0;
  if (g_sim3d.hud_bg3) {
    const PpuOverlayCapture *capture =
        &g_sim3d.prior_captures[kPpuOverlaySource_Bg3];
    int extra = (g_sim3d.width - kActRaiserAuthenticWidth) / 2;
    bg_x0 = capture->x0 + extra;
    bg_x1 = capture->x1 + extra;
    bg_y0 = capture->y0;
    bg_y1 = capture->y1;
    if (bg_x0 < 0) bg_x0 = 0;
    if (bg_x1 > g_sim3d.width) bg_x1 = g_sim3d.width;
    if (bg_y0 < 0) bg_y0 = 0;
    if (bg_y1 > g_sim3d.height) bg_y1 = g_sim3d.height;
    for (int y = bg_y0; y < bg_y1; y++) {
      for (int x = bg_x0; x < bg_x1; x++) {
        bool skip_obj = g_sim3d.hud_obj &&
            g_sim3d_hud_obj_mask[
                (size_t)y * (size_t)g_sim3d.width + (size_t)x] != 0;
        RestoreTownHudPixel(authentic_pixels, authentic_pitch, x, y,
                            true, skip_obj);
      }
    }
  }

  if (g_sim3d.hud_obj) {
    int obj_y1 = g_sim3d.hud_obj_mask_y1;
    if (obj_y1 > g_sim3d.height) obj_y1 = g_sim3d.height;
    for (int y = g_sim3d.hud_obj_mask_y0;
         y < obj_y1; y++) {
      for (int x = g_sim3d.hud_obj_mask_x0;
           x < g_sim3d.hud_obj_mask_x1; x++) {
        if (!g_sim3d_hud_obj_mask[
                (size_t)y * (size_t)g_sim3d.width + (size_t)x])
          continue;
        if (x >= bg_x0 && x < bg_x1 && y >= bg_y0 && y < bg_y1)
          continue;  /* The BG3 loop patched the union once already. */
        RestoreTownHudPixel(authentic_pixels, authentic_pitch, x, y,
                            false, true);
      }
    }
  }

  memcpy(g_sim3d.ppu->overlayCaptures, g_sim3d.prior_captures,
         sizeof(g_sim3d.prior_captures));
}

/* Colour-math layer bit for a captured plane, matching CGADSUB's own layout
 * (bit 0 BG1 ... bit 4 OBJ, bit 5 backdrop). Mode 1 has no BG4, so bit 3 has
 * no plane to name. */
static uint8_t ColorMathLayerBit(int plane) {
  switch (plane) {
    case kSim3DPlane_Bg1Low: case kSim3DPlane_Bg1High: return 0x01;
    case kSim3DPlane_Bg2Low: case kSim3DPlane_Bg2High: return 0x02;
    case kSim3DPlane_Bg3Low: case kSim3DPlane_Bg3High: return 0x04;
    case kSim3DPlane_Obj0: case kSim3DPlane_Obj1:
    case kSim3DPlane_Obj2: case kSim3DPlane_Obj3: return 0x10;
    default: return 0;
  }
}

/* Applies the frame's fixed-colour add to every plane CGADSUB names.
 *
 * Done here, on the captured plane pixels, rather than in either compositor:
 * the authentic rebuild, the flat recomposition and the projected textures all
 * read these same buffers, so one application serves all three and diagnostic
 * D2 runs verify it against the real hardware output. Unlike the
 * targeted-miracle half-add -- which combines two planes and therefore has to
 * stay a compositing policy -- a fixed-colour add is a property of one layer
 * and belongs baked into it.
 *
 * The arithmetic is the part that matters. The PPU adds in 5-bit component
 * space and only then maps through `brightnessMult` to 8 bits, so adding the
 * expanded colour to the expanded pixel is NOT the same operation: it differs
 * on 168 of the 1024 (component, add) pairs, which byte-exact D2 diagnostics
 * expose. The 5-bit value is recovered by inverting that table (injective at
 * full brightness, which the capture gate requires), added with the hardware's
 * clamp at 31, and mapped forward again. */
static void ApplyFixedColorAdd(void) {
  if (!g_sim3d.fixed_add_mask) return;

  uint8_t inverse[256];
  memset(inverse, 0xFF, sizeof(inverse));
  for (int i = 0; i < 32; i++)
    inverse[g_sim3d.brightness_mult[i]] = (uint8_t)i;

  const uint8_t add[3] = {
    g_sim3d.fixed_add_r, g_sim3d.fixed_add_g, g_sim3d.fixed_add_b,
  };
  const int shift[3] = { 16, 8, 0 };  /* ARGB: red, green, blue */
  size_t count = (size_t)g_sim3d.width * (size_t)g_sim3d.height;

  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!(g_sim3d.captured_plane_mask & (1u << plane))) continue;
    uint8_t bit = ColorMathLayerBit(plane);
    if (!bit || !(g_sim3d.fixed_add_mask & bit) || !g_sim3d_layer_pixels[plane])
      continue;
    bool obj = IsObjPlane(plane);
    uint32_t *pixels = g_sim3d_layer_pixels[plane];
    for (size_t i = 0; i < count; i++) {
      uint32_t pixel = pixels[i];
      if (!(pixel >> 24)) continue;
      /* OBJ colour math is confined to palettes 4-7; the capture marks those
       * pixels for exactly this reason and the half-add path reads the same
       * marker. */
      if (obj && !ObjPixelUsesColorMath(pixel)) continue;
      uint32_t out = pixel & 0xFF000000u;
      for (int c = 0; c < 3; c++) {
        uint8_t byte = (uint8_t)(pixel >> shift[c]);
        uint8_t five = inverse[byte];
        /* A byte the table never produces cannot be a palette colour this
         * frame; leaving it be is safer than guessing, and D2 diagnostics will
         * notice if that assumption is ever wrong. */
        if (five == 0xFF) {
          out |= (uint32_t)byte << shift[c];
          continue;
        }
        int sum = five + add[c];
        if (sum > 31) sum = 31;
        out |= (uint32_t)g_sim3d.brightness_mult[sum] << shift[c];
      }
      pixels[i] = out;
    }
  }
}

/* With the gate no longer vetoing, a mismatch produces no view transition and
 * would otherwise be silent -- and `[sim3d-view]` was the whole first step of
 * the DEBUG.md playbook for this symptom. Report the edges only: mismatching
 * frames tend to arrive in runs, and one line per frame would bury the console
 * exactly when something is wrong. */
static void ReportFidelityChange(uint32_t mismatch, uint16_t game_frame) {
  static int previous = -1;  /* -1 unknown, 0 matching, 1 mismatching */
  int current = mismatch ? 1 : 0;
  if (current == previous) return;
  if (previous != -1 || current)
    fprintf(stderr,
            "[sim3d-d2] gf=%u fidelity %s (mismatch=%u px, view retained)\n",
            (unsigned)game_frame, current ? "lost" : "regained",
            (unsigned)mismatch);
  previous = current;
}

void Sim3D_FinishCapture(uint8_t *authentic_pixels,
                         int authentic_pitch, uint16_t game_frame) {
  if (!g_sim3d.active || !authentic_pixels ||
      authentic_pitch < g_sim3d.width * 4)
    return;

  /* Before anything reads the planes, including the authentic rebuild inside
   * RestoreTownHudPolicy and any armed diagnostic comparison. */
  ApplyFixedColorAdd();

  bool dump_armable = DemoArtifactsArmable();
  bool trace_armed = SimRenderMetadata_TraceArmed();
  bool diagnostics_armed =
      g_sim3d.inspector_active || dump_armable || trace_armed;

  /* The promoted HUD owns the full width of its own rows, so those rows keep
   * every captured pixel; the same span the authentic-side restore uses. */
  int hud_span_rows = g_sim3d.hud_bg3
      ? g_sim3d.prior_captures[kPpuOverlaySource_Bg3].y1 : 0;
  /* Ground projection consumes the individual planes directly. Building a
   * second full-frame CPU composite in that profile was pure dead work unless
   * a diagnostic reader needed its hash/difference. */
  if (g_sim3d.flat_stage_requested || diagnostics_armed)
    ComposeFlatPixelsPolicy(
        g_sim3d_flat_pixels, g_sim3d.width, g_sim3d.height,
        g_sim3d.width * (int)sizeof(uint32_t), g_sim3d.backdrop_argb,
        g_sim3d.live_x0, g_sim3d.live_x1, g_sim3d_layer_pixels,
        g_sim3d.captured_plane_mask,
        g_sim3d.object_half_add, hud_span_rows);
  uint32_t mismatch = 0;
  uint64_t flat_hash = 0;
  if (diagnostics_armed)
    mismatch = BuildDifference(authentic_pixels, authentic_pitch,
                               dump_armable, &flat_hash);
  g_sim3d.mismatch_pixels = mismatch;
  /* Difference count and composed-frame hash are diagnostics now that a pixel
   * mismatch no longer vetoes the enhanced frame. Checkpoints arm the D1 trace,
   * while the inspector and dump paths identify themselves explicitly. Fold the
   * hash into the diagnostic comparison so those modes scan the frame once; in
   * ordinary play neither value has a reader and both stay zero. */
  g_sim3d.separated_hash = flat_hash;
  /* A requested diagnostic dump is useful for failed fidelity gates too: it
   * preserves the authentic/composed/difference triplet that names which
   * pixels the recomposition got wrong. Since the gate stopped vetoing this is
   * the only way to see them, the frame itself having been kept. */
  MaybeDumpDemoArtifacts(authentic_pixels, authentic_pitch, game_frame);

  bool atlas_ready = SimRenderMetadata_AtlasReady();
  if (!atlas_ready && !g_sim3d.raw_obj_planes) {
    /* The atlas state is immutable between the pre-scanout decision and here,
     * so this is a broken producer contract rather than an ordinary fallback.
     * Fail closed instead of publishing a frame with neither sprite source. */
    g_sim3d.status = kSim3DCapture_AtlasInvalid;
    RestoreTownHudPolicy(authentic_pixels, authentic_pitch);
    return;
  }
  /* The D2 pixel gate measures but no longer vetoes (ledger §31). Dropping a
   * mismatching frame to the authentic composite traded a few wrong pixels for
   * a whole-screen perspective change, and a one-frame flat flash reads as a
   * far worse artifact than the mismatch it hides -- the same argument §24
   * already accepted for the object-metadata gate. Strictness belongs in the
   * checkpoints, where it is free: they still assert
   * separated_mismatch_pixels_total == 0, and the count and status below still
   * carry the failure into the D1 trace and the console. */
  g_sim3d.separated_valid = true;
  g_sim3d.status = !atlas_ready
      ? kSim3DCapture_AtlasInvalid
      : diagnostics_armed && mismatch
          ? kSim3DCapture_PixelMismatch : kSim3DCapture_Ready;
  if (diagnostics_armed) ReportFidelityChange(mismatch, game_frame);
  RestoreTownHudPolicy(authentic_pixels, authentic_pitch);

  /* A nonzero diagnostic mask intentionally produces an incomplete A1 while
   * retaining the full-composite equality result above as the safety gate. */
  if (g_sim3d.flat_stage_requested && g_sim3d.diagnostic_layer_mask) {
    ComposeFlatPixelsPolicy(
        g_sim3d_flat_pixels, g_sim3d.width, g_sim3d.height,
        g_sim3d.width * (int)sizeof(uint32_t), g_sim3d.backdrop_argb,
        g_sim3d.live_x0, g_sim3d.live_x1, g_sim3d_layer_pixels,
        g_sim3d.diagnostic_layer_mask, g_sim3d.object_half_add,
        hud_span_rows);
    /* The diagnostic dump above deliberately records the complete A/B image;
     * this masked rebuild is only the flat presentation selected on screen. */
  }
}

/* One line whenever the simulation presentation changes ownership or loses a
 * usable separated composite. `None` is a real observable state: resetting to
 * unknown there hid the 1-3 frame drops this diagnostic exists to explain.
 * Keep an authentic picker/dialogue fallback distinct from an enhanced view
 * whose capture failed, because only the latter points at the D1/D2 pipeline. */
void Sim3D_LogViewTransition(const SimFrameData *frame) {
  enum {
    kLoggedView_Unknown = -1,
    kLoggedView_None,
    kLoggedView_AuthenticFallback,
    kLoggedView_EnhancedInvalid,
    kLoggedView_Enhanced,
  };
  static int previous = kLoggedView_Unknown;
  static uint16_t previous_game_frame;
  if (!frame) return;
  int current = frame->view == kSimView_None
      ? kLoggedView_None
      : frame->view != kSimView_Enhanced
          ? kLoggedView_AuthenticFallback
          : frame->separated_valid
              ? kLoggedView_Enhanced : kLoggedView_EnhancedInvalid;
  if (current != previous) {
    static const char *const names[] = {
      "none", "authentic (view fallback)",
      "authentic (enhanced capture invalid)", "enhanced",
    };
    if (previous != kLoggedView_Unknown)
      fprintf(stderr,
              "[sim3d-view] gf=%u %s -> %s (%s, view=%s, integrity=$%X,"
              " mismatch=%u px) after %u frame(s)\n",
              (unsigned)frame->game_frame,
              names[previous], names[current],
              Sim3D_CaptureStatusName((Sim3DCaptureStatus)
                                      frame->separated_status),
              Sim3D_ViewName(frame->view),
              (unsigned)frame->integrity_flags,
              (unsigned)frame->separated_mismatch_pixels,
              (unsigned)(frame->game_frame - previous_game_frame));
    /* The registers only when they are the reason. Printing them on every
     * transition would bury the one line that matters under the ordinary
     * picker and dialogue flips. */
    if (previous != kLoggedView_Unknown &&
        frame->separated_status == kSim3DCapture_UnsupportedColorMath)
      fprintf(stderr,
              "[sim3d-view]   colour math: cgwsel=$%02X cgadsub=$%02X"
              " fixed=$%04X screen=$%02X/$%02X brightness=%u\n",
              frame->separated_cgwsel, frame->separated_cgadsub,
              (unsigned)frame->separated_fixed_color,
              frame->separated_screen_main, frame->separated_screen_sub,
              frame->separated_brightness);
    previous = current;
    previous_game_frame = frame->game_frame;
  }
}

void Sim3D_RenderTownCanvas(const SimFrameData *frame, const uint8 *wram,
                            const Ppu *ppu) {
  if (!frame || !frame->town) {
    /* Leaving a town drops the canvas: it is town-space, and the next town
     * would otherwise inherit this one's ground. */
    if (SimTownCanvas_Town()) SimTownCanvas_Reset();
    if (SimBackgroundVoxels_Serial()) SimBackgroundVoxels_Reset();
    return;
  }
  if (!ppu) return;
  /* Only render alongside a capture that actually composed. A pixel mismatch
   * no longer disqualifies the frame: the canvas has to follow whatever the
   * separated view does, or removing the D2 veto would just trade one
   * whole-screen flash for the ground extension popping in and out on the same
   * frames. The statuses that still block here are the ones where no usable
   * plane capture exists at all. */
  if (!g_sim3d.separated_valid ||
      (g_sim3d.status != kSim3DCapture_Ready &&
       g_sim3d.status != kSim3DCapture_PixelMismatch &&
       g_sim3d.status != kSim3DCapture_AtlasInvalid))
    return;
  SimTownCanvas_Render(frame->town, wram, ppu->vram, ppu->cgram,
                       PPU_brightness(ppu), g_sim3d.backdrop_argb);
  if (frame->background_voxel_enabled) {
    SimBackgroundVoxels_Build(frame->town, wram, SimTownCanvas_Pixels(),
                              ppu->vram, SimTownCanvas_Serial(),
                              SimTownCanvas_TilemapSerial(),
                              frame->background_voxel_wind_hold != 0);
  } else if (SimBackgroundVoxels_Serial()) {
    SimBackgroundVoxels_Reset();
  }
}

SimRenderFeatureMask Sim3D_ImplementedFeatures(void) {
  if (!g_sim3d.separated_valid) return 0;
  SimRenderFeatureMask features = kSim3DShippedFeatures;
  if (!g_sim3d.billboard_renderer_ready)
    features &= (SimRenderFeatureMask)~(
        (SimRenderFeatureMask)kSimFeature_ObjectBillboards);
  return features;
}

static int ClampTuning(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

void Sim3D_AnnotateFrame(SimFrameData *frame, const Sim3DTuning *tuning) {
  if (!frame || !tuning) return;
  int pitch_mrad = tuning->pitch_mrad;
  int yaw_mrad = tuning->yaw_mrad;
  int distance_x100 = tuning->distance_x100;
  frame->landscape_height_pct =
      (uint16_t)ClampTuning(
          tuning->landscape_height_pct,
          kSimTownTerrainLandscapeHeightMinimumPct,
          kSimTownTerrainLandscapeHeightMaximumPct);
  frame->height_scale_x100 =
      (uint16_t)ClampTuning(tuning->height_scale_x100, 0, 0xFFFF);
  SimBackgroundVoxelPresetConfig voxel_config =
      SimBackgroundVoxelPreset_Resolve(
          (SimBackgroundVoxelPreset)ClampTuning(
              tuning->voxel_preset, kSimBackgroundVoxelPreset_Off,
              kSimBackgroundVoxelPreset_Custom),
          (SimBackgroundVoxelPresetConfig){
            .enabled = true,
            .detail = (SimBackgroundVoxelDetail)ClampTuning(
                tuning->voxel_detail, kSimBackgroundVoxelDetail_Low,
                kSimBackgroundVoxelDetail_Ultra),
            .lod = (SimBackgroundVoxelLod)ClampTuning(
                tuning->voxel_lod, kSimBackgroundVoxelLod_Fixed,
                kSimBackgroundVoxelLod_Adaptive),
            .shading = (SimBackgroundVoxelShading)ClampTuning(
                tuning->voxel_shading, kSimBackgroundVoxelShading_Basic,
                kSimBackgroundVoxelShading_MaterialAware),
            .style = (SimBackgroundVoxelStyle)ClampTuning(
                tuning->voxel_style, kSimBackgroundVoxelStyle_Basic,
                kSimBackgroundVoxelStyle_Varied),
            .facing = (SimBackgroundVoxelFacing)ClampTuning(
                tuning->voxel_facing, kSimBackgroundVoxelFacing_Shared,
                kSimBackgroundVoxelFacing_PerModel),
            .render_scale = (SimBackgroundVoxelRenderScale)ClampTuning(
                tuning->voxel_render_scale,
                kSimBackgroundVoxelRenderScale_Native,
                kSimBackgroundVoxelRenderScale_PixelClean),
          });
  frame->background_voxel_enabled = voxel_config.enabled;
  frame->background_voxel_wind_hold =
      (uint8_t)(tuning->windmill_wind_stops_all ? 1 : 0);
  frame->background_voxel_detail = (uint8_t)voxel_config.detail;
  frame->background_voxel_lod = (uint8_t)voxel_config.lod;
  frame->background_voxel_shading = (uint8_t)voxel_config.shading;
  frame->background_voxel_style = (uint8_t)voxel_config.style;
  frame->background_voxel_facing = (uint8_t)voxel_config.facing;
  frame->background_voxel_render_scale =
      (uint8_t)voxel_config.render_scale;
  frame->shadow_opacity_pct =
      (uint8_t)ClampTuning(tuning->shadow_opacity_pct, 0, kPercentScale);
  frame->height_pop_pct =
      (uint8_t)ClampTuning(tuning->height_pop_pct, 0, 255);
  /* Azimuth wraps; elevation and softness clamp. */
  frame->light_azimuth_deg = (uint16_t)(((tuning->light_azimuth_deg % 360) +
                                         360) % 360);
  frame->light_elevation_deg =
      (uint8_t)ClampTuning(tuning->light_elevation_deg, 5, 90);
  frame->shadow_softness_pct =
      (uint8_t)ClampTuning(tuning->shadow_softness_pct, 0, kPercentScale);
  frame->rim_strength_pct =
      (uint8_t)ClampTuning(tuning->rim_strength_pct, 0, kPercentScale);
  frame->underlay_haze_pct =
      (uint8_t)ClampTuning(tuning->underlay_haze_pct, 0, kPercentScale);
  frame->cloud_opacity_pct =
      (uint8_t)ClampTuning(tuning->cloud_opacity_pct, 0, kPercentScale);
  frame->cloud_falloff_px =
      (uint16_t)ClampTuning(tuning->cloud_falloff_px, 1, 2048);
  frame->cloud_inset_px =
      (uint16_t)ClampTuning(tuning->cloud_inset_px, 0, 2048);
  /* The shroud clears exactly what the real-OAM plus synthetic part channels
   * can populate: the authentic window plus their shared extended margins,
   * expressed in captured-texture columns. Asking the producer rather than
   * re-deriving it means drawable reach and cover cannot drift apart. */
  {
    int screen_x0 = g_sim3d.width > kActRaiserAuthenticWidth
        ? (g_sim3d.width - kActRaiserAuthenticWidth) / 2 : 0;
    int clear_x0 = screen_x0 - tuning->sprite_margin_left;
    int clear_x1 = screen_x0 + kActRaiserAuthenticWidth +
        tuning->sprite_margin_right;
    frame->cloud_clear_x0 = (int16_t)clear_x0;
    frame->cloud_clear_x1 = (int16_t)clear_x1;
    frame->cloud_clear_y0 = (int16_t)-tuning->sprite_margin_top;
    frame->cloud_clear_y1 = (int16_t)(
        kActRaiserAuthenticHeight + tuning->sprite_margin_bottom);
  }
  /* The same margins unconverted, for the per-record cull predicate. That
   * evaluates in the emitter's biased coordinates, not captured-texture
   * columns, so it cannot use cloud_clear_*. */
  frame->sprite_margin_left = (int16_t)tuning->sprite_margin_left;
  frame->sprite_margin_right = (int16_t)tuning->sprite_margin_right;
  frame->sprite_margin_top = (int16_t)tuning->sprite_margin_top;
  frame->sprite_margin_bottom = (int16_t)tuning->sprite_margin_bottom;
  frame->cull_lead_px = (uint16_t)ClampTuning(tuning->cull_lead_px, 1, 512);
  frame->cull_haze_pct =
      (uint8_t)ClampTuning(tuning->cull_haze_pct, 0, kPercentScale);
  frame->cull_dim_pct =
      (uint8_t)ClampTuning(tuning->cull_dim_pct, 0, kPercentScale);
  frame->cull_haze_lead_px =
      (uint16_t)ClampTuning(tuning->cull_haze_lead_px, 1, 1024);
  frame->cull_corner_px =
      (uint16_t)ClampTuning(tuning->cull_corner_px, 0, 512);
  frame->underlay_defocus_pct =
      (uint8_t)ClampTuning(tuning->underlay_defocus_pct, 0, kPercentScale);
  frame->cloud_altitude_px =
      (uint16_t)ClampTuning(tuning->cloud_altitude_px, 0, 512);
  frame->cloud_drift_pct =
      (uint16_t)ClampTuning(tuning->cloud_drift_pct, 0, 500);
  frame->world_navigation_lighting =
      tuning->world_navigation_lighting ? 1 : 0;
  frame->world_navigation_clouds =
      tuning->world_navigation_clouds ? 1 : 0;
  frame->world_navigation_backdrop =
      tuning->world_navigation_backdrop ? 1 : 0;
  frame->world_navigation_haze =
      tuning->world_navigation_haze ? 1 : 0;
  frame->cull_lift_inset = tuning->cull_lift_inset ? 1 : 0;
  frame->backdrop_strength_pct =
      (uint8_t)ClampTuning(tuning->backdrop_strength_pct, 0, kPercentScale);
  frame->backdrop_horizon_pct =
      (uint8_t)ClampTuning(tuning->backdrop_horizon_pct, 0, kPercentScale);
  /* The capture is centred on the authentic 256-column window, so this is the
   * captured-texture column that holds SNES x = 0 — the offset the underlay
   * needs to turn a texture column into a town pixel. */
  frame->underlay_screen_x0 = g_sim3d.width > kActRaiserAuthenticWidth
      ? (uint16_t)((g_sim3d.width - kActRaiserAuthenticWidth) / 2) : 0;
  frame->separated_valid = g_sim3d.separated_valid;
  frame->separated_status = (uint8_t)g_sim3d.status;
  frame->separated_plane_mask =
      (uint16_t)g_sim3d.captured_plane_mask;
  frame->separated_mismatch_pixels = g_sim3d.mismatch_pixels;
  frame->separated_hash = g_sim3d.separated_hash;
  frame->separated_backdrop_argb = g_sim3d.backdrop_argb;
  frame->separated_cgwsel = g_sim3d.cgwsel;
  frame->separated_cgadsub = g_sim3d.cgadsub;
  frame->separated_fixed_color = g_sim3d.fixed_color;
  frame->separated_screen_main = g_sim3d.screen_main;
  frame->separated_screen_sub = g_sim3d.screen_sub;
  frame->separated_brightness = g_sim3d.brightness;
  frame->object_half_add = g_sim3d.object_half_add;
  /* These three settings own only the simulation-town perspective camera.
   * World navigation is driven by its captured affine scene and must not
   * appear to inherit a free/dynamic town pose merely because both views
   * share this tuning annotation pass. */
  if (frame->view == kSimView_WorldNavigation) {
    frame->projection_pitch_mrad = 0;
    frame->projection_yaw_mrad = 0;
    frame->projection_distance_x100 = 0;
  } else {
    frame->projection_pitch_mrad = (int16_t)pitch_mrad;
    frame->projection_yaw_mrad = (int16_t)yaw_mrad;
    frame->projection_distance_x100 = (uint16_t)distance_x100;
  }
}
