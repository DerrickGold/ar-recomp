#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <execinfo.h>

#include "snes.h"
#include "../debug_server.h"
#include "snes_regs.h"

extern void ar_vramraw(uint16_t vaddr, uint8_t val, int port);
#include "../ar_trace.h"


extern bool g_new_ppu;
void PpuDrawWholeLineOldPpu(Ppu *ppu, int line);
static void PpuDrawWholeLine(Ppu *ppu, uint y);

static bool ppu_evaluateSprites(Ppu* ppu, int line);
static uint16_t ppu_getVramRemap(Ppu* ppu);


Ppu* ppu_init(void) {
  /* ppu_reset preserves host-owned render bindings. Start them deterministically
   * NULL before the first reset instead of reading indeterminate malloc data. */
  Ppu* ppu = calloc(1, sizeof(Ppu));
  return ppu;
}

void ppu_free(Ppu* ppu) {
  free(ppu);
}

void ppu_reset(Ppu* ppu) {
  {
    size_t pitch = ppu->renderPitch;
    uint8_t *renderBuffer = ppu->renderBuffer;
    uint32_t renderFlags = ppu->renderFlags;
    uint32_t overlayPitch[kPpuOverlaySource_Count];
    uint8_t *overlayBuffer[kPpuOverlaySource_Count];
    uint8_t *m7Buffer = ppu->m7OverlayBuffer;
    uint32_t m7Pitch = ppu->m7OverlayPitch;
    uint8_t m7Scale = ppu->m7OverlayScale;
    memcpy(overlayPitch, ppu->overlayRenderPitch, sizeof(overlayPitch));
    memcpy(overlayBuffer, ppu->overlayRenderBuffer, sizeof(overlayBuffer));
    memset(ppu, 0, sizeof(*ppu));
    ppu->renderBuffer = renderBuffer;
    ppu->renderPitch = (uint32_t)pitch;
    ppu->renderFlags = renderFlags;
    memcpy(ppu->overlayRenderPitch, overlayPitch, sizeof(overlayPitch));
    memcpy(ppu->overlayRenderBuffer, overlayBuffer, sizeof(overlayBuffer));
    ppu->m7OverlayBuffer = m7Buffer;
    ppu->m7OverlayPitch = m7Pitch;
    ppu->m7OverlayScale = m7Scale;
    /* Surviving surfaces may hold pre-reset content; force one full clear. */
    for (int i = 0; i < kPpuOverlaySource_Count; i++)
      ppu->overlayRenderMaybeDirty[i] = ppu->overlayRenderBuffer[i] != NULL;
    ppu->m7OverlayMaybeDirty = ppu->m7OverlayBuffer != NULL;
  }
  ppu->vramIncrement = 1;
}

void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli) {
  assert(offsetof(Ppu, cgwsel) + 1 - offsetof(Ppu, inidisp) == PPU_SAVESTATE_REGS_SIZE);
  assert(offsetof(Ppu, vram) + 0x10000 - offsetof(Ppu, cgram) == PPU_SAVESTATE_MEM_SIZE);
  uint32 version[2] = {'P' | 'P' << 8 | 'U' << 16 | '0' << 24, PPU_SAVESTATE_REGS_SIZE + PPU_SAVESTATE_MEM_SIZE};
  sli->func(sli, version, 8);
  sli->func(sli, &ppu->inidisp, PPU_SAVESTATE_REGS_SIZE);
  sli->func(sli, &ppu->cgram, PPU_SAVESTATE_MEM_SIZE);
}

void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags) {
  ppu->renderPitch = (uint)pitch;
  ppu->renderBuffer = pixels;
  ppu->renderFlags = render_flags;
}

void PpuClearOverlayBindings(Ppu *ppu) {
  memset(ppu->overlayRenderBuffer, 0, sizeof(ppu->overlayRenderBuffer));
  memset(ppu->overlayRenderPitch, 0, sizeof(ppu->overlayRenderPitch));
  PpuClearOverlayCaptures(ppu);
}

bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch) {
  if ((unsigned)source >= kPpuOverlaySource_Count ||
      (pixels && (!pitch || pitch % sizeof(uint32_t) != 0 ||
                  pitch / sizeof(uint32_t) < kPpuXPixels ||
                  pitch / sizeof(uint32_t) > kPpuBufWidth)))
    return false;
  ppu->overlayRenderBuffer[source] = pixels;
  ppu->overlayRenderPitch[source] = pixels ? (uint32_t)pitch : 0;
  /* A newly bound buffer's contents are unknown; force one full clear. */
  ppu->overlayRenderMaybeDirty[source] = pixels != NULL;
  if (!pixels)
    memset(&ppu->overlayCaptures[source], 0,
           sizeof(ppu->overlayCaptures[source]));
  return true;
}

void PpuClearOverlayCaptures(Ppu *ppu) {
  memset(ppu->overlayCaptures, 0, sizeof(ppu->overlayCaptures));
  memset(&ppu->m7Override, 0, sizeof(ppu->m7Override));
}

bool PpuBindMode7OverlaySurface(Ppu *ppu, uint8_t *pixels, size_t pitch,
                                uint8_t scale) {
  if (pixels && (scale < 1 || scale > 4 || !pitch ||
                 pitch % sizeof(uint32_t) != 0 ||
                 pitch / sizeof(uint32_t) < (size_t)kPpuXPixels * scale ||
                 pitch / sizeof(uint32_t) > (size_t)kPpuBufWidth * scale))
    return false;
  ppu->m7OverlayBuffer = pixels;
  ppu->m7OverlayPitch = pixels ? (uint32_t)pitch : 0;
  ppu->m7OverlayScale = pixels ? scale : 0;
  /* A newly bound buffer's contents are unknown; force one full clear. */
  ppu->m7OverlayMaybeDirty = pixels != NULL;
  if (!pixels)
    memset(&ppu->m7Override, 0, sizeof(ppu->m7Override));
  return true;
}

bool PpuSetMode7Override(Ppu *ppu, const uint32_t *rgba, int width,
                         int height, int canvas_x0, int canvas_y0,
                         int canvas_x1, int canvas_y1, uint8_t wrap) {
  if (!ppu->m7OverlayBuffer || !rgba || width <= 0 || height <= 0 ||
      canvas_x1 <= canvas_x0 || canvas_y1 <= canvas_y0 ||
      canvas_x0 < 0 || canvas_y0 < 0 || canvas_x1 > 1024 || canvas_y1 > 1024)
    return false;
  ppu->m7Override.wrap = wrap;
  ppu->m7Override.rgba = rgba;
  ppu->m7Override.width = width;
  ppu->m7Override.height = height;
  ppu->m7Override.canvasX0 = canvas_x0;
  ppu->m7Override.canvasY0 = canvas_y0;
  ppu->m7Override.canvasX1 = canvas_x1;
  ppu->m7Override.canvasY1 = canvas_y1;
  return true;
}

bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source,
                          int x, int y, int width, int height, uint8_t flags) {
  if ((unsigned)source >= kPpuOverlaySource_Count || width <= 0 || height <= 0)
    return false;
  int64_t requested_x1 = (int64_t)x + width;
  int64_t requested_y1 = (int64_t)y + height;
  int x0 = IntMax(x, -kPpuExtraLeftRight);
  int x1 = requested_x1 < kPpuXPixels + kPpuExtraLeftRight
      ? (int)requested_x1 : kPpuXPixels + kPpuExtraLeftRight;
  int y0 = IntMax(y, 0);
  int y1 = requested_y1 < 240 ? (int)requested_y1 : 240;
  if (x1 <= x0 || y1 <= y0)
    return false;
  PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  capture->x0 = (int16_t)x0;
  capture->x1 = (int16_t)x1;
  capture->y0 = (int16_t)y0;
  capture->y1 = (int16_t)y1;
  capture->flags = flags & kPpuOverlayFlag_RemoveFromGame;
  capture->oamFirst = 0;
  capture->oamCount = 0;
  return true;
}

bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count) {
  PpuOverlayCapture *capture =
      &ppu->overlayCaptures[kPpuOverlaySource_Obj];
  if (first >= 128 || !count || count > 128 - first ||
      capture->x1 <= capture->x0 || capture->y1 <= capture->y0)
    return false;
  capture->oamFirst = first;
  capture->oamCount = count;
  return true;
}

// Clear the per-frame widescreen layer-policy state (clamp, padding, bands).
// The game policy re-applies these every frame after choosing the margin mode,
// so resetting here keeps stale clamps from a previous frame/mode from leaking.
static inline void PpuResetLayerClamps(Ppu *ppu) {
  ppu->wsLayerClamp = 0;
  ppu->wsLayerMirror = 0;
  ppu->wsLayerRepeat = 0;
  memset(ppu->wsClampY0, 0, sizeof(ppu->wsClampY0));
  memset(ppu->wsClampY1, 0, sizeof(ppu->wsClampY1));
  memset(ppu->wsRepeatY0, 0, sizeof(ppu->wsRepeatY0));
  memset(ppu->wsRepeatY1, 0, sizeof(ppu->wsRepeatY1));
  memset(ppu->wsMarginGapL, 0, sizeof(ppu->wsMarginGapL));
  memset(ppu->wsMarginGapR, 0, sizeof(ppu->wsMarginGapR));
}

void PpuSetExtraSpace(Ppu *ppu, uint8_t extra) {
  if (extra > kPpuExtraLeftRight)
    extra = kPpuExtraLeftRight;
  // Symmetric border: equal columns added on each side. extraLeftRight is the
  // centering budget; extraLeftCur/extraRightCur are the per-side columns the
  // window/sprite/composite paths actually render.
  ppu->extraLeftRight = extra;
  ppu->extraLeftCur = extra;
  ppu->extraRightCur = extra;
  PpuResetLayerClamps(ppu);
}

void PpuSetExtraSpaceCentered(Ppu *ppu, uint8_t budget) {
  if (budget > kPpuExtraLeftRight)
    budget = kPpuExtraLeftRight;
  // Render only the authentic 256 columns but keep the centering budget so the
  // composite places them in the middle of the wider framebuffer (the caller
  // clears the side margins to black -> letterbox/pillarbox). Used for bounded
  // screens (overworld, title) where there is no valid BG past 256 to show.
  ppu->extraLeftRight = budget;
  ppu->extraLeftCur = 0;
  ppu->extraRightCur = 0;
  PpuResetLayerClamps(ppu);
}

void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom) {
  // Per-frame asymmetric fill within the centering budget (extraLeftRight).
  // Mirrors zelda3's PpuSetExtraSideSpace; left/right clamp to the budget so
  // the line renderer's window edges stay inside the priority buffers, bottom
  // clamps to the 16px overscan band. See ppu.h for the symmetric-vs-dynamic
  // distinction.
  ppu->extraLeftCur = (uint8_t)IntMin(IntMax(left, 0), ppu->extraLeftRight);
  ppu->extraRightCur = (uint8_t)IntMin(IntMax(right, 0), ppu->extraLeftRight);
  ppu->extraBottomCur = (uint8_t)IntMin(IntMax(bottom, 0), 16);
}

void PpuSetWidescreenHudSplit(Ppu *ppu, uint8_t height, uint8_t left_end,
                              uint8_t right_start, uint8_t left_only_y) {
  // See ppu.h. Equal bounds select the two-way left/right form; reversed or
  // empty bounds are invalid and disable the split.
  if (left_end == 0 || left_end > right_start) height = 0;
  if (!height)
    left_only_y = 0;
  else if (left_only_y > height)
    left_only_y = height;
  ppu->wsHudSplitHeight = height;
  ppu->wsHudLeftEnd = left_end;
  ppu->wsHudRightStart = right_start;
  ppu->wsHudLeftOnlyY = left_only_y;
}

void PpuSetWidescreenBg3Widen(Ppu *ppu, uint8_t from_y) {
  ppu->wsBg3WidenY = from_y;
}

void PpuSetWidescreenLayerClamp(Ppu *ppu, uint8_t mask) {
  // Bit L (0..3) clamps BG(L+1) to the authentic 256 columns even in
  // widescreen. See ppu.h. Set per frame by the game's widescreen policy;
  // 0 = all layers extended.
  ppu->wsLayerClamp = mask;
}

void PpuSetWidescreenLayerMirror(Ppu *ppu, uint8_t mask) {
  // See ppu.h. Only the Mode-1 4bpp paths currently consume these bits; other
  // layers are clamped by PpuLayerExtra so unsupported mirroring cannot expose
  // stale offscreen tilemap data.
  ppu->wsLayerMirror = mask;
}

void PpuSetWidescreenLayerRepeat(Ppu *ppu, uint8_t mask) {
  // See ppu.h. This shares the isolated-layer path with mirror padding, but
  // samples the opposite authentic edge so raster-scrolled art continues in
  // the same direction instead of reversing at the widescreen boundary.
  ppu->wsLayerRepeat = mask;
}

void PpuSetWidescreenLayerMarginGap(Ppu *ppu, uint8_t layer, uint8_t left_px,
                                    uint8_t right_px) {
  // Margins of BG(layer+1) skip the first left_px/right_px offscreen pixels
  // (the game's UI staging columns) and sample the tilemap beyond them. See
  // ppu.h; re-apply per frame (the extra-space setters reset it).
  if (layer < 4) {
    ppu->wsMarginGapL[layer] = left_px;
    ppu->wsMarginGapR[layer] = right_px;
  }
}

void PpuSetWidescreenLayerClampBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                    uint8_t y1) {
  // Clamp BG(layer+1) to the authentic 256 on scanlines [y0,y1) only. See
  // ppu.h. y1<=y0 disables the band. Re-apply per frame (the extra-space
  // setters reset it), like the other widescreen setters.
  if (layer < 4) {
    ppu->wsClampY0[layer] = y0;
    ppu->wsClampY1[layer] = y1;
  }
}

void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1) {
  // Repeat BG(layer+1)'s authentic rendered scanline into both margins only on
  // [y0,y1). The draw policy gives this band precedence over whole-layer clamp.
  if (layer < 4) {
    ppu->wsRepeatY0[layer] = y0;
    ppu->wsRepeatY1[layer] = y1;
  }
}

bool ppu_checkOverscan(Ppu* ppu) {
  // called at (0,225)
  ppu->frameOverscan = PPU_overscan(ppu); // set if we have a overscan-frame
  return ppu->frameOverscan;
}

void ppu_handleVblank(Ppu* ppu) {
  // called either right after ppu_checkOverscan at (0,225), or at (0,240)
  if(!PPU_forcedBlank(ppu)) {
    ppu->oamAdr = ppu->oamaddl;
    ppu->oamInHigh = ppu->oamaddh & 1;
    ppu->oamSecondWrite = false;
  }
  ppu->frameInterlace = PPU_interlace(ppu); // set if we have a interlaced frame
}

static inline void ClearBackdrop(PpuPixelPrioBufs *buf) {
  for (size_t i = 0; i != arraysize(buf->data); i += 4)
    *(uint64*)&buf->data[i] = 0x0500050005000500;
}

// mosaicModulo is sized for the logical 256-wide screen, but widescreen window
// edges can fall in the border (negative on the left, >=256 on the right).
// Clamp the lookup so the rare mosaic+widescreen combination stays in-bounds;
// border mosaic alignment is approximate but never reads out of range. With
// extra==0 (authentic) every index is already in [0,256), so this is a no-op.
static inline uint8 PpuMosaicAt(Ppu *ppu, int i) {
  return ppu->mosaicModulo[(unsigned)i < (unsigned)kPpuXPixels ? i : (i < 0 ? 0 : kPpuXPixels - 1)];
}

void ppu_runLine(Ppu* ppu, int line) {
  if(line == 0) {
    // Always-on: snapshot the OAM the scanline renderer is about to consume.
    debug_server_on_oam_render();
    if (PPU_mosaicSize(ppu) != ppu->lastMosaicModulo) {
      int mod = PPU_mosaicSize(ppu);
      ppu->lastMosaicModulo = mod;
      for (int i = 0, j = 0; i < countof(ppu->mosaicModulo); i++) {
        ppu->mosaicModulo[i] = i - j;
        j = (j + 1 == mod ? 0 : j + 1);
      }
    }


    // pre-render line
    // TODO: this now happens halfway into the first line
    ppu->mosaicStartLine = 1;
    ppu->rangeOver = false;
    ppu->timeOver = false;
    ppu->evenFrame = !ppu->evenFrame;
  } else {
    // Cache the brightness computation
    if (PPU_brightness(ppu) != ppu->lastBrightnessMult) {
      uint8_t ppu_brightness = PPU_brightness(ppu);
      ppu->lastBrightnessMult = ppu_brightness;
      for (int i = 0; i < 32; i++)
        ppu->brightnessMultHalf[i * 2] = ppu->brightnessMultHalf[i * 2 + 1] = ppu->brightnessMult[i] =
        ((i << 3) | (i >> 2)) * ppu_brightness / 15;
      // Store 31 extra entries to remove the need for clamping to 31.
      memset(&ppu->brightnessMult[32], ppu->brightnessMult[31], 31);
    }

    // evaluate sprites
    ClearBackdrop(&ppu->objBuffer);
    if (ppu->overlayRenderBuffer[kPpuOverlaySource_Obj])
      memset(&ppu->overlayBuffers[kPpuOverlaySource_Obj], 0,
             sizeof(ppu->overlayBuffers[kPpuOverlaySource_Obj]));
    ppu->lineHasSprites = !PPU_forcedBlank(ppu) && ppu_evaluateSprites(ppu, line - 1);

    if (g_new_ppu) {
      PpuDrawWholeLine(ppu, line);
    } else {
      PpuDrawWholeLineOldPpu(ppu, line);
    }
  }
}

typedef struct PpuWindows {
  // Up to 5 window spans + 2 margin-gap splits (PpuApplyMarginGap) = 7 spans,
  // 8 edges.
  int16 edges[8];
  uint8 nr;
  uint8 bits;
} PpuWindows;

// Per-layer widescreen side margin. BG3 (layer 2) carries the HUD and is
// clamped to the authentic 256-wide region so a BG3 status bar never tiles into
// the margins -- EXCEPT on scanlines >= wsBg3WidenY, where the game renders
// level content on BG3 (e.g. SMW water) that should fill 16:9 like BG1/BG2.
static inline int PpuLayerExtra(Ppu *ppu, uint layer, int y, int extra) {
  /* A promoted HUD line composites across the complete presentation canvas
   * even when a finite world's live margin is narrower. Layer 5 is the color
   * window used by the final compositor; widening only that logical layer does
   * not grant BG1/BG2 or sprites any additional world visibility. Their
   * outside-live-margin pixels remain the cleared backdrop, while the BG3 HUD
   * split below supplies the fixed-screen status pixels. */
  if (layer == 5 && ppu->wsHudSplitHeight &&
      y < ppu->wsHudSplitHeight && ppu->extraLeftRight)
    return ppu->extraLeftRight;

  // Clamp metadata exists only for BG1-BG4. The same window helper is also
  // used for the color-math window (logical layer 5), which must never index
  // these four-entry arrays.
  if (layer < 4) {
    // Game-forced per-layer clamp (UI/dialog/bounded layers): keep this layer
    // in the authentic 256 so it never tiles wrapped/garbage columns into the
    // margins while the wide world layers beside it still extend.
    if ((ppu->wsLayerClamp | ppu->wsLayerMirror | ppu->wsLayerRepeat) &
        (1u << layer))
      return 0;
    // Per-layer clamp band: clamp only the rows a bounded UI element occupies,
    // so wide world content on the same layer stays wide above/below it.
    if (ppu->wsClampY1[layer] > ppu->wsClampY0[layer] &&
        y >= ppu->wsClampY0[layer] && y < ppu->wsClampY1[layer])
      return 0;
    // A repeat band is first rendered only in the authentic center, then its
    // isolated scanline is merged into the margins by the 4bpp policy path.
    if (ppu->wsRepeatY1[layer] > ppu->wsRepeatY0[layer] &&
        y >= ppu->wsRepeatY0[layer] && y < ppu->wsRepeatY1[layer])
      return 0;
  }
  if (layer != 2)
    return extra;
  return (ppu->wsBg3WidenY && y >= ppu->wsBg3WidenY) ? extra : 0;
}

static void PpuWindows_Clear(PpuWindows *win, Ppu *ppu, uint layer, int y) {
  win->edges[0] = -PpuLayerExtra(ppu, layer, y, ppu->extraLeftCur);
  win->edges[1] = 256 + PpuLayerExtra(ppu, layer, y, ppu->extraRightCur);
  win->nr = 1;
  win->bits = 0;
}

static void PpuWindows_Calc(PpuWindows *win, Ppu *ppu, uint layer, int y) {
  // Evaluate which spans to render based on the window settings.
  // There are at most 5 windows.
  // Algorithm from Snes9x
  uint32 winflags = GET_WINDOW_FLAGS(ppu, layer);
  uint nr = 1;
  int window_right = 256 + PpuLayerExtra(ppu, layer, y, ppu->extraRightCur);
  win->edges[0] = -PpuLayerExtra(ppu, layer, y, ppu->extraLeftCur);
  win->edges[1] = window_right;
  uint i, j;
  int t;
  // Widescreen: the 8-bit window coordinate space cannot express the side
  // margins, so a window edge pinned at the hardware extreme (0 / 255) is
  // read as "to the screen border" and extended into the active margin.
  // Without this, any screen that runs a full-width window — e.g. the
  // color-math clip window ActRaiser's sim/menu engine keeps enabled — has
  // its margins land in the "outside the window" region and clip to black
  // even though the BG layers rendered valid pixels there. A game clipping
  // at exactly [0,255] on purpose gets the same visual on the authentic 256
  // columns either way; only the margin treatment differs. No-op at extra=0.
  int w1l = ppu->window1left, w1r = ppu->window1right;
  int w2l = ppu->window2left, w2r = ppu->window2right;
  if (win->edges[0] != 0 || window_right != 256) {
    if (w1l == 0) w1l = win->edges[0];
    if (w1r == 255) w1r = window_right - 1;
    if (w2l == 0) w2l = win->edges[0];
    if (w2r == 255) w2r = window_right - 1;
  }
  bool w1_ena = (winflags & kWindow1Enabled) && w1l <= w1r;
  if (w1_ena) {
    if (w1l > win->edges[0]) {
      win->edges[nr] = w1l;
      win->edges[++nr] = window_right;
    }
    if (w1r + 1 < window_right) {
      win->edges[nr] = w1r + 1;
      win->edges[++nr] = window_right;
    }
  }
  bool w2_ena = (winflags & kWindow2Enabled) && w2l <= w2r;
  if (w2_ena) {
    for (i = 0; i <= nr && (t = w2l) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
    for (; i <= nr && (t = w2r + 1) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
  }
  win->nr = nr;
  // get a bitmap of how regions map to windows
  uint8 w1_bits = 0, w2_bits = 0;
  if (w1_ena) {
    for (i = 0; win->edges[i] != w1l; i++);
    for (j = i; win->edges[j] != w1r + 1; j++);
    w1_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow1Enabled | kWindow1Inversed)) == (kWindow1Enabled | kWindow1Inversed))
    w1_bits = ~w1_bits;
  if (w2_ena) {
    for (i = 0; win->edges[i] != w2l; i++);
    for (j = i; win->edges[j] != w2r + 1; j++);
    w2_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow2Enabled | kWindow2Inversed)) == (kWindow2Enabled | kWindow2Inversed))
    w2_bits = ~w2_bits;
  win->bits = w1_bits | w2_bits;
}

// Split the span containing screen-x `xpos` into two spans at xpos, keeping
// the disabled bit and per-span fetch bias of the original for both halves.
static void PpuWindowsSplit(PpuWindows *win, int16 *bias, int xpos) {
  for (uint i = 0; i < win->nr; i++) {
    if (win->edges[i] < xpos && xpos < win->edges[i + 1]) {
      for (uint j = win->nr; j >= i + 1; j--)
        win->edges[j + 1] = win->edges[j];
      win->edges[i + 1] = (int16)xpos;
      for (uint j = win->nr - 1; j >= i + 1; j--)
        bias[j + 1] = bias[j];
      bias[i + 1] = bias[i];
      uint8 lo = win->bits & (uint8)((1u << (i + 1)) - 1);
      uint8 hi = (uint8)((win->bits >> (i + 1)) << (i + 2));
      win->bits = lo | hi | (uint8)(((win->bits >> i) & 1) << (i + 1));
      win->nr++;
      return;
    }
  }
}

// Widescreen margin source gap (PpuSetWidescreenLayerMarginGap): games park
// UI-construction tiles in the tilemap columns just past the visible screen
// (an offscreen staging area — invisible on hardware, exposed by widescreen
// margins). Skip those columns: split the window spans at the authentic
// screen edges and bias the tilemap fetch of the margin sub-spans outward by
// the gap, so margins show the layer's content BEYOND the staging strip while
// the game keeps its offscreen scratch. No-op at gap 0 or without margins.
static void PpuApplyMarginGap(Ppu *ppu, uint layer, PpuWindows *win, int16 *bias) {
  int gl = ppu->wsMarginGapL[layer], gr = ppu->wsMarginGapR[layer];
  if (!(gl | gr) || !(ppu->extraLeftCur | ppu->extraRightCur))
    return;
  PpuWindowsSplit(win, bias, 0);
  PpuWindowsSplit(win, bias, 256);
  for (uint k = 0; k < win->nr; k++) {
    if (win->edges[k + 1] <= 0)
      bias[k] = (int16)(bias[k] - gl);
    else if (win->edges[k] >= 256)
      bias[k] = (int16)(bias[k] + gr);
  }
}

// Draw a whole line of a 4bpp background layer into bgBuffers
static void PpuDrawBackground_4bpp(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                   uint y, bool sub, uint layer,
                                   PpuZbufType zhi, PpuZbufType zlo) {
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2 | (bits >> (14 + i)) & 4 | (bits >> (21 + i)) & 8; \
  if ((bits & (0x01010101u << i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2 | (bits >> (21 - i)) & 4 | (bits >> (28 - i)) & 8; \
  if ((bits & (0x80808080 >> i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | (uint32)addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  int16 ws_bias[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  PpuApplyMarginGap(ppu, layer, &win, ws_bias);
  y += ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + ppu->hScroll[layer] + ws_bias[windex];
    uint w = win.edges[windex + 1] - win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + win.edges[windex] + kPpuExtraLeftRight;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];
#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
}

// Draw a whole line of a 2bpp background layer into bgBuffers
static void PpuDrawBackground_2bpp(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                   uint y, bool sub, uint layer,
                                   PpuZbufType zhi, PpuZbufType zlo) {
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[(ta) + (tile) * 8 & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  // Widescreen HUD split (PpuSetWidescreenHudSplit): on HUD scanlines,
  // replace the single span with five — left chunk re-anchored to the left
  // border edge, a transparent gap, the centered chunk, a gap, and the
  // right chunk re-anchored to the right border edge. ws_bias shifts each
  // drawn span's source x so the chunks keep sampling their authentic
  // tilemap columns. Applied only when the computed window set is one
  // full drawn span: games (SMW) often enable screen-level window masking
  // ($212E) with no window selected for the layer, which still routes
  // through PpuWindows_Calc but degenerates to a single span. An actual
  // window shape (e.g. the level-start iris) keeps the authentic centered
  // HUD for those frames — split + real windows don't compose.
  int16 ws_bias[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  /* Anchor HUD chunks to the full presentation canvas, not the live world
   * margins. Finite rooms can temporarily set extraLeftCur/extraRightCur to
   * zero at a camera edge while the allocated centering budget—and therefore
   * the physical widescreen border—remains present. A fixed-screen HUD should
   * continue to occupy that border even when the world underneath it cannot. */
  int hud_extra = ppu->extraLeftRight;
  if (dstbuf == &ppu->bgBuffers[sub] && layer == 2 &&
      y < ppu->wsHudSplitHeight && hud_extra &&
      win.nr == 1 && !(win.bits & 1)) {
    if (ppu->wsHudLeftOnlyY < ppu->wsHudSplitHeight &&
        y >= ppu->wsHudLeftOnlyY) {
      /* Lower left-only form: preserve the complete source row as one chunk.
       * This is useful for status rows whose content crosses an upper band's
       * left/center boundary (for example a long enemy-health bar). */
      win.edges[0] = -hud_extra;
      win.edges[1] = 256 - hud_extra;
      ws_bias[0] = hud_extra;
    } else if (ppu->wsHudLeftEnd == ppu->wsHudRightStart) {
      /* Two-way form: no centered HUD group. Source [0,split) hugs the left
       * presentation edge and [split,256) hugs the right. */
      win.nr = 3;
      win.bits = 0x02;  // span 1 is the vacated center gap
      win.edges[0] = -hud_extra;
      win.edges[1] = ppu->wsHudLeftEnd - hud_extra;
      win.edges[2] = ppu->wsHudRightStart + hud_extra;
      win.edges[3] = 256 + hud_extra;
      ws_bias[0] = hud_extra;
      ws_bias[2] = -(int16)hud_extra;
    } else {
      /* Three-way form: retain a centered source chunk between the corners. */
      win.nr = 5;
      win.bits = 0x0A;  // spans 1 and 3 are the gaps
      win.edges[0] = -hud_extra;
      win.edges[1] = ppu->wsHudLeftEnd - hud_extra;
      win.edges[2] = ppu->wsHudLeftEnd;
      win.edges[3] = ppu->wsHudRightStart;
      win.edges[4] = ppu->wsHudRightStart + hud_extra;
      win.edges[5] = 256 + hud_extra;
      ws_bias[0] = hud_extra;
      ws_bias[4] = -(int16)hud_extra;
    }
  } else {
    PpuApplyMarginGap(ppu, layer, &win, ws_bias);
  }
  y += ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);

  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + ppu->hScroll[layer] + ws_bias[windex];
    uint w = win.edges[windex + 1] - win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + win.edges[windex] + kPpuExtraLeftRight;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];

#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef NEXT_TP
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
}


// Draw a whole line of a 4bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_4bpp_mosaic(Ppu *ppu,
                                          PpuPixelPrioBufs *dstbuf, uint y,
                                          bool sub, uint layer,
                                          PpuZbufType zhi,
                                          PpuZbufType zlo) {
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2 | (bits >> 21) & 4 | (bits >> 28) & 8
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | (uint32)addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  y = ppu->mosaicModulo[y] + ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + sx + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + win.edges[windex + 1] + kPpuExtraLeftRight;
    uint x = sx + ppu->hScroll[layer];
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int mosaic_size = PPU_mosaicSize(ppu);
    int w = mosaic_size - (sx - PpuMosaicAt(ppu, sx));
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        int i = 0;
        do {
          if (z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = mosaic_size;
    } while (dstz_end - dstz != 0);
  }
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
}

// Composite one isolated 4bpp layer into the live priority buffer, padding its
// active side margins from the authentic rendered scanline. Rendering into a
// temporary layer buffer is important: directly copying the live center would
// also duplicate sprites and lower-priority BGs visible through transparent
// pixels. Comparing isolated z/color words reproduces the normal per-layer
// priority merge. `repeat` chooses cyclic continuation instead of reflection.
static void PpuMergePaddedBackground(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                     const PpuPixelPrioBufs *layerbuf,
                                     bool repeat) {
  PpuZbufType *dst = dstbuf->data;
  const PpuZbufType *src = layerbuf->data;
  for (int x = 0; x < kPpuXPixels; x++) {
    int i = x + kPpuExtraLeftRight;
    if (src[i] > dst[i])
      dst[i] = src[i];
  }
  for (int x = -(int)ppu->extraLeftCur; x < 0; x++) {
    int di = x + kPpuExtraLeftRight;
    int sx = repeat ? kPpuXPixels + x : -x;
    int si = sx + kPpuExtraLeftRight;
    if (src[si] > dst[di])
      dst[di] = src[si];
  }
  for (int x = kPpuXPixels;
       x < kPpuXPixels + (int)ppu->extraRightCur; x++) {
    int di = x + kPpuExtraLeftRight;
    int sx = repeat ? x - kPpuXPixels : kPpuXPixels * 2 - 2 - x;
    int si = sx + kPpuExtraLeftRight;
    if (src[si] > dst[di])
      dst[di] = src[si];
  }
}

static void PpuDrawBackground_4bpp_policy(Ppu *ppu,
                                          PpuPixelPrioBufs *dstbuf,
                                          uint y, bool sub,
                                          uint layer, PpuZbufType zhi,
                                          PpuZbufType zlo, bool mosaic) {
  uint8_t padding = ppu->wsLayerMirror | ppu->wsLayerRepeat;
  bool repeat_band = layer < 4 &&
      ppu->wsRepeatY1[layer] > ppu->wsRepeatY0[layer] &&
      y >= ppu->wsRepeatY0[layer] && y < ppu->wsRepeatY1[layer];
  if (!(padding & (1u << layer)) && !repeat_band) {
    if (mosaic)
      PpuDrawBackground_4bpp_mosaic(ppu, dstbuf, y, sub,
                                    layer, zhi, zlo);
    else
      PpuDrawBackground_4bpp(ppu, dstbuf, y, sub, layer,
                             zhi, zlo);
    return;
  }

  PpuPixelPrioBufs layerbuf;
  ClearBackdrop(&layerbuf);
  if (mosaic)
    PpuDrawBackground_4bpp_mosaic(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  else
    PpuDrawBackground_4bpp(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  PpuMergePaddedBackground(ppu, dstbuf, &layerbuf,
                           repeat_band ||
                           (ppu->wsLayerRepeat & (1u << layer)) != 0);
}

// Draw a whole line of a 2bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_2bpp_mosaic(Ppu *ppu,
                                          PpuPixelPrioBufs *dstbuf,
                                          int y, bool sub, uint layer,
                                          PpuZbufType zhi,
                                          PpuZbufType zlo) {
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 8) & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  y = ppu->mosaicModulo[y] + ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + sx + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + win.edges[windex + 1] + kPpuExtraLeftRight;
    uint x = sx + ppu->hScroll[layer];
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int mosaic_size = PPU_mosaicSize(ppu);
    int w = mosaic_size - (sx - PpuMosaicAt(ppu, sx));
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        uint i = 0;
        do {
          if (z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = mosaic_size;
    } while (dstz_end - dstz != 0);
  }
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
}


// Assumes it's drawn on an empty backdrop
/* Mode-7 override sampling for one screen pixel whose canvas position lands
 * inside the override rectangle. Returns true when the pixel is "removed":
 * the caller must then skip the authentic tile write (main and subscreen,
 * preventing a color-math ghost). On the main pass this also renders the
 * scale x scale texture subsamples into the bound Mode-7 overlay surface,
 * stepping the live matrix at fractional increments so per-scanline HDMA
 * warps apply to the substituted art. Subsamples keep their texture alpha
 * (host blends edges over the authentic frame); removal itself is decided
 * by the base sample so translucent art fringes never punch holes. */
static bool PpuMode7OverrideSample(Ppu *ppu, bool sub, int screen_x, uint y,
                                   uint32 xpos, uint32 ypos, int dx, int dy) {
  const PpuMode7Override *ov = &ppu->m7Override;
  if (!ov->wrap && (xpos > 0x3ffff || ypos > 0x3ffff))
    return false; /* wrapped canvas repetition: keep authentic sampling */
  uint32 cx = xpos >> 8 & 0x3ff, cy = ypos >> 8 & 0x3ff;
  if (cx < (uint32)ov->canvasX0 || cx >= (uint32)ov->canvasX1 ||
      cy < (uint32)ov->canvasY0 || cy >= (uint32)ov->canvasY1)
    return false;
  uint32 span_x = (uint32)(ov->canvasX1 - ov->canvasX0) << 8;
  uint32 span_y = (uint32)(ov->canvasY1 - ov->canvasY0) << 8;
  uint32 fx = ((cx - (uint32)ov->canvasX0) << 8) | (xpos & 0xff);
  uint32 fy = ((cy - (uint32)ov->canvasY0) << 8) | (ypos & 0xff);
  uint32 base = ov->rgba[(size_t)((uint64)fy * ov->height / span_y) *
                             ov->width +
                         (uint64)fx * ov->width / span_x];
  bool remove = (base >> 24) >= 0x80;
  if (sub || y == 0)
    return remove;

  int scale = ppu->m7OverlayScale;
  uint32 pitch = ppu->m7OverlayPitch;
  int surface_extra = ((int)(pitch / sizeof(uint32)) / scale - kPpuXPixels) / 2;
  int column = (screen_x + surface_extra) * scale;
  if (column < 0 || (uint32)(column + scale) > pitch / sizeof(uint32))
    return remove;
  int y_flip = PPU_m7yFlip(ppu) ? -1 : 1;
  int row_dx = y_flip * ppu->m7matrix[1], row_dy = y_flip * ppu->m7matrix[3];
  int brightness = ppu->inidisp & 0xf;

  for (int r = 0; r < scale; r++) {
    uint32 *dst = (uint32 *)(ppu->m7OverlayBuffer +
                             ((size_t)(y - 1) * scale + r) * pitch) + column;
    for (int i = 0; i < scale; i++) {
      uint32 sample_x = xpos + (uint32)(row_dx * r / scale)
                             + (uint32)(dx * i / scale);
      uint32 sample_y = ypos + (uint32)(row_dy * r / scale)
                             + (uint32)(dy * i / scale);
      if (!ov->wrap && (sample_x > 0x3ffff || sample_y > 0x3ffff))
        continue;
      uint32 scx = sample_x >> 8 & 0x3ff, scy = sample_y >> 8 & 0x3ff;
      if (scx < (uint32)ov->canvasX0 || scx >= (uint32)ov->canvasX1 ||
          scy < (uint32)ov->canvasY0 || scy >= (uint32)ov->canvasY1)
        continue;
      uint32 sfx = ((scx - (uint32)ov->canvasX0) << 8) | (sample_x & 0xff);
      uint32 sfy = ((scy - (uint32)ov->canvasY0) << 8) | (sample_y & 0xff);
      uint32 argb = ov->rgba[(size_t)((uint64)sfy * ov->height / span_y) *
                                 ov->width +
                             (uint64)sfx * ov->width / span_x];
      if (brightness != 15) {
        uint32 red = ((argb >> 16 & 0xff) * brightness + 7) / 15;
        uint32 green = ((argb >> 8 & 0xff) * brightness + 7) / 15;
        uint32 blue = ((argb & 0xff) * brightness + 7) / 15;
        argb = (argb & 0xff000000u) | red << 16 | green << 8 | blue;
      }
      dst[i] = argb;
    }
  }
  return remove;
}

static void PpuDrawBackground_mode7(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                    uint y, bool sub, PpuZbufType z) {
  int layer = 0;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);

  // expand 13-bit values to signed values
  int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  uint8 mosaic_enabled = PPU_mosaicEnabled(ppu, 0);
  if (mosaic_enabled)
    y = ppu->mosaicModulo[y];
  uint32 ry = PPU_m7yFlip(ppu) ? 255 - y : y;
  uint32 m7startX = (ppu->m7matrix[0] * clippedH & ~63) + (ppu->m7matrix[1] * ry & ~63) +
    (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8);
  uint32 m7startY = (ppu->m7matrix[2] * clippedH & ~63) + (ppu->m7matrix[3] * ry & ~63) +
    (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int x = win.edges[windex], x2 = win.edges[windex + 1], tile;
    PpuZbufType *dstz = dstbuf->data + x + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + x2 + kPpuExtraLeftRight;
    uint32 rx = PPU_m7xFlip(ppu) ? 255 - x : x;
    uint32 xpos = m7startX + ppu->m7matrix[0] * rx;
    uint32 ypos = m7startY + ppu->m7matrix[2] * rx;
    uint32 dx = PPU_m7xFlip(ppu) ? -ppu->m7matrix[0] : ppu->m7matrix[0];
    uint32 dy = PPU_m7xFlip(ppu) ? -ppu->m7matrix[2] : ppu->m7matrix[2];
    uint32 outside_value = PPU_m7largeField(ppu) ? 0x3ffff : 0xffffffff;
    bool char_fill = PPU_m7charFill(ppu);
    if (mosaic_enabled) {
      int w = PPU_mosaicSize(ppu) - (x - PpuMosaicAt(ppu, x));
      do {
        w = IntMin(w, dstz_end - dstz);
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel) {
          int i = 0;
          do dstz[i] = pixel + z; while (++i != w);
        }
      } while (xpos += dx * w, ypos += dy * w, dstz += w, w = PPU_mosaicSize(ppu), dstz_end - dstz != 0);
    } else if (ppu->m7OverlayBuffer && ppu->m7Override.rgba) {
      int screen_x = x;
      do {
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        if (PpuMode7OverrideSample(ppu, sub, screen_x, y, xpos, ypos,
                                   (int)dx, (int)dy))
          continue;
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel)
          dstz[0] = pixel + z;
      } while (xpos += dx, ypos += dy, ++screen_x, ++dstz != dstz_end);
    } else {
      do {
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel)
          dstz[0] = pixel + z;
      } while (xpos += dx, ypos += dy, ++dstz != dstz_end);
    }
  }
}

static void PpuDrawSprites(Ppu *ppu, uint y, uint sub, bool clear_backdrop) {
  int layer = 4;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int left = win.edges[windex];
    int width = win.edges[windex + 1] - left;
    PpuZbufType *src = ppu->objBuffer.data + left + kPpuExtraLeftRight;
    PpuZbufType *dst = ppu->bgBuffers[sub].data + left + kPpuExtraLeftRight;
    if (clear_backdrop) {
      memcpy(dst, src, width * sizeof(uint16));
    } else {
      do {
        if (src[0] > dst[0])
          dst[0] = src[0];
      } while (src++, dst++, --width);
    }
  }
}

static bool PpuOverlayActiveOnLine(Ppu *ppu, PpuOverlaySource source,
                                   int screen_y) {
  if ((unsigned)source >= kPpuOverlaySource_Count ||
      !ppu->overlayRenderBuffer[source] ||
      !ppu->overlayRenderPitch[source])
    return false;
  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  return capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
         screen_y >= capture->y0 && screen_y < capture->y1;
}

static uint32 PpuOverlayColor(Ppu *ppu, PpuZbufType pixel) {
  /* Isolated layer buffers may contain the priority-only backdrop marker.
   * A zero palette index is still transparent for a captured BG/OBJ plane. */
  if (!(pixel & 0xff)) return 0;
  uint32 color = ppu->cgram[pixel & 0xff];
  return 0xff000000u |
      (uint32)ppu->brightnessMult[color & 0x1f] << 16 |
      (uint32)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
      ppu->brightnessMult[(color >> 10) & 0x1f];
}

/* Captures are per-frame game policy fixed before scanout, so a surface whose
 * capture is inactive receives no writes this frame. Skip its per-line clear
 * when it is already all-transparent (the usual case); a surface written last
 * frame is cleared for one more full frame, then its dirty flag drops on the
 * final line. */
static void PpuClearOverlayRenderLine(Ppu *ppu, uint y) {
  if (y == 0) return;
  int screen_y = (int)y - 1;
  bool last_line = screen_y == 223;
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    uint8_t *pixels = ppu->overlayRenderBuffer[source];
    uint32_t pitch = ppu->overlayRenderPitch[source];
    if (!pixels || !pitch)
      continue;
    const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    bool active = capture->x1 > capture->x0 && capture->y1 > capture->y0;
    if (!active && !ppu->overlayRenderMaybeDirty[source])
      continue;
    memset(pixels + (size_t)screen_y * pitch, 0, pitch);
    if (last_line)
      ppu->overlayRenderMaybeDirty[source] = active;
  }
  if (ppu->m7OverlayBuffer && ppu->m7OverlayPitch) {
    bool active = ppu->m7Override.rgba != NULL;
    if (active || ppu->m7OverlayMaybeDirty) {
      for (int r = 0; r < ppu->m7OverlayScale; r++)
        memset(ppu->m7OverlayBuffer +
                   ((size_t)screen_y * ppu->m7OverlayScale + r) *
                       ppu->m7OverlayPitch,
               0, ppu->m7OverlayPitch);
      if (last_line)
        ppu->m7OverlayMaybeDirty = active;
    }
  }
}

static void PpuWriteOverlayRenderLine(Ppu *ppu, PpuOverlaySource source,
                                      uint y) {
  if (y == 0) return;
  int screen_y = (int)y - 1;
  if (!PpuOverlayActiveOnLine(ppu, source, screen_y))
    return;

  uint32_t pitch = ppu->overlayRenderPitch[source];
  int width = (int)(pitch / sizeof(uint32));
  int texture_extra = IntMax((width - kPpuXPixels) / 2, 0);
  int screen_min = -texture_extra;
  int screen_max = width - texture_extra;
  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  int x0 = IntMax(capture->x0, screen_min);
  int x1 = IntMin(capture->x1, screen_max);
  if (x1 <= x0 || x0 + kPpuExtraLeftRight < 0 ||
      x1 + kPpuExtraLeftRight > kPpuBufWidth)
    return;

  uint32 *dst = (uint32 *)(ppu->overlayRenderBuffer[source] +
                            (size_t)screen_y * pitch);
  const PpuZbufType *src = ppu->overlayBuffers[source].data;
  for (int x = x0; x < x1; x++)
    dst[x + texture_extra] =
        PpuOverlayColor(ppu, src[x + kPpuExtraLeftRight]);
}

static PpuPixelPrioBufs *PpuBeginBackgroundOverlay(Ppu *ppu, uint y,
                                                   bool sub, uint layer) {
  int screen_y = (int)y - 1;
  PpuOverlaySource source = (PpuOverlaySource)layer;
  if (!PpuOverlayActiveOnLine(ppu, source, screen_y))
    return &ppu->bgBuffers[sub];
  memset(&ppu->overlayBuffers[source], 0,
         sizeof(ppu->overlayBuffers[source]));
  return &ppu->overlayBuffers[source];
}

static void PpuFinishBackgroundOverlay(Ppu *ppu, uint y, bool sub,
                                       uint layer,
                                       PpuPixelPrioBufs *layerbuf) {
  PpuOverlaySource source = (PpuOverlaySource)layer;
  if (layerbuf != &ppu->overlayBuffers[source])
    return;

  if (!sub)
    PpuWriteOverlayRenderLine(ppu, source, y);

  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  PpuZbufType *dst = ppu->bgBuffers[sub].data;
  const PpuZbufType *src = layerbuf->data;
  bool remove = (capture->flags & kPpuOverlayFlag_RemoveFromGame) != 0;
  for (int i = 0; i < kPpuBufWidth; i++) {
    int x = i - kPpuExtraLeftRight;
    if (remove && x >= capture->x0 && x < capture->x1)
      continue;
    if (src[i] > dst[i])
      dst[i] = src[i];
  }
}

static void PpuDrawBackgrounds(Ppu *ppu, int y, bool sub) {
  // Top 4 bits contain the prio level, and bottom 4 bits the layer type.
  // SPRITE_PRIO_TO_PRIO can be used to convert from obj prio to this prio.
  //  15: BG3 tiles with priority 1 if bit 3 of $2105 is set
  //  14: Sprites with priority 3 (4 * sprite_prio + 2)
  //  12: BG1 tiles with priority 1
  //  11: BG2 tiles with priority 1
  //  10: Sprites with priority 2 (4 * sprite_prio + 2)
  //  8: BG1 tiles with priority 0
  //  7: BG2 tiles with priority 0
  //  6: Sprites with priority 1 (4 * sprite_prio + 2)
  //  3: BG3 tiles with priority 1 if bit 3 of $2105 is clear
  //  2: Sprites with priority 0 (4 * sprite_prio + 2)
  //  1: BG3 tiles with priority 0
  //  0: backdrop

  if (PPU_mode(ppu) == 1) {
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, true);

    bool mosaic_size = PPU_mosaicSize(ppu) > 1;
    PpuPixelPrioBufs *layerbuf =
        PpuBeginBackgroundOverlay(ppu, y, sub, 0);
    PpuDrawBackground_4bpp_policy(
        ppu, layerbuf, y, sub, 0, 0xc000, 0x8000,
        mosaic_size && PPU_mosaicEnabled(ppu, 0));
    PpuFinishBackgroundOverlay(ppu, y, sub, 0, layerbuf);

    layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 1);
    PpuDrawBackground_4bpp_policy(
        ppu, layerbuf, y, sub, 1, 0xb100, 0x7100,
        mosaic_size && PPU_mosaicEnabled(ppu, 1));
    PpuFinishBackgroundOverlay(ppu, y, sub, 1, layerbuf);

    uint bg3prio = PPU_bg3priority(ppu) ? 0xf200 : 0x3200;
    layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 2);
    if (mosaic_size && PPU_mosaicEnabled(ppu, 2))
      PpuDrawBackground_2bpp_mosaic(ppu, layerbuf, y, sub, 2,
                                    bg3prio, 0x1200);
    else
      PpuDrawBackground_2bpp(ppu, layerbuf, y, sub, 2,
                             bg3prio, 0x1200);
    PpuFinishBackgroundOverlay(ppu, y, sub, 2, layerbuf);
  } else {
    // mode 7
    PpuPixelPrioBufs *layerbuf =
        PpuBeginBackgroundOverlay(ppu, y, sub, 0);
    PpuDrawBackground_mode7(ppu, layerbuf, y, sub, 0x5000);
    PpuFinishBackgroundOverlay(ppu, y, sub, 0, layerbuf);
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, false);
  }
}

static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {
  PpuClearOverlayRenderLine(ppu, y);
  if (PPU_forcedBlank(ppu)) {
    uint8 *dst = &ppu->renderBuffer[(y - 1) * ppu->renderPitch];
    size_t n = sizeof(uint32) * (256 + ppu->extraLeftRight * 2);
    memset(dst, 0, n);
    return;
  }

  // Default background is backdrop
  ClearBackdrop(&ppu->bgBuffers[0]);

  // Render main screen
  PpuDrawBackgrounds(ppu, y, false);

  // Render also the subscreen?
  bool rendered_subscreen = false;
  if (PPU_preventMathMode(ppu) != 3 && PPU_addSubscreen(ppu) && PPU_mathEnabled(ppu)) {
    ClearBackdrop(&ppu->bgBuffers[1]);
    if (ppu->screenEnabled[1] != 0) {
      PpuDrawBackgrounds(ppu, y, true);
      rendered_subscreen = true;
    }
  }

  // Color window affects the drawing mode in each region
  PpuWindows cwin;
  PpuWindows_Calc(&cwin, ppu, 5, y);
  static const uint8 kCwBitsMod[8] = {
    0x00, 0xff, 0xff, 0x00,
    0xff, 0x00, 0xff, 0x00,
  };
  uint32 cw_clip_math = ((cwin.bits & kCwBitsMod[PPU_clipMode(ppu)]) ^ kCwBitsMod[PPU_clipMode(ppu) + 4]) |
    ((cwin.bits & kCwBitsMod[PPU_preventMathMode(ppu)]) ^ kCwBitsMod[PPU_preventMathMode(ppu) + 4]) << 8;

  uint32 *dst = (uint32*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;

  /* Normal scanlines cover only the finite world's live side margins. HUD
   * split lines cover the full presentation budget so their edge-anchored BG3
   * chunks survive at a level boundary. PpuLayerExtra made the color-window
   * spans use the matching full interval above. */
  int composite_left = ppu->extraLeftCur;
  if (ppu->wsHudSplitHeight && y < ppu->wsHudSplitHeight)
    composite_left = ppu->extraLeftRight;
  dst += (ppu->extraLeftRight - composite_left);

  uint32 windex = 0;
  do {
    uint32 left = cwin.edges[windex] + kPpuExtraLeftRight, right = cwin.edges[windex + 1] + kPpuExtraLeftRight;
    // If clip is set, then zero out the rgb values from the main screen.
    uint32 clip_color_mask = (cw_clip_math & 1) ? 0x1f : 0;
    uint32 math_enabled_cur = PPU_mathEnabled(ppu) & ((cw_clip_math & 0x100) ? -1 : 0);
    uint32 fixed_color = ppu->fixedColor;
    if (math_enabled_cur == 0 || fixed_color == 0 && !PPU_halfColor(ppu) && !rendered_subscreen) {
      // Math is disabled (or has no effect), so can avoid the per-pixel maths check
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff];
        dst[0] = ppu->brightnessMult[color & clip_color_mask] << 16 |
          ppu->brightnessMult[(color >> 5) & clip_color_mask] << 8 |
          ppu->brightnessMult[(color >> 10) & clip_color_mask];
      } while (dst++, ++i < right);
    } else {
      uint8 *half_color_map = PPU_halfColor(ppu) ? ppu->brightnessMultHalf : ppu->brightnessMult;
      // Store this in locals
      math_enabled_cur |= PPU_addSubscreen(ppu) << 8 | PPU_subtractColor(ppu) << 9;
      // Need to check for each pixel whether to use math or not based on the main screen layer.
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff], color2;
        uint8 main_layer = (ppu->bgBuffers[0].data[i] >> 8) & 0xf;
        uint32 r = color & clip_color_mask;
        uint32 g = (color >> 5) & clip_color_mask;
        uint32 b = (color >> 10) & clip_color_mask;
        uint8 *color_map = ppu->brightnessMult;
        if (math_enabled_cur & (1 << main_layer)) {
          if (math_enabled_cur & 0x100) {  // addSubscreen ?
            if ((ppu->bgBuffers[1].data[i] & 0xff) != 0)
              color2 = ppu->cgram[ppu->bgBuffers[1].data[i] & 0xff], color_map = half_color_map;
            else  // Don't halve if PPU_addSubscreen(ppu) && backdrop
              color2 = fixed_color;
          } else {
            color2 = fixed_color, color_map = half_color_map;
          }
          uint32 r2 = (color2 & 0x1f), g2 = ((color2 >> 5) & 0x1f), b2 = ((color2 >> 10) & 0x1f);
          if (math_enabled_cur & 0x200) {  // subtractColor?
            r = (r >= r2) ? r - r2 : 0;
            g = (g >= g2) ? g - g2 : 0;
            b = (b >= b2) ? b - b2 : 0;
          } else {
            r += r2;
            g += g2;
            b += b2;
          }
        }
        dst[0] = color_map[b] | color_map[g] << 8 | color_map[r] << 16;
      } while (dst++, ++i < right);
    }
  } while (cw_clip_math >>= 1, ++windex < cwin.nr);

  PpuWriteOverlayRenderLine(ppu, kPpuOverlaySource_Obj, y);

}

static bool ppu_evaluateSprites(Ppu* ppu, int line) {
  static const uint8 spriteSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };

  // TODO: iterate over oam normally to determine in-range sprites,
  //   then iterate those in-range sprites in reverse for tile-fetching
  // TODO: rectangular sprites, wierdness with sprites at -256
  uint8_t index = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) : 0;
  int spritesFound = 0;
  int tilesFound = 0;
  for(int i = 0; i < 128; i++) {
    uint8_t y = ppu->oam[index] >> 8;
    // check if the sprite is on this line and get the sprite size
    uint8_t row = line - y;
    int spriteSize = spriteSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = PPU_objInterlace(ppu) ? spriteSize / 2 : spriteSize;
    if(row < spriteHeight) {
      // in y-range, get the x location, using the high bit as well
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      // SNES OAM x is 9-bit; values >255 normally wrap to negative so sprites
      // can straddle the LEFT edge. In widescreen that wrap would also pull
      // legitimate right-margin sprites (screen x 256..) to the left, hiding
      // them. SMW already emits OAM for sprites out to screen x ~320 (its
      // GetDrawInfo draw window is [-64,320)), so push the wrap boundary past
      // the active right border: values in [256, 256+extraRightCur) stay
      // positive (right margin); [256+extraRightCur, 512) still wrap to the
      // left straddle range. With extraRightCur==0 this is the authentic
      // `if (x > 255) x -= 512`.
      if (x >= 256 + ppu->extraRightCur) x -= 512;
      // if in x-range: include sprites whose body pokes into the visible
      // area, which in widescreen starts at -extraLeftCur (the per-tile
      // gate below clips columns the same way). With extraLeftCur==0 this
      // is the authentic `x > -spriteSize`.
      if(x + spriteSize > -ppu->extraLeftCur) {
        // break if we found 32 sprites already (hardware cap; lifted by
        // kPpuRenderFlags_NoSpriteLimits — wide lines carry more sprites and
        // would hit the authentic cap earlier than a real console's view)
        spritesFound++;
        if(spritesFound > 32 &&
           !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) {
          ppu->rangeOver = true;
          break;
        }
        // update row according to obj-interlace
        if(PPU_objInterlace(ppu)) row = row * 2 + (ppu->evenFrame ? 0 : 1);
        // get some data for the sprite and y-flip row if needed
        int oam1 = ppu->oam[index + 1];
        int tile = oam1 & 0xff;
        int objAdr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
        int palette = (oam1 & 0xe00) >> 9;
        bool hFlipped = oam1 & 0x4000;
        if(oam1 & 0x8000) row = spriteSize - 1 - row;
        // fetch all tiles in x-range
        int paletteBase = 0x80 + 16 * ((oam1 & 0xe00) >> 9);
        int prio = SPRITE_PRIO_TO_PRIO((oam1 & 0x3000) >> 12, (oam1 & 0x800) == 0);
        PpuZbufType z = paletteBase + (prio << 8);

        for(int col = 0; col < spriteSize; col += 8) {
          if(col + x > -8 - ppu->extraLeftCur && col + x < 256 + ppu->extraRightCur) {
            // break if we found 34 8*1 slivers already (hardware cap;
            // lifted by kPpuRenderFlags_NoSpriteLimits, see the 32-sprite cap)
            tilesFound++;
            if(tilesFound > 34 &&
               !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) {
              ppu->timeOver = true;
              break;
            }
            // figure out which tile this uses, looping within 16x16 pages, and get it's data
            int usedCol = oam1 & 0x4000 ? spriteSize - 1 - col : col;
            int usedTile = ((((oam1 & 0xff) >> 4) + (row >> 3)) << 4) | (((oam1 & 0xf) + (usedCol >> 3)) & 0xf);
            uint16 *addr = &ppu->vram[(objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff];
            uint32 plane = addr[0] | (uint32)addr[8] << 16;
            // go over each pixel
            int px_left = IntMax(-(col + x + kPpuExtraLeftRight), 0);
            int px_right = IntMin(256 + kPpuExtraLeftRight - (col + x), 8);
            int slot = index >> 1;
            PpuOverlayCapture *obj_capture =
                &ppu->overlayCaptures[kPpuOverlaySource_Obj];
            bool capture_slot = PpuOverlayActiveOnLine(
                ppu, kPpuOverlaySource_Obj, line) &&
                obj_capture->oamCount && slot >= obj_capture->oamFirst &&
                slot < obj_capture->oamFirst + obj_capture->oamCount;

            for (int px = px_left; px < px_right; px++) {
              int shift = oam1 & 0x4000 ? px : 7 - px;
              uint32 bits = plane >> shift;
              int pixel = (bits >> 0) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8;
              if (pixel != 0) {
                int screen_x = col + x + px;
                int di = screen_x + kPpuExtraLeftRight;
                bool capture_pixel = capture_slot &&
                    screen_x >= obj_capture->x0 &&
                    screen_x < obj_capture->x1;
                if (capture_pixel) {
                  PpuZbufType *overlay =
                      &ppu->overlayBuffers[kPpuOverlaySource_Obj].data[di];
                  if ((*overlay & 0xff) == 0)
                    *overlay = z + pixel;
                }
                if (!capture_pixel ||
                    !(obj_capture->flags & kPpuOverlayFlag_RemoveFromGame)) {
                  PpuZbufType *world = &ppu->objBuffer.data[di];
                  if ((*world & 0xff) == 0)
                    *world = z + pixel;
                }
              }
            }

          }
        }
        if(tilesFound > 34 &&
           !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits))
          break; // break out of sprite-loop if max tiles found
      }
    }
    index += 2;
  }
  return tilesFound != 0;
}

static uint16_t ppu_getVramRemap(Ppu* ppu) {
  uint16_t adr = ppu->vramPointer;
  switch(ppu->vramRemapMode) {
    case 0: return adr;
    case 1: return (adr & 0xff00) | ((adr & 0xe0) >> 5) | ((adr & 0x1f) << 3);
    case 2: return (adr & 0xfe00) | ((adr & 0x1c0) >> 6) | ((adr & 0x3f) << 3);
    case 3: return (adr & 0xfc00) | ((adr & 0x380) >> 7) | ((adr & 0x7f) << 3);
  }
  return adr;
}

uint8_t ppu_read(Ppu* ppu, uint8_t adr) {
  switch(adr) {
  case 0x34:
  case 0x35:
  case 0x36: {
    int result = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
    return (result >> (8 * (adr - 0x34))) & 0xff;
  }
    case 0x37: {
      /* Frame-level recomp runs do not model per-scanline PPU time, but
       * games still use SLHV/OPVCT busy-waits to wait until late visible
       * scanlines before touching HDMA/window tables. Latch a stable
       * late-line value so those waits can complete. */
      ppu->hCount = 0;
      ppu->vCount = 0xc0;
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      ppu->countersLatched = true;
      return 0;
    }
    case 0x38: {
      uint8_t ret = 0;
      if(ppu->oamInHigh) {
        ret = ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite];
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ret = ppu->oam[ppu->oamAdr] & 0xff;
        } else {
          ret = ppu->oam[ppu->oamAdr++] >> 8;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      return ret;
    }
    case 0x39: {
      uint16_t val = ppu->vramReadBuffer;
      /* AR_BAFREAD=1: log the source addr + returned word of BAF5's $2139
       * readback, host-frame gated (AR_HF_LO/HI), to see the true read region. */
      { static int en=-1; static long lo,hi; if(en<0){en=getenv("AR_BAFREAD")?1:0;
          const char*a=getenv("AR_HF_LO"),*b=getenv("AR_HF_HI");
          lo=a?atol(a):-1; hi=b?atol(b):-1;}
        if(en){ extern const char *g_last_recomp_func; extern int snes_frame_counter;
          const char*f=g_last_recomp_func;
          if(f&&f[8]=='B'&&f[9]=='A'&&f[10]=='F'&&(lo<0||(snes_frame_counter>=lo&&(hi<0||snes_frame_counter<=hi)))){
            static int nl; if(nl++<64)
              fprintf(stderr,"[bafread] hf=%d vramPtr=$%04x readBuf=$%04x remap=$%04x\n",
                snes_frame_counter, ppu->vramPointer, ppu->vramReadBuffer, ppu_getVramRemap(ppu)&0x7fff); } } }
      if(!ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      return val & 0xff;
    }
    case 0x3a: {
      uint16_t val = ppu->vramReadBuffer;
      if(ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      return val >> 8;
    }
    case 0x3b: {
      uint8_t ret = 0;
      if(!ppu->cgramSecondWrite) {
        ret = ppu->cgram[ppu->cgramPointer] & 0xff;
      } else {
        ret = ((ppu->cgram[ppu->cgramPointer++] >> 8) & 0x7f);
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      return ret;
    }
    case 0x3c: {
      uint8_t val = 0;
      if(ppu->hCountSecond) {
        val = ((ppu->hCount >> 8) & 1);
      } else {
        val = ppu->hCount & 0xff;
      }
      ppu->hCountSecond = !ppu->hCountSecond;
      return val;
    }
    case 0x3d: {
      uint8_t val = 0;
      if(ppu->vCountSecond) {
        val = ((ppu->vCount >> 8) & 1);
      } else {
        val = ppu->vCount & 0xff;
      }
      ppu->vCountSecond = !ppu->vCountSecond;
      return val;
    }
    case 0x3e: {
      uint8_t val = 0x1; // ppu1 version (4 bit)
      val |= ppu->rangeOver << 6;
      val |= ppu->timeOver << 7;
      return val;
    }
    case 0x3f: {
      uint8_t val = 0x3; // ppu2 version (4 bit), bit 4: ntsc/pal
      val |= ppu->countersLatched << 6;
      val |= ppu->evenFrame << 7;
      ppu->countersLatched = false; // TODO: only when ppulatch is set
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      return val;
    }
    default: {
      /* Invalid/write-only PPU register read (e.g. $2100-$2133).
       * Almost always a side effect of a misconfigured DMA/garbage
       * read on the boot path. Return open-bus 0 instead of crashing
       * so the game keeps progressing; log once per register. */
      {
        extern const char *g_last_recomp_func;
        static uint64_t seen_mask;
        static int bt_dumped;
        if (adr < 64 && !(seen_mask & (1ull << adr))) {
          seen_mask |= (1ull << adr);
          fprintf(stderr, "[ppu_read] invalid read of $21%02X (open-bus 0) "
                  "from %s\n", adr, g_last_recomp_func ? g_last_recomp_func : "?");
          if (!bt_dumped) {
            bt_dumped = 1;
            void *bt[24];
            int n = backtrace(bt, 24);
            backtrace_symbols_fd(bt, n, 2);
            /* Snapshot the dispatch ring at the first garbage access — the
             * crash usually aborts before DumpDiagState, so capture the
             * last-N dispatches feeding into the leak right here. */
            extern void CpuDispatchLogWriteFile(const char *path);
            const char *rd = getenv("AR_RUN_DIR");  /* per-run dir (run_dir.c) */
            char dlpath[300];
            snprintf(dlpath, sizeof dlpath, "%s/crash_dispatch_log.json",
                     rd && rd[0] ? rd : "saves");
            CpuDispatchLogWriteFile(dlpath);
            fprintf(stderr, "[ppu_read] dispatch ring -> %s\n", dlpath);
          }
        }
      }
      return 0;
    }
  }
}

void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {
//  if (adr != 24 && adr != 25)
//    printf("ppu_write(%d, %d)\n", adr, val);
  switch(adr) {
    case INIDISP & 0xff:
      if (getenv("AR_INIDISP2") && val != ppu->inidisp) {
        extern int snes_frame_counter; extern uint8 g_ram[0x20000];
        extern const char *g_last_recomp_func;
        fprintf(stderr, "[inidisp2] f=%d $2100 %02x->%02x (bright=%d fblank=%d) $18=%02x by=%s\n",
          snes_frame_counter, ppu->inidisp, val, val & 0xf, (val & 0x80) ? 1 : 0, g_ram[0x18],
          g_last_recomp_func ? g_last_recomp_func : "?");
      }
      ppu->inidisp = val;
      if (ar_trace_active()) ar_trace_reg(0x2100, val); /* brightness + forced-blank */
      break;
    case OBSEL & 0xff:
      ppu->obsel = val;
      break;
    case OAMADDL & 0xff:
      ppu->oamaddl = val;
      ppu->oamAdr = val;
      ppu->oamInHigh = ppu->oamaddh & 1;
      ppu->oamSecondWrite = false;
      break;
    case OAMADDH & 0xff:
      ppu->oamaddh = val;
      ppu->oamInHigh = val & 1;
      ppu->oamAdr = ppu->oamaddl;
      ppu->oamSecondWrite = false;
      break;
    case 0x04: {
      if(ppu->oamInHigh) {
        int hidx = ((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite;
        ppu->highOam[hidx] = val;
        debug_server_on_oam_write(1, (uint16_t)hidx, (uint16_t)val);
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ppu->oamBuffer = val;
        } else {
          uint16_t widx = ppu->oamAdr;
          uint16_t word = (uint16_t)((val << 8) | ppu->oamBuffer);
          ppu->oam[ppu->oamAdr++] = word;
          debug_server_on_oam_write(0, widx, word);
          if (ar_trace_active()) ar_trace_ppumem("oam", widx, word); /* sprite word */
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      break;
    }
    case BGMODE & 0xff:
      assert((val & 0xf0) == 0);
      ppu->bgmode = val;
      if (ar_trace_active()) ar_trace_reg(0x2105, val);
      break;
    case MOSAIC & 0xff:
      ppu->mosaic = val;
      if (ar_trace_active()) ar_trace_reg(0x2106, val);
      ppu->mosaicStartLine = 0;// ppu->snes->vPos;
      break;
    case BG1SC & 0xff:
    case BG2SC & 0xff:
    case BG3SC & 0xff:
    case BG4SC & 0xff:
      ppu->bgXsc[adr - 7] = val;
      if (ar_trace_active()) ar_trace_reg(0x2100 | adr, val);
      break;
    case BG12NBA & 0xff:
      ppu->bgTileAdr = ppu->bgTileAdr & 0xff00 | val;
      if (ar_trace_active()) ar_trace_reg(0x210B, val);
      break;
    case BG34NBA & 0xff:
      ppu->bgTileAdr = ppu->bgTileAdr & 0xff | val << 8;
      if (ar_trace_active()) ar_trace_reg(0x210C, val);
      break;
    case 0x0d: {
      ppu->m7matrix[6] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-HOFS
    }
    case 0x0f:
    case 0x11:
    case 0x13: {
      ppu->hScroll[(adr - 0xd) / 2] = ((val << 8) | (ppu->scrollPrev & 0xf8) | (ppu->scrollPrev2 & 0x7)) & 0x3ff;
      ppu->scrollPrev = val;
      ppu->scrollPrev2 = val;
      break;
    }
    case 0x0e: {
      ppu->m7matrix[7] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-VOFS
    }
    case 0x10:
    case 0x12:
    case 0x14: {
      ppu->vScroll[(adr - 0xe) / 2] = ((val << 8) | ppu->scrollPrev) & 0x3ff;
      ppu->scrollPrev = val;
      break;
    }
    case 0x15: {
      if((val & 3) == 0) {
        ppu->vramIncrement = 1;
      } else if((val & 3) == 1) {
        ppu->vramIncrement = 32;
      } else {
        ppu->vramIncrement = 128;
      }
      ppu->vramRemapMode = (val & 0xc) >> 2;
      ppu->vramIncrementOnHigh = val & 0x80;
      break;
    }
    case 0x16: {
      ppu->vramPointer = (ppu->vramPointer & 0xff00) | val;
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x17: {
      ppu->vramPointer = (ppu->vramPointer & 0x00ff) | (val << 8);
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      if (ar_trace_active()) ar_trace_vmadd(ppu->vramPointer, "2117");
      /* AR_BAFSRC=1: log the VRAM address BAF5 sets before its readback
       * ($7F:B800 fill via $2139). Reveals whether the readback source is the
       * graphics region ($0000) or a wrong tilemap region ($6800) at the seal. */
      { static int en = -1; if (en < 0) en = getenv("AR_BAFSRC") ? 1 : 0;
        if (en) { extern const char *g_last_recomp_func; extern uint8 g_ram[0x20000];
          const char *f = g_last_recomp_func;
          if (f && (f[8]=='B'&&f[9]=='A'&&f[10]=='F')) {
            unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
            static unsigned lastgf = 0xffffffff;
            if (gf != lastgf) { lastgf = gf;
              fprintf(stderr, "[bafsrc] gf=%u vramPointer=$%04x func=%s\n",
                      gf, ppu->vramPointer, f); } } } }
      /* AR_VMADD=1: log every VMADD ($2116/$2117) set with func + block PC,
       * host-frame gated (AR_HF_LO/HI) — trace the crossed VRAM pointer. */
      { static int en=-1; static long lo,hi; if(en<0){en=getenv("AR_VMADD")?1:0;
          const char*a=getenv("AR_HF_LO"),*b=getenv("AR_HF_HI");
          lo=a?atol(a):-1; hi=b?atol(b):-1;}
        if(en){ extern const char *g_last_recomp_func; extern int snes_frame_counter;
          extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
          if(lo<0||(snes_frame_counter>=lo&&(hi<0||snes_frame_counter<=hi))){
            static int nl; if(nl++<400){
              uint32_t blk=g_ar_blk_ring[(g_ar_blk_idx-1u)&1023u];
              fprintf(stderr,"[vmadd] hf=%d VMADD=$%04x blk=$%06X func=%s\n",
                snes_frame_counter, ppu->vramPointer, blk,
                g_last_recomp_func?g_last_recomp_func:"?"); } } } }
      break;
    }
    case 0x18: {
      // TODO: vram access during rendering (also cgram and oam)
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;
      // $2118 == low byte of word; byte_addr = word << 1.
      debug_server_on_vram_write(((uint32_t)(vramAdr & 0x7fff) << 1), val);
      /* AR_VRAMWATCH=1 (2026-07-05): lair-seal tilemap corruption. Log every
       * VRAM write into the BG tilemap region [$0000,$1000) with the game func
       * that issued it, within a frame window (AR_VW_LO/AR_VW_HI, game-frame),
       * to catch who scatters the lair tiles. Rate-limited. */
      {
        extern int ar_vramwatch(uint16_t vaddr, uint8_t val);
        ar_vramwatch(vramAdr & 0x7fff, val);
      }
      ar_vramraw(vramAdr & 0x7fff, val, 0x18);
      if (ar_trace_active()) ar_trace_vram(vramAdr & 0x7fff, val, "b18");
      if(!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x19: {
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);
      ar_vramraw(vramAdr & 0x7fff, val, 0x19);
      if (ar_trace_active()) ar_trace_vram(vramAdr & 0x7fff, val, "b19");
      // $2119 == high byte of word; byte_addr = (word << 1) + 1.
      debug_server_on_vram_write(((uint32_t)(vramAdr & 0x7fff) << 1) + 1, val);
      if(ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case M7SEL & 0xff:
      ppu->m7sel = val;
      break;
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e:
      ppu->m7matrix[adr - 0x1b] = (val << 8) | ppu->m7prev;
      ppu->m7prev = val;
      break;
    case 0x1f:
    case 0x20:
      ppu->m7matrix[adr - 0x1b] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      break;
    case 0x21:
      ppu->cgramPointer = val;
      ppu->cgramSecondWrite = false;
      break;
    case 0x22:
      if(!ppu->cgramSecondWrite) {
        ppu->cgramBuffer = val;
      } else {
        uint16_t centry = ppu->cgramPointer;
        uint16_t cval = (val << 8) | ppu->cgramBuffer;
        ppu->cgram[ppu->cgramPointer++] = cval;
        if (ar_trace_active()) ar_trace_ppumem("cgram", centry, cval); /* palette word */
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      break;
    case 0x23:
      ppu->windowsel = (ppu->windowsel & ~0xff) | val;
      break;
    case 0x24:
      ppu->windowsel = (ppu->windowsel & ~0xff00) | (val << 8);
      break;
    case 0x25:
      ppu->windowsel = (ppu->windowsel & ~0xff0000) | (val << 16);
      break;
    case 0x26:
      ppu->window1left = val;
      break;
    case 0x27:
      ppu->window1right = val;
      break;
    case 0x28:
      ppu->window2left = val;
      break;
    case 0x29:
      ppu->window2right = val;
      break;
    case WBGLOG & 0xff:
      ppu->wbgobjlog = ppu->wbgobjlog & 0xff00 | val;
      break;
    case WOBJLOG & 0xff:
      ppu->wbgobjlog = ppu->wbgobjlog & 0xff | val << 8;
      break;
    case TM & 0xff:
      ppu->screenEnabled[0] = val;
      if (ar_trace_active()) ar_trace_reg(0x212C, val); /* main-screen layer enable */
      break;
    case TS & 0xff:
      ppu->screenEnabled[1] = val;
      if (ar_trace_active()) ar_trace_reg(0x212D, val); /* sub-screen layer enable */
      break;
    case TMW & 0xff:
      ppu->screenWindowed[0] = val;
      break;
    case TSW & 0xff:
      ppu->screenWindowed[1] = val;
      break;
    case CGWSEL & 0xff:
      ppu->cgwsel = val;
      break;
    case CGADSUB & 0xff:
      ppu->cgadsub = val;
      break;
    case COLDATA & 0xff:
      if (val & 0x80) ppu->fixedColor = (ppu->fixedColor & ~(0x1f << 10)) | (val & 0x1f) << 10; // blue
      if (val & 0x40) ppu->fixedColor = (ppu->fixedColor & ~(0x1f <<  5)) | (val & 0x1f) << 5;  // green
      if (val & 0x20) ppu->fixedColor = (ppu->fixedColor & ~(0x1f <<  0)) | (val & 0x1f) << 0;  // red
      break;
    case SETINI & 0xff:
      ppu->setini = val;
      break;
    default:
      break;
  }
}

int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags) {
  return 1;
}
