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

/* Optional frame-scoped tilemap source for a 4bpp BG layer. The callback owns
 * only the 16-bit SNES tilemap word; character pixels, palette, priority,
 * windows, mosaic and color math continue through the ordinary PPU path.
 * Returning false is a valid transparent finite-world result. Once a binding
 * is accepted the callback must be total: provider failure is resolved before
 * binding, never midway through a scanline. */
typedef bool (*PpuVirtualTilemapLookup)(const void *context,
                                       int32_t tile_x, int32_t tile_y,
                                       uint16_t *entry);

/* Optional presentation-only classification for a virtual tilemap word.
 * Bands 0..2 route a captured BG pixel to its far, ordinary, or priority
 * Diorama surface without changing the word or its native z-buffer rank.
 * Returning false preserves the hardware priority-bit split. */
typedef bool (*PpuVirtualTilemapBandLookup)(const void *context,
                                           int32_t tile_x, int32_t tile_y,
                                           uint16_t entry, uint8_t *band);

enum {
  /* Default is synthetic margins only. Callers may opt a proven world layer
   * into provider ownership of the authentic 256x224 viewport too. */
  kPpuVirtualTilemapFlag_IncludeAuthentic = 1,
};

typedef struct PpuVirtualTilemapBinding {
  PpuVirtualTilemapLookup lookup;
  PpuVirtualTilemapBandLookup band_lookup;
  const void *context;
  int32_t camera_x;
  int32_t camera_y;
  uint16_t hscroll_anchor;
  uint16_t vscroll_anchor;
  uint8_t flags;
} PpuVirtualTilemapBinding;

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
  // default to 0 (authentic 256-wide output). 128 per side gives a 512-pixel
  // line buffer; the frontend's smaller live cap leaves tilemap-streaming
  // slack while this compile-time allocation remains a power of two.
  kPpuExtraLeftRight = 128,
  // Full internal width of the priority buffers (logical 256 + both borders).
  // THE LINE-BUFFER WIDTH: how many columns the per-scanline renderer can fill.
  // Bounded by the background tilemap ring and frontend streaming policy
  // (kWsExtraMax in widescreen.h), not by the host allocation. It does not
  // grow for the OBJ apron.
  kPpuBufWidth = kPpuXPixels + kPpuExtraLeftRight * 2,
  // ── Display width vs resolve width ───────────────────────────────────────
  // The apron: extra columns per side that host ARGB SURFACES carry beyond the
  // line-buffer width, so a sprite can be fully resolved before it reaches the
  // visible edge instead of being clipped as it crosses it. Content for those
  // columns cannot come from the scanline path -- that path is bounded by the
  // display margins and by the 9-bit encoding -- so it arrives from
  // capture-time part rasterization (PpuRasterizeParts) instead.
  //
  // 64 = one maximum sprite width, the smallest apron that can hold a whole
  // sprite. Deliberately NOT folded into kPpuBufWidth: widening the line
  // buffers would pay ClearBackdrop memsets over apron columns on every line
  // of every frame for room only the capture path uses.
  kPpuObjApron = 64,
  // THE SURFACE WIDTH: allocation width of every host ARGB overlay/plane
  // surface, and therefore the denominator every normalized-U computation
  // divides by. Distinct from kPpuBufWidth on purpose -- see the apron note.
  kPpuSurfaceWidth = kPpuBufWidth + kPpuObjApron * 2,
  // Authentic visible scanline count. Distinct from kPpuXPixels' axis: 224 is
  // how many lines are OUTPUT, and it is also the threshold above which an OAM
  // Y byte is a negative (offscreen-top) position -- see kPpuObjYWrap.
  kPpuYPixels = 224,
  // Sentinel for a layer extent with no additional presentation cap. The
  // source/edge/global canvas still supply their ordinary bounds.
  kPpuWidescreenExtentAvailable = UINT16_MAX,
  // Maximum vertical expansion *per side*, the compile-time ceiling for the
  // runtime ppu->extraTopCur/extraBottomCur (default 0 = authentic 224-line
  // output). OAM's 8-bit Y is ambiguous outside the authentic viewport, but
  // frontends that widen the emitter publish an exact signed position for each
  // committed slot. Margin scanlines therefore accept exact slots only (see
  // ppu_evaluateSprites) instead of treating the byte encoding as a range
  // limit. 64 covers the measured 48px action-camera jump while keeping the
  // fixed capture budget modest; it is presentation capacity, not gameplay
  // camera range.
  kPpuExtraTopBottom = 64,
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
  /* Bake an eligible BG's fixed-colour subtraction into its captured pixels.
   * Unlike half-add, subtraction cannot be represented by host alpha: it must
   * happen in the PPU's 5-bit component space before brightness expansion.
   * The caller admits only full, fixed-addend subtraction with no colour
   * window; authentic scanout is still untouched. */
  kPpuOverlayFlag_ApplyBgFixedColorSubtract = 8,
  /* This source is a subscreen input to a full, non-half colour add. Capture
   * only pixels where it wins the subscreen priority resolve and a resolved
   * main-world source enables math; separately captured BG3 is reinserted over
   * that result. The host can draw the sparse plane with saturated additive
   * blending. Unlike the alpha flags, this applies to BG and OBJ alike.
   * Extraction-only. */
  kPpuOverlayFlag_MarkFullAddSubscreen = 16,
  /* Export an opaque black/white mask of pixels for which this isolated
   * source is the final main-screen priority winner. The host can multiply a
   * presentation effect by the mask without a custom shader. Extraction-only;
   * authentic scanout remains unchanged. */
  kPpuOverlayFlag_MarkMainScreenWinner = 32,
  /* Export an opaque black/white mask where this isolated source wins the
   * priority resolve of the screen that owns it (main preferred, otherwise
   * sub). This is the environmental-effect counterpart to the main-only mask:
   * stages such as Marahna put BG1 and OBJ exclusively on TS, so an owning-
   * screen mask preserves their authentic occlusion before colour addition. */
  kPpuOverlayFlag_MarkOwningScreenWinner = 64,
};

typedef enum PpuOverlayTransparentFill {
  kPpuOverlayTransparentFill_None = 0,
  kPpuOverlayTransparentFill_Black,
  kPpuOverlayTransparentFill_Cgram,
} PpuOverlayTransparentFill;

/* Per-row widescreen padding policy. Inherit selects the whole-layer mask;
 * the remaining values intentionally mirror the host action-background edge
 * vocabulary without making the generic PPU depend on that game module. */
typedef enum PpuWidescreenBandFill {
  kPpuWidescreenBandFill_Inherit = 0,
  kPpuWidescreenBandFill_Transparent,
  kPpuWidescreenBandFill_LiveWorld,
  kPpuWidescreenBandFill_Clamp,
  kPpuWidescreenBandFill_Mirror,
  kPpuWidescreenBandFill_Repeat,
  kPpuWidescreenBandFill_RawWrap,
} PpuWidescreenBandFill;

typedef enum PpuWidescreenMotion {
  /* Historical behavior: a reflected scanline also reflects apparent motion. */
  kPpuWidescreenMotion_FillRelative = 0,
  /* Retain the authentic layer's apparent horizontal scroll direction. */
  kPpuWidescreenMotion_NormalScroll,
} PpuWidescreenMotion;

typedef struct PpuWidescreenLayerPolicy {
  PpuWidescreenBandFill fill;
  PpuWidescreenMotion motion;
  bool band_override;
} PpuWidescreenLayerPolicy;

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
  /* Full primary-plane backing applied before isolated pixels are resolved.
   * It is capture state rather than a flag because CGRAM supplies 256 possible
   * live colours. Split high-priority surfaces intentionally remain sparse. */
  uint8_t transparentFillMode;   /* PpuOverlayTransparentFill */
  uint8_t transparentFillCgram;
  /* Distinguishes an explicit None policy from no frontend policy. Rendering
   * treats both as transparent; immutable host presentation may need the
   * distinction to suppress an inherited/default backing. */
  uint8_t transparentFillConfigured;
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

/* A second, semantic OBJ capture that is written directly by the live sprite
 * evaluator. Unlike PpuRasterizeObjRange, this does not sample an endpoint
 * PPU state: each selected pixel is preserved when scanout fetches it. */
typedef struct PpuObjRangeCapture {
  int16_t x0, y0, x1, y1;
  uint8_t first, count;
  uint8_t *pixels;
  uint32_t pitch;
} PpuObjRangeCapture;

/* One resolved sprite part: exact position, attribute word, size. The atom
 * every OBJ position consumer operates on -- OAM is one way to obtain parts,
 * not the only one. Priority is deliberately NOT a separate field: it lives in
 * tile_attr bits 12-13, and a second copy would drift from the authoritative
 * one. */
typedef struct PpuObjPart {
  int16_t x, y;        /* exact screen position, free of the 8/9-bit moduli */
  uint16_t tile_attr;  /* OAM attribute word: tile, palette, priority, flips */
  uint8_t size;        /* square sprite edge in pixels (8/16/32/64) */
} PpuObjPart;

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
  // Synthetic margin rows available per BG layer before that layer reaches
  // either bounded world edge. A set bit clips rows farther outside the
  // viewport to transparent instead of letting the tilemap address wrap to the
  // opposite edge. Authentic lines are never clipped. See
  // PpuSetVerticalMarginLayerClip.
  uint8_t verticalMarginLayerClip;
  uint8_t verticalMarginTopRows[4];
  uint8_t verticalMarginBottomRows[4];
  /* Render-only, frame-scoped policy. This host state is outside both
   * savestate regions and ppu_reset deliberately does not preserve it. */
  PpuVirtualTilemapBinding virtualTilemap[4];
  // Per-slot exact position from the frontend. See PpuSetObjExactPosition.
  int16_t objPosX[128];
  int16_t objPosY[128];
  uint8_t objPosValid[128];
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
  // Whole-layer motion phase plus independently authored per-row fill/motion.
  // A zero row fill inherits the masks above. Synthetic rows inherit the
  // nearest authentic edge row through PpuLayerPolicyRow.
  uint8_t wsLayerNormalScroll;
  uint8_t wsBandFill[4][kPpuYPixels];
  uint8_t wsBandMotion[4][kPpuYPixels];
  // Independent presentation caps for BG1-BG4. Horizontal caps are resolved
  // per authentic scanline so mixed-content row bands can differ. Synthetic
  // rows inherit the nearest authentic edge row, so edge-anchored bands retain
  // their caps there. UINT16_MAX means the existing edge/source/canvas
  // availability is uncapped.
  uint16_t wsLayerExtentLeftDefault[4], wsLayerExtentRightDefault[4];
  uint16_t wsLayerExtentTop[4], wsLayerExtentBottom[4];
  uint16_t wsLayerExtentLeft[4][kPpuYPixels];
  uint16_t wsLayerExtentRight[4][kPpuYPixels];
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
  /* Per-pixel presentation band emitted by an owning BG virtual tilemap.
   * 0xff means "use authentic hardware priority". This metadata parallels
   * the isolated capture only; it never participates in native composition. */
  uint8_t overlayVirtualBands[2][kPpuBufWidth];
  /* Full-add OBJ resolve with host-relocated slots omitted. The ordinary OBJ
   * overlay still includes those slots so non-additive capture semantics do
   * not change. */
  PpuPixelPrioBufs overlayObjFullAddBuffer;
  PpuOverlayCapture overlayCaptures[kPpuOverlaySource_Count];
  uint8_t overlayObjRelocatedFirst, overlayObjRelocatedCount;
  PpuObjRangeCapture objRangeCapture;
  uint32_t renderPitch;
  uint8_t *renderBuffer;
  uint32_t overlayRenderPitch[kPpuOverlaySource_Count];
  uint8_t *overlayRenderBuffer[kPpuOverlaySource_Count];
  /* Optional priority-band split surfaces (PpuBindOverlayPrioSurface). When
   * bound, scanout routes each captured pixel of the source to the band
   * matching its hardware priority instead of the primary surface: OBJ bands
   * 1..3 receive sprite priorities 1..3 (primary keeps priority 0); BG band 1
   * receives priority-1 tiles and BG band 2 receives an optional far virtual
   * plane (primary keeps ordinary tiles). Bands share the primary's pitch and
   * capture rectangle. */
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

/* Resolve one row through the exact whole-layer/band padding policy used by
 * the renderer. screen_y is authentic-screen based: 0..223 is the center and
 * synthetic rows inherit the nearest edge. Kept inline because the scanline
 * renderer queries it in its hot path and diagnostics must share the same
 * implementation rather than copy its precedence rules. */
static inline PpuWidescreenLayerPolicy PpuResolveWidescreenLayerPolicy(
    const Ppu *ppu, uint8_t layer, int screen_y) {
  PpuWidescreenLayerPolicy policy = {
    .fill = kPpuWidescreenBandFill_RawWrap,
    .motion = kPpuWidescreenMotion_FillRelative,
  };
  if (!ppu || layer >= 4) return policy;
  const int row = screen_y < 0 ? 0
      : screen_y >= kPpuYPixels ? kPpuYPixels - 1 : screen_y;
  const uint8_t row_fill = ppu->wsBandFill[layer][row];
  if (row_fill) {
    policy.fill = (PpuWidescreenBandFill)row_fill;
    policy.motion = (PpuWidescreenMotion)ppu->wsBandMotion[layer][row];
    policy.band_override = true;
    return policy;
  }
  const uint8_t bit = (uint8_t)(1u << layer);
  /* Synthesized padding wins over Clamp; Repeat wins over Mirror. */
  if (ppu->wsLayerRepeat & bit)
    policy.fill = kPpuWidescreenBandFill_Repeat;
  else if (ppu->wsLayerMirror & bit)
    policy.fill = kPpuWidescreenBandFill_Mirror;
  else if (ppu->wsLayerClamp & bit)
    policy.fill = kPpuWidescreenBandFill_Clamp;
  if (ppu->wsLayerNormalScroll & bit)
    policy.motion = kPpuWidescreenMotion_NormalScroll;
  return policy;
}

static inline bool PpuMapWidescreenLayerXWithPolicy(
    const Ppu *ppu, uint8_t layer, int screen_x, int *source_x,
    const PpuWidescreenLayerPolicy *policy) {
  if (!ppu || layer >= 4 || !source_x || !policy) return false;
  if (screen_x >= 0 && screen_x < kPpuXPixels) {
    *source_x = screen_x;
    return true;
  }
  switch (policy->fill) {
    case kPpuWidescreenBandFill_Mirror:
      *source_x = screen_x < 0
          ? -screen_x : kPpuXPixels * 2 - 2 - screen_x;
      if (policy->motion == kPpuWidescreenMotion_NormalScroll) {
        *source_x = (*source_x -
            2 * (int)(ppu->hScroll[layer] & 0xff)) & 0xff;
      }
      return *source_x >= 0 && *source_x < kPpuXPixels;
    case kPpuWidescreenBandFill_Repeat:
      *source_x = screen_x < 0
          ? kPpuXPixels + screen_x : screen_x - kPpuXPixels;
      return *source_x >= 0 && *source_x < kPpuXPixels;
    case kPpuWidescreenBandFill_Transparent:
    case kPpuWidescreenBandFill_Clamp:
      return false;
    case kPpuWidescreenBandFill_LiveWorld:
    case kPpuWidescreenBandFill_RawWrap:
    case kPpuWidescreenBandFill_Inherit:
    default:
      *source_x = screen_x;
      return true;
  }
}

/* Map a displayed X through that resolved policy. Authentic and raw/live
 * coordinates map directly; Clamp/Transparent synthetic coordinates return
 * false. This is the shared source-inspection seam for renderer and tooling. */
static inline bool PpuMapWidescreenLayerX(
    const Ppu *ppu, uint8_t layer, int screen_y, int screen_x,
    int *source_x, PpuWidescreenLayerPolicy *out_policy) {
  if (out_policy) *out_policy = (PpuWidescreenLayerPolicy){ 0 };
  if (!ppu || layer >= 4 || !source_x) return false;
  const PpuWidescreenLayerPolicy policy =
      PpuResolveWidescreenLayerPolicy(ppu, layer, screen_y);
  if (out_policy) *out_policy = policy;
  return PpuMapWidescreenLayerXWithPolicy(
      ppu, layer, screen_x, source_x, &policy);
}

#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + (level6 ? 2 : 0))
#define SPRITE_PRIO_TO_PRIO_HI(prio) ((prio) * 4 + 2)

#define IS_SCREEN_ENABLED(ppu, sub, layer) (ppu->screenEnabled[sub] & (1 << layer))
#define IS_SCREEN_WINDOWED(ppu, sub, layer) (ppu->screenWindowed[sub] & (1 << layer))
#define GET_WINDOW_FLAGS(ppu, layer) (ppu->windowsel >> (layer * 4))

#define PPU_brightness(ppu) (ppu->inidisp & 0xf)
#define PPU_forcedBlank(ppu) (ppu->inidisp & 0x80)

#define PPU_objSize(ppu) (ppu->obsel >> 5)
/* [OBJSEL size select][high-table size bit] -> pixel size. Exposed because a
 * host that builds parts WITHOUT an OAM slot (the action apron channel) still
 * has to agree with PpuResolveObjSlots about how big they are. */
extern const uint8_t kPpuSpriteSizes[8][2];
static inline int PpuObjSizeForSizeBit(const Ppu *ppu, int large) {
  return ppu ? kPpuSpriteSizes[PPU_objSize(ppu)][large ? 1 : 0] : 0;
}
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

/* Capture one validated contiguous OAM range into an independent ARGB surface
 * as the normal sprite evaluator fetches it. The rectangle is in authentic
 * screen coordinates and is cleared lazily, one selected scanline at a time.
 * Selected slots retain their own first-writer order but are isolated from
 * unrelated OAM, which is required when the host relocates a HUD sprite after
 * scanout. Policy is frame-scoped and cleared by PpuClearOverlayCaptures. */
bool PpuSetObjRangeCapture(Ppu *ppu, uint8_t first, uint8_t count,
                           int x, int y, int width, int height,
                           uint8_t *pixels, size_t pitch);

// Clear/bind persistent transparent ARGB host-overlay surfaces. Bindings survive
// ppu_reset; capture rectangles do not and are configured by game policy each
// frame. Surfaces are 256-kPpuSurfaceWidth pixels wide and use the same full-frame
// coordinate system as renderBuffer.
// Passing NULL disables extraction for that source. Call ClearBindings once
// after PPU creation so a frontend can explicitly own all optional surfaces.
void PpuClearOverlayBindings(Ppu *ppu);
bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch);

// Bind an additional priority-band surface for a source, splitting the
// captured layer by hardware priority at scanout. OBJ: band n (1..3)
// receives sprites of OAM priority n, the primary surface keeps priority 0.
// BG sources: band 1 receives priority-1 tiles, band 2 receives virtual-far
// pixels, and the primary keeps ordinary tiles. Requires a bound primary
// surface and shares its pitch and capture rectangle. Rebinding or unbinding
// the primary drops every band, so bind bands after their primary. NULL
// unbinds one band.
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
// transparency, windows, mosaic, and master brightness resolved. BG sources
// are exported from the main-screen rendering when enabled there, otherwise
// from their subscreen rendering; a source enabled on both is exported once
// from main so screen-specific window policy remains deterministic.
void PpuClearOverlayCaptures(Ppu *ppu);
bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source,
                          int x, int y, int width, int height, uint8_t flags);
/* Configure the primary surface's backing policy independently of capture
 * geometry; explicit None is retained as configured policy for immutable host
 * presentation. This may be called before or after PpuSetOverlayCapture. */
bool PpuSetOverlayTransparentFill(Ppu *ppu, PpuOverlaySource source,
                                  PpuOverlayTransparentFill mode,
                                  uint8_t cgram_index);
/* Resolves the current frame's fill through brightness and CGRAM. Returns
 * zero for no fill, otherwise opaque ARGB (including opaque black). */
uint32_t PpuOverlayTransparentFillColor(const Ppu *ppu,
                                        PpuOverlaySource source);

// Select a contiguous OAM slot range for an already configured OBJ capture.
// The game remains responsible for validating what those slots represent.
bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count);

/* Exclude a contiguous subset of the captured OBJ range from a full-add
 * subscreen resolve. The host is promising to reinsert these objects elsewhere
 * (for example, moving a status-bar icon to an anchored HUD), so the extracted
 * world must expose the BG/OBJ pixels they covered. Cleared with the per-frame
 * overlay policy. */
bool PpuSetOverlayRelocatedOamRange(Ppu *ppu, uint8_t first, uint8_t count);

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
// Sprite positions outside the authentic viewport are ambiguous in OAM's
// 8-bit Y. Margin scanlines therefore draw only slots carrying an exact signed
// position from the frontend. A frontend that widens its draw predicate and
// publishes those positions gets real backgrounds and sprites on both sides;
// one that does not gets backgrounds only, without aliased/parked OBJ garbage.
void PpuSetExtraVerticalSpace(Ppu *ppu, int top, int bottom);

// Bound one BG layer independently within both synthetic vertical margins.
// `top_rows`/`bottom_rows` are the real world scanlines immediately outside the
// authentic viewport for BG(layer+1), clamped to kPpuExtraTopBottom. Rows
// farther out are transparent for that layer instead of wrapping through its
// tilemap.
//
// This does not change authentic scanlines or the other BG layers. It exists
// for mixed-depth scenes where (for example) BG1 is deep inside a tall level
// while a bounded BG2 parallax plane is still at camera Y=0. Re-apply each
// frame after PpuSetExtraVerticalSpace; that setter clears all layer clips.
void PpuSetVerticalMarginLayerClip(Ppu *ppu, uint8_t layer,
                                   int top_rows, int bottom_rows);

// Bind a finite virtual tilemap to a 4bpp BG layer. By default, authentic
// x=[0,256), scanlines y=[1,224] continue to use the native VRAM ring;
// kPpuVirtualTilemapFlag_IncludeAuthentic opts a proven layer into provider
// ownership there too. `hscroll_anchor`/`vscroll_anchor` are the 10-bit PPU
// phases matching the full camera values at bind time; live per-line scroll
// changes are added as signed wrapped deltas. NULL clears one layer.
// PpuSetExtraSpace, PpuSetExtraSpaceCentered and ppu_reset clear all bindings.
bool PpuSetVirtualTilemap(Ppu *ppu, uint8_t layer,
                          const PpuVirtualTilemapBinding *binding);
void PpuClearVirtualTilemaps(Ppu *ppu);

// Exact per-slot OAM position, for a frontend that owns the sprite emitter.
//
// OAM X is 9 bits (mod 512) against a 256-wide screen and Y is 8 bits
// (mod 256) against a 224-line screen, so neither field says where a sprite
// IS -- only where it is modulo the encoding. Everything outside the visible
// screen is therefore ambiguous: a parked slot, a sprite hanging off one edge,
// and a sprite off the opposite edge land in the same values.
//
// An exact position carries the UN-TRUNCATED form of exactly the value the
// stored bytes encode -- not a recomputed "true" position -- so a slot with one
// renders identically wherever the bytes were not lossy, and differs only where
// the encoding aliased. That makes the ambiguity disappear at the source
// instead of being guessed at from the bytes.
//
// Slots WITHOUT an exact position keep the authentic modular byte decode
// untouched, and are not drawn on above-screen scanlines at all: nothing
// outside the emitter the frontend owns has a position that can be trusted up
// there.
//
// Call PpuClearObjExactPositions once per frame wherever the shadow OAM is
// cleared, then PpuSetObjExactPosition per slot at the point the emitter FULLY
// commits the slot (both axes stored). Publishing earlier leaks: an emitter
// that accepts Y, publishes, then rejects X re-parks the slot in OAM while the
// stale exact position stays valid. `slot` indexes sprites (0..127), matching
// ppu->oam word-pair order.
void PpuClearObjExactPositions(Ppu *ppu);
void PpuSetObjExactPosition(Ppu *ppu, uint8_t slot, int x, int y);

// ── Part resolution ──────────────────────────────────────────────────────
// Decode OAM slots [first, first+count) into parts, in the renderer's
// priority-rotation order (earlier entries own an overlapping opaque pixel, so
// array order IS paint order). Every part must carry `priority` in its
// attribute word or the whole resolve fails, matching the range API's
// contract. Exact positions are consulted, which is the point: bounds and
// rasterization then agree with the scanline evaluator about where sprites
// are.
/* Resolve one slot without requiring callers to duplicate exact-position,
 * high-OAM size, and widescreen wrap policy. */
bool PpuResolveObjSlot(Ppu *ppu, uint8_t slot, PpuObjPart *out_part);
bool PpuResolveObjSlots(Ppu *ppu, uint8_t first, uint8_t count,
                        uint8_t priority, PpuObjPart *out_parts,
                        int max_parts, int *out_count);

// Union bounding box of explicit parts. False when count <= 0.
bool PpuGetPartBounds(const PpuObjPart *parts, int count,
                      PpuObjRangeBounds *out);

// Rasterize explicit parts into `pixels` (ARGB, `bounds->x0/y0` at the top
// left), clipping to the buffer. Screen-independent: no scanline limits, no
// viewport clipping -- the caller chooses the window by choosing the bounds.
bool PpuRasterizeParts(Ppu *ppu, const PpuObjPart *parts, int count,
                       const PpuObjRangeBounds *bounds, uint32_t *pixels,
                       int width, int height, size_t pitch);

// Columns of apron a surface bound at `pitch` carries per side. Derived from
// the pitch rather than assumed, because the apron is a RUNTIME bind decision
// (diorama binds wide, flat binds narrow) while kPpuObjApron is only the
// compile-time capacity. Content for screen x lands at column
// x + PpuSurfaceOriginX(ppu, pitch).
static inline int PpuSurfaceApron(const Ppu *ppu, size_t pitch) {
  int width = (int)(pitch / sizeof(uint32_t));
  int scanline_span = kPpuXPixels + ppu->extraLeftRight * 2;
  int apron = (width - scanline_span) / 2;
  return apron > 0 ? apron : 0;
}

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
void PpuSetWidescreenLayerNormalScroll(Ppu *ppu, uint8_t mask);

// Override one layer's fill and motion on authentic rows [y0,y1). Multiple
// non-overlapping calls may be made per layer each frame. The caller owns
// overlap validation; later calls deterministically replace earlier rows.
void PpuSetWidescreenLayerBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                               uint8_t y1, PpuWidescreenBandFill fill,
                               PpuWidescreenMotion motion);

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

// Compatibility spelling for callers that need one legacy repeat band.
void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1);

// Limit one BG layer's contribution outside the authentic 256x224 viewport.
// Each fixed value is a maximum number of presentation pixels on that side;
// kPpuWidescreenExtentAvailable leaves that side governed by the existing
// edge/source/canvas availability. Caps never manufacture pixels and never
// remove authentic pixels. Re-apply after PpuSetExtraSpace each frame.
void PpuSetWidescreenLayerExtent(Ppu *ppu, uint8_t layer,
                                 uint16_t left, uint16_t right,
                                 uint16_t top, uint16_t bottom);

// Override the horizontal caps for one half-open authentic scanline band.
// The coordinate convention deliberately matches PpuSetWidescreenLayerBand so
// fill, motion and extent change on the same raster line. Invalid/empty bands
// are ignored.
void PpuSetWidescreenLayerExtentBand(Ppu *ppu, uint8_t layer,
                                     uint8_t y0, uint8_t y1,
                                     uint16_t left, uint16_t right);

int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags);

#endif
