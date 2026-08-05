#ifndef PPU_H
#define PPU_H

#include "../types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "saveload.h"

typedef struct Ppu Ppu;

typedef struct BgLayer {
  uint16_t xhScroll;
  uint16_t xvScroll;
  bool xtilemapWider;
  bool xtilemapHigher;
  uint16_t xtilemapAdr;
  uint16_t xtileAdr;
  bool xxbigTiles;
  bool xxmosaicEnabled;
} BgLayer;

enum {
  kPpuXPixels = 256,
  // Maximum widescreen expansion *per side*, baked into the priority-buffer
  // capacity. This is a compile-time ceiling only; the actual extra columns
  // rendered each frame are the runtime ppu->extraLeftCur/extraRightCur, which
  // default to 0 (authentic 256-wide output). 96 per side allows up to a
  // 448-pixel internal width, comfortably past 16:9 at 224 lines.
  kPpuExtraLeftRight = 96,
  // Full internal width of the priority buffers (logical 256 + both borders).
  kPpuBufWidth = kPpuXPixels + kPpuExtraLeftRight * 2,
  // Authentic visible scanline count. Distinct from kPpuXPixels' axis: 224 is
  // how many lines are OUTPUT, and it is also the threshold above which an OAM
  // Y byte is a negative (offscreen-top) position -- see kPpuObjYWrap.
  kPpuYPixels = 224,
  // Maximum vertical expansion *per side*, the compile-time ceiling for the
  // runtime ppu->extraTopCur/extraBottomCur (default 0 = authentic 224-line
  // output). 32 is not an arbitrary budget: OAM Y is EIGHT bits with a 256
  // modulus against a 224-line screen, so the only sprite positions outside
  // the visible band that the encoding can express at all are the 32 in
  // [kPpuObjYNegativeFrom, kPpuObjYWrap) -- which is exactly the above-screen
  // band. Asking for more top margin than this renders backgrounds with no
  // sprites on them; asking for bottom margin re-uses that same range and is
  // therefore ambiguous with above-screen sprites (see ppu_evaluateSprites).
  // Contrast the horizontal axis, where X is NINE bits (kPpuObjXWrap = 512)
  // against a 256-wide screen and the whole upper half is free for margins.
  kPpuExtraTopBottom = 32,
  // Full internal height of the render target (224 + both bands).
  kPpuBufHeight = kPpuYPixels + kPpuExtraTopBottom * 2,
  // OAM sprite POSITION WRAPS. These are properties of the OAM encoding, not of
  // the screen size, and they are deliberately named separately from
  // kPpuXPixels/kPpuYPixels even though two of them share those values:
  //
  //   X is 9 bits, so a sprite straddling the left edge is encoded as a large
  //   positive value; subtracting the 512 modulus recovers the negative screen
  //   position. The 512 is 2^9 -- the encoding's range -- NOT two screens.
  //
  //   Y is 8 bits with no sign bit, so the hardware treats anything at or above
  //   224 as negative and subtracts the 256 modulus (2^8). The 224 threshold
  //   happens to equal kPpuYPixels and the 256 modulus happens to equal
  //   kPpuXPixels; both coincidences, and writing them as those constants would
  //   imply they track the screen geometry, which they do not.
  kPpuObjXWrap = 512,   // 2^9: the OAM X field's modulus
  kPpuObjYWrap = 256,   // 2^8: the OAM Y field's modulus
  kPpuObjYNegativeFrom = 224,  // Y at/above this is a negative position
  // CGRAM entries. 256 because a palette index is one byte -- unrelated to
  // kPpuXPixels sharing the value.
  kPpuCgramEntries = 0x100,
  // Distinct OBJ tile ids. 256 because the OAM attribute's tile field is one
  // byte (the 9th bit selects which of the two name-base tables it indexes, so
  // it widens the ADDRESS space, not the id space).
  kPpuObjTileIds = 256,
  // OAM low table, in 16-bit words: 128 sprites x 2 words. 256 for that reason,
  // not because it matches any pixel dimension.
  kPpuOamWords = 0x100,
};

typedef uint16_t PpuZbufType;

typedef struct PpuPixelPrioBufs {
  // This holds the prio in the upper 8 bits and the color in the lower 8 bits.
  // Sized for the widescreen border; logical screen x maps to
  // data[x + kPpuExtraLeftRight].
  PpuZbufType data[kPpuBufWidth];
} PpuPixelPrioBufs;

/* Renderer-neutral host-overlay extraction. BG source values deliberately
 * match the PPU layer indices; OBJ is the fifth screen layer. Each source can
 * own one screen-space capture rectangle and one full-frame ARGB destination
 * surface. A caller can crop several independently placed graphics from one
 * captured bounding rectangle after scanout. */
typedef enum PpuOverlaySource {
  kPpuOverlaySource_Bg1 = 0,
  kPpuOverlaySource_Bg2 = 1,
  kPpuOverlaySource_Bg3 = 2,
  kPpuOverlaySource_Bg4 = 3,
  kPpuOverlaySource_Obj = 4,
  kPpuOverlaySource_Count = 5,
} PpuOverlaySource;

enum {
  /* Do not merge captured pixels back into either main or subscreen. The host
   * can then reinsert them without a duplicate remaining in renderBuffer. */
  kPpuOverlayFlag_RemoveFromGame = 1,
  /* Preserve the SNES OBJ color-math eligibility in captured ARGB alpha:
   * palette groups 4-7 use alpha $80, groups 0-3 remain opaque. This is an
   * extraction-only annotation; it never changes authentic PPU scanout. */
  kPpuOverlayFlag_MarkObjColorMath = 2,
  /* Same idea for a BG plane: this layer is HALF-ADDED with the subscreen, so
   * its captured pixels carry alpha $80 and a host that composites the planes
   * in depth order reproduces the blend as ordinary alpha. (main+sub)/2 is
   * exactly a 50% source-over of the layer onto what is behind it, which is
   * what the subscreen holds.
   *
   * Without this, a half-added BG is captured fully opaque and hides the planes
   * behind it instead of tinting them -- the "background lost its transparency"
   * report from 2026-07-26, measured in Fillmore act 2 as cgwsel=$02
   * cgadsub=$43 (half, add, subscreen addend, math on BG1+BG2).
   *
   * The caller decides eligibility per source, because it is not simply "the
   * CGADSUB bit is set": a layer that is ALSO on the subscreen is half-added
   * with itself, which is the identity, and must stay opaque. In that measured
   * frame BG1 is on the subscreen and BG2 is not, so only BG2 is marked.
   * Extraction-only, like the OBJ flag: authentic scanout is untouched. */
  kPpuOverlayFlag_MarkBgHalfAdd = 4,
};

/* Mode-7 canvas-space texture override (per-frame game policy, cleared with
 * the captures). While active, main-screen Mode-7 BG1 pixels whose canvas
 * coordinates fall inside [canvasX0,canvasX1)x[canvasY0,canvasY1) and whose
 * texture sample is opaque are removed from the game frame (main+sub) and
 * rendered instead — through the live matrix, per-scanline HDMA included —
 * into the bound Mode-7 overlay surface at `scale` subsamples per axis. */
/* Extent of one Mode-7 canvas instance, in canvas pixels. The matrix samples a
 * 128x128 tilemap of 8x8 tiles, so a single instance spans 1024 in each axis and
 * repeats beyond that. Distinct from any screen or texture dimension: it is the
 * addressable space the transform reads, not anything that gets displayed at that
 * size. Consumers validating a canvas rect bound against this. */
enum { kPpuMode7CanvasExtent = 1024 };

typedef struct PpuMode7Override {
  const uint32_t *rgba; /* ARGB words, width*height, row-major */
  int width, height;
  int canvasX0, canvasY0, canvasX1, canvasY1; /* canvas px, x1/y1 exclusive */
  /* 0: substitute only the primary [0,1024) canvas instance — wrapped
   * repetitions keep the authentic sparse tile sampling (a zoomed-out wrap
   * renders faint speckle on hardware; solid supersampled copies there
   * would be a fidelity break). 1: substitute every wrapped instance, for
   * canvases that genuinely tile. */
  uint8_t wrap;
} PpuMode7Override;

typedef struct PpuOverlayCapture {
  /* SNES screen coordinates after scroll/window/mosaic processing. X may be
   * negative or exceed 255 when a widescreen margin is active. Endpoints are
   * exclusive. y uses visible output coordinates (0 is the first scanline). */
  int16_t x0, x1;
  int16_t y0, y1;
  uint8_t flags;
  /* OBJ-only selector. A zero count captures no objects. Games validate any
   * semantic identity (HUD icon, portrait, etc.) before supplying the range. */
  uint8_t oamFirst, oamCount;
} PpuOverlayCapture;

/* Screen-space bounds of one semantic OAM range. Endpoints are exclusive.
 * The range raster API below decodes complete OBJ graphics (including pixels
 * outside the current visible viewport) so a game can pack them into an
 * isolated host atlas without cropping the already-composited framebuffer. */
typedef struct PpuObjRangeBounds {
  int16_t x0, y0, x1, y1;
} PpuObjRangeBounds;

enum {
  kPpuRenderFlags_NewRenderer = 1,
  // Render mode7 upsampled by 4x4
  kPpuRenderFlags_4x4Mode7 = 2,
  // Use 240 height instead of 224
  kPpuRenderFlags_Height240 = 4,
  // Disable sprite render limits
  kPpuRenderFlags_NoSpriteLimits = 8,
};

typedef struct Layer {
  bool xmainScreenEnabled;
  bool xsubScreenEnabled;
  bool xmainScreenWindowed;
  bool xsubScreenWindowed;
} Layer;

typedef struct WindowLayer {
  bool xwindow1enabled;
  bool xwindow2enabled;
  bool xwindow1inversed;
  bool xwindow2inversed;
  uint8_t xmaskLogic;
} WindowLayer;

#define PPU_SAVESTATE_REGS_SIZE 0x40
#define PPU_SAVESTATE_MEM_SIZE 0x10420

struct Ppu {
  // Snes registers. Saved to snapshot. Need to be stable
  // -- START OF SNAPSHOT, 0x40 bytes
  uint8 inidisp;
  uint8 obsel;
  uint8 oamaddl;
  uint8 oamaddh;
  uint8 bgmode;
  uint8 mosaic;
  uint8 bgXsc[4];
  uint16 bgTileAdr;
  uint8 m7sel;
  uint8 setini;
  uint16 hScroll[4];
  uint16 vScroll[4];
  int16_t m7matrix[8]; // a, b, c, d, x, y, h, v
  uint16 fixedColor;
  uint32 windowsel;
  uint8 window1left;
  uint8 window1right;
  uint8 window2left;
  uint8 window2right;
  uint16 wbgobjlog;
  uint8 screenEnabled[2];
  uint8 screenWindowed[2];
  uint8 cgadsub;
  uint8 cgwsel;
  // -- END OF SNAPSHOT

  // vram access
  uint16_t vramPointer;
  bool vramIncrementOnHigh;
  uint8_t vramRemapMode;
  uint8_t vramIncrement;
  uint16_t vramReadBuffer;
  // cgram access
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  // oam access
  uint8_t oamAdr;
  bool oamInHigh;
  bool oamSecondWrite;
  uint8_t oamBuffer;
  bool timeOver;
  bool rangeOver;
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  uint8_t mosaicStartLine;
  uint8_t m7prev;
  // mode 7 internal
  int32_t m7startX;
  int32_t m7startY;
  // settings
  bool evenFrame;
  bool frameOverscan; // if we are overscanning this frame (determined at 0,225)
  bool frameInterlace; // if we are interlacing this frame (determined at start vblank)
  // latching
  uint16_t hCount;
  uint16_t vCount;
  bool hCountSecond;
  bool vCountSecond;
  bool countersLatched;
  // pixel buffer (xbgr)
  // times 2 for even and odd frame

  uint8_t extraLeftCur, extraRightCur, extraLeftRight;
  // Vertical margin, the transpose of extraLeftCur/extraRightCur: scanlines
  // rendered ABOVE line 1 and BELOW line 224, showing world the authentic
  // viewport crops. Both clamp to kPpuExtraTopBottom and default to 0, so a
  // caller that never touches them gets bit-identical 224-line output.
  //
  // Unlike the horizontal pair there is no separate centering budget: the
  // render target is always allocated for the full kPpuExtraTopBottom band
  // (see PpuVerticalOrigin), because nothing needs to pillarbox vertically --
  // a host that wants fewer lines just crops the ones it asked for.
  uint8_t extraTopCur, extraBottomCur;
  // Per-slot exact OAM Y from the frontend. See PpuSetObjYOverride.
  int16_t objYOverride[128];
  uint8_t objYOverrideValid[128];
  // Widescreen HUD split (see PpuSetWidescreenHudSplit). 0 height = off.
  uint8_t wsHudSplitHeight, wsHudLeftEnd, wsHudRightStart;
  uint8_t wsHudPlayerRowY;
  uint8_t wsHudLeftOnlyY;
  // Widescreen BG3 widen (see PpuSetWidescreenBg3Widen). Scanlines >= this let
  // BG3 (layer 2) extend into the side margins like BG1/BG2 instead of staying
  // clamped to the authentic 256-wide region. 0 = off (BG3 clamped everywhere,
  // so a BG3 status bar never tiles into the margins). SMW sets it to the HUD
  // band height so water/level content on BG3 below the bar fills 16:9.
  uint8_t wsBg3WidenY;
  // Widescreen: synthesize mirror/repeat padding out to the full centering
  // budget (extraLeftRight) for CAPTURED layers, instead of stopping at the
  // live per-side margin (extraLeftCur/extraRightCur). See
  // PpuSetWidescreenPadCapturedToBudget. 0 = off (padding stops at the live
  // margin, the pre-existing behaviour for every path).
  uint8_t wsPadCapturedToBudget;
  // Widescreen per-layer clamp (see PpuSetWidescreenLayerClamp). Bit L set =>
  // BGL+1 (layer L, 0..3) is clamped to the authentic 256-wide region even in
  // widescreen. For UI/dialog/status layers whose tilemap is only 256 wide, so
  // they never tile wrapped/garbage columns into the margins while the world
  // layers beside them stay wide. 0 = every layer extended (default).
  uint8_t wsLayerClamp;
  // Widescreen per-layer mirror fill (see PpuSetWidescreenLayerMirror). Bit L
  // keeps BGL+1 authentic in the center, then reflects its rendered edge pixels
  // into the side margins. Used for decorative 256-wide layers that have no
  // real offscreen world data. 0 = disabled.
  uint8_t wsLayerMirror;
  // Widescreen per-layer repeat fill (see PpuSetWidescreenLayerRepeat). Bit L
  // keeps BGL+1 authentic in the center, then cyclically continues that
  // rendered 256px scanline into the margins. Unlike reflection, this keeps
  // raster/HDMA parallax moving in the same direction across the seam.
  uint8_t wsLayerRepeat;
  // Per-layer widescreen clamp BAND (see PpuSetWidescreenLayerClampBand): on
  // scanlines [y0,y1) layer L is clamped to the authentic 256, while it still
  // extends into the margins outside that band. This is the generic "overlay
  // plane" ("BG2.5"): a bounded UI element (dialog box, menu panel) sharing a
  // layer with genuinely-wide world content (pillars) — clamp only the rows the
  // UI occupies so the world layer stays wide above/below it. y1<=y0 = off.
  uint8_t wsClampY0[4], wsClampY1[4];
  // Per-layer cyclic-repeat BAND (see PpuSetWidescreenLayerRepeatBand): on
  // scanlines [y0,y1), repeat the authentic rendered scanline into both
  // margins. This takes precedence over a whole-layer clamp on those rows,
  // allowing animated/raster content to share a BG with bounded scenery.
  uint8_t wsRepeatY0[4], wsRepeatY1[4];
  // Widescreen margin source gap per layer, in pixels per side (see
  // PpuSetWidescreenLayerMarginGap): margins skip the first N offscreen
  // columns (games' UI staging area) and sample the tilemap beyond them.
  uint8_t wsMarginGapL[4], wsMarginGapR[4];
  uint8_t lastMosaicModulo;
  uint8_t lastBrightnessMult;
  bool lineHasSprites;
  // kPpuRenderFlags_* for this session (PpuBeginDrawing). NoSpriteLimits
  // lifts the hardware 32-sprites/34-tiles per-scanline caps — on widescreen
  // lines with more sprites visible, the authentic caps clip sprites EARLIER
  // than a real console would relative to the wider view.
  uint32_t renderFlags;
  PpuPixelPrioBufs bgBuffers[2];
  PpuPixelPrioBufs objBuffer;
  /* Per-source isolated priority pixels for generic host-overlay captures. */
  PpuPixelPrioBufs overlayBuffers[kPpuOverlaySource_Count];
  PpuOverlayCapture overlayCaptures[kPpuOverlaySource_Count];
  uint32_t renderPitch;
  uint8_t *renderBuffer;
  uint32_t overlayRenderPitch[kPpuOverlaySource_Count];
  uint8_t *overlayRenderBuffer[kPpuOverlaySource_Count];
  /* Optional priority-band split surfaces (PpuBindOverlayPrioSurface). When
   * bound, scanout routes each captured pixel of the source to the band
   * matching its hardware priority instead of the primary surface: OBJ bands
   * 1..3 receive sprite priorities 1..3 (primary keeps priority 0); BG band 1
   * receives priority-1 tiles (primary keeps priority 0). Bands share the
   * primary's pitch and capture rectangle. */
  uint8_t *overlayRenderBands[kPpuOverlaySource_Count][3];
  /* Overlay surfaces are cleared lazily: a surface whose capture is inactive
   * and whose flag here is clear is already all-transparent, so its
   * per-scanline clear can be skipped (the common case — captures are rare).
   * Set when a frame ends with the capture active (content was written) and
   * on (re)bind, since a caller-provided buffer's contents are unknown. */
  uint8_t overlayRenderMaybeDirty[kPpuOverlaySource_Count];
  /* Exact content metadata for the most recently rendered frame. Bit 0 is the
   * primary surface; bits 1..3 are the optional priority-band surfaces.
   * Cleared at frame start and set only when scanout routes a nontransparent
   * pixel to that destination. */
  uint8_t overlayRenderContentMask[kPpuOverlaySource_Count];
  uint8_t m7OverlayMaybeDirty;
  /* Mode-7 override: persistent scaled surface binding + per-frame policy. */
  uint8_t *m7OverlayBuffer;
  uint32_t m7OverlayPitch;
  uint8_t m7OverlayScale;
  PpuMode7Override m7Override;
  uint8_t brightnessMult[32 + 31];
  uint8_t brightnessMultHalf[32 * 2];
  uint8_t mosaicModulo[kPpuXPixels];

  void *pad2;

  // -- START OF SNAPSHOT, 0x10420 bytes
  uint16_t cgram[kPpuCgramEntries];
  uint16_t oam[kPpuOamWords];
  uint8_t highOam[0x20];
  uint16_t vram[0x8000];
  // -- END OF SNAPSHOT


};

#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + (level6 ? 2 : 0))
#define SPRITE_PRIO_TO_PRIO_HI(prio) ((prio) * 4 + 2)

#define IS_SCREEN_ENABLED(ppu, sub, layer) (ppu->screenEnabled[sub] & (1 << layer))
#define IS_SCREEN_WINDOWED(ppu, sub, layer) (ppu->screenWindowed[sub] & (1 << layer))
#define GET_WINDOW_FLAGS(ppu, layer) (ppu->windowsel >> (layer * 4))

#define PPU_brightness(ppu) (ppu->inidisp & 0xf)
#define PPU_forcedBlank(ppu) (ppu->inidisp & 0x80)

#define PPU_objSize(ppu) (ppu->obsel >> 5)
#define PPU_objTileAdr1(ppu) ((ppu->obsel & 7) << 13)
#define PPU_objTileAdr2(ppu) (PPU_objTileAdr1(ppu) + (((ppu->obsel & 0x18) + 8) << 9))

#define PPU_objPriority(ppu) (ppu->oamaddh & 0x80)

#define PPU_mode(ppu) (ppu->bgmode & 7)
#define PPU_bg3priority(ppu) (ppu->bgmode & 0x8)
#define PPU_bigTiles(ppu, layer) (ppu->bgmode >> layer & 0x10)

#define PPU_mosaicEnabled(ppu, layer) (ppu->mosaic & (1 << layer))
#define PPU_mosaicSize(ppu) ((ppu->mosaic >> 4) + 1)

#define PPU_bgTilemapWider(ppu, layer) (ppu->bgXsc[layer] & 0x1)
#define PPU_bgTilemapHigher(ppu, layer) (ppu->bgXsc[layer] & 0x2)
#define PPU_bgTilemapAdr(ppu, layer) ((ppu->bgXsc[layer] & 0xfc) << 8)
#define PPU_bgTileAdr(ppu, layer) ((ppu->bgTileAdr >> (layer * 4) & 0xf) << 12)

#define PPU_m7xFlip(ppu) (ppu->m7sel & 0x1)
#define PPU_m7yFlip(ppu) (ppu->m7sel & 0x2)
#define PPU_m7charFill(ppu) (ppu->m7sel & 0x40)
#define PPU_m7largeField(ppu) (ppu->m7sel & 0x80)

#define PPU_directColor(ppu) ((ppu->cgwsel & 0x1) != 0)
#define PPU_addSubscreen(ppu) ((ppu->cgwsel & 0x2) != 0)
#define PPU_preventMathMode(ppu) (ppu->cgwsel >> 4 & 0x3)
#define PPU_clipMode(ppu) (ppu->cgwsel >> 6 & 0x3)

#define PPU_mathEnabled(ppu) (ppu->cgadsub & 0x3f)
#define PPU_halfColor(ppu) ((ppu->cgadsub & 0x40) != 0)
#define PPU_subtractColor(ppu) ((ppu->cgadsub & 0x80) != 0)

#define PPU_fixedColorR(ppu) (ppu->fixedColor & 0x1f)
#define PPU_fixedColorG(ppu) (ppu->fixedColor >> 5 & 0x1f)
#define PPU_fixedColorB(ppu) (ppu->fixedColor >> 10 & 0x1f)

#define PPU_interlace(ppu) ((ppu->setini & 0x1) != 0)
#define PPU_objInterlace(ppu) ((ppu->setini & 0x2) != 0)
#define PPU_overscan(ppu) ((ppu->setini & 0x4) != 0)
#define PPU_pseudoHires(ppu) ((ppu->setini & 0x8) != 0)
#define PPU_m7extBg(ppu) ((ppu->setini & 0x40) != 0)


enum {
  kWindow1Inversed = 1,
  kWindow1Enabled = 2,
  kWindow2Inversed = 4,
  kWindow2Enabled = 8,
};


Ppu* ppu_init(void);
void ppu_free(Ppu* ppu);
void ppu_reset(Ppu* ppu);
bool ppu_checkOverscan(Ppu* ppu);
void ppu_handleVblank(Ppu* ppu);
void ppu_runLine(Ppu* ppu, int line);
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);
void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli);
void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags);

/* Reusable semantic-OBJ extraction. `first/count` select contiguous OAM slots
 * and every selected slot must have `priority` (the raw 0..3 OAM priority).
 * Bounds use the renderer's live OBJ size/high-bit/widescreen wrap policy.
 * Rasterization clears the supplied rectangle, then resolves overlapping
 * parts in the PPU's live OAM priority-rotation order. Colors use the same
 * VRAM tile, flip, palette, and master-brightness conversion as scanout.
 * These functions never mutate OAM, VRAM, or CGRAM. */
bool PpuGetObjRangeBounds(Ppu *ppu, uint8_t first, uint8_t count,
                          uint8_t priority, PpuObjRangeBounds *out);
bool PpuRasterizeObjRange(Ppu *ppu, uint8_t first, uint8_t count,
                          uint8_t priority, const PpuObjRangeBounds *bounds,
                          uint32_t *pixels, int width, int height,
                          size_t pitch);

// Clear/bind persistent transparent ARGB host-overlay surfaces. Bindings survive
// ppu_reset; capture rectangles do not and are configured by game policy each
// frame. Surfaces are 256-kPpuBufWidth pixels wide and use the same full-frame
// coordinate system as renderBuffer.
// Passing NULL disables extraction for that source. Call ClearBindings once
// after PPU creation so a frontend can explicitly own all optional surfaces.
void PpuClearOverlayBindings(Ppu *ppu);
bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch);

// Bind an additional priority-band surface for a source, splitting the
// captured layer by hardware priority at scanout. OBJ: band n (1..3)
// receives sprites of OAM priority n, the primary surface keeps priority 0.
// BG sources: band 1 receives priority-1 tiles, the primary keeps
// priority 0. Requires a bound primary surface and shares its pitch and
// capture rectangle. Rebinding or unbinding the primary drops every band,
// so bind bands after their primary. NULL unbinds one band.
bool PpuBindOverlayPrioSurface(Ppu *ppu, PpuOverlaySource source, int band,
                               uint8_t *pixels);

// Reports whether scanout wrote any nontransparent pixel to an overlay surface
// in the most recently rendered frame. band 0 is the primary; 1..3 select the
// corresponding priority-band surface. Invalid or unbound inputs return false.
bool PpuOverlaySurfaceHasContent(const Ppu *ppu, PpuOverlaySource source,
                                 int band);

// Clear per-frame capture policy, then configure an arbitrary screen-space
// rectangle from BG1-BG4 or OBJ. With RemoveFromGame, pixels inside the rect
// are omitted from both main and subscreen while still exported with palette,
// transparency, windows, mosaic, and master brightness resolved.
void PpuClearOverlayCaptures(Ppu *ppu);
bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source,
                          int x, int y, int width, int height, uint8_t flags);

// Select a contiguous OAM slot range for an already configured OBJ capture.
// The game remains responsible for validating what those slots represent.
bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count);

// Bind the persistent Mode-7 override surface: a transparent ARGB buffer
// covering the full render frame at `scale` (1-4) subsamples per axis, i.e.
// (256+2*extra)*scale x 224*scale pixels with the given byte pitch. Survives
// ppu_reset like the other overlay bindings; NULL disables the feature.
bool PpuBindMode7OverlaySurface(Ppu *ppu, uint8_t *pixels, size_t pitch,
                                uint8_t scale);

// Per-frame policy (cleared by PpuClearOverlayCaptures): substitute `rgba`
// (ARGB words, width x height) for the given Mode-7 canvas-pixel rectangle.
// Sampling runs inside the Mode-7 layer draw, so rotation, zoom, HDMA
// per-scanline matrix effects, windows, and INIDISP brightness all apply.
// Texture alpha < 0x80 leaves the authentic canvas pixel in place.
bool PpuSetMode7Override(Ppu *ppu, const uint32_t *rgba, int width,
                         int height, int canvas_x0, int canvas_y0,
                         int canvas_x1, int canvas_y1, uint8_t wrap);

// Set the symmetric widescreen border, in pixels per side (clamped to
// kPpuExtraLeftRight). 0 restores authentic 256-wide rendering. The internal
// render width becomes 256 + 2*extra. Drives the dormant extraLeftCur/
// extraRightCur/extraLeftRight machinery used by the line renderer.
void PpuSetExtraSpace(Ppu *ppu, uint8_t extra);

// Render authentic 256-wide content centered within a `budget`-per-side wider
// framebuffer (no border columns drawn). For bounded screens; caller blacks
// out the side margins to pillarbox.
void PpuSetExtraSpaceCentered(Ppu *ppu, uint8_t budget);

// Asymmetric per-side widescreen margin (the snesrev/zelda3 model, see
// attribution in IMPROVEMENTS.md). The centering budget (extraLeftRight) must
// already be set via PpuSetExtraSpaceCentered/PpuSetExtraSpace; this fills the
// per-frame extraLeftCur/extraRightCur/extraBottomCur within that budget,
// clamped so the window/sprite/composite paths never read past the
// priority-buffer capacity (left/right) or the 16px overscan bottom. Negative
// inputs clamp to 0. (0,0,0) collapses to a centered pillarbox. Callers
// re-apply per frame (ppu_reset zeroes the fields). Used by games whose own
// scroll/room-bounds state drives the visible margin dynamically (Zelda),
// versus PpuSetExtraSpace's fixed symmetric border (SMW).
void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom);

// Vertical margin, in scanlines above line 1 and below line 224 (each clamped
// to kPpuExtraTopBottom, negative inputs to 0). (0,0) restores authentic
// 224-line rendering exactly. The frontend drives the whole loop -- see
// ppu_runMarginLine -- so setting this alone renders nothing extra; it tells
// the line renderer where the authentic band sits inside the taller target and
// how far the margin bands are allowed to reach.
//
// `top` is the axis that works: above-screen sprite positions are already
// expressible in OAM (see kPpuExtraTopBottom), so those scanlines get real
// backgrounds AND real sprites. `bottom` gets backgrounds only -- the OAM Y
// encoding cannot distinguish a sprite below line 224 from one above line 0,
// so the game's own object coordinates carry no usable data down there.
void PpuSetExtraVerticalSpace(Ppu *ppu, int top, int bottom);

// Exact per-slot OAM Y, for a frontend that owns the sprite emitter.
//
// The OAM Y field is 8 bits against a 224-line screen, so it cannot say where a
// sprite is -- only where it is modulo 256. Everything outside the visible band
// is therefore ambiguous: a slot parked above the screen, a sprite hanging off
// the bottom, and a sprite genuinely above the top all land in the same byte
// values, and a vertical margin renders all three in the same place.
//
// An override carries the UN-TRUNCATED form of exactly the value the byte
// encodes -- not a recomputed "true" position -- so a slot with one renders
// identically wherever the byte was not lossy, and differs only where the 8-bit
// field aliased. That makes the ambiguity disappear at the source instead of
// being guessed at from the byte.
//
// Slots WITHOUT an override keep the authentic mod-256 byte path untouched, and
// are not drawn on above-screen scanlines at all: nothing outside the emitter
// the frontend owns has a position that can be trusted up there.
//
// Call PpuClearObjYOverrides once per frame wherever the shadow OAM is cleared,
// then PpuSetObjYOverride per slot as the emitter writes it. `slot` indexes
// sprites (0..127), matching ppu->oam word-pair order.
void PpuClearObjYOverrides(Ppu *ppu);
void PpuSetObjYOverride(Ppu *ppu, uint8_t slot, int y);

// Row within the render target that authentic scanline 0 occupies. The host
// allocates kPpuBufHeight rows and crops what it wants around this origin.
static inline int PpuVerticalOrigin(const Ppu *ppu) {
  return ppu->extraTopCur;
}

// Total scanlines the current margin configuration renders.
static inline int PpuRenderedHeight(const Ppu *ppu) {
  return kPpuYPixels + ppu->extraTopCur + ppu->extraBottomCur;
}

// Render one margin scanline. `line` is the ordinary 1-based PPU line number
// extended past its authentic range: <= 0 for the top band (0 is the scanline
// directly above line 1), > kPpuYPixels for the bottom band. ppu_runLine's
// line-0 frame setup must already have run this frame; this entry point is
// deliberately separate so that "line 0" keeps meaning that setup and never
// collides with the first above-screen scanline.
//
// Per-scanline register state (HDMA, windows) has no authentic value outside
// the visible band, so the caller decides the policy by WHEN it calls this:
// before the authentic loop the registers still hold their pre-frame values
// (hold-first), after it they hold the last line's (hold-last).
void ppu_runMarginLine(Ppu *ppu, int line);

// Widescreen HUD split (opt-in, configured by the game frontend): for
// scanlines < height, BG3 (layer 2) is drawn as up to three chunks — source
// [0,left_end) anchored to the LEFT border edge, [left_end,right_start)
// kept centered (unmoved), [right_start,256) anchored to the RIGHT border
// edge. Set left_end==right_start for a two-way corner layout with no centered
// chunk. On scanlines [left_only_y,height), the complete source [0,256) is
// instead anchored to the left presentation edge; set left_only_y >= height
// to disable that lower band. The vacated spans stay transparent. height 0 =
// off (authentic).
// The anchors use the full centering budget, independently of finite-world
// live side margins; the final compositor uses that same full budget on HUD
// scanlines while world layers remain bounded. Only takes effect while that
// border budget is active and BG3 is not shaped by a real window; mosaic lines
// fall back to centered. Like the extra-space setters, callers re-apply per
// frame.
void PpuSetWidescreenHudSplit(Ppu *ppu, uint8_t height, uint8_t left_end,
                              uint8_t right_start, uint8_t player_row_y,
                              uint8_t left_only_y);

// Let BG3 (layer 2) render into the widescreen side margins on scanlines
// >= from_y, instead of being clamped to the authentic 256-wide region. Pass
// the HUD band height so the status bar above it stays clamped (or split) while
// level content on BG3 below it (e.g. SMW water) fills 16:9. from_y 0 = off.
// Like the other widescreen setters, callers re-apply per frame.
void PpuSetWidescreenBg3Widen(Ppu *ppu, uint8_t from_y);

// Per-layer widescreen clamp: bit L (0..3) keeps BG(L+1) in the authentic 256
// columns even while other layers extend into the margins. For scenes that mix
// genuinely-wide world layers with 256-wide UI/dialog/status layers (or layers
// whose offscreen tilemap data is not meant to be shown) — clamp the latter so
// they never tile wrapped/garbage columns into the border. mask 0 = every layer
// extended (default). Independent of the BG3-specific widen/split controls; a
// layer clamped here is clamped regardless of wsBg3WidenY. Re-apply per frame.
void PpuSetWidescreenLayerClamp(Ppu *ppu, uint8_t mask);

// Mirror-fill BG-layer side margins from the authentic 256-wide rendered
// result. Bit L reflects BG(L+1) without duplicating the boundary pixel:
// left destination x<0 samples -x, right destination x>=256 samples 510-x.
// Reflection happens after tile decode/windowing but before layer priority and
// color math are finalized, so transparency, priority, palette animation, and
// sub-screen behavior remain layer-correct. The current implementation applies
// to Mode-1 4bpp BG1/BG2; unsupported layers remain authentically clamped.
// Re-apply per frame. A mirror bit takes visual precedence over a clamp bit.
void PpuSetWidescreenLayerMirror(Ppu *ppu, uint8_t mask);

// Repeat-fill BG-layer side margins from the authentic 256-wide rendered
// result. Left x<0 samples 256+x; right x>=256 samples x-256. Because this is
// performed independently on each already-rendered scanline, per-line HDMA
// scroll, transparency, priority, palette animation, and color math remain
// layer-correct. The current implementation applies to Mode-1 4bpp BG1/BG2;
// unsupported layers remain authentically clamped. Re-apply per frame. A
// repeat bit takes precedence if the same layer is also marked for mirroring.
void PpuSetWidescreenLayerRepeat(Ppu *ppu, uint8_t mask);

// Widescreen: when enabled, a layer whose margins are SYNTHESIZED (mirror or
// repeat padding, not fetched from tilemap) pads a CAPTURED layer buffer out to
// the full centering budget rather than stopping at the live per-side margin.
//
// Why this exists: a host that captures a layer (PpuBindOverlaySurface) samples
// the whole fixed capture span, which is sized from the budget -- but the line
// renderer only ever writes within the live margin, and the live margin shrinks
// to 0 as a finite world's camera reaches its bound. The never-written columns
// then read as transparent/black at the capture's edge even though the padding
// source (the authentic 256 columns) is always available. Widening costs a
// memcpy-style compare-store per padded column and no additional tilemap fetch.
//
// Deliberately scoped to captured buffers: the game's own framebuffer must keep
// the live margin exactly, because a narrower margin there is the intended
// pillarbox at a world edge. 0 = off (previous behaviour everywhere).
// Re-apply per frame: the extra-space setters reset it.
void PpuSetWidescreenPadCapturedToBudget(Ppu *ppu, uint8_t enabled);

// Clamp BG(layer+1) to the authentic 256 on scanlines [y0,y1) only (the generic
// "overlay plane" / BG2.5): a bounded UI element sharing a layer with wide world
// content is confined to the center on its own rows, while the layer stays wide
// above and below. y1<=y0 disables. Re-apply per frame (the extra-space setters
// reset it). Independent of the whole-layer clamp and the BG3 widen/split.
void PpuSetWidescreenLayerClampBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                    uint8_t y1);

// Cyclically repeat BG(layer+1)'s authentic rendered scanline into the margins
// on scanlines [y0,y1) only. This is the banded form of
// PpuSetWidescreenLayerRepeat: it preserves per-line scroll, tile animation,
// transparency, priority, and color math, and takes precedence over a
// whole-layer clamp for the selected rows. Currently supported by the Mode-1
// 4bpp BG1/BG2 path. y1<=y0 disables. Re-apply per frame.
void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1);

// Widescreen margin source gap: the margins of BG(layer+1) skip the first
// left_px/right_px pixels past the authentic screen edges and sample the
// tilemap beyond them. For games that park UI-construction tiles in the
// offscreen columns adjacent to the visible screen (a staging area that
// hardware never shows but widescreen margins would expose) — the gap keeps
// the staging strip invisible while the layer still fills the margins with
// its real repeating/world content. Applies to the 4bpp/2bpp BG paths
// (mosaic lines fall back to ungapped). 0/0 = off. Re-apply per frame.
void PpuSetWidescreenLayerMarginGap(Ppu *ppu, uint8_t layer, uint8_t left_px,
                                    uint8_t right_px);

int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags);

#endif
