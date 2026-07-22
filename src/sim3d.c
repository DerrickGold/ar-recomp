#include "sim3d.h"

#include "sim_town_canvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "snes/ppu.h"

_Static_assert(kSim3DMaxWidth == kPpuBufWidth,
               "SIM capture width must match the PPU buffer ceiling");

uint8_t *g_sim3d_layer_pixels[kSim3DPlane_Count];
uint32_t g_sim3d_flat_pixels[kSim3DMaxWidth * kSim3DMaxHeight];
uint32_t g_sim3d_difference_pixels[kSim3DMaxWidth * kSim3DMaxHeight];

typedef struct Sim3DCaptureState {
  bool active;
  bool bindings_owned;
  bool separated_valid;
  bool hud_handoff;
  bool hud_bg3;
  bool hud_obj;
  bool object_half_add;
  Sim3DCaptureStatus status;
  int width, height;
  int live_x0, live_x1;
  int hud_obj_priority;
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
  uint32_t mismatch_pixels;
  uint64_t separated_hash;
} Sim3DCaptureState;

static Sim3DCaptureState g_sim3d;
static uint32_t g_sim3d_hud_obj_mask[kSim3DMaxWidth * kSim3DMaxHeight];

int Sim3D_ObjPlaneForPriority(int priority) {
  static const int planes[] = {
    kSim3DPlane_Obj0, kSim3DPlane_Obj1,
    kSim3DPlane_Obj2, kSim3DPlane_Obj3,
  };
  return priority >= 0 && priority < 4 ? planes[priority] : -1;
}

static uint8_t ExpandColor5(uint32_t value, int brightness) {
  uint32_t expanded = (value << 3) | (value >> 2);
  return (uint8_t)(expanded * (uint32_t)brightness / 15);
}

static uint32_t BackdropArgb(const Ppu *ppu) {
  uint32_t color = ppu->cgram[0];
  int brightness = PPU_brightness(ppu);
  return 0xff000000u |
      (uint32_t)ExpandColor5(color & 0x1f, brightness) << 16 |
      (uint32_t)ExpandColor5((color >> 5) & 0x1f, brightness) << 8 |
      ExpandColor5((color >> 10) & 0x1f, brightness);
}

static bool AllocatePlanes(void) {
  const size_t bytes =
      (size_t)kSim3DMaxWidth * kSim3DMaxHeight * sizeof(uint32_t);
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
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
  return capture->oamFirst == kActRaiserHudObjOamFirst &&
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
  memset(g_sim3d_hud_obj_mask, 0, sizeof(g_sim3d_hud_obj_mask));
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
  for (int y = 0; y < raster_height; y++) {
    int screen_y = bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kSim3DMaxHeight) continue;
    for (int x = 0; x < raster_width; x++) {
      int texture_x = bounds.x0 + x + extra;
      if (texture_x < 0 || texture_x >= width) continue;
      g_sim3d_hud_obj_mask[(size_t)screen_y * width + texture_x] =
          raster[(size_t)y * raster_width + x];
    }
  }
  g_sim3d.hud_obj_priority = priority;
  return true;
}

bool Sim3D_BeginFrame(void) {
  bool restore_bindings = g_sim3d.bindings_owned;
  g_sim3d.active = false;
  g_sim3d.bindings_owned = false;
  g_sim3d.separated_valid = false;
  g_sim3d.hud_handoff = false;
  g_sim3d.hud_bg3 = false;
  g_sim3d.hud_obj = false;
  g_sim3d.object_half_add = false;
  g_sim3d.ppu = NULL;
  g_sim3d.status = kSim3DCapture_Inactive;
  g_sim3d.mismatch_pixels = 0;
  g_sim3d.separated_hash = 0;
  return restore_bindings;
}

bool Sim3D_PrepareCapture(Ppu *ppu, const Sim3DCaptureRequest *request) {
  if (!ppu || !request || !request->town) return false;
  if (!request->master_enabled) {
    g_sim3d.status = kSim3DCapture_MasterOff;
    return false;
  }
  if (!(request->requested_features & kSimFeature_SeparatedComposite)) {
    g_sim3d.status = kSim3DCapture_NotRequested;
    return false;
  }
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
  if (!AllocatePlanes()) {
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
      g_sim3d_layer_pixels[kSim3DPlane_Bg1Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg1, 1,
      g_sim3d_layer_pixels[kSim3DPlane_Bg1High]);
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg2,
      g_sim3d_layer_pixels[kSim3DPlane_Bg2Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg2, 1,
      g_sim3d_layer_pixels[kSim3DPlane_Bg2High]);
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg3,
      g_sim3d_layer_pixels[kSim3DPlane_Bg3Low], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg3, 1,
      g_sim3d_layer_pixels[kSim3DPlane_Bg3High]);
  ok &= PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Obj,
      g_sim3d_layer_pixels[kSim3DPlane_Obj0], pitch);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Obj, 1,
      g_sim3d_layer_pixels[kSim3DPlane_Obj1]);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Obj, 2,
      g_sim3d_layer_pixels[kSim3DPlane_Obj2]);
  ok &= PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Obj, 3,
      g_sim3d_layer_pixels[kSim3DPlane_Obj3]);

  int extra = (request->width - kPpuXPixels) / 2;
  for (int source = kPpuOverlaySource_Bg1;
       ok && source <= kPpuOverlaySource_Bg3; source++) {
    ok &= PpuSetOverlayCapture(ppu, (PpuOverlaySource)source,
                               -extra, 0, request->width, request->height, 0);
  }
  if (ok) {
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
  g_sim3d.backdrop_argb = BackdropArgb(ppu);
  g_sim3d.diagnostic_layer_mask = request->diagnostic_layer_mask;
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
    uint8_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    bool object_half_add, int full_width_rows) {
  if (!dst || width <= 0 || height <= 0 || pitch < width * 4) return;
  if (live_x0 < 0) live_x0 = 0;
  if (live_x1 > width) live_x1 = width;
  if (live_x1 < live_x0) live_x1 = live_x0;
  uint32_t enabled = plane_mask ? plane_mask : (1u << kSim3DPlane_Count) - 1;

  for (int y = 0; y < height; y++) {
    uint32_t *out = (uint32_t *)((uint8_t *)dst + (size_t)y * pitch);
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
      const uint32_t *src = (const uint32_t *)(
          planes[plane] + (size_t)y * width * sizeof(uint32_t));
      for (int x = 0; x < width; x++) {
        if (!(src[x] >> 24)) continue;
        if (clip_to_live && (x < live_x0 || x >= live_x1)) continue;
        if (object_half_add && IsObjPlane(plane) &&
            ObjPixelUsesColorMath(src[x])) {
          size_t index = (size_t)y * width + x;
          uint32_t bg1_high = ((uint32_t *)
              planes[kSim3DPlane_Bg1High])[index];
          uint32_t bg1_low = ((uint32_t *)
              planes[kSim3DPlane_Bg1Low])[index];
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
    uint8_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    int full_width_rows) {
  ComposeFlatPixelsPolicy(dst, width, height, pitch, backdrop_argb,
                          live_x0, live_x1, planes, plane_mask, false,
                          full_width_rows);
}

static uint64_t HashRgb(const uint32_t *pixels, int width, int height,
                        int pitch) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (int y = 0; y < height; y++) {
    const uint32_t *row = (const uint32_t *)(
        (const uint8_t *)pixels + (size_t)y * pitch);
    for (int x = 0; x < width; x++) {
      uint32_t color = row[x] & 0xffffff;
      for (int byte = 0; byte < 3; byte++) {
        hash ^= color & 0xff;
        hash *= UINT64_C(1099511628211);
        color >>= 8;
      }
    }
  }
  return hash;
}

static bool WritePpm(const char *path, const uint8_t *pixels,
                     int width, int height, int pitch) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
  for (int y = 0; ok && y < height; y++) {
    const uint32_t *row = (const uint32_t *)(
        pixels + (size_t)y * pitch);
    for (int x = 0; x < width; x++) {
      uint32_t color = row[x];
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
 * prefix; it does not participate in ordinary screenshots or presentation. */
static void MaybeDumpDemoArtifacts(const uint8_t *authentic_pixels,
                                   int authentic_pitch,
                                   uint16_t game_frame) {
  static bool initialized;
  static bool attempted;
  static unsigned target_game_frame;
  static bool on_mismatch;
  static char prefix[768];
  if (!initialized) {
    initialized = true;
    const char *value = getenv("AR_SIM3D_D2_DUMP_PREFIX");
    if (value) snprintf(prefix, sizeof(prefix), "%s", value);
    value = getenv("AR_SIM3D_D2_DUMP_AT_GF");
    target_game_frame = value && value[0]
        ? (unsigned)strtoul(value, NULL, 0) : 0;
    /* A fidelity failure cannot be dumped by frame number, because nobody
     * knows the number until after it happens. Arming this instead captures
     * the first frame that actually mismatches, which is the only frame worth
     * looking at. */
    value = getenv("AR_SIM3D_DUMP_ON_MISMATCH");
    on_mismatch = value && value[0] && value[0] != '0';
  }
  bool triggered = on_mismatch ? g_sim3d.mismatch_pixels > 0
                               : game_frame >= target_game_frame;
  if (attempted || !prefix[0] || !triggered ||
      (g_sim3d.status != kSim3DCapture_Capturing &&
       g_sim3d.status != kSim3DCapture_Ready))
    return;
  attempted = true;

  char path_a[1024], path_b[1024], path_difference[1024], path_json[1024];
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
    char plane_path[1024];
    snprintf(plane_path, sizeof(plane_path), "%s-plane-%02d-%s.ppm",
             prefix, plane, plane_names[plane]);
    ok &= WritePpm(plane_path, g_sim3d_layer_pixels[plane],
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

static uint32_t BuildDifference(const uint8_t *authentic_pixels,
                                int authentic_pitch) {
  uint32_t mismatch = 0;
  for (int y = 0; y < g_sim3d.height; y++) {
    const uint32_t *authentic = (const uint32_t *)(
        authentic_pixels + (size_t)y * authentic_pitch);
    const uint32_t *flat =
        &g_sim3d_flat_pixels[(size_t)y * g_sim3d.width];
    uint32_t *difference =
        &g_sim3d_difference_pixels[(size_t)y * g_sim3d.width];
    for (int x = 0; x < g_sim3d.width; x++) {
      uint32_t a = authentic[x], b = flat[x];
      int ar = a >> 16 & 0xff, ag = a >> 8 & 0xff, ab = a & 0xff;
      int br = b >> 16 & 0xff, bg = b >> 8 & 0xff, bb = b & 0xff;
      int dr = ar > br ? ar - br : br - ar;
      int dg = ag > bg ? ag - bg : bg - ag;
      int db = ab > bb ? ab - bb : bb - ab;
      difference[x] = 0xff000000u | (uint32_t)dr << 16 |
                      (uint32_t)dg << 8 | (uint32_t)db;
      if (dr || dg || db) mismatch++;
    }
  }
  return mismatch;
}

static bool SkipHudPlanePixel(int plane, int x, int y) {
  int extra = (g_sim3d.width - kActRaiserAuthenticWidth) / 2;
  int screen_x = x - extra;
  if (g_sim3d.hud_bg3 &&
      (plane == kSim3DPlane_Bg3Low || plane == kSim3DPlane_Bg3High)) {
    const PpuOverlayCapture *capture =
        &g_sim3d.prior_captures[kPpuOverlaySource_Bg3];
    if (screen_x >= capture->x0 && screen_x < capture->x1 &&
        y >= capture->y0 && y < capture->y1)
      return true;
  }
  if (g_sim3d.hud_obj &&
      plane == Sim3D_ObjPlaneForPriority(g_sim3d.hud_obj_priority) &&
      g_sim3d_hud_obj_mask[(size_t)y * g_sim3d.width + x])
    return true;
  return false;
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
      uint32_t *dst = (uint32_t *)(g_sim3d.hud_bg_pixels +
                                   (size_t)y * g_sim3d.hud_bg_pitch);
      for (int x = capture->x0; x < capture->x1; x++) {
        int texture_x = x + (g_sim3d.width - kActRaiserAuthenticWidth) / 2;
        size_t index = (size_t)y * g_sim3d.width + texture_x;
        uint32_t high =
            ((uint32_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg3High])[index];
        uint32_t low =
            ((uint32_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg3Low])[index];
        dst[texture_x] = high ? high : low;
      }
    }
  }
  if (g_sim3d.hud_obj_pixels && g_sim3d.hud_obj_pitch) {
    memset(g_sim3d.hud_obj_pixels, 0,
           (size_t)g_sim3d.hud_obj_pitch * kActRaiserSimulationHudHeight);
    for (int y = 0; y < kActRaiserSimulationHudHeight; y++) {
      uint32_t *dst = (uint32_t *)(g_sim3d.hud_obj_pixels +
                                   (size_t)y * g_sim3d.hud_obj_pitch);
      memcpy(dst, &g_sim3d_hud_obj_mask[(size_t)y * g_sim3d.width],
             (size_t)g_sim3d.width * sizeof(uint32_t));
    }
  }

  /* Rebuild the authentic framebuffer exactly as the original RemoveFromGame
   * captures do: omit BG3's HUD rectangle and the validated HUD OAM pixels,
   * then let lower hardware-rank planes show through. PPU framebuffers carry
   * RGB with a zero alpha byte, unlike the ARGB plane textures. */
  uint32_t enabled = (1u << kSim3DPlane_Count) - 1;
  for (int y = 0; y < g_sim3d.height; y++) {
    uint32_t *dst = (uint32_t *)(authentic_pixels +
                                 (size_t)y * authentic_pitch);
    bool hud_composite_span = g_sim3d.hud_bg3 &&
        y < g_sim3d.prior_captures[kPpuOverlaySource_Bg3].y1;
    for (int x = 0; x < g_sim3d.width; x++) {
      bool live = x >= g_sim3d.live_x0 && x < g_sim3d.live_x1;
      uint32_t color = (live || hud_composite_span)
          ? g_sim3d.backdrop_argb : 0xff000000u;
      for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
        if (!(enabled & (1u << plane)) || SkipHudPlanePixel(plane, x, y))
          continue;
        /* Same live-area rule the composed side uses: a wrapped-negative OBJ
         * rasterizes into margin columns the hardware leaves black, and this
         * rebuild feeds the authentic framebuffer, so letting it through here
         * corrupts the very image the fidelity gate compares against. */
        if (!live && !hud_composite_span) continue;
        uint32_t pixel = ((uint32_t *)g_sim3d_layer_pixels[plane])[
            (size_t)y * g_sim3d.width + x];
        if (pixel >> 24) {
          if (g_sim3d.object_half_add && IsObjPlane(plane) &&
              ObjPixelUsesColorMath(pixel)) {
            size_t index = (size_t)y * g_sim3d.width + x;
            uint32_t bg1_high = ((uint32_t *)
                g_sim3d_layer_pixels[kSim3DPlane_Bg1High])[index];
            uint32_t bg1_low = ((uint32_t *)
                g_sim3d_layer_pixels[kSim3DPlane_Bg1Low])[index];
            uint32_t subscreen = bg1_high ? bg1_high : bg1_low;
            color = subscreen
                ? HalfAddRgb(OpaqueArgb(pixel), subscreen)
                : OpaqueArgb(pixel);
          } else {
            color = OpaqueArgb(pixel);
          }
        }
      }
      dst[x] = color & 0x00ffffffu;
    }
  }

  memcpy(g_sim3d.ppu->overlayCaptures, g_sim3d.prior_captures,
         sizeof(g_sim3d.prior_captures));

  /* Published SIM textures omit the HUD too; PresentSim3D adds the same
   * anchored host overlay as authentic flat presentation after either A/B
   * profile has rendered. */
  for (int y = 0; y < g_sim3d.height; y++) {
    for (int x = 0; x < g_sim3d.width; x++) {
      size_t index = (size_t)y * g_sim3d.width + x;
      for (int plane = 0; plane < kSim3DPlane_Count; plane++)
        if (SkipHudPlanePixel(plane, x, y))
          ((uint32_t *)g_sim3d_layer_pixels[plane])[index] = 0;
      g_sim3d_flat_pixels[index] =
          ((uint32_t *)(authentic_pixels + (size_t)y * authentic_pitch))[x] |
          0xff000000u;
    }
  }
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
 * read these same buffers, so one application serves all three and the D2
 * equality gate verifies it against the real hardware output. Unlike the
 * targeted-miracle half-add -- which combines two planes and therefore has to
 * stay a compositing policy -- a fixed-colour add is a property of one layer
 * and belongs baked into it.
 *
 * The arithmetic is the part that matters. The PPU adds in 5-bit component
 * space and only then maps through `brightnessMult` to 8 bits, so adding the
 * expanded colour to the expanded pixel is NOT the same operation: it differs
 * on 168 of the 1024 (component, add) pairs, which the byte-exact D2 gate
 * would reject outright. The 5-bit value is recovered by inverting that table (injective at
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
  size_t count = (size_t)g_sim3d.width * g_sim3d.height;

  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    uint8_t bit = ColorMathLayerBit(plane);
    if (!bit || !(g_sim3d.fixed_add_mask & bit) || !g_sim3d_layer_pixels[plane])
      continue;
    bool obj = IsObjPlane(plane);
    uint32_t *pixels = (uint32_t *)g_sim3d_layer_pixels[plane];
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
         * frame; leaving it be is safer than guessing, and the D2 gate will
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

void Sim3D_FinishCapture(uint8_t *authentic_pixels,
                         int authentic_pitch, uint16_t game_frame) {
  if (!g_sim3d.active || !authentic_pixels ||
      authentic_pitch < g_sim3d.width * 4)
    return;

  /* Before anything reads the planes -- the authentic rebuild inside
   * RestoreTownHudPolicy included, since that rebuild feeds the very image
   * the fidelity gate compares against. */
  ApplyFixedColorAdd();

  /* The promoted HUD owns the full width of its own rows, so those rows keep
   * every captured pixel; the same span the authentic-side restore uses. */
  int hud_span_rows = g_sim3d.hud_bg3
      ? g_sim3d.prior_captures[kPpuOverlaySource_Bg3].y1 : 0;
  ComposeFlatPixelsPolicy(
      g_sim3d_flat_pixels, g_sim3d.width, g_sim3d.height,
      g_sim3d.width * (int)sizeof(uint32_t), g_sim3d.backdrop_argb,
      g_sim3d.live_x0, g_sim3d.live_x1, g_sim3d_layer_pixels, 0,
      g_sim3d.object_half_add, hud_span_rows);

  uint32_t mismatch = BuildDifference(authentic_pixels, authentic_pitch);
  g_sim3d.mismatch_pixels = mismatch;
  g_sim3d.separated_hash = HashRgb(
      g_sim3d_flat_pixels, g_sim3d.width, g_sim3d.height,
      g_sim3d.width * (int)sizeof(uint32_t));
  /* A requested diagnostic dump is useful for failed fidelity gates too: it
   * preserves the authentic/composed/difference triplet that explains why
   * the frame remained on the authentic renderer. */
  MaybeDumpDemoArtifacts(authentic_pixels, authentic_pitch, game_frame);

  if (mismatch) {
    g_sim3d.status = kSim3DCapture_PixelMismatch;
    RestoreTownHudPolicy(authentic_pixels, authentic_pitch);
    return;
  }
  if (!SimRenderMetadata_AtlasReady()) {
    g_sim3d.status = kSim3DCapture_AtlasInvalid;
    RestoreTownHudPolicy(authentic_pixels, authentic_pitch);
    return;
  }
  g_sim3d.separated_valid = true;
  g_sim3d.status = kSim3DCapture_Ready;
  RestoreTownHudPolicy(authentic_pixels, authentic_pitch);

  /* A nonzero diagnostic mask intentionally produces an incomplete A1 while
   * retaining the full-composite equality result above as the safety gate. */
  if (g_sim3d.diagnostic_layer_mask) {
    ComposeFlatPixelsPolicy(
        g_sim3d_flat_pixels, g_sim3d.width, g_sim3d.height,
        g_sim3d.width * (int)sizeof(uint32_t), g_sim3d.backdrop_argb,
        g_sim3d.live_x0, g_sim3d.live_x1, g_sim3d_layer_pixels,
        g_sim3d.diagnostic_layer_mask, g_sim3d.object_half_add,
        hud_span_rows);
    /* Keep A/B Difference useful for isolated-layer inspection without
     * changing the full-composite fidelity result published above. */
    BuildDifference(authentic_pixels, authentic_pitch);
  }
}

/* One line whenever a town frame changes between rendering enhanced and
 * falling back to the authentic composite. A single dropped frame is a visible
 * flicker but is invisible in a status census, and reproducing it under a trace
 * is awkward; this makes the game report it in the console as it happens, with
 * the reason and the game frame to look up in AR_SIM3D_D1_TRACE. Transitions
 * only, so a healthy session prints at most a couple of lines per town entry. */
void Sim3D_LogViewTransition(const SimFrameData *frame) {
  static int previous = -1;  /* -1 unknown, 0 authentic, 1 enhanced */
  static uint16_t previous_game_frame;
  if (!frame || !frame->town) {
    previous = -1;
    return;
  }
  int current = (frame->view == kSimView_Enhanced && frame->separated_valid)
      ? 1 : 0;
  if (current != previous) {
    if (previous >= 0)
      fprintf(stderr,
              "[sim3d-view] gf=%u %s -> %s (%s, view=%s, integrity=$%X,"
              " mismatch=%u px) after %u frame(s)\n",
              (unsigned)frame->game_frame,
              previous ? "enhanced" : "authentic",
              current ? "enhanced" : "authentic",
              Sim3D_CaptureStatusName((Sim3DCaptureStatus)
                                      frame->separated_status),
              Sim3D_ViewName(frame->view),
              (unsigned)frame->integrity_flags,
              (unsigned)frame->separated_mismatch_pixels,
              (unsigned)(frame->game_frame - previous_game_frame));
    /* The registers only when they are the reason. Printing them on every
     * transition would bury the one line that matters under the ordinary
     * picker and dialogue flips. */
    if (previous >= 0 &&
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
    return;
  }
  if (!ppu) return;
  /* Only render alongside a capture that passed the D2 byte-equality gate.
   * The canvas draws the same tiles the authentic frame does, so a frame the
   * gate rejected is a frame whose PPU state is not understood, and the
   * off-screen extension should not be trusted to interpret it either. */
  if (!g_sim3d.separated_valid || g_sim3d.status != kSim3DCapture_Ready)
    return;
  SimTownCanvas_Render(frame->town, wram, ppu->vram, ppu->cgram,
                       PPU_brightness(ppu), g_sim3d.backdrop_argb);
}

SimRenderFeatureMask Sim3D_ImplementedFeatures(void) {
  return g_sim3d.separated_valid ? kSim3DShippedFeatures : 0;
}

static int ClampTuning(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

void Sim3D_AnnotateFrame(SimFrameData *frame, const Sim3DTuning *tuning) {
  if (!frame || !tuning) return;
  int pitch_mrad = tuning->pitch_mrad;
  int yaw_mrad = tuning->yaw_mrad;
  int distance_x100 = tuning->distance_x100;
  frame->height_scale_x100 =
      (uint16_t)ClampTuning(tuning->height_scale_x100, 0, 0xFFFF);
  frame->shadow_opacity_pct =
      (uint8_t)ClampTuning(tuning->shadow_opacity_pct, 0, 100);
  frame->height_pop_pct =
      (uint8_t)ClampTuning(tuning->height_pop_pct, 0, 255);
  /* Azimuth wraps; elevation and softness clamp. */
  frame->light_azimuth_deg = (uint16_t)(((tuning->light_azimuth_deg % 360) +
                                         360) % 360);
  frame->light_elevation_deg =
      (uint8_t)ClampTuning(tuning->light_elevation_deg, 5, 90);
  frame->shadow_softness_pct =
      (uint8_t)ClampTuning(tuning->shadow_softness_pct, 0, 100);
  frame->rim_strength_pct =
      (uint8_t)ClampTuning(tuning->rim_strength_pct, 0, 100);
  frame->underlay_haze_pct =
      (uint8_t)ClampTuning(tuning->underlay_haze_pct, 0, 100);
  frame->cloud_opacity_pct =
      (uint8_t)ClampTuning(tuning->cloud_opacity_pct, 0, 100);
  frame->cloud_falloff_px =
      (uint16_t)ClampTuning(tuning->cloud_falloff_px, 1, 2048);
  frame->cloud_inset_px =
      (uint16_t)ClampTuning(tuning->cloud_inset_px, 0, 2048);
  /* The shroud clears exactly what OAM can populate: the authentic window
   * plus the emitter's own live margins, expressed in captured-texture
   * columns. Asking the emitter rather than re-deriving it means the two
   * cannot drift apart. */
  {
    int screen_x0 = g_sim3d.width > kActRaiserAuthenticWidth
        ? (g_sim3d.width - kActRaiserAuthenticWidth) / 2 : 0;
    int clear_x0 = screen_x0 - tuning->sprite_margin_left;
    int clear_x1 = screen_x0 + kActRaiserAuthenticWidth +
        tuning->sprite_margin_right;
    frame->cloud_clear_x0 = (uint16_t)(clear_x0 < 0 ? 0 : clear_x0);
    frame->cloud_clear_x1 = (uint16_t)clear_x1;
  }
  /* The same margins unconverted, for the per-record cull predicate. That
   * evaluates in the emitter's biased coordinates, not captured-texture
   * columns, so it cannot use cloud_clear_*. */
  frame->sprite_margin_left = (int16_t)tuning->sprite_margin_left;
  frame->sprite_margin_right = (int16_t)tuning->sprite_margin_right;
  frame->cull_lead_px = (uint16_t)ClampTuning(tuning->cull_lead_px, 1, 512);
  frame->cull_haze_pct = (uint8_t)ClampTuning(tuning->cull_haze_pct, 0, 100);
  frame->cull_dim_pct = (uint8_t)ClampTuning(tuning->cull_dim_pct, 0, 100);
  frame->cull_haze_lead_px =
      (uint16_t)ClampTuning(tuning->cull_haze_lead_px, 1, 1024);
  frame->cull_corner_px =
      (uint16_t)ClampTuning(tuning->cull_corner_px, 0, 512);
  frame->underlay_defocus_pct =
      (uint8_t)ClampTuning(tuning->underlay_defocus_pct, 0, 100);
  frame->cloud_altitude_px =
      (uint16_t)ClampTuning(tuning->cloud_altitude_px, 0, 512);
  frame->cloud_drift_pct =
      (uint16_t)ClampTuning(tuning->cloud_drift_pct, 0, 500);
  frame->cull_lift_inset = tuning->cull_lift_inset ? 1 : 0;
  frame->backdrop_strength_pct =
      (uint8_t)ClampTuning(tuning->backdrop_strength_pct, 0, 100);
  frame->backdrop_horizon_pct =
      (uint8_t)ClampTuning(tuning->backdrop_horizon_pct, 0, 100);
  /* The capture is centred on the authentic 256-column window, so this is the
   * captured-texture column that holds SNES x = 0 — the offset the underlay
   * needs to turn a texture column into a town pixel. */
  frame->underlay_screen_x0 = g_sim3d.width > kActRaiserAuthenticWidth
      ? (uint16_t)((g_sim3d.width - kActRaiserAuthenticWidth) / 2) : 0;
  frame->separated_valid = g_sim3d.separated_valid;
  frame->separated_status = (uint8_t)g_sim3d.status;
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
  frame->projection_pitch_mrad = (int16_t)pitch_mrad;
  frame->projection_yaw_mrad = (int16_t)yaw_mrad;
  frame->projection_distance_x100 = (uint16_t)distance_x100;
}
