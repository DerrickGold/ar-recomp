#ifndef SNESRECOMP_NEXT_PPU_H
#define SNESRECOMP_NEXT_PPU_H

#include "../types.h"
#include "saveload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ppu Ppu;

typedef bool (*PpuVirtualTilemapLookup)(const void *, int32_t, int32_t,
                                        uint16_t *);
/* Optional zero-copy scanline companion to lookup.  A provider exposes up to
 * capacity consecutive tile coordinates (tile_x + i * tile_step) through a
 * borrowed pointer and word stride.  A NULL entries result is a finite-world
 * gap of the returned length.  The view remains valid until the provider's
 * next span call; returning zero asks the PPU to use scalar lookup. */
typedef size_t (*PpuVirtualTilemapSpanLookup)(
    const void *, int32_t, int32_t, int32_t, size_t,
    const uint16_t **, ptrdiff_t *);
typedef bool (*PpuVirtualTilemapBandLookup)(const void *, int32_t, int32_t,
                                            uint16_t, uint8_t *);

enum { kPpuVirtualTilemapFlag_IncludeAuthentic = 1 };

typedef struct PpuVirtualTilemapBinding {
    PpuVirtualTilemapLookup lookup;
    PpuVirtualTilemapSpanLookup lookup_span;
    PpuVirtualTilemapBandLookup band_lookup;
    const void *context;
    int32_t camera_x, camera_y;
    uint16_t hscroll_anchor, vscroll_anchor;
    uint8_t flags;
} PpuVirtualTilemapBinding;

typedef struct BgLayer {
    uint16_t xhScroll, xvScroll;
    bool xtilemapWider, xtilemapHigher;
    uint16_t xtilemapAdr, xtileAdr;
    bool xxbigTiles, xxmosaicEnabled;
} BgLayer;

enum {
    kPpuXPixels = 256,
    kPpuExtraLeftRight = 128,
    kPpuBufWidth = 512,
    kPpuObjApron = 64,
    kPpuSurfaceWidth = 640,
    kPpuYPixels = 224,
    kPpuWidescreenExtentAvailable = UINT16_MAX,
    kPpuExtraTopBottom = 64,
    kPpuBufHeight = 352,
    kPpuObjXWrap = 512,
    kPpuObjYWrap = 256,
    kPpuObjYNegativeFrom = 224,
    kPpuCgramEntries = 256,
    kPpuObjTileIds = 256,
    kPpuOamWords = 256
};

/* Bitsets follow the target's native integer width.  This keeps the common
 * set-bit iteration in registers on 32-bit ports such as ARMv6K/ARM11 while
 * retaining the wider desktop path.  A build may force either width to run
 * the same implementation through parity tests on a different host. */
#ifndef SNESRECOMP_PPU_BIT_WORD_BITS
#if UINTPTR_MAX <= UINT32_MAX
#define SNESRECOMP_PPU_BIT_WORD_BITS 32
#else
#define SNESRECOMP_PPU_BIT_WORD_BITS 64
#endif
#endif

#if SNESRECOMP_PPU_BIT_WORD_BITS == 32
typedef uint32_t PpuBitWord;
#elif SNESRECOMP_PPU_BIT_WORD_BITS == 64
typedef uint64_t PpuBitWord;
#else
#error "SNESRECOMP_PPU_BIT_WORD_BITS must be 32 or 64"
#endif

enum {
    kPpuBitWordBits = SNESRECOMP_PPU_BIT_WORD_BITS,
    kPpuObjMaskWords = 128 / kPpuBitWordBits,
    kPpuPixelMaskWords = kPpuXPixels / kPpuBitWordBits
};

typedef uint16_t PpuZbufType;
typedef struct PpuPixelPrioBufs { PpuZbufType data[kPpuBufWidth]; }
    PpuPixelPrioBufs;

enum { kPpuObjSampleCacheCount = 8 };

/* Per-scanline OBJ winners for the coordinate spaces and source subsets that
 * scanout may use.  A handful of entries covers the live/authentic views plus
 * overlay removal, relocation, and semantic HUD ranges.  Keeping this bounded
 * scratch storage on the PPU avoids re-walking all 128 OAM slots for every
 * output pixel. */
typedef struct PpuObjSampleCache {
    PpuPixelPrioBufs pixels;
    PpuBitWord opaque[kPpuPixelMaskWords];
    int16_t screen_y, x_offset;
    uint8_t include_first, include_count;
    uint8_t exclude_first, exclude_count;
    bool valid;
} PpuObjSampleCache;

/* Decoded planar rows are derived scratch, indexed by their first VRAM word.
 * Keeping the source words beside the pixels makes direct VRAM mutation and
 * savestate loads self-invalidating without a dirty tree or write barrier. */
typedef struct PpuDecoded4bppRow {
    uint32_t source;
    uint32_t pixels;
} PpuDecoded4bppRow;

/* The virtual tilemap callback returns one word for an entire 8x8 tile.  The
 * renderer samples pixels, so retain the last word per eligible BG layer for
 * the duration of one scanline instead of repeating the same callback eight
 * times. */
typedef struct PpuVirtualSampleCache {
    int32_t tile_x, tile_y;
    uint16_t entry;
    uint8_t band;
    bool found;
    bool valid;
} PpuVirtualSampleCache;

/* Mode 7 is affine across a scanline.  This cache carries the transformed
 * canvas coordinate between adjacent pixel samples (and shares it between
 * EXTBG's two logical layers), preserving the general sampler while avoiding
 * a complete origin calculation for every pixel. */
typedef struct PpuMode7SampleCache {
    uint32_t source_x, source_y;
    int32_t screen_x;
    int sample_y, step_x, step_y;
    bool valid;
} PpuMode7SampleCache;

typedef enum PpuOverlaySource {
    kPpuOverlaySource_Bg1,
    kPpuOverlaySource_Bg2,
    kPpuOverlaySource_Bg3,
    kPpuOverlaySource_Bg4,
    kPpuOverlaySource_Obj,
    kPpuOverlaySource_Count
} PpuOverlaySource;

enum {
    kPpuOverlayFlag_RemoveFromGame = 1,
    kPpuOverlayFlag_MarkObjColorMath = 2,
    kPpuOverlayFlag_MarkBgHalfAdd = 4,
    kPpuOverlayFlag_ApplyBgFixedColorSubtract = 8,
    kPpuOverlayFlag_MarkFullAddSubscreen = 16,
    kPpuOverlayFlag_MarkMainScreenWinner = 32,
    kPpuOverlayFlag_MarkOwningScreenWinner = 64
};

typedef enum PpuOverlayTransparentFill {
    kPpuOverlayTransparentFill_None,
    kPpuOverlayTransparentFill_Black,
    kPpuOverlayTransparentFill_Cgram
} PpuOverlayTransparentFill;

typedef enum PpuWidescreenBandFill {
    kPpuWidescreenBandFill_Inherit,
    kPpuWidescreenBandFill_Transparent,
    kPpuWidescreenBandFill_LiveWorld,
    kPpuWidescreenBandFill_Clamp,
    kPpuWidescreenBandFill_Mirror,
    kPpuWidescreenBandFill_Repeat,
    kPpuWidescreenBandFill_RawWrap
} PpuWidescreenBandFill;

typedef enum PpuWidescreenMotion {
    kPpuWidescreenMotion_FillRelative,
    kPpuWidescreenMotion_NormalScroll
} PpuWidescreenMotion;

typedef struct PpuWidescreenLayerPolicy {
    PpuWidescreenBandFill fill;
    PpuWidescreenMotion motion;
    bool band_override;
} PpuWidescreenLayerPolicy;

enum { kPpuMode7CanvasExtent = 1024 };

typedef struct PpuMode7Override {
    const uint32_t *rgba;
    int width, height;
    int canvasX0, canvasY0, canvasX1, canvasY1;
    uint8_t wrap;
} PpuMode7Override;

typedef struct PpuOverlayCapture {
    int16_t x0, x1, y0, y1;
    uint8_t flags;
    uint8_t transparentFillMode, transparentFillCgram;
    uint8_t transparentFillConfigured;
    uint8_t oamFirst, oamCount;
} PpuOverlayCapture;

typedef struct PpuObjRangeBounds { int16_t x0, y0, x1, y1; }
    PpuObjRangeBounds;

typedef struct PpuObjRangeCapture {
    int16_t x0, y0, x1, y1;
    uint8_t first, count;
    uint8_t *pixels;
    uint32_t pitch;
} PpuObjRangeCapture;

/* Per-PPU scanline workspace. Keeping the capture planes with the renderer
 * avoids a roughly 16 KiB call frame on constrained ports without introducing
 * process-global state or changing the portable rendering algorithm. */
typedef struct PpuNativeLineScratch {
    uint16_t layerMain[kPpuOverlaySource_Count][kPpuBufWidth];
    uint16_t layerSub[kPpuOverlaySource_Count][kPpuBufWidth];
    uint16_t mainPixels[kPpuBufWidth], subPixels[kPpuBufWidth];
    uint16_t originalMain[kPpuBufWidth], originalSub[kPpuBufWidth];
    uint8_t bands[2][kPpuBufWidth];
} PpuNativeLineScratch;

typedef struct PpuObjPart {
    int16_t x, y;
    uint16_t tile_attr;
    uint8_t size;
} PpuObjPart;

enum {
    kPpuRenderFlags_NewRenderer = 1,
    kPpuRenderFlags_4x4Mode7 = 2,
    kPpuRenderFlags_Height240 = 4,
    kPpuRenderFlags_NoSpriteLimits = 8,
    /* Test/diagnostic switch for comparing optimized scanout against the
     * general per-pixel renderer without changing any emulated state. */
    kPpuRenderFlags_ReferencePixelRenderer = 16
};

typedef struct Layer {
    bool xmainScreenEnabled, xsubScreenEnabled;
    bool xmainScreenWindowed, xsubScreenWindowed;
} Layer;

typedef struct WindowLayer {
    bool xwindow1enabled, xwindow2enabled;
    bool xwindow1inversed, xwindow2inversed;
    uint8_t xmaskLogic;
} WindowLayer;

#define PPU_SAVESTATE_REGS_SIZE 0x40
#define PPU_SAVESTATE_MEM_SIZE 0x10420

struct Ppu {
    uint8 inidisp, obsel, oamaddl, oamaddh, bgmode, mosaic;
    uint8 bgXsc[4];
    uint16 bgTileAdr;
    uint8 m7sel, setini;
    uint16 hScroll[4], vScroll[4];
    int16_t m7matrix[8];
    uint16 fixedColor;
    uint32 windowsel;
    uint8 window1left, window1right, window2left, window2right;
    uint16 wbgobjlog;
    uint8 screenEnabled[2], screenWindowed[2], cgadsub, cgwsel;

    uint16_t vramPointer;
    bool vramIncrementOnHigh;
    uint8_t vramRemapMode, vramIncrement;
    uint16_t vramReadBuffer;
    uint8_t cgramPointer;
    bool cgramSecondWrite;
    uint8_t cgramBuffer, oamAdr;
    bool oamInHigh, oamSecondWrite;
    uint8_t oamBuffer;
    bool timeOver, rangeOver;
    uint8_t scrollPrev, scrollPrev2, mosaicStartLine, m7prev;
    int32_t m7startX, m7startY;
    bool evenFrame, frameOverscan, frameInterlace;
    uint16_t hCount, vCount;
    bool hCountSecond, vCountSecond, countersLatched;

    uint8_t extraLeftCur, extraRightCur, extraLeftRight;
    uint8_t extraTopCur, extraBottomCur;
    uint8_t verticalMarginLayerClip;
    uint8_t verticalMarginTopRows[4], verticalMarginBottomRows[4];
    PpuVirtualTilemapBinding virtualTilemap[4];
    int16_t objPosX[128], objPosY[128];
    uint8_t objPosValid[128], objCameraRelative[128];
    uint8_t wsHudSplitHeight, wsHudLeftEnd, wsHudRightStart;
    uint8_t wsHudPlayerRowY, wsHudLeftOnlyY, wsBg3WidenY;
    uint8_t wsPadCapturedToBudget, wsLayerClamp, wsLayerMirror;
    uint8_t wsLayerRepeat, wsLayerNormalScroll;
    uint8_t wsBandFill[4][kPpuYPixels];
    uint8_t wsBandMotion[4][kPpuYPixels];
    uint16_t wsLayerExtentLeftDefault[4], wsLayerExtentRightDefault[4];
    uint16_t wsLayerExtentTop[4], wsLayerExtentBottom[4];
    uint16_t wsLayerExtentLeft[4][kPpuYPixels];
    uint16_t wsLayerExtentRight[4][kPpuYPixels];
    uint8_t lastMosaicModulo, lastBrightnessMult;
    bool lineHasSprites;
    uint32_t renderFlags;
    uint32_t cgramRgb[kPpuCgramEntries];
    bool cgramRgbValid;
    PpuPixelPrioBufs bgBuffers[2], objBuffer;
    /* Dense hardware-shaped 128-bit eligibility for every signed scanline the
     * renderer can expose. Rebuilt lazily after OAM geometry changes, then
     * consumed in hardware rotation order. */
    PpuBitWord objScanlineMasks[kPpuBufHeight][kPpuObjMaskWords];
    bool objScanlineMasksValid;
    PpuObjSampleCache objSampleCache[kPpuObjSampleCacheCount];
    PpuVirtualSampleCache virtualSampleCache[2];
    PpuMode7SampleCache mode7SampleCache;
    PpuNativeLineScratch nativeLineScratch;
    PpuDecoded4bppRow decoded4bppRows[0x8000];
    /* Low 16 bits are the source word; high 16 bits hold eight 2-bit pixels. */
    uint32_t decoded2bppRows[0x8000];
    PpuPixelPrioBufs overlayBuffers[kPpuOverlaySource_Count];
    uint8_t overlayVirtualBands[2][kPpuBufWidth];
    PpuPixelPrioBufs overlayObjFullAddBuffer;
    PpuOverlayCapture overlayCaptures[kPpuOverlaySource_Count];
    uint8_t overlayObjRelocatedFirst, overlayObjRelocatedCount;
    PpuObjRangeCapture objRangeCapture;
    uint32_t renderPitch;
    uint8_t *renderBuffer;
    uint32_t authenticRenderPitch;
    uint8_t *authenticRenderBuffer;
    uint16_t authenticHScroll[2][kPpuYPixels];
    int16_t authenticObjOffsetX;
    uint8_t authenticHScrollMask;
    uint32_t overlayRenderPitch[kPpuOverlaySource_Count];
    uint8_t *overlayRenderBuffer[kPpuOverlaySource_Count];
    uint8_t *overlayRenderBands[kPpuOverlaySource_Count][3];
    uint8_t overlayRenderMaybeDirty[kPpuOverlaySource_Count];
    uint8_t overlayRenderContentMask[kPpuOverlaySource_Count];
    uint8_t m7OverlayMaybeDirty;
    uint8_t *m7OverlayBuffer;
    uint32_t m7OverlayPitch;
    uint8_t m7OverlayScale;
    PpuMode7Override m7Override;
    uint8_t brightnessMult[63], brightnessMultHalf[64];
    uint8_t mosaicModulo[kPpuXPixels];
    void *pad2;

    uint16_t cgram[kPpuCgramEntries];
    uint16_t oam[kPpuOamWords];
    uint8_t highOam[0x20];
    uint16_t vram[0x8000];
};

static inline PpuWidescreenLayerPolicy PpuResolveWidescreenLayerPolicy(
        const Ppu *ppu, uint8_t layer, int screen_y) {
    PpuWidescreenLayerPolicy policy = {
        kPpuWidescreenBandFill_RawWrap,
        kPpuWidescreenMotion_FillRelative,
        false
    };
    int row;
    uint8_t bit;
    if (ppu == NULL || layer >= 4u) return policy;
    row = screen_y < 0 ? 0 : screen_y >= kPpuYPixels ? kPpuYPixels - 1 : screen_y;
    if (ppu->wsBandFill[layer][row] != 0u) {
        policy.fill = (PpuWidescreenBandFill)ppu->wsBandFill[layer][row];
        policy.motion = (PpuWidescreenMotion)ppu->wsBandMotion[layer][row];
        policy.band_override = true;
        return policy;
    }
    bit = (uint8_t)(1u << layer);
    if (ppu->wsLayerRepeat & bit) policy.fill = kPpuWidescreenBandFill_Repeat;
    else if (ppu->wsLayerMirror & bit) policy.fill = kPpuWidescreenBandFill_Mirror;
    else if (ppu->wsLayerClamp & bit) policy.fill = kPpuWidescreenBandFill_Clamp;
    if (ppu->wsLayerNormalScroll & bit)
        policy.motion = kPpuWidescreenMotion_NormalScroll;
    return policy;
}

static inline bool PpuMapWidescreenLayerXWithPolicy(
        const Ppu *ppu, uint8_t layer, int x, int *source_x,
        const PpuWidescreenLayerPolicy *policy) {
    if (ppu == NULL || layer >= 4u || source_x == NULL || policy == NULL)
        return false;
    if (x >= 0 && x < kPpuXPixels) { *source_x = x; return true; }
    switch (policy->fill) {
        case kPpuWidescreenBandFill_Mirror:
            *source_x = x < 0 ? -x : kPpuXPixels * 2 - 2 - x;
            if (policy->motion == kPpuWidescreenMotion_NormalScroll)
                *source_x = (*source_x - 2 * (ppu->hScroll[layer] & 0xff)) & 0xff;
            return *source_x >= 0 && *source_x < kPpuXPixels;
        case kPpuWidescreenBandFill_Repeat:
            *source_x = x < 0 ? kPpuXPixels + x : x - kPpuXPixels;
            return *source_x >= 0 && *source_x < kPpuXPixels;
        case kPpuWidescreenBandFill_Transparent:
        case kPpuWidescreenBandFill_Clamp:
            return false;
        default:
            *source_x = x;
            return true;
    }
}

static inline bool PpuMapWidescreenLayerX(const Ppu *ppu, uint8_t layer,
        int screen_y, int x, int *source_x,
        PpuWidescreenLayerPolicy *out_policy) {
    PpuWidescreenLayerPolicy policy =
        PpuResolveWidescreenLayerPolicy(ppu, layer, screen_y);
    if (out_policy != NULL) *out_policy = policy;
    return PpuMapWidescreenLayerXWithPolicy(ppu, layer, x, source_x, &policy);
}

#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + ((level6) ? 2 : 0))
#define SPRITE_PRIO_TO_PRIO_HI(prio) ((prio) * 4 + 2)
#define IS_SCREEN_ENABLED(ppu, sub, layer) ((ppu)->screenEnabled[(sub)] & (1u << (layer)))
#define IS_SCREEN_WINDOWED(ppu, sub, layer) ((ppu)->screenWindowed[(sub)] & (1u << (layer)))
#define GET_WINDOW_FLAGS(ppu, layer) ((ppu)->windowsel >> ((layer) * 4))
#define PPU_brightness(ppu) ((ppu)->inidisp & 0x0f)
#define PPU_forcedBlank(ppu) ((ppu)->inidisp & 0x80)
#define PPU_objSize(ppu) ((ppu)->obsel >> 5)
#define PPU_objTileAdr1(ppu) (((ppu)->obsel & 7) << 13)
#define PPU_objTileAdr2(ppu) (PPU_objTileAdr1(ppu) + ((((ppu)->obsel & 0x18) + 8) << 9))
#define PPU_objPriority(ppu) ((ppu)->oamaddh & 0x80)
#define PPU_mode(ppu) ((ppu)->bgmode & 7)
#define PPU_bg3priority(ppu) ((ppu)->bgmode & 8)
#define PPU_bigTiles(ppu, layer) (((ppu)->bgmode >> (layer)) & 0x10)
#define PPU_mosaicEnabled(ppu, layer) ((ppu)->mosaic & (1u << (layer)))
#define PPU_mosaicSize(ppu) (((ppu)->mosaic >> 4) + 1)
#define PPU_bgTilemapWider(ppu, layer) ((ppu)->bgXsc[(layer)] & 1)
#define PPU_bgTilemapHigher(ppu, layer) ((ppu)->bgXsc[(layer)] & 2)
#define PPU_bgTilemapAdr(ppu, layer) (((ppu)->bgXsc[(layer)] & 0xfc) << 8)
#define PPU_bgTileAdr(ppu, layer) ((((ppu)->bgTileAdr >> ((layer) * 4)) & 0xf) << 12)
#define PPU_m7xFlip(ppu) ((ppu)->m7sel & 1)
#define PPU_m7yFlip(ppu) ((ppu)->m7sel & 2)
#define PPU_m7charFill(ppu) ((ppu)->m7sel & 0x40)
#define PPU_m7largeField(ppu) ((ppu)->m7sel & 0x80)
#define PPU_directColor(ppu) (((ppu)->cgwsel & 1) != 0)
#define PPU_addSubscreen(ppu) (((ppu)->cgwsel & 2) != 0)
#define PPU_preventMathMode(ppu) (((ppu)->cgwsel >> 4) & 3)
#define PPU_clipMode(ppu) (((ppu)->cgwsel >> 6) & 3)
#define PPU_mathEnabled(ppu) ((ppu)->cgadsub & 0x3f)
#define PPU_halfColor(ppu) (((ppu)->cgadsub & 0x40) != 0)
#define PPU_subtractColor(ppu) (((ppu)->cgadsub & 0x80) != 0)
#define PPU_fixedColorR(ppu) ((ppu)->fixedColor & 0x1f)
#define PPU_fixedColorG(ppu) (((ppu)->fixedColor >> 5) & 0x1f)
#define PPU_fixedColorB(ppu) (((ppu)->fixedColor >> 10) & 0x1f)
#define PPU_interlace(ppu) (((ppu)->setini & 1) != 0)
#define PPU_objInterlace(ppu) (((ppu)->setini & 2) != 0)
#define PPU_overscan(ppu) (((ppu)->setini & 4) != 0)
#define PPU_pseudoHires(ppu) (((ppu)->setini & 8) != 0)
#define PPU_m7extBg(ppu) (((ppu)->setini & 0x40) != 0)

enum { kWindow1Inversed = 1, kWindow1Enabled = 2,
       kWindow2Inversed = 4, kWindow2Enabled = 8 };

extern const uint8_t kPpuSpriteSizes[8][2];
static inline int PpuObjSizeForSizeBit(const Ppu *ppu, int large) {
    return ppu ? kPpuSpriteSizes[PPU_objSize(ppu)][large ? 1 : 0] : 0;
}
static inline int PpuSurfaceApron(const Ppu *ppu, size_t pitch) {
    int width = (int)(pitch / 4u);
    int span = kPpuXPixels + ppu->extraLeftRight * 2;
    int apron = (width - span) / 2;
    return apron > 0 ? apron : 0;
}
static inline int PpuVerticalOrigin(const Ppu *ppu) { return ppu->extraTopCur; }
static inline int PpuRenderedHeight(const Ppu *ppu) {
    return kPpuYPixels + ppu->extraTopCur + ppu->extraBottomCur;
}

Ppu *ppu_init(void);
void ppu_free(Ppu *);
void ppu_reset(Ppu *);
bool ppu_checkOverscan(Ppu *);
void ppu_handleVblank(Ppu *);
void ppu_runLine(Ppu *, int);
void ppu_runMarginLine(Ppu *, int);
uint8_t ppu_read(Ppu *, uint8_t);
void ppu_write(Ppu *, uint8_t, uint8_t);
void ppu_saveload(Ppu *, SaveLoadInfo *);
void PpuBeginDrawing(Ppu *, uint8_t *, size_t, uint32_t);
bool PpuBindAuthenticSurface(Ppu *, uint8_t *, size_t);
bool PpuAuthenticSurfaceBound(const Ppu *);
bool PpuAuthenticSurfaceReady(const Ppu *);
enum { kPpuAuthenticCameraLayer_Bg1 = 1, kPpuAuthenticCameraLayer_Bg2 = 2,
       kPpuAuthenticCameraLayer_All = 3 };
bool PpuSetAuthenticCameraFrame(Ppu *, uint8_t, const uint16_t *,
                                const uint16_t *, int);
bool PpuAuthenticCameraFrameReady(const Ppu *, uint8_t);
void PpuClearAuthenticCameraFrame(Ppu *);
void PpuClearOverlayBindings(Ppu *);
bool PpuBindOverlaySurface(Ppu *, PpuOverlaySource, uint8_t *, size_t);
bool PpuBindOverlayPrioSurface(Ppu *, PpuOverlaySource, int, uint8_t *);
bool PpuOverlaySurfaceHasContent(const Ppu *, PpuOverlaySource, int);
void PpuClearOverlayCaptures(Ppu *);
bool PpuSetOverlayCapture(Ppu *, PpuOverlaySource, int, int, int, int, uint8_t);
bool PpuSetOverlayTransparentFill(Ppu *, PpuOverlaySource,
                                  PpuOverlayTransparentFill, uint8_t);
uint32_t PpuOverlayTransparentFillColor(const Ppu *, PpuOverlaySource);
bool PpuSetOverlayOamRange(Ppu *, uint8_t, uint8_t);
bool PpuSetOverlayRelocatedOamRange(Ppu *, uint8_t, uint8_t);
bool PpuSetObjRangeCapture(Ppu *, uint8_t, uint8_t, int, int, int, int,
                           uint8_t *, size_t);
bool PpuBindMode7OverlaySurface(Ppu *, uint8_t *, size_t, uint8_t);
bool PpuSetMode7Override(Ppu *, const uint32_t *, int, int, int, int,
                         int, int, uint8_t);
void PpuSetExtraSpace(Ppu *, uint8_t);
void PpuSetExtraSpaceCentered(Ppu *, uint8_t);
void PpuSetExtraSideSpace(Ppu *, int, int, int);
void PpuSetExtraVerticalSpace(Ppu *, int, int);
void PpuSetVerticalMarginLayerClip(Ppu *, uint8_t, int, int);
bool PpuSetVirtualTilemap(Ppu *, uint8_t, const PpuVirtualTilemapBinding *);
void PpuClearVirtualTilemaps(Ppu *);
void PpuClearObjExactPositions(Ppu *);
void PpuSetObjExactPosition(Ppu *, uint8_t, int, int);
void PpuClearObjCameraRelative(Ppu *);
void PpuSetObjCameraRelative(Ppu *, uint8_t, bool);
bool PpuResolveObjSlot(Ppu *, uint8_t, PpuObjPart *);
bool PpuResolveObjSlots(Ppu *, uint8_t, uint8_t, uint8_t,
                        PpuObjPart *, int, int *);
bool PpuGetPartBounds(const PpuObjPart *, int, PpuObjRangeBounds *);
bool PpuRasterizeParts(Ppu *, const PpuObjPart *, int,
                       const PpuObjRangeBounds *, uint32_t *, int, int, size_t);
bool PpuGetObjRangeBounds(Ppu *, uint8_t, uint8_t, uint8_t,
                          PpuObjRangeBounds *);
bool PpuRasterizeObjRange(Ppu *, uint8_t, uint8_t, uint8_t,
                          const PpuObjRangeBounds *, uint32_t *, int, int,
                          size_t);
void PpuSetWidescreenHudSplit(Ppu *, uint8_t, uint8_t, uint8_t, uint8_t,
                              uint8_t);
void PpuSetWidescreenBg3Widen(Ppu *, uint8_t);
void PpuSetWidescreenLayerClamp(Ppu *, uint8_t);
void PpuSetWidescreenLayerMirror(Ppu *, uint8_t);
void PpuSetWidescreenLayerRepeat(Ppu *, uint8_t);
void PpuSetWidescreenLayerNormalScroll(Ppu *, uint8_t);
void PpuSetWidescreenLayerBand(Ppu *, uint8_t, uint8_t, uint8_t,
                               PpuWidescreenBandFill, PpuWidescreenMotion);
void PpuSetWidescreenPadCapturedToBudget(Ppu *, uint8_t);
void PpuSetWidescreenLayerRepeatBand(Ppu *, uint8_t, uint8_t, uint8_t);
void PpuSetWidescreenLayerExtent(Ppu *, uint8_t, uint16_t, uint16_t,
                                 uint16_t, uint16_t);
void PpuSetWidescreenLayerExtentBand(Ppu *, uint8_t, uint8_t, uint8_t,
                                     uint16_t, uint16_t);
int PpuGetCurrentRenderScale(Ppu *, uint32_t);

#endif
