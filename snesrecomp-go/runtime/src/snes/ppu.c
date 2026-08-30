#include "ppu.h"

#include "runner_internal.h"
#include "simd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if SR_SIMD_NEON
#include <arm_neon.h>
#define SR_PPU_TILE_SIMD 1
#elif SR_SIMD_SSE2
#include <emmintrin.h>
#define SR_PPU_TILE_SIMD 1
#else
#define SR_PPU_TILE_SIMD 0
#endif

#if defined(_MSC_VER)
#define SR_RESTRICT __restrict
#else
#define SR_RESTRICT restrict
#endif

const uint8_t kPpuSpriteSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

typedef struct SrPpuPixel {
    uint16_t color;
    uint8_t rank;
    uint8_t layer;
    uint8_t priority;
    uint8_t palette;
    uint8_t band;
    bool valid;
} SrPpuPixel;

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    return value > high ? high : value;
}

static int sign13(uint16_t value) {
    value &= 0x1fffu;
    return (value & 0x1000u) != 0u ? (int)value - 0x2000 : value;
}

static int floor_div8(int value) {
    return value >= 0 ? value / 8 : -((7 - value) / 8);
}

static int wrapped_delta10(uint16_t current, uint16_t anchor) {
    int delta = ((int)current - (int)anchor) & 0x3ff;
    return delta >= 0x200 ? delta - 0x400 : delta;
}

static int output_row(const Ppu *ppu, int screen_y) {
    return screen_y + PpuVerticalOrigin(ppu);
}

/* Captures beginning in the synthetic top margin use a compact, margin-local
 * destination. Captures beginning on the authentic screen retain absolute
 * SNES row coordinates even when the main render surface has a top apron. */
static int overlay_row(const PpuOverlayCapture *capture, int screen_y) {
    return capture->y0 < 0 ? screen_y - capture->y0 : screen_y;
}

static int surface_origin_x(const Ppu *ppu, size_t pitch) {
    return PpuSurfaceApron(ppu, pitch) + ppu->extraLeftRight;
}

static void note_surface_binding(Ppu *ppu) {
    if (++ppu->surfaceBindingGeneration == 0u)
        ++ppu->surfaceBindingGeneration;
}

static uint16_t remap_vram(const Ppu *ppu) {
    uint16_t address = ppu->vramPointer;
    switch (ppu->vramRemapMode) {
        case 1:
            return (uint16)((address & 0xff00u) | ((address & 0x00e0u) >> 5) |
                            ((address & 0x001fu) << 3));
        case 2:
            return (uint16)((address & 0xfe00u) | ((address & 0x01c0u) >> 6) |
                            ((address & 0x003fu) << 3));
        case 3:
            return (uint16)((address & 0xfc00u) | ((address & 0x0380u) >> 7) |
                            ((address & 0x007fu) << 3));
        default:
            return address;
    }
}

static void update_brightness(Ppu *ppu) {
    uint8_t brightness = PPU_brightness(ppu);
    if (ppu->lastBrightnessMult == brightness) return;
    ppu->lastBrightnessMult = brightness;
    for (int value = 0; value < 32; ++value) {
        uint8_t expanded = (uint8_t)((((value << 3) | (value >> 2)) *
                                      brightness) / 15);
        ppu->brightnessMult[value] = expanded;
        ppu->brightnessMultHalf[value * 2] = expanded;
        ppu->brightnessMultHalf[value * 2 + 1] = expanded;
    }
    memset(ppu->brightnessMult + 32, ppu->brightnessMult[31], 31u);
    ppu->cgramRgbValid = false;
}

static uint32_t color_argb(const Ppu *ppu, uint16_t color) {
    return 0xff000000u |
           ((uint32_t)ppu->brightnessMult[color & 0x1fu] << 16) |
           ((uint32_t)ppu->brightnessMult[(color >> 5) & 0x1fu] << 8) |
           ppu->brightnessMult[(color >> 10) & 0x1fu];
}

static uint32_t color_rgb(const Ppu *ppu, uint16_t color) {
    return color_argb(ppu, color) & 0x00ffffffu;
}

static void rebuild_cgram_rgb(Ppu *ppu) {
    for (int index = 0; index < kPpuCgramEntries; ++index)
        ppu->cgramRgb[index] = color_rgb(ppu, ppu->cgram[index]);
    ppu->cgramRgbValid = true;
}

static uint16_t color_math(uint16_t first, uint16_t second,
                           bool subtract, bool half) {
    int r = first & 31;
    int g = (first >> 5) & 31;
    int b = (first >> 10) & 31;
    int r2 = second & 31;
    int g2 = (second >> 5) & 31;
    int b2 = (second >> 10) & 31;
    if (subtract) {
        r -= r2; g -= g2; b -= b2;
        if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    } else {
        r += r2; g += g2; b += b2;
    }
    if (half) { r >>= 1; g >>= 1; b >>= 1; }
    if (r > 31) r = 31; if (g > 31) g = 31; if (b > 31) b = 31;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static void reset_layer_policy(Ppu *ppu) {
    ppu->wsLayerClamp = 0;
    ppu->wsLayerMirror = 0;
    ppu->wsLayerRepeat = 0;
    ppu->wsLayerNormalScroll = 0;
    ppu->wsPadCapturedToBudget = 0;
    memset(ppu->wsBandFill, 0, sizeof(ppu->wsBandFill));
    memset(ppu->wsBandMotion, 0, sizeof(ppu->wsBandMotion));
    memset(ppu->wsLayerExtentLeftDefault, 0xff,
           sizeof(ppu->wsLayerExtentLeftDefault));
    memset(ppu->wsLayerExtentRightDefault, 0xff,
           sizeof(ppu->wsLayerExtentRightDefault));
    memset(ppu->wsLayerExtentTop, 0xff, sizeof(ppu->wsLayerExtentTop));
    memset(ppu->wsLayerExtentBottom, 0xff, sizeof(ppu->wsLayerExtentBottom));
    memset(ppu->wsLayerExtentLeft, 0xff, sizeof(ppu->wsLayerExtentLeft));
    memset(ppu->wsLayerExtentRight, 0xff, sizeof(ppu->wsLayerExtentRight));
    memset(ppu->virtualTilemap, 0, sizeof(ppu->virtualTilemap));
    memset(ppu->abiVirtualTilemap, 0, sizeof(ppu->abiVirtualTilemap));
}

Ppu *ppu_init(void) {
    Ppu *ppu = (Ppu *)calloc(1u, sizeof(Ppu));
    if (ppu != NULL) {
        ppu->lastBrightnessMult = 0xffu;
        ppu->vramIncrement = 1u;
        ppu->surfaceBindingGeneration = 1u;
        reset_layer_policy(ppu);
    }
    return ppu;
}

void ppu_free(Ppu *ppu) { free(ppu); }

void ppu_reset(Ppu *ppu) {
    uint8_t *render;
    uint32_t render_pitch, render_height, flags;
    uint8_t *authentic;
    uint32_t authentic_pitch, authentic_height;
    uint8_t *overlays[kPpuOverlaySource_Count];
    uint32_t overlay_pitch[kPpuOverlaySource_Count];
    uint32_t overlay_height[kPpuOverlaySource_Count];
    uint8_t *bands[kPpuOverlaySource_Count][3];
    uint8_t *m7;
    uint32_t m7_pitch;
    uint8_t m7_scale;
    uint64_t surface_binding_generation;
    if (ppu == NULL) return;
    render = ppu->renderBuffer;
    render_pitch = ppu->renderPitch;
    render_height = ppu->renderHeight;
    flags = ppu->renderFlags;
    authentic = ppu->authenticRenderBuffer;
    authentic_pitch = ppu->authenticRenderPitch;
    authentic_height = ppu->authenticRenderHeight;
    memcpy(overlays, ppu->overlayRenderBuffer, sizeof(overlays));
    memcpy(overlay_pitch, ppu->overlayRenderPitch, sizeof(overlay_pitch));
    memcpy(overlay_height, ppu->overlayRenderHeight, sizeof(overlay_height));
    memcpy(bands, ppu->overlayRenderBands, sizeof(bands));
    m7 = ppu->m7OverlayBuffer;
    m7_pitch = ppu->m7OverlayPitch;
    m7_scale = ppu->m7OverlayScale;
    surface_binding_generation = ppu->surfaceBindingGeneration;
    memset(ppu, 0, sizeof(*ppu));
    ppu->renderBuffer = render;
    ppu->renderPitch = render_pitch;
    ppu->renderHeight = render_height;
    ppu->renderFlags = flags;
    ppu->authenticRenderBuffer = authentic;
    ppu->authenticRenderPitch = authentic_pitch;
    ppu->authenticRenderHeight = authentic_height;
    memcpy(ppu->overlayRenderBuffer, overlays, sizeof(overlays));
    memcpy(ppu->overlayRenderPitch, overlay_pitch, sizeof(overlay_pitch));
    memcpy(ppu->overlayRenderHeight, overlay_height, sizeof(overlay_height));
    memcpy(ppu->overlayRenderBands, bands, sizeof(bands));
    ppu->m7OverlayBuffer = m7;
    ppu->m7OverlayPitch = m7_pitch;
    ppu->m7OverlayScale = m7_scale;
    ppu->surfaceBindingGeneration = surface_binding_generation;
    note_surface_binding(ppu);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source)
        ppu->overlayRenderMaybeDirty[source] = overlays[source] != NULL;
    ppu->m7OverlayMaybeDirty = m7 != NULL;
    ppu->lastBrightnessMult = 0xffu;
    ppu->vramIncrement = 1u;
    reset_layer_policy(ppu);
}

void ppu_saveload(Ppu *ppu, SaveLoadInfo *info) {
    uint32_t version[2] = {0x30555050u,
                           PPU_SAVESTATE_REGS_SIZE + PPU_SAVESTATE_MEM_SIZE};
    if (ppu == NULL || info == NULL || info->func == NULL) return;
    if (!info->portable) {
        info->func(info, version, sizeof(version));
        info->func(info, &ppu->inidisp, PPU_SAVESTATE_REGS_SIZE);
        info->func(info, &ppu->cgram, PPU_SAVESTATE_MEM_SIZE);
    } else {
        uint32_t magic = 0x31555050u;
        uint32_t revision = 1u;
        saveload_u32(info, &magic);
        saveload_u32(info, &revision);
        if (!info->saving && (magic != 0x31555050u || revision != 1u)) {
            info->failed = true;
            return;
        }
        saveload_u8(info, &ppu->inidisp);
        saveload_u8(info, &ppu->obsel);
        saveload_u8(info, &ppu->oamaddl);
        saveload_u8(info, &ppu->oamaddh);
        saveload_u8(info, &ppu->bgmode);
        saveload_u8(info, &ppu->mosaic);
        saveload_bytes(info, ppu->bgXsc, sizeof(ppu->bgXsc));
        saveload_u16(info, &ppu->bgTileAdr);
        saveload_u8(info, &ppu->m7sel);
        saveload_u8(info, &ppu->setini);
        saveload_u16_array(info, ppu->hScroll, 4u);
        saveload_u16_array(info, ppu->vScroll, 4u);
        saveload_i16_array(info, ppu->m7matrix, 8u);
        saveload_u16(info, &ppu->fixedColor);
        saveload_u32(info, &ppu->windowsel);
        saveload_u8(info, &ppu->window1left);
        saveload_u8(info, &ppu->window1right);
        saveload_u8(info, &ppu->window2left);
        saveload_u8(info, &ppu->window2right);
        saveload_u16(info, &ppu->wbgobjlog);
        saveload_bytes(info, ppu->screenEnabled,
                       sizeof(ppu->screenEnabled));
        saveload_bytes(info, ppu->screenWindowed,
                       sizeof(ppu->screenWindowed));
        saveload_u8(info, &ppu->cgadsub);
        saveload_u8(info, &ppu->cgwsel);

        /* These read/write latches were accidentally omitted by the raw v9
         * PPU span. The versioned portable contract includes them. */
        saveload_u16(info, &ppu->vramPointer);
        saveload_bool(info, &ppu->vramIncrementOnHigh);
        saveload_u8(info, &ppu->vramRemapMode);
        saveload_u8(info, &ppu->vramIncrement);
        saveload_u16(info, &ppu->vramReadBuffer);
        saveload_u8(info, &ppu->cgramPointer);
        saveload_bool(info, &ppu->cgramSecondWrite);
        saveload_u8(info, &ppu->cgramBuffer);
        saveload_u8(info, &ppu->oamAdr);
        saveload_bool(info, &ppu->oamInHigh);
        saveload_bool(info, &ppu->oamSecondWrite);
        saveload_u8(info, &ppu->oamBuffer);
        saveload_bool(info, &ppu->timeOver);
        saveload_bool(info, &ppu->rangeOver);
        saveload_u8(info, &ppu->scrollPrev);
        saveload_u8(info, &ppu->scrollPrev2);
        saveload_u8(info, &ppu->mosaicStartLine);
        saveload_u8(info, &ppu->m7prev);
        saveload_i32(info, &ppu->m7startX);
        saveload_i32(info, &ppu->m7startY);
        saveload_bool(info, &ppu->evenFrame);
        saveload_bool(info, &ppu->frameOverscan);
        saveload_bool(info, &ppu->frameInterlace);
        saveload_u16(info, &ppu->hCount);
        saveload_u16(info, &ppu->vCount);
        saveload_bool(info, &ppu->hCountSecond);
        saveload_bool(info, &ppu->vCountSecond);
        saveload_bool(info, &ppu->countersLatched);

        saveload_u16_array(info, ppu->cgram, kPpuCgramEntries);
        saveload_u16_array(info, ppu->oam, kPpuOamWords);
        saveload_bytes(info, ppu->highOam, sizeof(ppu->highOam));
        saveload_u16_array(info, ppu->vram,
                           sizeof(ppu->vram) / sizeof(ppu->vram[0]));
    }
    /* Derived geometry is deliberately outside the serialized contract. A
     * save is observational and must not invalidate live caches. */
    if (!info->saving) {
        ppu->objScanlineMasksValid = false;
        ppu->cgramRgbValid = false;
    }
}

static bool output_surface_fits_geometry(
        const uint8_t *pixels, uint32_t pitch, uint32_t height,
        uint32_t horizontal_budget, uint32_t top, uint32_t bottom) {
    uint64_t required_width;
    uint64_t required_height;
    if (pixels == NULL) return pitch == 0u && height == 0u;
    required_width = (uint64_t)kPpuXPixels + horizontal_budget * 2u;
    required_height = (uint64_t)kPpuYPixels + top + bottom;
    return pitch != 0u && pitch % sizeof(uint32_t) == 0u &&
        pitch / sizeof(uint32_t) >= required_width &&
        height >= required_height && height <= kPpuBufHeight;
}

bool PpuOutputSurfacesFitGeometry(
        const Ppu *ppu, uint32_t horizontal_budget,
        uint32_t top, uint32_t bottom) {
    if (ppu == NULL || horizontal_budget > kPpuExtraLeftRight ||
        top > kPpuExtraTopBottom || bottom > kPpuExtraTopBottom)
        return false;
    return output_surface_fits_geometry(
               ppu->renderBuffer, ppu->renderPitch, ppu->renderHeight,
               horizontal_budget, top, bottom) &&
        output_surface_fits_geometry(
               ppu->authenticRenderBuffer, ppu->authenticRenderPitch,
               ppu->authenticRenderHeight, horizontal_budget, top, bottom);
}

bool PpuBeginDrawingSized(Ppu *ppu, uint8_t *pixels, size_t pitch,
                          uint32_t height, uint32_t render_flags) {
    if (ppu == NULL || pitch > UINT32_MAX ||
        !output_surface_fits_geometry(
            pixels, (uint32_t)pitch, height, ppu->extraLeftRight,
            ppu->extraTopCur, ppu->extraBottomCur))
        return false;
    ppu->renderBuffer = pixels;
    ppu->renderPitch = pixels != NULL ? (uint32_t)pitch : 0u;
    ppu->renderHeight = pixels != NULL ? height : 0u;
    ppu->renderFlags = render_flags;
    note_surface_binding(ppu);
    /* The struct remains intentionally inspectable to host enhancements.
     * Refresh the derived palette at output binding so direct CGRAM edits are
     * visible without a write barrier in the scanline hot path. */
    ppu->cgramRgbValid = false;
    return true;
}

void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch,
                     uint32_t render_flags) {
    (void)PpuBeginDrawingSized(
        ppu, pixels, pitch, pixels != NULL ? kPpuBufHeight : 0u,
        render_flags);
}

bool PpuBindAuthenticSurfaceSized(
        Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t height) {
    size_t minimum;
    if (ppu == NULL) return false;
    minimum = kPpuXPixels + (size_t)ppu->extraLeftRight * 2u;
    if (pixels != NULL && (pitch == 0u || pitch % 4u != 0u ||
        pitch / 4u < minimum || pitch / 4u > kPpuSurfaceWidth ||
        height < (uint32_t)PpuRenderedHeight(ppu) ||
        height > kPpuBufHeight)) return false;
    if (pixels == NULL && (pitch != 0u || height != 0u)) return false;
    ppu->authenticRenderBuffer = pixels;
    ppu->authenticRenderPitch = pixels != NULL ? (uint32_t)pitch : 0u;
    ppu->authenticRenderHeight = pixels != NULL ? height : 0u;
    note_surface_binding(ppu);
    return true;
}

bool PpuBindAuthenticSurface(Ppu *ppu, uint8_t *pixels, size_t pitch) {
    return PpuBindAuthenticSurfaceSized(
        ppu, pixels, pitch, pixels != NULL ? kPpuBufHeight : 0u);
}

bool PpuAuthenticSurfaceBound(const Ppu *ppu) {
    return ppu != NULL && ppu->authenticRenderBuffer != NULL &&
           ppu->authenticRenderPitch != 0u &&
           ppu->authenticRenderHeight != 0u;
}

bool PpuAuthenticSurfaceReady(const Ppu *ppu) {
    size_t width, required;
    if (!PpuAuthenticSurfaceBound(ppu)) return false;
    width = ppu->authenticRenderPitch / 4u;
    required = kPpuXPixels + (size_t)ppu->extraLeftRight * 2u;
    return width >= required && width <= kPpuSurfaceWidth &&
        ppu->authenticRenderHeight >= (uint32_t)PpuRenderedHeight(ppu);
}

bool PpuSetAuthenticCameraFrame(Ppu *ppu, uint8_t mask,
        const uint16_t bg1[kPpuYPixels], const uint16_t bg2[kPpuYPixels],
        int obj_offset_x) {
    if (ppu == NULL || (mask & ~kPpuAuthenticCameraLayer_All) != 0u ||
        ((mask & kPpuAuthenticCameraLayer_Bg1) != 0u && bg1 == NULL) ||
        ((mask & kPpuAuthenticCameraLayer_Bg2) != 0u && bg2 == NULL) ||
        obj_offset_x < INT16_MIN || obj_offset_x > INT16_MAX) return false;
    if ((mask & kPpuAuthenticCameraLayer_Bg1) != 0u)
        memcpy(ppu->authenticHScroll[0], bg1, sizeof(ppu->authenticHScroll[0]));
    if ((mask & kPpuAuthenticCameraLayer_Bg2) != 0u)
        memcpy(ppu->authenticHScroll[1], bg2, sizeof(ppu->authenticHScroll[1]));
    ppu->authenticHScrollMask = mask;
    ppu->authenticObjOffsetX = (int16_t)obj_offset_x;
    return true;
}

bool PpuAuthenticCameraFrameReady(const Ppu *ppu, uint8_t mask) {
    return ppu != NULL && (mask & ~kPpuAuthenticCameraLayer_All) == 0u &&
           (ppu->authenticHScrollMask & mask) == mask;
}

void PpuClearAuthenticCameraFrame(Ppu *ppu) {
    if (ppu != NULL) { ppu->authenticHScrollMask = 0u; ppu->authenticObjOffsetX = 0; }
}

void PpuClearOverlayBindings(Ppu *ppu) {
    if (ppu == NULL) return;
    memset(ppu->overlayRenderBuffer, 0, sizeof(ppu->overlayRenderBuffer));
    memset(ppu->overlayRenderPitch, 0, sizeof(ppu->overlayRenderPitch));
    memset(ppu->overlayRenderHeight, 0, sizeof(ppu->overlayRenderHeight));
    memset(ppu->overlayRenderBands, 0, sizeof(ppu->overlayRenderBands));
    memset(ppu->overlayRenderContentMask, 0,
           sizeof(ppu->overlayRenderContentMask));
    PpuClearOverlayCaptures(ppu);
    note_surface_binding(ppu);
}

bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch) {
    return PpuBindOverlaySurfaceSized(ppu, source, pixels, pitch, 0u);
}

bool PpuBindOverlaySurfaceSized(Ppu *ppu, PpuOverlaySource source,
                                uint8_t *pixels, size_t pitch,
                                size_t height) {
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        (pixels != NULL && (pitch == 0u || pitch % 4u != 0u ||
         pitch / 4u < kPpuXPixels || pitch / 4u > kPpuSurfaceWidth ||
         height > kPpuBufHeight)) ||
        (pixels == NULL && height != 0u)) return false;
    ppu->overlayRenderBuffer[source] = pixels;
    ppu->overlayRenderPitch[source] = pixels != NULL ? (uint32_t)pitch : 0u;
    ppu->overlayRenderHeight[source] = pixels != NULL ? (uint32_t)height : 0u;
    memset(ppu->overlayRenderBands[source], 0,
           sizeof(ppu->overlayRenderBands[source]));
    ppu->overlayRenderContentMask[source] = 0u;
    ppu->overlayRenderMaybeDirty[source] = pixels != NULL;
    if (pixels == NULL) memset(&ppu->overlayCaptures[source], 0,
                               sizeof(ppu->overlayCaptures[source]));
    note_surface_binding(ppu);
    return true;
}

bool PpuBindOverlayPrioSurface(Ppu *ppu, PpuOverlaySource source, int band,
                               uint8_t *pixels) {
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        band < 1 || band > 3 || (source != kPpuOverlaySource_Obj && band > 2) ||
        (pixels != NULL && ppu->overlayRenderBuffer[source] == NULL)) return false;
    ppu->overlayRenderBands[source][band - 1] = pixels;
    ppu->overlayRenderContentMask[source] &= (uint8_t)~(1u << band);
    if (pixels != NULL) ppu->overlayRenderMaybeDirty[source] = 1u;
    note_surface_binding(ppu);
    return true;
}

bool PpuOverlaySurfaceHasContent(const Ppu *ppu, PpuOverlaySource source,
                                 int band) {
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        band < 0 || band > 3) return false;
    if (band == 0) return ppu->overlayRenderBuffer[source] != NULL &&
                          (ppu->overlayRenderContentMask[source] & 1u) != 0u;
    return ppu->overlayRenderBands[source][band - 1] != NULL &&
           (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u;
}

void PpuClearOverlayCaptures(Ppu *ppu) {
    if (ppu == NULL) return;
    memset(ppu->overlayCaptures, 0, sizeof(ppu->overlayCaptures));
    ppu->overlayObjRelocatedFirst = ppu->overlayObjRelocatedCount = 0u;
    memset(&ppu->objRangeCapture, 0, sizeof(ppu->objRangeCapture));
    memset(&ppu->m7Override, 0, sizeof(ppu->m7Override));
}

bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source, int x, int y,
                          int width, int height, uint8_t flags) {
    int x0, x1, y0, y1;
    uint8_t valid_flags = kPpuOverlayFlag_RemoveFromGame |
        kPpuOverlayFlag_MarkObjColorMath | kPpuOverlayFlag_MarkBgHalfAdd |
        kPpuOverlayFlag_ApplyBgFixedColorSubtract |
        kPpuOverlayFlag_MarkFullAddSubscreen |
        kPpuOverlayFlag_MarkMainScreenWinner |
        kPpuOverlayFlag_MarkOwningScreenWinner;
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        width <= 0 || height <= 0) return false;
    x0 = clamp_int(x, -kPpuExtraLeftRight, kPpuXPixels + kPpuExtraLeftRight);
    x1 = clamp_int(x + width, -kPpuExtraLeftRight,
                   kPpuXPixels + kPpuExtraLeftRight);
    y0 = clamp_int(y, -kPpuExtraTopBottom, kPpuYPixels + kPpuExtraTopBottom);
    y1 = clamp_int(y + height, -kPpuExtraTopBottom,
                   kPpuYPixels + kPpuExtraTopBottom);
    if (x1 <= x0 || y1 <= y0) return false;
    ppu->overlayCaptures[source].x0 = (int16_t)x0;
    ppu->overlayCaptures[source].x1 = (int16_t)x1;
    ppu->overlayCaptures[source].y0 = (int16_t)y0;
    ppu->overlayCaptures[source].y1 = (int16_t)y1;
    ppu->overlayCaptures[source].flags = flags & valid_flags;
    ppu->overlayCaptures[source].oamFirst = 0u;
    ppu->overlayCaptures[source].oamCount = 0u;
    return true;
}

bool PpuSetOverlayTransparentFill(Ppu *ppu, PpuOverlaySource source,
                                  PpuOverlayTransparentFill mode,
                                  uint8_t cgram_index) {
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        mode < kPpuOverlayTransparentFill_None ||
        mode > kPpuOverlayTransparentFill_Cgram) return false;
    ppu->overlayCaptures[source].transparentFillMode = (uint8_t)mode;
    ppu->overlayCaptures[source].transparentFillCgram = cgram_index;
    ppu->overlayCaptures[source].transparentFillConfigured = 1u;
    return true;
}

uint32_t PpuOverlayTransparentFillColor(const Ppu *ppu,
                                        PpuOverlaySource source) {
    const PpuOverlayCapture *capture;
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count) return 0u;
    capture = &ppu->overlayCaptures[source];
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Black)
        return 0xff000000u;
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Cgram)
        return color_argb(ppu, ppu->cgram[capture->transparentFillCgram]);
    return 0u;
}

bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count) {
    PpuOverlayCapture *capture;
    if (ppu == NULL) return false;
    capture = &ppu->overlayCaptures[kPpuOverlaySource_Obj];
    if (first >= 128u || count == 0u || count > 128u - first ||
        capture->x1 <= capture->x0 || capture->y1 <= capture->y0) return false;
    capture->oamFirst = first;
    capture->oamCount = count;
    return true;
}

bool PpuSetOverlayRelocatedOamRange(Ppu *ppu, uint8_t first, uint8_t count) {
    const PpuOverlayCapture *capture;
    if (ppu == NULL) return false;
    capture = &ppu->overlayCaptures[kPpuOverlaySource_Obj];
    if (count == 0u || first >= 128u || count > 128u - first ||
        (capture->flags & kPpuOverlayFlag_MarkFullAddSubscreen) == 0u ||
        capture->oamCount == 0u || first < capture->oamFirst ||
        first + count > capture->oamFirst + capture->oamCount) return false;
    ppu->overlayObjRelocatedFirst = first;
    ppu->overlayObjRelocatedCount = count;
    return true;
}

bool PpuSetObjRangeCapture(Ppu *ppu, uint8_t first, uint8_t count,
        int x, int y, int width, int height, uint8_t *pixels, size_t pitch) {
    if (ppu == NULL || pixels == NULL || first >= 128u || count == 0u ||
        count > 128u - first || width <= 0 || height <= 0 || y < 0 ||
        y + height > kPpuYPixels || pitch % 4u != 0u ||
        pitch / 4u < kPpuXPixels || pitch / 4u > kPpuSurfaceWidth ||
        x < INT16_MIN || x + width > INT16_MAX) return false;
    ppu->objRangeCapture.x0 = (int16_t)x;
    ppu->objRangeCapture.x1 = (int16_t)(x + width);
    ppu->objRangeCapture.y0 = (int16_t)y;
    ppu->objRangeCapture.y1 = (int16_t)(y + height);
    ppu->objRangeCapture.first = first;
    ppu->objRangeCapture.count = count;
    ppu->objRangeCapture.pixels = pixels;
    ppu->objRangeCapture.pitch = (uint32_t)pitch;
    return true;
}

bool PpuBindMode7OverlaySurface(Ppu *ppu, uint8_t *pixels, size_t pitch,
                                uint8_t scale) {
    if (ppu == NULL || (pixels != NULL && (scale < 1u || scale > 4u ||
        pitch % 4u != 0u || pitch / 4u < (size_t)kPpuXPixels * scale ||
        pitch / 4u > (size_t)kPpuSurfaceWidth * scale))) return false;
    ppu->m7OverlayBuffer = pixels;
    ppu->m7OverlayPitch = pixels != NULL ? (uint32_t)pitch : 0u;
    ppu->m7OverlayScale = pixels != NULL ? scale : 0u;
    ppu->m7OverlayMaybeDirty = pixels != NULL;
    if (pixels == NULL) memset(&ppu->m7Override, 0, sizeof(ppu->m7Override));
    note_surface_binding(ppu);
    return true;
}

bool PpuSetMode7Override(Ppu *ppu, const uint32_t *rgba, int width, int height,
        int x0, int y0, int x1, int y1, uint8_t wrap) {
    if (ppu == NULL || ppu->m7OverlayBuffer == NULL || rgba == NULL ||
        width <= 0 || height <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 ||
        x1 > kPpuMode7CanvasExtent || y1 > kPpuMode7CanvasExtent) return false;
    ppu->m7Override.rgba = rgba;
    ppu->m7Override.width = width; ppu->m7Override.height = height;
    ppu->m7Override.canvasX0 = x0; ppu->m7Override.canvasY0 = y0;
    ppu->m7Override.canvasX1 = x1; ppu->m7Override.canvasY1 = y1;
    ppu->m7Override.wrap = wrap;
    return true;
}

void PpuClearVirtualTilemaps(Ppu *ppu) {
    if (ppu != NULL) memset(ppu->virtualTilemap, 0, sizeof(ppu->virtualTilemap));
}

bool PpuSetVirtualTilemap(Ppu *ppu, uint8_t layer,
                          const PpuVirtualTilemapBinding *binding) {
    if (ppu == NULL || layer >= 2u) return false;
    if (binding == NULL) { memset(&ppu->virtualTilemap[layer], 0,
                                  sizeof(ppu->virtualTilemap[layer])); return true; }
    if (binding->lookup == NULL || binding->hscroll_anchor > 0x3ffu ||
        binding->vscroll_anchor > 0x3ffu ||
        (binding->flags & ~kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u)
        return false;
    ppu->virtualTilemap[layer] = *binding;
    return true;
}

void PpuSetExtraSpace(Ppu *ppu, uint8_t extra) {
    if (ppu == NULL) return;
    if (extra > kPpuExtraLeftRight) extra = kPpuExtraLeftRight;
    ppu->extraLeftRight = ppu->extraLeftCur = ppu->extraRightCur = extra;
    reset_layer_policy(ppu);
}

void PpuSetExtraSpaceCentered(Ppu *ppu, uint8_t budget) {
    if (ppu == NULL) return;
    if (budget > kPpuExtraLeftRight) budget = kPpuExtraLeftRight;
    ppu->extraLeftRight = budget;
    ppu->extraLeftCur = ppu->extraRightCur = 0u;
    reset_layer_policy(ppu);
}

void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom) {
    (void)bottom;
    if (ppu == NULL) return;
    ppu->extraLeftCur = (uint8_t)clamp_int(left, 0, ppu->extraLeftRight);
    ppu->extraRightCur = (uint8_t)clamp_int(right, 0, ppu->extraLeftRight);
}

void PpuSetExtraVerticalSpace(Ppu *ppu, int top, int bottom) {
    if (ppu == NULL) return;
    ppu->extraTopCur = (uint8_t)clamp_int(top, 0, kPpuExtraTopBottom);
    ppu->extraBottomCur = (uint8_t)clamp_int(bottom, 0, kPpuExtraTopBottom);
    ppu->verticalMarginLayerClip = 0u;
    memset(ppu->verticalMarginTopRows, 0, sizeof(ppu->verticalMarginTopRows));
    memset(ppu->verticalMarginBottomRows, 0,
           sizeof(ppu->verticalMarginBottomRows));
}

void PpuSetVerticalMarginLayerClip(Ppu *ppu, uint8_t layer,
                                   int top_rows, int bottom_rows) {
    if (ppu == NULL || layer >= 4u) return;
    ppu->verticalMarginTopRows[layer] =
        (uint8_t)clamp_int(top_rows, 0, kPpuExtraTopBottom);
    ppu->verticalMarginBottomRows[layer] =
        (uint8_t)clamp_int(bottom_rows, 0, kPpuExtraTopBottom);
    ppu->verticalMarginLayerClip |= (uint8_t)(1u << layer);
}

void PpuClearObjExactPositions(Ppu *ppu) {
    if (ppu != NULL) {
        memset(ppu->objPosValid, 0, sizeof(ppu->objPosValid));
        ppu->objScanlineMasksValid = false;
    }
}

void PpuSetObjExactPosition(Ppu *ppu, uint8_t slot, int x, int y) {
    if (ppu == NULL || slot >= 128u) return;
    ppu->objPosX[slot] = (int16_t)clamp_int(x, INT16_MIN, INT16_MAX);
    ppu->objPosY[slot] = (int16_t)clamp_int(y, INT16_MIN, INT16_MAX);
    ppu->objPosValid[slot] = 1u;
    ppu->objScanlineMasksValid = false;
}

void PpuClearObjCameraRelative(Ppu *ppu) {
    if (ppu != NULL) memset(ppu->objCameraRelative, 0,
                            sizeof(ppu->objCameraRelative));
}

void PpuSetObjCameraRelative(Ppu *ppu, uint8_t slot, bool relative) {
    if (ppu != NULL && slot < 128u) ppu->objCameraRelative[slot] = relative;
}

void PpuSetWidescreenHudSplit(Ppu *ppu, uint8_t height, uint8_t left_end,
        uint8_t right_start, uint8_t player_y, uint8_t left_only_y) {
    if (ppu == NULL) return;
    if (left_end == 0u || left_end > right_start) height = 0u;
    if (height == 0u) player_y = left_only_y = 0u;
    else {
        if (player_y > height) player_y = height;
        if (left_only_y > height) left_only_y = height;
        if (player_y > left_only_y) player_y = left_only_y;
    }
    ppu->wsHudSplitHeight = height; ppu->wsHudLeftEnd = left_end;
    ppu->wsHudRightStart = right_start; ppu->wsHudPlayerRowY = player_y;
    ppu->wsHudLeftOnlyY = left_only_y;
}

void PpuSetWidescreenBg3Widen(Ppu *ppu, uint8_t from_y) {
    if (ppu != NULL) ppu->wsBg3WidenY = from_y;
}
void PpuSetWidescreenLayerClamp(Ppu *ppu, uint8_t mask) {
    if (ppu != NULL) ppu->wsLayerClamp = mask;
}
void PpuSetWidescreenLayerMirror(Ppu *ppu, uint8_t mask) {
    if (ppu != NULL) ppu->wsLayerMirror = mask;
}
void PpuSetWidescreenLayerRepeat(Ppu *ppu, uint8_t mask) {
    if (ppu != NULL) ppu->wsLayerRepeat = mask;
}
void PpuSetWidescreenLayerNormalScroll(Ppu *ppu, uint8_t mask) {
    if (ppu != NULL) ppu->wsLayerNormalScroll = mask;
}
void PpuClearWidescreenLayerBands(Ppu *ppu) {
    if (ppu == NULL) return;
    memset(ppu->wsBandFill, 0, sizeof(ppu->wsBandFill));
    memset(ppu->wsBandMotion, 0, sizeof(ppu->wsBandMotion));
}
void PpuSetWidescreenPadCapturedToBudget(Ppu *ppu, uint8_t enabled) {
    if (ppu != NULL) ppu->wsPadCapturedToBudget = enabled != 0u;
}

void PpuSetWidescreenLayerBand(Ppu *ppu, uint8_t layer, uint8_t y0,
        uint8_t y1, PpuWidescreenBandFill fill, PpuWidescreenMotion motion) {
    if (ppu == NULL || layer >= 4u || y0 >= y1 || y1 > kPpuYPixels ||
        fill < kPpuWidescreenBandFill_Transparent ||
        fill > kPpuWidescreenBandFill_RawWrap ||
        motion > kPpuWidescreenMotion_NormalScroll) return;
    for (unsigned y = y0; y < y1; ++y) {
        ppu->wsBandFill[layer][y] = (uint8_t)fill;
        ppu->wsBandMotion[layer][y] = (uint8_t)motion;
    }
}

void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1) {
    PpuSetWidescreenLayerBand(ppu, layer, y0, y1,
        kPpuWidescreenBandFill_Repeat, kPpuWidescreenMotion_FillRelative);
}

void PpuSetWidescreenLayerExtent(Ppu *ppu, uint8_t layer, uint16_t left,
        uint16_t right, uint16_t top, uint16_t bottom) {
    if (ppu == NULL || layer >= 4u) return;
    ppu->wsLayerExtentLeftDefault[layer] = left;
    ppu->wsLayerExtentRightDefault[layer] = right;
    ppu->wsLayerExtentTop[layer] = top;
    ppu->wsLayerExtentBottom[layer] = bottom;
    for (int y = 0; y < kPpuYPixels; ++y) {
        ppu->wsLayerExtentLeft[layer][y] = left;
        ppu->wsLayerExtentRight[layer][y] = right;
    }
}

void PpuSetWidescreenLayerExtentBand(Ppu *ppu, uint8_t layer, uint8_t y0,
        uint8_t y1, uint16_t left, uint16_t right) {
    if (ppu == NULL || layer >= 4u || y0 >= y1 || y1 > kPpuYPixels) return;
    for (unsigned y = y0; y < y1; ++y) {
        ppu->wsLayerExtentLeft[layer][y] = left;
        ppu->wsLayerExtentRight[layer][y] = right;
    }
}

static int obj_x(const Ppu *ppu, unsigned slot) {
    unsigned word = slot * 2u;
    int x;
    if (ppu->objPosValid[slot]) return ppu->objPosX[slot];
    x = ppu->oam[word] & 0xff;
    x |= ((ppu->highOam[word >> 3] >> (word & 7u)) & 1u) << 8;
    if (x >= kPpuXPixels + ppu->extraRightCur) x -= kPpuObjXWrap;
    return x;
}

static int obj_y(const Ppu *ppu, unsigned slot) {
    int y;
    if (ppu->objPosValid[slot]) return ppu->objPosY[slot];
    y = ppu->oam[slot * 2u] >> 8;
    return y >= kPpuObjYNegativeFrom ? y - kPpuObjYWrap : y;
}

static int obj_size(const Ppu *ppu, unsigned slot) {
    unsigned word = slot * 2u;
    unsigned large = (ppu->highOam[word >> 3] >> ((word & 7u) + 1u)) & 1u;
    return kPpuSpriteSizes[(ppu->obsel >> 5) & 7u][large];
}

static void rebuild_obj_scanline_masks(Ppu *ppu) {
    memset(ppu->objScanlineMasks, 0, sizeof(ppu->objScanlineMasks));
    for (unsigned slot = 0; slot < 128u; ++slot) {
        int first = obj_y(ppu, slot);
        int end = first + obj_size(ppu, slot);
        PpuBitWord bit = (PpuBitWord)1u << (slot % kPpuBitWordBits);
        unsigned word = slot / kPpuBitWordBits;
        first = clamp_int(first, -kPpuExtraTopBottom,
                          kPpuYPixels + kPpuExtraTopBottom);
        end = clamp_int(end, -kPpuExtraTopBottom,
                        kPpuYPixels + kPpuExtraTopBottom);
        for (int y = first; y < end; ++y)
            ppu->objScanlineMasks[y + kPpuExtraTopBottom][word] |= bit;
    }
    ppu->objScanlineMasksValid = true;
}

static const PpuBitWord *obj_scanline_masks(Ppu *ppu, int screen_y) {
    int row = screen_y + kPpuExtraTopBottom;
    if (row < 0 || row >= kPpuBufHeight) return NULL;
    if (!ppu->objScanlineMasksValid) rebuild_obj_scanline_masks(ppu);
    return ppu->objScanlineMasks[row];
}

bool PpuResolveObjSlot(Ppu *ppu, uint8_t slot, PpuObjPart *part) {
    if (ppu == NULL || part == NULL || slot >= 128u) return false;
    part->x = (int16_t)obj_x(ppu, slot);
    part->y = (int16_t)obj_y(ppu, slot);
    part->tile_attr = ppu->oam[slot * 2u + 1u];
    part->size = (uint8_t)obj_size(ppu, slot);
    part->reserved = 0u;
    return true;
}

bool PpuResolveObjSlots(Ppu *ppu, uint8_t first, uint8_t count,
        uint8_t priority, PpuObjPart *parts, int capacity, int *out_count) {
    unsigned start;
    int count_out = 0;
    if (ppu == NULL || parts == NULL || out_count == NULL || first >= 128u ||
        count == 0u || count > 128u - first || priority > 3u) return false;
    start = (ppu->oamaddh & 0x80u) != 0u ? (ppu->oamaddl >> 1) : 0u;
    for (unsigned step = 0; step < 128u; ++step) {
        unsigned slot = (start + step) & 127u;
        uint16_t attr;
        if (slot < first || slot >= first + count) continue;
        attr = ppu->oam[slot * 2u + 1u];
        if (((attr >> 12) & 3u) != priority || count_out >= capacity) return false;
        if (!PpuResolveObjSlot(ppu, (uint8_t)slot, &parts[count_out])) return false;
        ++count_out;
    }
    *out_count = count_out;
    return true;
}

bool PpuGetPartBounds(const PpuObjPart *parts, int count,
                      PpuObjRangeBounds *bounds) {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    if (parts == NULL || bounds == NULL || count <= 0) return false;
    for (int index = 0; index < count; ++index) {
        if (parts[index].x < x0) x0 = parts[index].x;
        if (parts[index].y < y0) y0 = parts[index].y;
        if (parts[index].x + parts[index].size > x1)
            x1 = parts[index].x + parts[index].size;
        if (parts[index].y + parts[index].size > y1)
            y1 = parts[index].y + parts[index].size;
    }
    if (x1 <= x0 || y1 <= y0) return false;
    bounds->x0 = (int16_t)x0; bounds->y0 = (int16_t)y0;
    bounds->x1 = (int16_t)x1; bounds->y1 = (int16_t)y1;
    return true;
}

static uint32_t decoded_4bpp_row(Ppu *ppu, int word_address) {
    unsigned address = (unsigned)word_address & 0x7fffu;
    uint16_t plane01 = ppu->vram[address];
    uint16_t plane23 = ppu->vram[(address + 8u) & 0x7fffu];
    uint32_t source = (uint32_t)plane01 | ((uint32_t)plane23 << 16);
    PpuDecoded4bppRow *cache = &ppu->decoded4bppRows[address];
    if (cache->source != source) {
        uint32_t pixels = 0u;
        for (int x = 0; x < 8; ++x) {
            int bit = 7 - x;
            unsigned pixel = (plane01 >> bit) & 1u;
            pixel |= ((plane01 >> (bit + 8)) & 1u) << 1;
            pixel |= ((plane23 >> bit) & 1u) << 2;
            pixel |= ((plane23 >> (bit + 8)) & 1u) << 3;
            pixels |= pixel << (x * 4);
        }
        cache->source = source;
        cache->pixels = pixels;
    }
    return cache->pixels;
}

static uint16_t decoded_2bpp_row(Ppu *ppu, int word_address) {
    unsigned address = (unsigned)word_address & 0x7fffu;
    uint16_t source = ppu->vram[address];
    uint32_t cached = ppu->decoded2bppRows[address];
    if ((uint16_t)cached != source) {
        uint16_t pixels = 0u;
        for (int x = 0; x < 8; ++x) {
            int bit = 7 - x;
            unsigned pixel = (source >> bit) & 1u;
            pixel |= ((source >> (bit + 8)) & 1u) << 1;
            pixels |= (uint16_t)(pixel << (x * 2));
        }
        cached = (uint32_t)source | ((uint32_t)pixels << 16);
        ppu->decoded2bppRows[address] = cached;
    }
    return (uint16_t)(cached >> 16);
}

static int tile_pixel(Ppu *ppu, int tile_address, int tile,
                      int row, int x, int bpp) {
    int base = tile_address + tile * (bpp == 2 ? 8 : bpp == 4 ? 16 : 32);
    int shift = 7 - (x & 7);
    int pixel = 0;
    if (bpp == 4)
        return (int)((decoded_4bpp_row(ppu, base + row) >>
                      ((x & 7) * 4)) & 15u);
    if (bpp == 2)
        return (decoded_2bpp_row(ppu, base + row) >>
                ((x & 7) * 2)) & 3u;
    uint16_t word = ppu->vram[(base + row) & 0x7fff];
    pixel |= (word >> shift) & 1;
    pixel |= ((word >> (shift + 8)) & 1) << 1;
    if (bpp >= 4) {
        word = ppu->vram[(base + row + 8) & 0x7fff];
        pixel |= ((word >> shift) & 1) << 2;
        pixel |= ((word >> (shift + 8)) & 1) << 3;
    }
    if (bpp >= 8) {
        word = ppu->vram[(base + row + 16) & 0x7fff];
        pixel |= ((word >> shift) & 1) << 4;
        pixel |= ((word >> (shift + 8)) & 1) << 5;
        word = ppu->vram[(base + row + 24) & 0x7fff];
        pixel |= ((word >> shift) & 1) << 6;
        pixel |= ((word >> (shift + 8)) & 1) << 7;
    }
    return pixel;
}

static int obj_pixel(Ppu *ppu, const PpuObjPart *part,
                     int screen_x, int screen_y) {
    int x = screen_x - part->x;
    int y = screen_y - part->y;
    int tile, tile_x, tile_y, address;
    if (x < 0 || y < 0 || x >= part->size || y >= part->size) return 0;
    if ((part->tile_attr & 0x4000u) != 0u) x = part->size - 1 - x;
    if ((part->tile_attr & 0x8000u) != 0u) y = part->size - 1 - y;
    tile_x = x >> 3; tile_y = y >> 3;
    tile = part->tile_attr & 0xff;
    tile = (((tile >> 4) + tile_y) << 4) | (((tile & 15) + tile_x) & 15);
    address = (part->tile_attr & 0x100u) != 0u ?
        ((ppu->obsel & 7u) << 13) + (((ppu->obsel & 0x18u) + 8u) << 9) :
        ((ppu->obsel & 7u) << 13);
    return tile_pixel(ppu, address, tile, y & 7, x & 7, 4);
}

bool PpuRasterizeParts(Ppu *ppu, const PpuObjPart *parts, int count,
        const PpuObjRangeBounds *bounds, uint32_t *pixels,
        int width, int height, size_t pitch) {
    if (ppu == NULL || parts == NULL || bounds == NULL || pixels == NULL ||
        count <= 0 || width <= 0 || height <= 0 || pitch < (size_t)width * 4u)
        return false;
    update_brightness(ppu);
    for (int y = 0; y < height; ++y)
        memset((uint8_t *)pixels + (size_t)y * pitch, 0, (size_t)width * 4u);
    for (int index = 0; index < count; ++index) {
        const PpuObjPart *part = &parts[index];
        int palette = 0x80 + ((part->tile_attr >> 9) & 7u) * 16;
        int y0 = part->y > bounds->y0 ? part->y : bounds->y0;
        int raster_y1 = bounds->y0 + height;
        int y1 = part->y + part->size < raster_y1
            ? part->y + part->size : raster_y1;
        int tile_columns = part->size >> 3;
        int tile_address = (part->tile_attr & 0x100u) != 0u ?
            ((ppu->obsel & 7u) << 13) + (((ppu->obsel & 0x18u) + 8u) << 9) :
            ((ppu->obsel & 7u) << 13);
        int base_tile = part->tile_attr & 0xff;
        bool hflip = (part->tile_attr & 0x4000u) != 0u;
        for (int screen_y = y0; screen_y < y1; ++screen_y) {
            int local_y = screen_y - part->y;
            int output_y = screen_y - bounds->y0;
            uint32_t *row = (uint32_t *)((uint8_t *)pixels +
                (size_t)output_y * pitch);
            if ((part->tile_attr & 0x8000u) != 0u)
                local_y = part->size - 1 - local_y;
            for (int output_tile = 0; output_tile < tile_columns;
                 ++output_tile) {
                int source_tile = hflip ? tile_columns - 1 - output_tile
                                        : output_tile;
                int tile = (((base_tile >> 4) + (local_y >> 3)) << 4) |
                           (((base_tile & 15) + source_tile) & 15);
                uint32_t decoded = decoded_4bpp_row(
                    ppu, tile_address + tile * 16 + (local_y & 7));
                int output_x = part->x + output_tile * 8;
                for (int tile_x = 0; tile_x < 8; ++tile_x) {
                    int screen_x = output_x + tile_x;
                    int destination_x = screen_x - bounds->x0;
                    int source_x = hflip ? 7 - tile_x : tile_x;
                    int pixel;
                    if (destination_x < 0 || destination_x >= width ||
                        row[destination_x] != 0u) continue;
                    pixel = (decoded >> (source_x * 4)) & 15u;
                    if (pixel != 0)
                        row[destination_x] = color_argb(
                            ppu, ppu->cgram[palette + pixel]);
                }
            }
        }
    }
    return true;
}

bool PpuGetObjRangeBounds(Ppu *ppu, uint8_t first, uint8_t count,
                          uint8_t priority, PpuObjRangeBounds *bounds) {
    PpuObjPart parts[128]; int n = 0;
    return PpuResolveObjSlots(ppu, first, count, priority, parts, 128, &n) &&
           PpuGetPartBounds(parts, n, bounds);
}

bool PpuRasterizeObjRange(Ppu *ppu, uint8_t first, uint8_t count,
        uint8_t priority, const PpuObjRangeBounds *bounds, uint32_t *pixels,
        int width, int height, size_t pitch) {
    PpuObjPart parts[128]; PpuObjRangeBounds actual; int n = 0;
    if (bounds == NULL || !PpuResolveObjSlots(ppu, first, count, priority,
        parts, 128, &n) || !PpuGetPartBounds(parts, n, &actual) ||
        memcmp(&actual, bounds, sizeof(actual)) != 0 ||
        width != actual.x1 - actual.x0 || height != actual.y1 - actual.y0)
        return false;
    return PpuRasterizeParts(ppu, parts, n, &actual, pixels, width, height, pitch);
}

static int bpp_for_mode(int mode, int layer) {
    static const uint8_t depths[8][4] = {
        {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
        {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}
    };
    return layer >= 0 && layer < 4 ? depths[mode & 7][layer] : 0;
}

static uint8_t layer_rank(const Ppu *ppu, int layer, int priority) {
    static const uint8_t ranks[10][5][4] = {
        {{9,12,0,0}, {8,11,0,0}, {3,6,0,0}, {2,5,0,0}, {4,7,10,13}},
        {{7,10,0,0}, {6,9,0,0}, {2,4,0,0}, {0,0,0,0}, {3,5,8,11}},
        {{4,8,0,0}, {2,6,0,0}, {0,0,0,0}, {0,0,0,0}, {3,5,7,9}},
        {{4,8,0,0}, {2,6,0,0}, {0,0,0,0}, {0,0,0,0}, {3,5,7,9}},
        {{4,8,0,0}, {2,6,0,0}, {0,0,0,0}, {0,0,0,0}, {3,5,7,9}},
        {{4,8,0,0}, {2,6,0,0}, {0,0,0,0}, {0,0,0,0}, {3,5,7,9}},
        {{3,6,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {2,4,5,7}},
        {{3,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {2,4,5,6}},
        {{6,9,0,0}, {5,8,0,0}, {2,11,0,0}, {0,0,0,0}, {3,4,7,10}},
        {{4,0,0,0}, {2,6,0,0}, {0,0,0,0}, {0,0,0,0}, {3,5,7,8}}
    };
    int mode = PPU_mode(ppu);
    if (mode == 1 && PPU_bg3priority(ppu)) mode = 8;
    if (mode == 7 && PPU_m7extBg(ppu)) mode = 9;
    if ((unsigned)layer >= 5u || (unsigned)priority >= 4u) return 0u;
    return ranks[mode][layer][priority];
}

static bool window_inside(const Ppu *ppu, int layer, int screen_x) {
    unsigned flags = (ppu->windowsel >> (layer * 4)) & 15u;
    bool first = false, second = false;
    int x = clamp_int(screen_x, 0, 255);
    if ((flags & kWindow1Enabled) != 0u) {
        first = x >= ppu->window1left && x <= ppu->window1right;
        if ((flags & kWindow1Inversed) != 0u) first = !first;
    }
    if ((flags & kWindow2Enabled) != 0u) {
        second = x >= ppu->window2left && x <= ppu->window2right;
        if ((flags & kWindow2Inversed) != 0u) second = !second;
    }
    if ((flags & kWindow1Enabled) == 0u) return second;
    if ((flags & kWindow2Enabled) == 0u) return first;
    switch ((ppu->wbgobjlog >> (layer * 2)) & 3u) {
        case 0: return first || second;
        case 1: return first && second;
        case 2: return first != second;
        default: return first == second;
    }
}

/* Native-window output can only change at the two inclusive endpoints of each
 * hardware interval.  Probe those transitions once per line so uniform
 * windows are hoisted out of all tiled pixel loops without allocating a mask. */
static int native_window_uniform(const Ppu *ppu, int layer) {
    unsigned flags = (ppu->windowsel >> (layer * 4)) & 15u;
    int expected = window_inside(ppu, layer, 0) ? 1 : 0;
    int transitions[4];
    int count = 0;
    if ((flags & kWindow1Enabled) != 0u) {
        if (ppu->window1left > 0u) transitions[count++] = ppu->window1left;
        if (ppu->window1right < 255u)
            transitions[count++] = ppu->window1right + 1;
    }
    if ((flags & kWindow2Enabled) != 0u) {
        if (ppu->window2left > 0u) transitions[count++] = ppu->window2left;
        if (ppu->window2right < 255u)
            transitions[count++] = ppu->window2right + 1;
    }
    for (int index = 0; index < count; ++index) {
        if ((window_inside(ppu, layer, transitions[index]) ? 1 : 0) != expected)
            return -1;
    }
    return expected;
}

typedef struct NativeWindowRuns {
    uint16_t edges[6];
    uint8_t inside[5];
    uint8_t count;
} NativeWindowRuns;

static void native_window_runs(const Ppu *ppu, int layer,
                               NativeWindowRuns *runs) {
    unsigned flags = (ppu->windowsel >> (layer * 4)) & 15u;
    int edge_count = 1;
    runs->edges[0] = 0u;
    if ((flags & kWindow1Enabled) != 0u) {
        if (ppu->window1left > 0u)
            runs->edges[edge_count++] = ppu->window1left;
        if (ppu->window1right < 255u)
            runs->edges[edge_count++] = (uint16_t)(ppu->window1right + 1u);
    }
    if ((flags & kWindow2Enabled) != 0u) {
        if (ppu->window2left > 0u)
            runs->edges[edge_count++] = ppu->window2left;
        if (ppu->window2right < 255u)
            runs->edges[edge_count++] = (uint16_t)(ppu->window2right + 1u);
    }
    runs->edges[edge_count++] = kPpuXPixels;
    for (int index = 1; index < edge_count; ++index) {
        uint16_t edge = runs->edges[index];
        int sorted = index;
        while (sorted > 0 && runs->edges[sorted - 1] > edge) {
            runs->edges[sorted] = runs->edges[sorted - 1];
            --sorted;
        }
        runs->edges[sorted] = edge;
    }
    {
        int unique = 1;
        for (int index = 1; index < edge_count; ++index) {
            if (runs->edges[index] != runs->edges[unique - 1])
                runs->edges[unique++] = runs->edges[index];
        }
        runs->count = (uint8_t)(unique - 1);
    }
    for (int index = 0; index < runs->count; ++index)
        runs->inside[index] = window_inside(ppu, layer, runs->edges[index]);
}

static bool layer_position(Ppu *ppu, int layer, int screen_x, int screen_y,
                           bool overlay_padding, int *source_x) {
    /* Native pixels never need widescreen extent or fill-policy resolution. */
    if ((unsigned)screen_x < kPpuXPixels &&
        (unsigned)screen_y < kPpuYPixels) {
        *source_x = screen_x;
        return true;
    }
    int row = clamp_int(screen_y, 0, kPpuYPixels - 1);
    PpuWidescreenLayerPolicy policy;
    uint16_t cap;
    if (screen_y < 0) {
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            -screen_y > ppu->verticalMarginTopRows[layer]) return false;
        if (ppu->wsLayerExtentTop[layer] != kPpuWidescreenExtentAvailable &&
            -screen_y > ppu->wsLayerExtentTop[layer]) return false;
    } else if (screen_y >= kPpuYPixels) {
        int distance = screen_y - (kPpuYPixels - 1);
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            distance > ppu->verticalMarginBottomRows[layer]) return false;
        if (ppu->wsLayerExtentBottom[layer] != kPpuWidescreenExtentAvailable &&
            distance > ppu->wsLayerExtentBottom[layer]) return false;
    }
    if (screen_x < 0) {
        cap = ppu->wsLayerExtentLeft[layer][row];
        if (cap != kPpuWidescreenExtentAvailable && -screen_x > cap) return false;
    } else if (screen_x >= kPpuXPixels) {
        cap = ppu->wsLayerExtentRight[layer][row];
        if (cap != kPpuWidescreenExtentAvailable &&
            screen_x - (kPpuXPixels - 1) > cap) return false;
    }
    if (layer == 2 && screen_x != clamp_int(screen_x, 0, 255) &&
        (ppu->wsBg3WidenY == 0u || row < ppu->wsBg3WidenY)) return false;
    policy = PpuResolveWidescreenLayerPolicy(ppu, (uint8_t)layer, screen_y);
    if (overlay_padding && ppu->wsPadCapturedToBudget &&
        (policy.fill == kPpuWidescreenBandFill_Mirror ||
         policy.fill == kPpuWidescreenBandFill_Repeat)) {
        return PpuMapWidescreenLayerXWithPolicy(ppu, (uint8_t)layer,
                                                screen_x, source_x, &policy);
    }
    return PpuMapWidescreenLayerXWithPolicy(ppu, (uint8_t)layer,
                                            screen_x, source_x, &policy);
}

bool PpuResolveBackgroundCoordinate(
        Ppu *ppu, uint8_t layer, int screen_x, int screen_y,
        int *source_x, int *sample_y,
        PpuWidescreenLayerPolicy *out_policy, bool *out_mosaic) {
    int fetch_x = screen_x;
    int fetch_y = screen_y + 1;
    bool mosaic;
    if (ppu == NULL || layer >= 4u || source_x == NULL || sample_y == NULL)
        return false;
    mosaic = PPU_mosaicEnabled(ppu, layer) && PPU_mosaicSize(ppu) > 1;
    if (mosaic) {
        int size = PPU_mosaicSize(ppu);
        fetch_x -= ((fetch_x % size) + size) % size;
        fetch_y -= ((fetch_y % size) + size) % size;
    }
    if (out_policy != NULL)
        *out_policy = PpuResolveWidescreenLayerPolicy(
            ppu, layer, screen_y);
    if (out_mosaic != NULL) *out_mosaic = mosaic;
    *sample_y = fetch_y;
    return layer_position(
        ppu, layer, fetch_x, screen_y, false, source_x);
}

static int mode7_clipped_scroll(int value) {
    return (value & 0x2000) != 0 ? value | ~1023 : value & 1023;
}

static int32_t mode7_rounded_product(int left, int right) {
    return (int32_t)((uint32_t)(left * right) & ~UINT32_C(63));
}

/* Mode 7 applies the center/scroll correction to 8.8 fixed-point products,
 * rounding each term independently to the hardware's 6-bit precision before
 * adding the per-scanline and per-pixel contributions.  Keeping the result in
 * wrapping unsigned fixed-point form also preserves large-field detection for
 * negative coordinates. */
static void mode7_line_transform(const Ppu *ppu, int sample_y,
        uint32_t *start_x, uint32_t *start_y, int *step_x, int *step_y) {
    int a = ppu->m7matrix[0], b = ppu->m7matrix[1];
    int c = ppu->m7matrix[2], d = ppu->m7matrix[3];
    int cx = sign13((uint16_t)ppu->m7matrix[4]);
    int cy = sign13((uint16_t)ppu->m7matrix[5]);
    int h = sign13((uint16_t)ppu->m7matrix[6]);
    int v = sign13((uint16_t)ppu->m7matrix[7]);
    int clipped_h = mode7_clipped_scroll(h - cx);
    int clipped_v = mode7_clipped_scroll(v - cy);
    int row = PPU_m7yFlip(ppu) ? 255 - sample_y : sample_y;
    int first_x = PPU_m7xFlip(ppu) ? 255 : 0;
    int32_t origin_x = mode7_rounded_product(a, clipped_h) +
        mode7_rounded_product(b, row) +
        mode7_rounded_product(b, clipped_v) + cx * 256;
    int32_t origin_y = mode7_rounded_product(c, clipped_h) +
        mode7_rounded_product(d, row) +
        mode7_rounded_product(d, clipped_v) + cy * 256;
    *start_x = (uint32_t)(origin_x + a * first_x);
    *start_y = (uint32_t)(origin_y + c * first_x);
    *step_x = PPU_m7xFlip(ppu) ? -a : a;
    *step_y = PPU_m7xFlip(ppu) ? -c : c;
}

static void mode7_sample_coordinates(Ppu *ppu, int x, int sample_y,
        uint32_t *source_x, uint32_t *source_y) {
    PpuMode7SampleCache *cache = &ppu->mode7SampleCache;
    if (!cache->valid || cache->sample_y != sample_y) {
        mode7_line_transform(ppu, sample_y, &cache->source_x,
                             &cache->source_y, &cache->step_x,
                             &cache->step_y);
        cache->screen_x = 0;
        cache->sample_y = sample_y;
        cache->valid = true;
    }
    if (x == cache->screen_x + 1) {
        cache->source_x += (uint32_t)cache->step_x;
        cache->source_y += (uint32_t)cache->step_y;
    } else if (x != cache->screen_x) {
        uint32_t start_x, start_y;
        int step_x, step_y;
        mode7_line_transform(ppu, sample_y, &start_x, &start_y,
                             &step_x, &step_y);
        cache->source_x = start_x + (uint32_t)(step_x * x);
        cache->source_y = start_y + (uint32_t)(step_y * x);
        cache->step_x = step_x;
        cache->step_y = step_y;
    }
    cache->screen_x = x;
    *source_x = cache->source_x;
    *source_y = cache->source_y;
}

static bool sample_mode7(Ppu *ppu, int layer, int x, int y, SrPpuPixel *out) {
    uint32_t px, py;
    int tile, pixel;
    mode7_sample_coordinates(ppu, x, y, &px, &py);
    if (PPU_m7largeField(ppu) && (px | py) > UINT32_C(0x3ffff)) {
        if (!PPU_m7charFill(ppu)) return false;
        tile = 0;
    } else {
        tile = ppu->vram[
            (((py >> 11) & 0x7fu) * 128u + ((px >> 11) & 0x7fu)) &
            0x7fffu] & 0xff;
    }
    pixel = ppu->vram[
        (tile * 64 + ((py >> 8) & 7u) * 8u + ((px >> 8) & 7u)) &
        0x7fffu] >> 8;
    if (pixel == 0) return false;
    out->valid = true; out->color = ppu->cgram[pixel]; out->layer = (uint8_t)layer;
    out->priority = layer == 1 ? (pixel >> 7) : 0;
    out->rank = layer_rank(ppu, layer, out->priority);
    out->palette = (uint8_t)pixel; out->band = 0xffu;
    return true;
}

static void sample_bg_vram_tile(Ppu *ppu, int layer, int source_x,
                                int sample_y, uint16_t *entry,
                                int *in_x, int *in_y) {
    int world_x = source_x + ppu->hScroll[layer];
    int world_y = sample_y + ppu->vScroll[layer];
    int tile_size = PPU_bigTiles(ppu, layer) ? 16 : 8;
    int map_x, map_y;
    int map_address;
    if (world_x >= 0 && world_y >= 0) {
        int shift = tile_size == 16 ? 4 : 3;
        map_x = world_x >> shift;
        map_y = world_y >> shift;
        *in_x = world_x & (tile_size - 1);
        *in_y = world_y & (tile_size - 1);
    } else {
        map_x = floor_div8(world_x) / (tile_size / 8);
        map_y = floor_div8(world_y) / (tile_size / 8);
        *in_x = ((world_x % tile_size) + tile_size) % tile_size;
        *in_y = ((world_y % tile_size) + tile_size) % tile_size;
    }
    map_address = PPU_bgTilemapAdr(ppu, layer) + (map_x & 31) +
                  ((map_y & 31) << 5);
    if ((map_x & 32) != 0 && PPU_bgTilemapWider(ppu, layer))
        map_address += 0x400;
    if ((map_y & 32) != 0 && PPU_bgTilemapHigher(ppu, layer))
        map_address += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
    *entry = ppu->vram[map_address & 0x7fff];
}

static bool sample_bg(Ppu *ppu, int layer, int screen_x, int screen_y,
                      bool overlay_padding, SrPpuPixel *out) {
    int source_x, bpp, tile_size, map_x, map_y, in_x, in_y;
    int tile_address, pixel, palette;
    uint16_t entry;
    PpuVirtualTilemapBinding *binding;
    bool use_virtual, padding_from_authentic;
    uint8_t band = 0xffu;
    int sample_x = screen_x;
    /* The public scanline contract is one-based for BG/Mode-7 fetches even
     * though framebuffer and OBJ coordinates are zero-based. */
    int sample_y = screen_y + 1;
    if (PPU_mosaicEnabled(ppu, layer) && PPU_mosaicSize(ppu) > 1) {
        int size = PPU_mosaicSize(ppu);
        /* Mosaic groups are anchored in display space.  Quantizing after
         * scroll/policy mapping moves every group whenever HScroll changes. */
        sample_x -= ((sample_x % size) + size) % size;
        sample_y -= ((sample_y % size) + size) % size;
    }
    if (!layer_position(ppu, layer, sample_x, screen_y, overlay_padding,
                        &source_x)) return false;
    if (PPU_mode(ppu) == 7)
        return sample_mode7(ppu, layer, source_x, sample_y, out);
    bpp = bpp_for_mode(PPU_mode(ppu), layer);
    if (bpp == 0) return false;
    binding = &ppu->virtualTilemap[layer];
    /* Repeat and mirror are padding policies, not additional world spans.
     * Their source is the already-resolved authentic viewport.  A virtual
     * binding without IncludeAuthentic owns raw offscreen world coordinates,
     * but must not be consulted after a padding policy has remapped a margin
     * coordinate back into [0,255].  The legacy scanline contract makes this
     * ordering explicit by rendering the authentic layer first and copying
     * from it; preserve that ownership in the direct sampler. */
    {
        PpuWidescreenLayerPolicy policy =
            PpuResolveWidescreenLayerPolicy(ppu, (uint8_t)layer, screen_y);
        padding_from_authentic =
            (screen_x < 0 || screen_x >= kPpuXPixels) &&
            screen_y >= 0 && screen_y < kPpuYPixels &&
            (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) == 0u &&
            (policy.fill == kPpuWidescreenBandFill_Mirror ||
             policy.fill == kPpuWidescreenBandFill_Repeat);
    }
    use_virtual = PPU_mode(ppu) == 1 && binding->lookup != NULL &&
        !padding_from_authentic &&
        (screen_x < 0 || screen_x >= kPpuXPixels || screen_y < 0 ||
         screen_y >= kPpuYPixels ||
         (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u);
    if (use_virtual) {
        PpuVirtualSampleCache *cache = &ppu->virtualSampleCache[layer];
        PpuVirtualTilemapLookupResult result;
        bool cache_enabled =
            (ppu->renderFlags &
             kPpuRenderFlags_ReferencePixelRenderer) == 0u;
        int world_x = binding->camera_x + source_x +
            wrapped_delta10(ppu->hScroll[layer], binding->hscroll_anchor);
        /* Synthetic vertical rows continue through the virtual world just as
         * VRAM-backed tilemap rows do.  Clamping here held the first/last
         * authentic tile row across the diorama apron, producing visibly
         * stretched tiles instead of letting finite-world extents cut the
         * layer off at its authored boundary. */
        int world_y = binding->camera_y + sample_y +
            wrapped_delta10(ppu->vScroll[layer], binding->vscroll_anchor);
        map_x = floor_div8(world_x); map_y = floor_div8(world_y);
        in_x = world_x - map_x * 8; in_y = world_y - map_y * 8;
        if (cache_enabled && cache->valid &&
            cache->tile_x == map_x && cache->tile_y == map_y) {
            result = cache->result;
            entry = cache->entry;
            band = cache->band;
        } else {
            result = binding->lookup(
                binding->context, map_x, map_y, &entry);
            if (cache_enabled) {
                cache->tile_x = map_x;
                cache->tile_y = map_y;
                cache->result = (uint8_t)result;
                cache->valid = true;
            }
            if (result == kPpuVirtualTilemapLookup_Found &&
                binding->band_lookup != NULL)
                (void)binding->band_lookup(
                    binding->context, map_x, map_y, entry, &band);
            if (cache_enabled) {
                cache->entry = entry;
                cache->band = band;
            }
        }
        if (result == kPpuVirtualTilemapLookup_Transparent)
            return false;
        if (result == kPpuVirtualTilemapLookup_FallbackAuthentic) {
            sample_bg_vram_tile(
                ppu, layer, source_x, sample_y, &entry, &in_x, &in_y);
            band = 0xffu;
        } else if (result != kPpuVirtualTilemapLookup_Found) {
            return false;
        }
    } else {
        sample_bg_vram_tile(
            ppu, layer, source_x, sample_y, &entry, &in_x, &in_y);
    }
    tile_size = PPU_bigTiles(ppu, layer) ? 16 : 8;
    if ((entry & 0x4000u) != 0u) in_x = tile_size - 1 - in_x;
    if ((entry & 0x8000u) != 0u) in_y = tile_size - 1 - in_y;
    tile_address = PPU_bgTileAdr(ppu, layer);
    pixel = tile_pixel(ppu, tile_address,
        (entry & 0x3ffu) + (in_x >> 3) + ((in_y >> 3) << 4),
        in_y & 7, in_x & 7, bpp);
    if (pixel == 0) return false;
    if (bpp == 2) palette = (PPU_mode(ppu) == 0 ? layer * 32 : 0) +
                            ((entry >> 10) & 7u) * 4 + pixel;
    else if (bpp == 4) palette = ((entry >> 10) & 7u) * 16 + pixel;
    else palette = pixel;
    out->valid = true; out->color = ppu->cgram[palette & 0xff];
    out->rank = layer_rank(ppu, layer, (entry >> 13) & 1u);
    out->layer = (uint8_t)layer; out->priority = (entry >> 13) & 1u;
    out->palette = (uint8_t)palette; out->band = band;
    return true;
}

static bool slot_in_range(unsigned slot, unsigned first, unsigned count) {
    return count != 0u && slot >= first && slot < first + count;
}

/* Pure-C bit iteration keeps OBJ scanout portable while allowing compilers to
 * lower this recognized de-Bruijn form to their target's count-zero primitive. */
#if SNESRECOMP_PPU_BIT_WORD_BITS == 64
static unsigned lowest_set_bit_index(uint64_t value) {
    static const uint8_t indices[64] = {
         0,  1, 48,  2, 57, 49, 28,  3,
        61, 58, 50, 42, 38, 29, 17,  4,
        62, 55, 59, 36, 53, 51, 43, 22,
        45, 39, 33, 30, 24, 18, 12,  5,
        63, 47, 56, 27, 60, 41, 37, 16,
        54, 35, 52, 21, 44, 32, 23, 11,
        46, 26, 40, 15, 34, 20, 31, 10,
        25, 14, 19,  9, 13,  8,  7,  6
    };
    uint64_t isolated = value & (UINT64_C(0) - value);
    return indices[(isolated * UINT64_C(0x03f79d71b4cb0a89)) >> 58];
}
#else
static unsigned lowest_set_bit_index(uint32_t value) {
    static const uint8_t indices[32] = {
         0,  1, 28,  2, 29, 14, 24,  3,
        30, 22, 20, 15, 25, 17,  4,  8,
        31, 27, 13, 23, 21, 19, 16,  7,
        26, 12, 18,  6, 11,  5, 10,  9
    };
    uint32_t isolated = value & (0u - value);
    return indices[(isolated * UINT32_C(0x077cb531)) >> 27];
}
#endif

static unsigned append_obj_slots(PpuBitWord mask, unsigned base,
                                 uint8_t *slots, unsigned count) {
    while (mask != 0u) {
        unsigned bit = lowest_set_bit_index(mask);
        slots[count++] = (uint8_t)(base + bit);
        mask &= mask - 1u;
    }
    return count;
}

static void build_obj_sample_cache(Ppu *ppu, PpuObjSampleCache *cache,
                                   int screen_y, int x_offset,
                                   int include_first, int include_count,
                                   int exclude_first, int exclude_count) {
    unsigned start = (ppu->oamaddh & 0x80u) != 0u
        ? ppu->oamaddl >> 1 : 0u;
    const PpuBitWord *eligible = obj_scanline_masks(ppu, screen_y);
    uint8_t ordered_slots[128];
    unsigned slot_count = 0u;
    bool margin = screen_y < 0 || screen_y >= kPpuYPixels;
    memset(cache->pixels.data, 0, sizeof(cache->pixels.data));
    memset(cache->opaque, 0, sizeof(cache->opaque));
    if (eligible == NULL) goto finish;
#if SNESRECOMP_PPU_BIT_WORD_BITS == 64
    if (start < 64u) {
        slot_count = append_obj_slots(
            eligible[0] & (~UINT64_C(0) << start), 0u,
            ordered_slots, slot_count);
        slot_count = append_obj_slots(
            eligible[1], 64u, ordered_slots, slot_count);
        if (start != 0u)
            slot_count = append_obj_slots(
                eligible[0] & ((UINT64_C(1) << start) - 1u), 0u,
                ordered_slots, slot_count);
    } else {
        unsigned offset = start - 64u;
        slot_count = append_obj_slots(
            eligible[1] & (~UINT64_C(0) << offset), 64u,
            ordered_slots, slot_count);
        slot_count = append_obj_slots(
            eligible[0], 0u, ordered_slots, slot_count);
        if (offset != 0u)
            slot_count = append_obj_slots(
                eligible[1] & ((UINT64_C(1) << offset) - 1u), 64u,
                ordered_slots, slot_count);
    }
#else
    {
        unsigned start_word = start / kPpuBitWordBits;
        unsigned start_bit = start % kPpuBitWordBits;
        PpuBitWord at_or_above = eligible[start_word] &
            ((PpuBitWord)~(PpuBitWord)0u << start_bit);
        slot_count = append_obj_slots(
            at_or_above, start_word * kPpuBitWordBits,
            ordered_slots, slot_count);
        for (unsigned step = 1u; step < kPpuObjMaskWords; ++step) {
            unsigned word = (start_word + step) & (kPpuObjMaskWords - 1u);
            slot_count = append_obj_slots(
                eligible[word], word * kPpuBitWordBits,
                ordered_slots, slot_count);
        }
        if (start_bit != 0u) {
            PpuBitWord below = eligible[start_word] &
                (((PpuBitWord)1u << start_bit) - 1u);
            slot_count = append_obj_slots(
                below, start_word * kPpuBitWordBits,
                ordered_slots, slot_count);
        }
    }
#endif
    for (unsigned step = 0; step < slot_count; ++step) {
        unsigned slot = ordered_slots[step];
        PpuObjPart part;
        uint8_t rank;
        int palette_base, local_y, tile_y, tile_row;
        int tile_columns, tile_address, base_tile;
        bool hflip;
        if (include_count > 0 &&
            !slot_in_range(slot, (unsigned)include_first,
                           (unsigned)include_count)) continue;
        if (exclude_count > 0 &&
            slot_in_range(slot, (unsigned)exclude_first,
                          (unsigned)exclude_count)) continue;
        if (margin && !ppu->objPosValid[slot]) continue;
        if (!PpuResolveObjSlot(ppu, (uint8_t)slot, &part)) continue;
        if (ppu->objCameraRelative[slot]) part.x += x_offset;
        rank = layer_rank(ppu, 4, (part.tile_attr >> 12) & 3u);
        palette_base = 0x80 + ((part.tile_attr >> 9) & 7u) * 16;
        local_y = screen_y - part.y;
        if ((part.tile_attr & 0x8000u) != 0u)
            local_y = part.size - 1 - local_y;
        tile_y = local_y >> 3;
        tile_row = local_y & 7;
        tile_columns = part.size >> 3;
        tile_address = (part.tile_attr & 0x100u) != 0u ?
            ((ppu->obsel & 7u) << 13) + (((ppu->obsel & 0x18u) + 8u) << 9) :
            ((ppu->obsel & 7u) << 13);
        base_tile = part.tile_attr & 0xff;
        hflip = (part.tile_attr & 0x4000u) != 0u;
        for (int output_tile = 0; output_tile < tile_columns; ++output_tile) {
            int source_tile = hflip ? tile_columns - 1 - output_tile
                                    : output_tile;
            int tile = (((base_tile >> 4) + tile_y) << 4) |
                       (((base_tile & 15) + source_tile) & 15);
            uint32_t decoded = decoded_4bpp_row(
                ppu, tile_address + tile * 16 + tile_row);
            int output_x = part.x + output_tile * 8;
            for (int tile_x = 0; tile_x < 8; ++tile_x) {
                int x = output_x + tile_x;
                int source_x = hflip ? 7 - tile_x : tile_x;
                int pixel, index;
                PpuZbufType current;
                if (x < -kPpuExtraLeftRight ||
                    x >= kPpuXPixels + kPpuExtraLeftRight) continue;
                pixel = (decoded >> (source_x * 4)) & 15u;
                if (pixel == 0) continue;
                index = x + kPpuExtraLeftRight;
                current = cache->pixels.data[index];
                if ((current & 0xffu) == 0u || rank > (current >> 8)) {
                    cache->pixels.data[index] = (PpuZbufType)(
                        ((PpuZbufType)rank << 8) | (palette_base + pixel));
                    if ((unsigned)x < kPpuXPixels)
                        cache->opaque[(unsigned)x / kPpuBitWordBits] |=
                            (PpuBitWord)1u <<
                            ((unsigned)x % kPpuBitWordBits);
                }
            }
        }
    }
finish:
    cache->screen_y = (int16_t)screen_y;
    cache->x_offset = (int16_t)x_offset;
    cache->include_first = (uint8_t)include_first;
    cache->include_count = (uint8_t)include_count;
    cache->exclude_first = (uint8_t)exclude_first;
    cache->exclude_count = (uint8_t)exclude_count;
    cache->valid = true;
}

static PpuObjSampleCache *get_obj_sample_cache(Ppu *ppu, int screen_y,
        int x_offset, int include_first, int include_count,
        int exclude_first, int exclude_count) {
    PpuObjSampleCache *cache = NULL;
    for (int slot = 0; slot < kPpuObjSampleCacheCount; ++slot) {
        PpuObjSampleCache *candidate = &ppu->objSampleCache[slot];
        if (candidate->valid && candidate->screen_y == screen_y &&
            candidate->x_offset == x_offset &&
            candidate->include_first == include_first &&
            candidate->include_count == include_count &&
            candidate->exclude_first == exclude_first &&
            candidate->exclude_count == exclude_count) {
            cache = candidate;
            break;
        }
        if (!candidate->valid && cache == NULL) cache = candidate;
    }
    if (cache != NULL && !cache->valid)
        build_obj_sample_cache(ppu, cache, screen_y, x_offset,
                               include_first, include_count,
                               exclude_first, exclude_count);
    return cache;
}

static bool sample_obj_cached(Ppu *ppu, int screen_x, int screen_y,
        int x_offset, int include_first, int include_count,
        int exclude_first, int exclude_count,
        SrPpuPixel *out, bool *handled) {
    PpuObjSampleCache *cache;
    int index = screen_x + kPpuExtraLeftRight;
    *handled = true;
    if (index < 0 || index >= kPpuBufWidth) return false;
    cache = get_obj_sample_cache(ppu, screen_y, x_offset,
                                 include_first, include_count,
                                 exclude_first, exclude_count);
    if (cache == NULL) {
        *handled = false;
        return false;
    }
    {
        PpuZbufType encoded = cache->pixels.data[index];
        int palette_index = encoded & 0xffu;
        uint8_t rank = (uint8_t)(encoded >> 8);
        if (palette_index == 0) return false;
        memset(out, 0, sizeof(*out));
        out->valid = true;
        out->color = ppu->cgram[palette_index];
        out->rank = rank;
        out->layer = 4u;
        out->palette = (uint8_t)((palette_index - 0x80) >> 4);
        out->band = 0xffu;
        for (uint8_t priority = 0; priority < 4u; ++priority) {
            if (layer_rank(ppu, 4, priority) == rank) {
                out->priority = priority;
                break;
            }
        }
        return true;
    }
}

static bool sample_obj_filtered(Ppu *ppu, int screen_x, int screen_y,
        int x_offset, int include_first, int include_count,
        int exclude_first, int exclude_count, SrPpuPixel *out,
        int *out_slot) {
    unsigned start = (ppu->oamaddh & 0x80u) != 0u ? ppu->oamaddl >> 1 : 0u;
    bool margin = screen_y < 0 || screen_y >= kPpuYPixels;
    memset(out, 0, sizeof(*out));
    /* A capture of OAM 0..127 is the complete OBJ source, not a filtered
     * source.  Canonicalize it before selecting the sampler so full-scene
     * layer capture shares the scanline cache with ordinary composition.
     * Leaving it spelled as a range turned the same 128-slot walk into an
     * inner-pixel loop in diorama/action mode. */
    if ((ppu->renderFlags &
         kPpuRenderFlags_ReferencePixelRenderer) == 0u &&
        include_first == 0 && include_count == 128) include_count = 0;
    /* Conversely, removing OAM 0..127 is an empty source.  Do not prove that
     * by walking all 128 slots once for every destination pixel. */
    if ((ppu->renderFlags &
         kPpuRenderFlags_ReferencePixelRenderer) == 0u &&
        exclude_first == 0 && exclude_count == 128) return false;
    if (out_slot == NULL &&
        (ppu->renderFlags & kPpuRenderFlags_ReferencePixelRenderer) == 0u) {
        bool handled;
        bool found = sample_obj_cached(
            ppu, screen_x, screen_y, x_offset,
            include_first, include_count, exclude_first, exclude_count,
            out, &handled);
        if (handled) return found;
    }
    for (unsigned step = 0; step < 128u; ++step) {
        unsigned slot = (start + step) & 127u;
        PpuObjPart part;
        int pixel, palette;
        uint8_t rank;
        if (include_count > 0 &&
            !slot_in_range(slot, (unsigned)include_first,
                           (unsigned)include_count)) continue;
        if (exclude_count > 0 &&
            slot_in_range(slot, (unsigned)exclude_first,
                          (unsigned)exclude_count)) continue;
        if (margin && !ppu->objPosValid[slot]) continue;
        if (!PpuResolveObjSlot(ppu, (uint8_t)slot, &part)) continue;
        if (ppu->objCameraRelative[slot]) part.x += x_offset;
        pixel = obj_pixel(ppu, &part, screen_x, screen_y);
        if (pixel == 0) continue;
        rank = layer_rank(ppu, 4, (part.tile_attr >> 12) & 3u);
        if (out->valid && rank <= out->rank) continue;
        palette = 0x80 + ((part.tile_attr >> 9) & 7u) * 16 + pixel;
        out->valid = true; out->color = ppu->cgram[palette & 0xff];
        out->rank = rank; out->layer = 4u;
        out->priority = (part.tile_attr >> 12) & 3u;
        out->palette = (part.tile_attr >> 9) & 7u; out->band = 0xffu;
        if (out_slot != NULL) *out_slot = (int)slot;
    }
    return out->valid;
}

static bool capture_active(const PpuOverlayCapture *capture, int x, int y) {
    return capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
           x >= capture->x0 && x < capture->x1 &&
           y >= capture->y0 && y < capture->y1;
}

/* Capture removal must fail open when the host has no destination surface.
 * Pure-headless builds intentionally create no presentation textures, but the
 * game-side policy can still describe its ordinary HUD split.  Treating that
 * unbound policy as destructive would erase the source without exporting it. */
static bool capture_surface_bound(const Ppu *ppu, int source) {
    return ppu->overlayRenderBuffer[source] != NULL &&
           ppu->overlayRenderPitch[source] != 0u;
}

static void clear_overlay_row(Ppu *ppu, int source, int screen_y) {
    PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    bool active = capture->x1 > capture->x0 && capture->y1 > capture->y0;
    bool last_line =
        screen_y == kPpuYPixels - 1 + ppu->extraBottomCur;
    int row;
    uint32_t fill;
    if (ppu->overlayRenderBuffer[source] == NULL ||
        (!active && !ppu->overlayRenderMaybeDirty[source])) return;
    row = overlay_row(capture, screen_y);
    if (last_line) ppu->overlayRenderMaybeDirty[source] = active;
    if (row < 0 || row >= kPpuBufHeight) return;
    memset(ppu->overlayRenderBuffer[source] +
           (size_t)row * ppu->overlayRenderPitch[source], 0,
           ppu->overlayRenderPitch[source]);
    for (int band = 0; band < 3; ++band) if (ppu->overlayRenderBands[source][band])
        memset(ppu->overlayRenderBands[source][band] +
               (size_t)row * ppu->overlayRenderPitch[source], 0,
               ppu->overlayRenderPitch[source]);
    if (screen_y < capture->y0 || screen_y >= capture->y1) return;
    fill = (capture->flags & (kPpuOverlayFlag_MarkMainScreenWinner |
                              kPpuOverlayFlag_MarkOwningScreenWinner)) != 0u
        ? 0xff000000u
        : PpuOverlayTransparentFillColor(ppu, (PpuOverlaySource)source);
    if (fill != 0u) {
        uint32_t *pixels = (uint32_t *)(ppu->overlayRenderBuffer[source] +
            (size_t)row * ppu->overlayRenderPitch[source]);
        int origin = surface_origin_x(ppu, ppu->overlayRenderPitch[source]);
        for (int x = capture->x0; x < capture->x1; ++x) pixels[origin + x] = fill;
        if ((ppu->screenEnabled[0] & (1u << source)) != 0u ||
            (ppu->screenEnabled[1] & (1u << source)) != 0u)
            ppu->overlayRenderContentMask[source] |= 1u;
    }
}

static void write_overlay(Ppu *ppu, int source, int x, int y,
                          const SrPpuPixel *pixel, uint32_t override_color) {
    PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    uint8_t *surface;
    int band = 0, row, origin;
    uint32_t color;
    if (ppu->overlayRenderBuffer[source] == NULL ||
        !capture_active(capture, x, y)) return;
    if (source == kPpuOverlaySource_Obj) band = pixel->priority;
    else if (pixel->band != 0xffu && source < 2) {
        /* Virtual tilemap classifications are semantic presentation bands:
         * 1 is ordinary, 2 is high, and 0 is the far/background plane. */
        band = pixel->band == 1u ? 0 : pixel->band == 2u ? 1 : 2;
    }
    else if (pixel->band != 0xffu) band = pixel->band;
    else band = pixel->priority;
    surface = band > 0 && band <= 3 && ppu->overlayRenderBands[source][band - 1]
        ? ppu->overlayRenderBands[source][band - 1]
        : ppu->overlayRenderBuffer[source];
    if (surface == ppu->overlayRenderBuffer[source]) band = 0;
    row = overlay_row(capture, y);
    origin = surface_origin_x(ppu, ppu->overlayRenderPitch[source]);
    color = override_color != 0u ? override_color : color_argb(ppu,
        (capture->flags & kPpuOverlayFlag_ApplyBgFixedColorSubtract) != 0u &&
                source < 4
            ? color_math(pixel->color, ppu->fixedColor, true, false)
            : pixel->color);
    if ((capture->flags & kPpuOverlayFlag_MarkObjColorMath) != 0u &&
        source == kPpuOverlaySource_Obj && pixel->palette >= 4u)
        color = (color & 0x00ffffffu) | 0x80000000u;
    if ((capture->flags & kPpuOverlayFlag_MarkBgHalfAdd) != 0u && source < 4)
        color = (color & 0x00ffffffu) | 0x80000000u;
    ((uint32_t *)(surface + (size_t)row * ppu->overlayRenderPitch[source]))
        [origin + x] = color;
    ppu->overlayRenderContentMask[source] |= (uint8_t)(1u << band);
}

static bool source_visible_on_screen(const Ppu *ppu, int source, bool sub,
                                     int x) {
    unsigned screen = sub ? 1u : 0u;
    return (ppu->screenEnabled[screen] & (1u << source)) != 0u &&
           ((ppu->screenWindowed[screen] & (1u << source)) == 0u ||
            !window_inside(ppu, source, x));
}

/* The packed capture path resolves MarkFullAddSubscreen itself (see the
 * full-add export in render_native_capture_line), so such a line no longer has
 * to be handed to the per-pixel reference sampler.  The two winner-mask
 * policies still need the reference screen resolves. */
static bool capture_needs_reference_sampler(
        const PpuOverlayCapture *capture) {
    return (capture->flags & (kPpuOverlayFlag_MarkMainScreenWinner |
                              kPpuOverlayFlag_MarkOwningScreenWinner)) != 0u;
}

static bool capture_is_deferred(const PpuOverlayCapture *capture) {
    return (capture->flags & (kPpuOverlayFlag_MarkFullAddSubscreen |
                              kPpuOverlayFlag_MarkMainScreenWinner |
                              kPpuOverlayFlag_MarkOwningScreenWinner)) != 0u;
}

static void capture_obj_sources(Ppu *ppu, int x, int y, int obj_offset) {
    PpuOverlayCapture *capture =
        &ppu->overlayCaptures[kPpuOverlaySource_Obj];
    bool owner_sub = (ppu->screenEnabled[0] & 0x10u) == 0u;
    if (ppu->objRangeCapture.count != 0u &&
        x >= ppu->objRangeCapture.x0 && x < ppu->objRangeCapture.x1 &&
        y >= ppu->objRangeCapture.y0 && y < ppu->objRangeCapture.y1) {
        SrPpuPixel pixel;
        if (sample_obj_filtered(ppu, x, y, obj_offset,
                ppu->objRangeCapture.first, ppu->objRangeCapture.count,
                0, 0, &pixel, NULL)) {
            int origin = surface_origin_x(ppu, ppu->objRangeCapture.pitch);
            uint32_t *row = (uint32_t *)(ppu->objRangeCapture.pixels +
                (size_t)y * ppu->objRangeCapture.pitch);
            row[origin + x] = color_argb(ppu, pixel.color);
        }
    }
    if (!capture_surface_bound(ppu, kPpuOverlaySource_Obj) ||
        capture->oamCount == 0u || !capture_active(capture, x, y) ||
        capture_is_deferred(capture) ||
        !source_visible_on_screen(ppu, kPpuOverlaySource_Obj, owner_sub, x))
        return;
    {
        SrPpuPixel pixel;
        if (sample_obj_filtered(ppu, x, y, obj_offset,
                capture->oamFirst, capture->oamCount, 0, 0,
                &pixel, NULL))
            write_overlay(ppu, kPpuOverlaySource_Obj, x, y, &pixel, 0u);
    }
}

static SrPpuPixel resolve_screen(Ppu *ppu, int x, int y, bool sub,
        bool capture, int obj_offset, bool honor_removal,
        int excluded_source, bool exclude_relocated_obj,
        SrPpuPixel *unremoved_out) {
    SrPpuPixel result = {ppu->cgram[0], 1u, 5u, 0u, 0u, 0xffu, true};
    SrPpuPixel unremoved = result;
    for (int layer = 0; layer < 4; ++layer) {
        SrPpuPixel pixel = {0};
        PpuOverlayCapture *cap = &ppu->overlayCaptures[layer];
        bool enabled, export_this_pass, removed;
        if (layer == excluded_source) continue;
        enabled = source_visible_on_screen(ppu, layer, sub, x);
        /* Disabled layers cannot win this screen or be exported from it.
         * Reject them before the comparatively expensive tile fetch. */
        if (!enabled || !sample_bg(ppu, layer, x, y, capture, &pixel))
            continue;
        export_this_pass = enabled &&
            (!sub || (ppu->screenEnabled[0] & (1u << layer)) == 0u);
        if (capture && export_this_pass && capture_active(cap, x, y) &&
            !capture_is_deferred(cap))
            write_overlay(ppu, layer, x, y, &pixel, 0u);
        if (unremoved_out != NULL && enabled && pixel.rank > unremoved.rank)
            unremoved = pixel;
        removed = honor_removal && capture_surface_bound(ppu, layer) &&
                  capture_active(cap, x, y) &&
                  (cap->flags & kPpuOverlayFlag_RemoveFromGame) != 0u;
        if (removed) continue;
        if (pixel.rank > result.rank) result = pixel;
    }
    if (excluded_source != kPpuOverlaySource_Obj &&
        source_visible_on_screen(ppu, kPpuOverlaySource_Obj, sub, x)) {
        PpuOverlayCapture *cap =
            &ppu->overlayCaptures[kPpuOverlaySource_Obj];
        int exclude_first = 0, exclude_count = 0;
        SrPpuPixel pixel = {0};
        SrPpuPixel original_pixel = {0};
        bool original_found = false;
        if (unremoved_out != NULL) {
            original_found = sample_obj_filtered(
                ppu, x, y, obj_offset, 0, 0, 0, 0, &original_pixel, NULL);
            if (original_found && original_pixel.rank > unremoved.rank)
                unremoved = original_pixel;
        }
        if (exclude_relocated_obj && ppu->overlayObjRelocatedCount != 0u) {
            exclude_first = ppu->overlayObjRelocatedFirst;
            exclude_count = ppu->overlayObjRelocatedCount;
        } else if (honor_removal &&
                   capture_surface_bound(ppu, kPpuOverlaySource_Obj) &&
                   capture_active(cap, x, y) &&
                   (cap->flags & kPpuOverlayFlag_RemoveFromGame) != 0u) {
            exclude_first = cap->oamFirst;
            exclude_count = cap->oamCount;
        }
        if (exclude_count == 0 && unremoved_out != NULL) {
            pixel = original_pixel;
        } else {
            (void)sample_obj_filtered(ppu, x, y, obj_offset, 0, 0,
                                      exclude_first, exclude_count,
                                      &pixel, NULL);
        }
        if (pixel.valid &&
            pixel.rank > result.rank)
            result = pixel;
    }
    if (unremoved_out != NULL) *unremoved_out = unremoved;
    return result;
}

/* Resolve the ordinary main/sub pair with one tile/OBJ sample per source.
 * The two screens usually share BG1 for color math; resolving them separately
 * needlessly repeated every address calculation and planar tile fetch. */
static void resolve_screen_pair(Ppu *ppu, int x, int y, bool capture,
        int obj_offset, bool honor_removal, bool want_sub,
        bool want_unremoved, SrPpuPixel *main_out, SrPpuPixel *sub_out,
        SrPpuPixel *unremoved_main_out, SrPpuPixel *unremoved_sub_out) {
    SrPpuPixel backdrop = {ppu->cgram[0], 1u, 5u, 0u, 0u, 0xffu, true};
    SrPpuPixel main = backdrop, sub = backdrop;
    SrPpuPixel unremoved_main = backdrop, unremoved_sub = backdrop;
    for (int layer = 0; layer < 4; ++layer) {
        PpuOverlayCapture *cap = &ppu->overlayCaptures[layer];
        SrPpuPixel pixel = {0};
        bool main_enabled = source_visible_on_screen(ppu, layer, false, x);
        bool sub_enabled = want_sub &&
            source_visible_on_screen(ppu, layer, true, x);
        bool removed;
        if ((!main_enabled && !sub_enabled) ||
            !sample_bg(ppu, layer, x, y, capture, &pixel)) continue;
        if (capture && capture_active(cap, x, y) &&
            !capture_is_deferred(cap) &&
            (main_enabled || (sub_enabled &&
                (ppu->screenEnabled[0] & (1u << layer)) == 0u)))
            write_overlay(ppu, layer, x, y, &pixel, 0u);
        if (want_unremoved) {
            if (main_enabled && pixel.rank > unremoved_main.rank)
                unremoved_main = pixel;
            if (sub_enabled && pixel.rank > unremoved_sub.rank)
                unremoved_sub = pixel;
        }
        removed = honor_removal && capture_surface_bound(ppu, layer) &&
                  capture_active(cap, x, y) &&
                  (cap->flags & kPpuOverlayFlag_RemoveFromGame) != 0u;
        if (removed) continue;
        if (main_enabled && pixel.rank > main.rank) main = pixel;
        if (sub_enabled && pixel.rank > sub.rank) sub = pixel;
    }
    {
        const int source = kPpuOverlaySource_Obj;
        PpuOverlayCapture *cap = &ppu->overlayCaptures[source];
        bool main_enabled = source_visible_on_screen(ppu, source, false, x);
        bool sub_enabled = want_sub &&
            source_visible_on_screen(ppu, source, true, x);
        SrPpuPixel original = {0}, pixel = {0};
        int exclude_first = 0, exclude_count = 0;
        if (main_enabled || sub_enabled) {
            bool found = sample_obj_filtered(ppu, x, y, obj_offset,
                                             0, 0, 0, 0, &original, NULL);
            if (found && want_unremoved) {
                if (main_enabled && original.rank > unremoved_main.rank)
                    unremoved_main = original;
                if (sub_enabled && original.rank > unremoved_sub.rank)
                    unremoved_sub = original;
            }
            if (honor_removal && capture_surface_bound(ppu, source) &&
                capture_active(cap, x, y) &&
                (cap->flags & kPpuOverlayFlag_RemoveFromGame) != 0u) {
                exclude_first = cap->oamFirst;
                exclude_count = cap->oamCount;
            }
            if (exclude_count == 0) {
                pixel = original;
            } else {
                (void)sample_obj_filtered(ppu, x, y, obj_offset, 0, 0,
                                          exclude_first, exclude_count,
                                          &pixel, NULL);
            }
            if (pixel.valid) {
                if (main_enabled && pixel.rank > main.rank) main = pixel;
                if (sub_enabled && pixel.rank > sub.rank) sub = pixel;
            }
        }
    }
    *main_out = main;
    *sub_out = sub;
    if (want_unremoved) {
        *unremoved_main_out = unremoved_main;
        *unremoved_sub_out = unremoved_sub;
    }
}

static uint16_t final_color(Ppu *ppu, int x, const SrPpuPixel *main,
                            const SrPpuPixel *sub) {
    uint16_t color = main->color;
    bool color_window = window_inside(ppu, 5, x);
    unsigned clip = PPU_clipMode(ppu);
    unsigned prevent = PPU_preventMathMode(ppu);
    bool clipped = clip == 3u || (clip == 2u && color_window) ||
                   (clip == 1u && !color_window);
    bool prevented = prevent == 3u || (prevent == 2u && color_window) ||
                     (prevent == 1u && !color_window);
    bool eligible = main->layer < 5u &&
                    (PPU_mathEnabled(ppu) & (1u << main->layer)) != 0u;
    if (main->layer == 4u && main->palette < 4u) eligible = false;
    if (clipped) color = 0u;
    if (eligible && !prevented) {
        bool use_sub = PPU_addSubscreen(ppu) && sub->layer != 5u;
        color = color_math(color, use_sub ? sub->color : ppu->fixedColor,
                           PPU_subtractColor(ppu),
                           PPU_halfColor(ppu) && (use_sub || !PPU_addSubscreen(ppu)));
    }
    return color;
}

/* Native scanlines have no reason to build a separate pixel record for every
 * BG layer.  A packed resolved pixel keeps the CGRAM index, priority rank, and
 * winning source in one word while sources are decoded directly into the
 * final main/sub scanlines. */
static uint16_t native_pack_pixel(unsigned palette, unsigned rank,
                                  unsigned layer) {
    /* Ranks are unique across the competing sources in tiled modes, so keep
     * the rank in the most-significant nibble.  A packed pixel can then serve
     * directly as its winner-selection key without extracting the rank in
     * every inner-loop comparison. */
    return (uint16_t)((palette & 0xffu) | ((layer & 7u) << 8) |
                      ((rank & 15u) << 12));
}

static unsigned native_pixel_rank(uint16_t pixel) {
    return pixel >> 12;
}

static unsigned native_pixel_layer(uint16_t pixel) {
    return (pixel >> 8) & 7u;
}

#if SR_PPU_TILE_SIMD
/* Turn the compact cached 2/4-bpp rows into one pixel per byte. The scalar
 * spreading is cheaper than eight independent extracts and gives both NEON
 * and SSE2 a naturally ordered vector without increasing the decoded-row
 * caches, which are deliberately kept cache-friendly. */
static uint64_t native_expand_tile_pixels(uint64_t pixels, int bpp) {
    if (bpp == 4) {
        pixels = (pixels | (pixels << 16)) & UINT64_C(0x0000ffff0000ffff);
        pixels = (pixels | (pixels << 8)) & UINT64_C(0x00ff00ff00ff00ff);
        return (pixels | (pixels << 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    }
    if (bpp == 2) {
        pixels = (pixels | (pixels << 24)) & UINT64_C(0x000000ff000000ff);
        pixels = (pixels | (pixels << 12)) & UINT64_C(0x000f000f000f000f);
        return (pixels | (pixels << 6)) & UINT64_C(0x0303030303030303);
    }
    return pixels;
}

/* Complete, unwindowed tiles dominate ordinary Modes 0-6. Do their eight
 * transparent/priority tests in parallel for either resolved screen.
 * Architecture choice is compile-time only: unsupported targets keep the
 * scalar implementation below, with no runtime dispatch or probe latency. */
static void native_apply_tile_simd(uint16_t *destination,
        uint16_t packed_base, uint64_t decoded_pixels, int bpp, bool hflip) {
    uint64_t expanded = native_expand_tile_pixels(decoded_pixels, bpp);
#if SR_SIMD_NEON
    uint16x8_t values = vmovl_u8(vcreate_u8(expanded));
    uint16x8_t current;
    uint16x8_t packed;
    uint16x8_t replace;
    if (hflip) {
        values = vcombine_u16(vrev64_u16(vget_high_u16(values)),
                              vrev64_u16(vget_low_u16(values)));
    }
    current = vld1q_u16(destination);
    packed = vaddq_u16(values, vdupq_n_u16(packed_base));
    replace = vandq_u16(vcgtq_u16(values, vdupq_n_u16(0u)),
                        vcgtq_u16(packed, current));
    vst1q_u16(destination, vbslq_u16(replace, packed, current));
#else
    __m128i values = _mm_unpacklo_epi8(
        _mm_cvtsi64_si128((long long)expanded), _mm_setzero_si128());
    __m128i current;
    __m128i packed;
    __m128i replace;
    const __m128i sign = _mm_set1_epi16((short)0x8000);
    if (hflip) {
        values = _mm_shufflelo_epi16(values, _MM_SHUFFLE(0, 1, 2, 3));
        values = _mm_shufflehi_epi16(values, _MM_SHUFFLE(0, 1, 2, 3));
        values = _mm_shuffle_epi32(values, _MM_SHUFFLE(1, 0, 3, 2));
    }
    current = _mm_loadu_si128((const __m128i *)destination);
    packed = _mm_add_epi16(values, _mm_set1_epi16((short)packed_base));
    replace = _mm_and_si128(
        _mm_cmpgt_epi16(values, _mm_setzero_si128()),
        _mm_cmpgt_epi16(_mm_xor_si128(packed, sign),
                        _mm_xor_si128(current, sign)));
    _mm_storeu_si128((__m128i *)destination,
        _mm_or_si128(_mm_and_si128(replace, packed),
                     _mm_andnot_si128(replace, current)));
#endif
}
#endif

/* Virtual Mode-1 providers already return a complete 4-bpp tile word.  Keep
 * the common unwindowed, full-tile winner operation as compact as the VRAM
 * path while retaining an exact scalar implementation for every target that
 * does not enable a compile-time SIMD backend. */
static void native_apply_virtual_4bpp_tile(uint16_t *destination,
        uint16_t packed_base, uint32_t decoded, bool hflip) {
#if SR_PPU_TILE_SIMD
    native_apply_tile_simd(
        destination, packed_base, decoded, 4, hflip);
#else
#define APPLY_VIRTUAL_PIXEL(offset_, value_) do {                          \
        unsigned value = (unsigned)(value_);                               \
        if (value != 0u) {                                                  \
            uint16_t packed = (uint16_t)(packed_base + value);              \
            if (packed > destination[(offset_)])                            \
                destination[(offset_)] = packed;                            \
        }                                                                   \
    } while (0)
    if (hflip) {
        APPLY_VIRTUAL_PIXEL(0, (decoded >> 28) & 15u);
        APPLY_VIRTUAL_PIXEL(1, (decoded >> 24) & 15u);
        APPLY_VIRTUAL_PIXEL(2, (decoded >> 20) & 15u);
        APPLY_VIRTUAL_PIXEL(3, (decoded >> 16) & 15u);
        APPLY_VIRTUAL_PIXEL(4, (decoded >> 12) & 15u);
        APPLY_VIRTUAL_PIXEL(5, (decoded >> 8) & 15u);
        APPLY_VIRTUAL_PIXEL(6, (decoded >> 4) & 15u);
        APPLY_VIRTUAL_PIXEL(7, decoded & 15u);
    } else {
        APPLY_VIRTUAL_PIXEL(0, decoded & 15u);
        APPLY_VIRTUAL_PIXEL(1, (decoded >> 4) & 15u);
        APPLY_VIRTUAL_PIXEL(2, (decoded >> 8) & 15u);
        APPLY_VIRTUAL_PIXEL(3, (decoded >> 12) & 15u);
        APPLY_VIRTUAL_PIXEL(4, (decoded >> 16) & 15u);
        APPLY_VIRTUAL_PIXEL(5, (decoded >> 20) & 15u);
        APPLY_VIRTUAL_PIXEL(6, (decoded >> 24) & 15u);
        APPLY_VIRTUAL_PIXEL(7, decoded >> 28);
    }
#undef APPLY_VIRTUAL_PIXEL
#endif
}

static bool native_capture_intersects(const PpuOverlayCapture *capture,
                                      int screen_y) {
    return capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
           capture->x1 > 0 && capture->x0 < kPpuXPixels &&
           screen_y >= capture->y0 && screen_y < capture->y1;
}

static bool native_capture_line_needed(const Ppu *ppu, int screen_y) {
    const PpuObjRangeCapture *range = &ppu->objRangeCapture;
    if (range->count != 0u && range->pixels != NULL &&
        range->x1 > 0 && range->x0 < kPpuXPixels &&
        screen_y >= range->y0 && screen_y < range->y1)
        return true;
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        if (capture_surface_bound(ppu, source) &&
            native_capture_intersects(
                &ppu->overlayCaptures[source], screen_y))
            return true;
    }
    return false;
}

static bool native_capture_policy_on_line(const Ppu *ppu, int screen_y) {
    const PpuObjRangeCapture *range = &ppu->objRangeCapture;
    if (range->count != 0u && range->pixels != NULL &&
        screen_y >= range->y0 && screen_y < range->y1)
        return true;
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        if (capture_surface_bound(ppu, source) &&
            capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
            screen_y >= capture->y0 && screen_y < capture->y1)
            return true;
    }
    return false;
}

static bool native_fast_eligible(const Ppu *ppu, int screen_y,
                                 bool capture) {
    uint8_t visible_layers = ppu->screenEnabled[0] | ppu->screenEnabled[1];
    int mode = PPU_mode(ppu);
    if ((ppu->renderFlags & kPpuRenderFlags_ReferencePixelRenderer) != 0u)
        return false;
    /* The packed capture path can mix tile-span sources with the reference
     * sampler for sources that need an unusual padding policy.  That makes it
     * safe for synthetic vertical rows too; ordinary non-capture scanout still
     * has no destination storage outside the authentic 224 lines. */
    if ((screen_y < 0 || screen_y >= kPpuYPixels) && !capture)
        return false;
    for (int layer = 0; layer < 4; ++layer) {
        const PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[layer];
        if (bpp_for_mode(mode, layer) == 0 &&
            !(mode == 7 && layer == 1 && PPU_m7extBg(ppu))) continue;
        /* Tiled modes resolve mosaic per layer.  Rejecting the complete
         * scanline made every other BG, OBJ, and composition stage pay the
         * reference pixel-renderer cost for a local fetch effect.  Mode 7
         * still needs a dedicated affine mosaic kernel. */
        if (mode == 7 && (visible_layers & (1u << layer)) != 0u &&
            PPU_mosaicEnabled(ppu, layer) && PPU_mosaicSize(ppu) > 1)
            return false;
        if (mode == 1 && binding->lookup != NULL &&
            (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u &&
            PPU_bigTiles(ppu, layer))
            return false;
    }
    if (!capture) return true;
    /* The scaled host override writes subpixels into a separate surface while
     * deciding removal from the base affine sample.  Keep that uncommon case
     * on its dedicated path; ordinary Mode 7 can use packed capture. */
    if (mode == 7 && ppu->m7Override.rgba != NULL) return false;
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        const PpuOverlayCapture *capture_policy =
            &ppu->overlayCaptures[source];
        if (capture_surface_bound(ppu, source) &&
            native_capture_intersects(capture_policy, screen_y) &&
            capture_needs_reference_sampler(capture_policy))
            return false;
    }
    return true;
}

typedef struct NativeLayerWindowPlan {
    /* 0 = hidden, 1 = always visible, 2 = visible outside window runs. */
    uint8_t main_mode;
    uint8_t sub_mode;
    uint8_t run;
    NativeWindowRuns runs;
} NativeLayerWindowPlan;

static void native_layer_window_plan(const Ppu *ppu, int layer,
                                     bool want_sub,
                                     NativeLayerWindowPlan *plan) {
    unsigned layer_bit = 1u << layer;
    bool main_enabled = (ppu->screenEnabled[0] & layer_bit) != 0u;
    bool sub_enabled = want_sub &&
        (ppu->screenEnabled[1] & layer_bit) != 0u;
    bool main_windowed = (ppu->screenWindowed[0] & layer_bit) != 0u;
    bool sub_windowed = (ppu->screenWindowed[1] & layer_bit) != 0u;
    int uniform = main_windowed || sub_windowed
        ? native_window_uniform(ppu, layer) : 0;
    plan->main_mode = main_enabled
        ? (uint8_t)(!main_windowed || uniform == 0 ? 1 : uniform < 0 ? 2 : 0)
        : 0u;
    plan->sub_mode = sub_enabled
        ? (uint8_t)(!sub_windowed || uniform == 0 ? 1 : uniform < 0 ? 2 : 0)
        : 0u;
    plan->run = 0u;
    if (plan->main_mode == 2u || plan->sub_mode == 2u)
        native_window_runs(ppu, layer, &plan->runs);
}

static bool native_window_plan_inside(NativeLayerWindowPlan *plan, int x) {
    if (plan->main_mode != 2u && plan->sub_mode != 2u) return false;
    while (plan->run + 1u < plan->runs.count &&
           x >= plan->runs.edges[plan->run + 1u])
        ++plan->run;
    return plan->runs.inside[plan->run] != 0u;
}

static void native_resolve_vram_bg_span(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub, int left, int right, int origin,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels);

/* Virtual Mode-1 maps expose one 8x8 tile word at a time.  Resolve a complete
 * tile span into the packed scanline just like the VRAM-backed tiled path;
 * calling the general pixel sampler here would throw away both the provider's
 * tile-shaped contract and the native winner buffers. */
static void native_resolve_virtual_bg_span(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub, int left, int right, int origin,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels,
        uint8_t *SR_RESTRICT bands) {
    PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[layer];
    NativeLayerWindowPlan plan;
    PpuWidescreenLayerPolicy policy;
    int extent_row = clamp_int(screen_y, 0, kPpuYPixels - 1);
    int sample_y = screen_y + 1;
    int world_y = binding->camera_y + sample_y +
        wrapped_delta10(ppu->vScroll[layer], binding->vscroll_anchor);
    int tile_y = floor_div8(world_y);
    int fine_y = world_y - tile_y * 8;
    int x_delta = wrapped_delta10(
        ppu->hScroll[layer], binding->hscroll_anchor);
    int tile_address = PPU_bgTileAdr(ppu, layer);
    const uint16_t *batch_entries = NULL;
    ptrdiff_t batch_entry_step = 0;
    size_t batch_remaining = 0u;
    uint16_t extent;
    if (screen_y < 0) {
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            -screen_y > ppu->verticalMarginTopRows[layer]) return;
        if (ppu->wsLayerExtentTop[layer] !=
                kPpuWidescreenExtentAvailable &&
            -screen_y > ppu->wsLayerExtentTop[layer]) return;
    } else if (screen_y >= kPpuYPixels) {
        int distance = screen_y - (kPpuYPixels - 1);
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            distance > ppu->verticalMarginBottomRows[layer]) return;
        if (ppu->wsLayerExtentBottom[layer] !=
                kPpuWidescreenExtentAvailable &&
            distance > ppu->wsLayerExtentBottom[layer]) return;
    }
    if (left < 0) {
        extent = ppu->wsLayerExtentLeft[layer][extent_row];
        if (extent != kPpuWidescreenExtentAvailable && left < -(int)extent)
            left = -(int)extent;
    }
    if (right > kPpuXPixels) {
        extent = ppu->wsLayerExtentRight[layer][extent_row];
        if (extent != kPpuWidescreenExtentAvailable &&
            right > kPpuXPixels + (int)extent)
            right = kPpuXPixels + (int)extent;
    }
    if (left >= right) return;
    policy = PpuResolveWidescreenLayerPolicy(
        ppu, (uint8_t)layer, screen_y);
    native_layer_window_plan(ppu, layer, want_sub, &plan);
    if (plan.main_mode == 0u && plan.sub_mode == 0u) return;
    for (int x = left; x < right;) {
        int source_x;
        int step = 1;
        int segment_right = right;
        if (!PpuMapWidescreenLayerXWithPolicy(
                ppu, (uint8_t)layer, x, &source_x, &policy)) {
            if (x < 0) x = right < 0 ? right : 0;
            else if (x >= kPpuXPixels) break;
            else ++x;
            continue;
        }
        if (x < 0) {
            if (segment_right > 0) segment_right = 0;
            if (policy.fill == kPpuWidescreenBandFill_Mirror) step = -1;
        } else if (x < kPpuXPixels) {
            if (segment_right > kPpuXPixels)
                segment_right = kPpuXPixels;
        } else if (policy.fill == kPpuWidescreenBandFill_Mirror) {
            step = -1;
        }
        int world_x = binding->camera_x + source_x + x_delta;
        int tile_x = floor_div8(world_x);
        int fine_x = world_x - tile_x * 8;
        int run = step > 0 ? 8 - fine_x : fine_x + 1;
        uint16_t entry = 0u;
        uint8_t band = 0xffu;
        PpuVirtualTilemapLookupResult result =
            kPpuVirtualTilemapLookup_Transparent;
        bool batched = false;
        if (run > segment_right - x) run = segment_right - x;
        /* Normal-scroll mirror subtracts twice the live H scroll modulo 256.
         * Split the span where that remapped source wraps from zero to 255;
         * the world coordinate is discontinuous there even if the decoded
         * tile row still has pixels remaining. */
        if (step < 0 &&
            policy.motion == kPpuWidescreenMotion_NormalScroll &&
            run > source_x + 1) run = source_x + 1;
        if (binding->lookup_span != NULL) {
            if (batch_remaining == 0u) {
                /* The request stops at every fill-direction and normal-scroll
                 * wrap boundary.  The returned run is therefore consumed
                 * before source mapping can become discontinuous. */
                int batch_pixels = segment_right - x;
                size_t capacity;
                if (step < 0 &&
                    policy.motion == kPpuWidescreenMotion_NormalScroll &&
                    batch_pixels > source_x + 1)
                    batch_pixels = source_x + 1;
                capacity = (size_t)(step > 0
                    ? fine_x + batch_pixels + 7
                    : 7 - fine_x + batch_pixels + 7) / 8u;
                if (capacity > kPpuSurfaceWidth / 8u + 2u)
                    capacity = kPpuSurfaceWidth / 8u + 2u;
                batch_remaining = binding->lookup_span(
                    binding->context, tile_x, tile_y, step,
                    capacity, &batch_entries, &batch_entry_step);
                if (batch_remaining > capacity) batch_remaining = 0u;
            }
            if (batch_remaining != 0u) {
                if (batch_entries != NULL) {
                    entry = *batch_entries;
                    result = kPpuVirtualTilemapLookup_Found;
                    if (batch_remaining > 1u)
                        batch_entries += batch_entry_step;
                }
                --batch_remaining;
                batched = true;
            }
        }
        if (!batched) {
            result = binding->lookup(
                binding->context, tile_x, tile_y, &entry);
        }
        if (result == kPpuVirtualTilemapLookup_FallbackAuthentic) {
            native_resolve_vram_bg_span(
                ppu, layer, screen_y, want_sub, x, x + run, origin,
                main_pixels, sub_pixels);
            x += run;
            continue;
        }
        if (result != kPpuVirtualTilemapLookup_Found) {
            x += run;
            continue;
        }
        if (bands != NULL && binding->band_lookup != NULL)
            (void)binding->band_lookup(
                binding->context, tile_x, tile_y, entry, &band);
        {
            int row = (entry & 0x8000u) != 0u ? 7 - fine_y : fine_y;
            uint32_t decoded = decoded_4bpp_row(
                ppu, tile_address + (entry & 0x3ffu) * 16 + row);
            unsigned palette_base = ((entry >> 10) & 7u) * 16u;
            unsigned rank = layer_rank(
                ppu, layer, (entry >> 13) & 1u);
            if (run == 8 && bands == NULL &&
                plan.main_mode != 2u && plan.sub_mode != 2u) {
                uint16_t packed_base = native_pack_pixel(
                    palette_base, rank, (unsigned)layer);
                bool hflip = ((entry & 0x4000u) != 0u) != (step < 0);
                if (plan.main_mode == 1u)
                    native_apply_virtual_4bpp_tile(
                        main_pixels + origin + x, packed_base,
                        decoded, hflip);
                if (plan.sub_mode == 1u)
                    native_apply_virtual_4bpp_tile(
                        sub_pixels + origin + x, packed_base,
                        decoded, hflip);
                x += run;
                continue;
            }
            for (int offset = 0; offset < run; ++offset) {
                int tile_x_pixel = fine_x + offset * step;
                int destination = origin + x + offset;
                bool inside;
                bool show_main, show_sub;
                unsigned pixel;
                uint16_t packed;
                if ((entry & 0x4000u) != 0u)
                    tile_x_pixel = 7 - tile_x_pixel;
                pixel = (decoded >> (tile_x_pixel * 4)) & 15u;
                if (pixel == 0u) continue;
                inside = native_window_plan_inside(&plan, x + offset);
                show_main = plan.main_mode == 1u ||
                    (plan.main_mode == 2u && !inside);
                show_sub = plan.sub_mode == 1u ||
                    (plan.sub_mode == 2u && !inside);
                packed = native_pack_pixel(
                    palette_base + pixel, rank, (unsigned)layer);
                if (show_main && packed > main_pixels[destination])
                    main_pixels[destination] = packed;
                if (show_sub && packed > sub_pixels[destination])
                    sub_pixels[destination] = packed;
                if (bands != NULL) bands[destination] = band;
            }
        }
        x += run;
    }
}

static void native_resolve_virtual_bg(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels,
        uint8_t *SR_RESTRICT bands) {
    native_resolve_virtual_bg_span(
        ppu, layer, screen_y, want_sub, 0, kPpuXPixels, 0,
        main_pixels, sub_pixels, bands);
}

static bool native_virtual_bg_span_eligible(const Ppu *ppu, int layer) {
    const PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[layer];
    return PPU_mode(ppu) == 1 && layer < 2 && binding->lookup != NULL &&
        (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u &&
        !PPU_bigTiles(ppu, layer) &&
        (!PPU_mosaicEnabled(ppu, layer) || PPU_mosaicSize(ppu) == 1);
}

/* Resolve an arbitrary display-space span of a VRAM-backed tiled layer.  The
 * authentic renderer already walks tiles, but synthesized margins used to
 * fall back to sample_bg for every pixel.  Hoist policy, extent, window and
 * tile-row work to the span while retaining the exact display-to-source map
 * used by the reference renderer. */
static void native_resolve_vram_bg_span(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub, int left, int right, int origin,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels) {
    NativeLayerWindowPlan plan;
    PpuWidescreenLayerPolicy policy;
    int mode = PPU_mode(ppu);
    int bpp = bpp_for_mode(mode, layer);
    int tile_size = PPU_bigTiles(ppu, layer) ? 16 : 8;
    int sample_y = screen_y + 1;
    int world_y = sample_y + ppu->vScroll[layer];
    int map_y, in_y;
    int row = clamp_int(screen_y, 0, kPpuYPixels - 1);
    int tile_address = PPU_bgTileAdr(ppu, layer);
    int tile_words = bpp * 4;
    uint16_t extent;
    if (bpp == 0 || left >= right) return;
    if (screen_y < 0) {
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            -screen_y > ppu->verticalMarginTopRows[layer]) return;
        if (ppu->wsLayerExtentTop[layer] !=
                kPpuWidescreenExtentAvailable &&
            -screen_y > ppu->wsLayerExtentTop[layer]) return;
    } else if (screen_y >= kPpuYPixels) {
        int distance = screen_y - (kPpuYPixels - 1);
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            distance > ppu->verticalMarginBottomRows[layer]) return;
        if (ppu->wsLayerExtentBottom[layer] !=
                kPpuWidescreenExtentAvailable &&
            distance > ppu->wsLayerExtentBottom[layer]) return;
    }
    if (left < 0) {
        extent = ppu->wsLayerExtentLeft[layer][row];
        if (extent != kPpuWidescreenExtentAvailable && left < -(int)extent)
            left = -(int)extent;
    }
    if (right > kPpuXPixels) {
        extent = ppu->wsLayerExtentRight[layer][row];
        if (extent != kPpuWidescreenExtentAvailable &&
            right > kPpuXPixels + (int)extent)
            right = kPpuXPixels + (int)extent;
    }
    /* BG3 HUD rows retain their authentic width. */
    if (layer == 2 &&
        (ppu->wsBg3WidenY == 0u || row < ppu->wsBg3WidenY)) {
        if (left < 0) left = 0;
        if (right > kPpuXPixels) right = kPpuXPixels;
    }
    if (left >= right) return;
    native_layer_window_plan(ppu, layer, want_sub, &plan);
    if (plan.main_mode == 0u && plan.sub_mode == 0u) return;
    policy = PpuResolveWidescreenLayerPolicy(
        ppu, (uint8_t)layer, screen_y);
    if (world_y >= 0) {
        int shift = tile_size == 16 ? 4 : 3;
        map_y = world_y >> shift;
        in_y = world_y & (tile_size - 1);
    } else {
        map_y = floor_div8(world_y) / (tile_size / 8);
        in_y = ((world_y % tile_size) + tile_size) % tile_size;
    }
    for (int x = left; x < right;) {
        int source_x;
        int step = 1;
        int segment_right = right;
        int world_x, map_x, fine_x;
        int map_address, sample_x, sample_row, tile, tile_base;
        int run;
        uint16_t entry;
        uint64_t decoded;
        unsigned palette_base, rank;
        if (!PpuMapWidescreenLayerXWithPolicy(
                ppu, (uint8_t)layer, x, &source_x, &policy)) {
            if (x < 0) x = right < 0 ? right : 0;
            else if (x >= kPpuXPixels) break;
            else ++x;
            continue;
        }
        if (x < 0) {
            if (segment_right > 0) segment_right = 0;
            if (policy.fill == kPpuWidescreenBandFill_Mirror) step = -1;
        } else if (x < kPpuXPixels) {
            if (segment_right > kPpuXPixels)
                segment_right = kPpuXPixels;
        } else if (policy.fill == kPpuWidescreenBandFill_Mirror) {
            step = -1;
        }
        world_x = source_x + ppu->hScroll[layer];
        if (world_x >= 0) {
            int shift = tile_size == 16 ? 4 : 3;
            map_x = world_x >> shift;
            fine_x = world_x & (tile_size - 1);
        } else {
            map_x = floor_div8(world_x) / (tile_size / 8);
            fine_x = ((world_x % tile_size) + tile_size) % tile_size;
        }
        run = step > 0 ? 8 - (fine_x & 7) : (fine_x & 7) + 1;
        if (run > segment_right - x) run = segment_right - x;
        if (step < 0 &&
            policy.motion == kPpuWidescreenMotion_NormalScroll &&
            run > source_x + 1) run = source_x + 1;
        map_address = PPU_bgTilemapAdr(ppu, layer) + (map_x & 31) +
                      ((map_y & 31) << 5);
        if ((map_x & 32) != 0 && PPU_bgTilemapWider(ppu, layer))
            map_address += 0x400;
        if ((map_y & 32) != 0 && PPU_bgTilemapHigher(ppu, layer))
            map_address +=
                PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
        entry = ppu->vram[map_address & 0x7fff];
        sample_x = fine_x;
        sample_row = in_y;
        if ((entry & 0x4000u) != 0u)
            sample_x = tile_size - 1 - sample_x;
        if ((entry & 0x8000u) != 0u)
            sample_row = tile_size - 1 - sample_row;
        tile = (entry & 0x3ffu) + (sample_x >> 3) +
               ((sample_row >> 3) << 4);
        tile_base = tile_address + tile * tile_words + (sample_row & 7);
        if (bpp == 2) decoded = decoded_2bpp_row(ppu, tile_base);
        else if (bpp == 4) decoded = decoded_4bpp_row(ppu, tile_base);
        else {
            uint16_t planes[4];
            decoded = 0u;
            for (int plane = 0; plane < 4; ++plane)
                planes[plane] = ppu->vram[(tile_base + plane * 8) & 0x7fff];
            for (int tile_x = 0; tile_x < 8; ++tile_x) {
                int bit = 7 - tile_x;
                unsigned value = 0u;
                for (int plane = 0; plane < 4; ++plane) {
                    value |= ((planes[plane] >> bit) & 1u) << (plane * 2);
                    value |= ((planes[plane] >> (bit + 8)) & 1u) <<
                             (plane * 2 + 1);
                }
                decoded |= (uint64_t)value << (tile_x * 8);
            }
        }
        if (bpp == 2)
            palette_base = (mode == 0 ? (unsigned)layer * 32u : 0u) +
                           ((entry >> 10) & 7u) * 4u;
        else if (bpp == 4)
            palette_base = ((entry >> 10) & 7u) * 16u;
        else
            palette_base = 0u;
        rank = layer_rank(ppu, layer, (entry >> 13) & 1u);
        for (int offset = 0; offset < run; ++offset) {
            int source_pixel = fine_x + offset * step;
            int decoded_x;
            int destination = origin + x + offset;
            bool inside, show_main, show_sub;
            unsigned pixel;
            uint16_t packed;
            if ((entry & 0x4000u) != 0u)
                source_pixel = tile_size - 1 - source_pixel;
            decoded_x = source_pixel & 7;
            pixel = bpp == 2
                ? (unsigned)((decoded >> (decoded_x * 2)) & 3u)
                : bpp == 4
                    ? (unsigned)((decoded >> (decoded_x * 4)) & 15u)
                    : (unsigned)((decoded >> (decoded_x * 8)) & 0xffu);
            if (pixel == 0u) continue;
            inside = native_window_plan_inside(&plan, x + offset);
            show_main = plan.main_mode == 1u ||
                (plan.main_mode == 2u && !inside);
            show_sub = plan.sub_mode == 1u ||
                (plan.sub_mode == 2u && !inside);
            packed = native_pack_pixel(
                palette_base + pixel, rank, (unsigned)layer);
            if (show_main) main_pixels[destination] = packed;
            if (show_sub) sub_pixels[destination] = packed;
        }
        x += run;
    }
}

/* Mode 7's transform changes linearly across a scanline.  Advance the two
 * affine numerators instead of recomputing four multiplies for every layer,
 * screen, and pixel as the general reference sampler must. */
static void native_resolve_mode7(Ppu *SR_RESTRICT ppu, int screen_y,
                                 bool want_sub,
                                 uint16_t *SR_RESTRICT main_pixels,
                                 uint16_t *SR_RESTRICT sub_pixels) {
    NativeLayerWindowPlan plans[2];
    uint32_t source_x, source_y;
    int x_step, y_step;
    unsigned rank0 = layer_rank(ppu, 0, 0);
    unsigned rank1[2];
    bool extbg = PPU_m7extBg(ppu);
    bool layer0_visible, layer1_visible;
    mode7_line_transform(ppu, screen_y + 1, &source_x, &source_y,
                         &x_step, &y_step);
    native_layer_window_plan(ppu, 0, want_sub, &plans[0]);
    layer0_visible = plans[0].main_mode != 0u || plans[0].sub_mode != 0u;
    layer1_visible = false;
    if (extbg) {
        native_layer_window_plan(ppu, 1, want_sub, &plans[1]);
        layer1_visible =
            plans[1].main_mode != 0u || plans[1].sub_mode != 0u;
        rank1[0] = layer_rank(ppu, 1, 0);
        rank1[1] = layer_rank(ppu, 1, 1);
    }
    if (!layer0_visible && !layer1_visible) return;
    for (int x = 0; x < kPpuXPixels;
         ++x, source_x += (uint32_t)x_step,
         source_y += (uint32_t)y_step) {
        int tile, pixel;
        bool inside;
        uint16_t packed;
        if (PPU_m7largeField(ppu) &&
            (source_x | source_y) > UINT32_C(0x3ffff)) {
            if (!PPU_m7charFill(ppu)) continue;
            tile = 0;
        } else {
            tile = ppu->vram[
                (((source_y >> 11) & 0x7fu) * 128u +
                 ((source_x >> 11) & 0x7fu)) & 0x7fffu] & 0xff;
        }
        pixel = ppu->vram[
            (tile * 64 + ((source_y >> 8) & 7u) * 8u +
             ((source_x >> 8) & 7u)) & 0x7fffu] >> 8;
        if (pixel == 0) continue;
        if (layer0_visible) {
            inside = native_window_plan_inside(&plans[0], x);
            packed = native_pack_pixel((unsigned)pixel, rank0, 0u);
            if ((plans[0].main_mode == 1u ||
                 (plans[0].main_mode == 2u && !inside)) &&
                packed > main_pixels[x])
                main_pixels[x] = packed;
            if ((plans[0].sub_mode == 1u ||
                 (plans[0].sub_mode == 2u && !inside)) &&
                packed > sub_pixels[x])
                sub_pixels[x] = packed;
        }
        if (layer1_visible) {
            inside = native_window_plan_inside(&plans[1], x);
            packed = native_pack_pixel((unsigned)pixel,
                rank1[(unsigned)pixel >> 7], 1u);
            if ((plans[1].main_mode == 1u ||
                 (plans[1].main_mode == 2u && !inside)) &&
                packed > main_pixels[x])
                main_pixels[x] = packed;
            if ((plans[1].sub_mode == 1u ||
                 (plans[1].sub_mode == 2u && !inside)) &&
                packed > sub_pixels[x])
                sub_pixels[x] = packed;
        }
    }
}

/* Capture-aware Mode 7 needs each affine source in its own packed plane rather
 * than merged directly into the final winner.  Resolve a complete synthetic
 * span with one transform setup, advancing the affine coordinates linearly
 * between policy boundaries. */
static void native_resolve_mode7_layer_span(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub, int left, int right, int origin,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels) {
    NativeLayerWindowPlan plan;
    PpuWidescreenLayerPolicy policy;
    uint32_t start_x, start_y;
    int affine_step_x, affine_step_y;
    int row = clamp_int(screen_y, 0, kPpuYPixels - 1);
    unsigned ranks[2];
    bool large_field = PPU_m7largeField(ppu) != 0;
    bool char_fill = PPU_m7charFill(ppu) != 0;
    uint16_t extent;
    if (left >= right || (layer == 1 && !PPU_m7extBg(ppu))) return;
    if (screen_y < 0) {
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            -screen_y > ppu->verticalMarginTopRows[layer]) return;
        if (ppu->wsLayerExtentTop[layer] !=
                kPpuWidescreenExtentAvailable &&
            -screen_y > ppu->wsLayerExtentTop[layer]) return;
    } else if (screen_y >= kPpuYPixels) {
        int distance = screen_y - (kPpuYPixels - 1);
        if ((ppu->verticalMarginLayerClip & (1u << layer)) != 0u &&
            distance > ppu->verticalMarginBottomRows[layer]) return;
        if (ppu->wsLayerExtentBottom[layer] !=
                kPpuWidescreenExtentAvailable &&
            distance > ppu->wsLayerExtentBottom[layer]) return;
    }
    if (left < 0) {
        extent = ppu->wsLayerExtentLeft[layer][row];
        if (extent != kPpuWidescreenExtentAvailable && left < -(int)extent)
            left = -(int)extent;
    }
    if (right > kPpuXPixels) {
        extent = ppu->wsLayerExtentRight[layer][row];
        if (extent != kPpuWidescreenExtentAvailable &&
            right > kPpuXPixels + (int)extent)
            right = kPpuXPixels + (int)extent;
    }
    if (left >= right) return;
    native_layer_window_plan(ppu, layer, want_sub, &plan);
    if (plan.main_mode == 0u && plan.sub_mode == 0u) return;
    policy = PpuResolveWidescreenLayerPolicy(
        ppu, (uint8_t)layer, screen_y);
    ranks[0] = layer_rank(ppu, layer, 0u);
    ranks[1] = layer_rank(ppu, layer, 1u);
    mode7_line_transform(ppu, screen_y + 1, &start_x, &start_y,
                         &affine_step_x, &affine_step_y);
    for (int x = left; x < right;) {
        int source_x;
        int source_step = 1;
        int segment_right = right;
        int run;
        int delta_x, delta_y;
        uint32_t sample_x, sample_y;
        if (!PpuMapWidescreenLayerXWithPolicy(
                ppu, (uint8_t)layer, x, &source_x, &policy)) {
            if (x < 0) x = right < 0 ? right : 0;
            else if (x >= kPpuXPixels) break;
            else ++x;
            continue;
        }
        if (x < 0) {
            if (segment_right > 0) segment_right = 0;
            if (policy.fill == kPpuWidescreenBandFill_Mirror)
                source_step = -1;
        } else if (x < kPpuXPixels) {
            if (segment_right > kPpuXPixels)
                segment_right = kPpuXPixels;
        } else if (policy.fill == kPpuWidescreenBandFill_Mirror) {
            source_step = -1;
        }
        run = segment_right - x;
        if (source_step < 0 &&
            policy.motion == kPpuWidescreenMotion_NormalScroll &&
            run > source_x + 1) run = source_x + 1;
        sample_x = start_x + (uint32_t)(affine_step_x * source_x);
        sample_y = start_y + (uint32_t)(affine_step_y * source_x);
        delta_x = affine_step_x * source_step;
        delta_y = affine_step_y * source_step;
        for (int offset = 0; offset < run; ++offset,
                 sample_x += (uint32_t)delta_x,
                 sample_y += (uint32_t)delta_y) {
            int tile, pixel;
            bool inside, show_main, show_sub;
            uint16_t packed;
            if (large_field &&
                (sample_x | sample_y) > UINT32_C(0x3ffff)) {
                if (!char_fill) continue;
                tile = 0;
            } else {
                tile = ppu->vram[
                    (((sample_y >> 11) & 0x7fu) * 128u +
                     ((sample_x >> 11) & 0x7fu)) & 0x7fffu] & 0xff;
            }
            pixel = ppu->vram[
                (tile * 64 + ((sample_y >> 8) & 7u) * 8u +
                 ((sample_x >> 8) & 7u)) & 0x7fffu] >> 8;
            if (pixel == 0) continue;
            inside = native_window_plan_inside(&plan, x + offset);
            show_main = plan.main_mode == 1u ||
                (plan.main_mode == 2u && !inside);
            show_sub = plan.sub_mode == 1u ||
                (plan.sub_mode == 2u && !inside);
            packed = native_pack_pixel(
                (unsigned)pixel,
                ranks[layer == 1 ? (unsigned)pixel >> 7 : 0u],
                (unsigned)layer);
            if (show_main && packed > main_pixels[origin + x + offset])
                main_pixels[origin + x + offset] = packed;
            if (show_sub && packed > sub_pixels[origin + x + offset])
                sub_pixels[origin + x + offset] = packed;
        }
        x += run;
    }
}

/* Mosaic is a fetch-coordinate effect on one BG, not a scanline rendering
 * mode.  Sample each display-anchored mosaic group once, then apply that
 * source to the layer's main/sub winner buffers while evaluating windows at
 * the destination coordinate.  Keeping this as a per-layer kernel lets OBJ,
 * other BGs, color math, and capture stay on their packed native paths.
 *
 * The general sampler remains the source of truth for this first kernel.  It
 * is invoked at most once per group (2-16 pixels) rather than once per source,
 * screen, and destination pixel; a future decoded-row sampler can replace the
 * single fetch here without changing the raster contract. */
static void native_resolve_bg_mosaic(Ppu *SR_RESTRICT ppu, int layer,
        int screen_y, bool want_sub,
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels,
        uint8_t *SR_RESTRICT bands) {
    NativeLayerWindowPlan plan;
    int size = PPU_mosaicSize(ppu);
    native_layer_window_plan(ppu, layer, want_sub, &plan);
    if (plan.main_mode == 0u && plan.sub_mode == 0u) return;
    for (int x = 0; x < kPpuXPixels; x += size) {
        SrPpuPixel pixel = {0};
        int right = x + size;
        uint16_t packed;
        if (right > kPpuXPixels) right = kPpuXPixels;
        if (!sample_bg(ppu, layer, x, screen_y, false, &pixel)) continue;
        packed = native_pack_pixel(pixel.palette, pixel.rank, pixel.layer);
        for (int destination = x; destination < right; ++destination) {
            bool inside = native_window_plan_inside(&plan, destination);
            bool show_main = plan.main_mode == 1u ||
                (plan.main_mode == 2u && !inside);
            bool show_sub = plan.sub_mode == 1u ||
                (plan.sub_mode == 2u && !inside);
            if (show_main && packed > main_pixels[destination])
                main_pixels[destination] = packed;
            if (show_sub && packed > sub_pixels[destination])
                sub_pixels[destination] = packed;
            if (bands != NULL) bands[destination] = pixel.band;
        }
    }
}

static void native_resolve_bg(Ppu *SR_RESTRICT ppu, int layer, int screen_y,
                              bool want_sub,
                              uint16_t *SR_RESTRICT main_pixels,
                              uint16_t *SR_RESTRICT sub_pixels,
                              uint8_t *SR_RESTRICT bands) {
    unsigned layer_bit = 1u << layer;
    bool main_enabled = (ppu->screenEnabled[0] & layer_bit) != 0u;
    bool sub_enabled = want_sub &&
        (ppu->screenEnabled[1] & layer_bit) != 0u;
    bool main_windowed = (ppu->screenWindowed[0] & layer_bit) != 0u;
    bool sub_windowed = (ppu->screenWindowed[1] & layer_bit) != 0u;
    int uniform_window = main_windowed || sub_windowed
        ? native_window_uniform(ppu, layer) : 0;
    bool main_always = main_enabled &&
        (!main_windowed || uniform_window == 0);
    bool sub_always = sub_enabled &&
        (!sub_windowed || uniform_window == 0);
    bool main_variable = main_enabled && main_windowed && uniform_window < 0;
    bool sub_variable = sub_enabled && sub_windowed && uniform_window < 0;
    NativeWindowRuns window_runs;
    int window_run = 0;
    int mode = PPU_mode(ppu);
    int bpp = bpp_for_mode(mode, layer);
    unsigned pixel_mask = (1u << bpp) - 1u;
    int tile_size = PPU_bigTiles(ppu, layer) ? 16 : 8;
    int tile_shift = tile_size == 16 ? 4 : 3;
    int world_y = screen_y + 1 + ppu->vScroll[layer];
    int map_y = world_y >> tile_shift;
    int in_y = world_y & (tile_size - 1);
    int h_scroll = ppu->hScroll[layer];
    int map_row_address = PPU_bgTilemapAdr(ppu, layer) +
                          ((map_y & 31) << 5);
    int tile_address = PPU_bgTileAdr(ppu, layer);
    int tile_words = bpp * 4;
    int x = 0;
    if (PPU_mosaicEnabled(ppu, layer) && PPU_mosaicSize(ppu) > 1) {
        native_resolve_bg_mosaic(ppu, layer, screen_y, want_sub,
                                 main_pixels, sub_pixels, bands);
        return;
    }
    if (mode == 1 && layer < 2 &&
        ppu->virtualTilemap[layer].lookup != NULL &&
        (ppu->virtualTilemap[layer].flags &
         kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u) {
        native_resolve_virtual_bg(ppu, layer, screen_y, want_sub,
                                  main_pixels, sub_pixels, bands);
        return;
    }
    if ((map_y & 32) != 0 && PPU_bgTilemapHigher(ppu, layer))
        map_row_address += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
    if ((!main_always && !main_variable) &&
        (!sub_always && !sub_variable)) return;
    if (main_variable || sub_variable)
        native_window_runs(ppu, layer, &window_runs);
    while (x < kPpuXPixels) {
        int world_x = x + h_scroll;
        int map_x = world_x >> tile_shift;
        int in_x = world_x & (tile_size - 1);
        int map_address = map_row_address + (map_x & 31);
        int sample_y = in_y;
        int run = 8 - (world_x & 7);
        uint16_t entry;
        uint64_t decoded_pixels = 0u;
        unsigned palette_base, rank;
        int tile, tile_base;
        if ((map_x & 32) != 0 && PPU_bgTilemapWider(ppu, layer))
            map_address += 0x400;
        entry = ppu->vram[map_address & 0x7fff];
        if ((entry & 0x8000u) != 0u) sample_y = tile_size - 1 - sample_y;
        {
            int sample_x = (entry & 0x4000u) != 0u
                ? tile_size - 1 - in_x : in_x;
            tile = (entry & 0x3ffu) + (sample_x >> 3) +
                   ((sample_y >> 3) << 4);
        }
        tile_base = tile_address + tile * tile_words;
        if (bpp == 2) {
            decoded_pixels = decoded_2bpp_row(
                ppu, tile_base + (sample_y & 7));
        } else if (bpp == 4) {
            decoded_pixels = decoded_4bpp_row(
                ppu, tile_base + (sample_y & 7));
        } else {
            uint16_t planes[4];
            int row_address = tile_base + (sample_y & 7);
            for (int plane = 0; plane < 4; ++plane)
                planes[plane] = ppu->vram[
                    (row_address + plane * 8) & 0x7fff];
            for (int tile_x = 0; tile_x < 8; ++tile_x) {
                int bit = 7 - tile_x;
                unsigned value = 0u;
                for (int plane = 0; plane < 4; ++plane) {
                    value |= ((planes[plane] >> bit) & 1u) << (plane * 2);
                    value |= ((planes[plane] >> (bit + 8)) & 1u) <<
                             (plane * 2 + 1);
                }
                decoded_pixels |= (uint64_t)value << (tile_x * 8);
            }
        }
        if (bpp == 2)
            palette_base = (mode == 0 ? (unsigned)layer * 32u : 0u) +
                           ((entry >> 10) & 7u) * 4u;
        else if (bpp == 4)
            palette_base = ((entry >> 10) & 7u) * 16u;
        else
            palette_base = 0u;
        rank = layer_rank(ppu, layer, (entry >> 13) & 1u);
        if (run > kPpuXPixels - x) run = kPpuXPixels - x;
        /* Most ordinary scanlines do not request a subscreen.  Keep that
         * invariant outside the eight-pixel group so its fixed-shift stores
         * do not retest main/sub visibility for every decoded pixel. */
        if (run == 8 && main_always && !sub_always &&
            !main_variable && !sub_variable) {
            uint16_t packed_base = native_pack_pixel(
                palette_base, rank, (unsigned)layer);
#if SR_PPU_TILE_SIMD
            native_apply_tile_simd(main_pixels + x, packed_base,
                                   decoded_pixels, bpp,
                                   (entry & 0x4000u) != 0u);
#else
#define APPLY_NATIVE_MAIN_PIXEL(offset_, value_) do {                     \
                unsigned value = (unsigned)(value_);                       \
                uint16_t packed = (uint16_t)(packed_base + value);          \
                uint16_t current = main_pixels[x + (offset_)];              \
                main_pixels[x + (offset_)] = value != 0u && packed > current\
                    ? packed : current;                                    \
            } while (0)
            if (bpp == 4) {
                uint32_t pixels = (uint32_t)decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_MAIN_PIXEL(0, (pixels >> 28) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 24) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 20) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 16) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 12) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 8) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 4) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels & 15u);
                } else {
                    APPLY_NATIVE_MAIN_PIXEL(0, pixels & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 4) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 8) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 12) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 16) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 20) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 24) & 15u);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels >> 28);
                }
            } else if (bpp == 2) {
                uint16_t pixels = (uint16_t)decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_MAIN_PIXEL(0, (pixels >> 14) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 12) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 10) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 8) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 6) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 4) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 2) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels & 3u);
                } else {
                    APPLY_NATIVE_MAIN_PIXEL(0, pixels & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 2) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 4) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 6) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 8) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 10) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 12) & 3u);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels >> 14);
                }
            } else {
                uint64_t pixels = decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_MAIN_PIXEL(0, (pixels >> 56) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 48) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 40) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 32) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 24) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 16) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 8) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels & 0xffu);
                } else {
                    APPLY_NATIVE_MAIN_PIXEL(0, pixels & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(1, (pixels >> 8) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(2, (pixels >> 16) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(3, (pixels >> 24) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(4, (pixels >> 32) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(5, (pixels >> 40) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(6, (pixels >> 48) & 0xffu);
                    APPLY_NATIVE_MAIN_PIXEL(7, pixels >> 56);
                }
            }
#undef APPLY_NATIVE_MAIN_PIXEL
#endif
            x += 8;
            continue;
        }
#if SR_PPU_TILE_SIMD
        /* The main-only case above remains separate because it dominates
         * ordinary scanout. Extend the same exact winner operation to modes
         * that expose a subscreen without adding branches to that hot case. */
        if (run == 8 && !main_variable && !sub_variable) {
            uint16_t packed_base = native_pack_pixel(
                palette_base, rank, (unsigned)layer);
            if (main_always)
                native_apply_tile_simd(main_pixels + x, packed_base,
                                       decoded_pixels, bpp,
                                       (entry & 0x4000u) != 0u);
            if (sub_always)
                native_apply_tile_simd(sub_pixels + x, packed_base,
                                       decoded_pixels, bpp,
                                       (entry & 0x4000u) != 0u);
            x += 8;
            continue;
        }
#endif
        /* After the one scroll-clipped tile at the left edge, ordinary tiled
         * scanlines consist almost entirely of complete 8-pixel runs.  Spell
         * those fixed shifts out so every compiler can fold the source-index
         * arithmetic; the variable-span loop remains the window/clipping
         * fallback. */
        if (run == 8 && !main_variable && !sub_variable) {
            uint16_t packed_base = native_pack_pixel(
                palette_base, rank, (unsigned)layer);
#define APPLY_NATIVE_BG_PIXEL(offset_, value_) do {                         \
                unsigned value = (unsigned)(value_);                        \
                if (value != 0u) {                                          \
                    uint16_t packed = (uint16_t)(packed_base + value);       \
                    if (main_always && packed > main_pixels[x + (offset_)])  \
                        main_pixels[x + (offset_)] = packed;                 \
                    if (sub_always && packed > sub_pixels[x + (offset_)])    \
                        sub_pixels[x + (offset_)] = packed;                  \
                }                                                           \
            } while (0)
            if (bpp == 4) {
                uint32_t pixels = (uint32_t)decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_BG_PIXEL(0, (pixels >> 28) & 15u);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 24) & 15u);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 20) & 15u);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 16) & 15u);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 12) & 15u);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 8) & 15u);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 4) & 15u);
                    APPLY_NATIVE_BG_PIXEL(7, pixels & 15u);
                } else {
                    APPLY_NATIVE_BG_PIXEL(0, pixels & 15u);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 4) & 15u);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 8) & 15u);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 12) & 15u);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 16) & 15u);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 20) & 15u);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 24) & 15u);
                    APPLY_NATIVE_BG_PIXEL(7, pixels >> 28);
                }
            } else if (bpp == 2) {
                uint16_t pixels = (uint16_t)decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_BG_PIXEL(0, (pixels >> 14) & 3u);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 12) & 3u);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 10) & 3u);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 8) & 3u);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 6) & 3u);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 4) & 3u);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 2) & 3u);
                    APPLY_NATIVE_BG_PIXEL(7, pixels & 3u);
                } else {
                    APPLY_NATIVE_BG_PIXEL(0, pixels & 3u);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 2) & 3u);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 4) & 3u);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 6) & 3u);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 8) & 3u);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 10) & 3u);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 12) & 3u);
                    APPLY_NATIVE_BG_PIXEL(7, pixels >> 14);
                }
            } else {
                uint64_t pixels = decoded_pixels;
                if ((entry & 0x4000u) != 0u) {
                    APPLY_NATIVE_BG_PIXEL(0, (pixels >> 56) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 48) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 40) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 32) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 24) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 16) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 8) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(7, pixels & 0xffu);
                } else {
                    APPLY_NATIVE_BG_PIXEL(0, pixels & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(1, (pixels >> 8) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(2, (pixels >> 16) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(3, (pixels >> 24) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(4, (pixels >> 32) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(5, (pixels >> 40) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(6, (pixels >> 48) & 0xffu);
                    APPLY_NATIVE_BG_PIXEL(7, pixels >> 56);
                }
            }
#undef APPLY_NATIVE_BG_PIXEL
            x += 8;
            continue;
        }
        for (int offset = 0; offset < run;) {
            bool inside = false;
            int span_end = run;
            if (main_variable || sub_variable) {
                int screen_x = x + offset;
                while (window_run + 1 < window_runs.count &&
                       screen_x >= window_runs.edges[window_run + 1])
                    ++window_run;
                inside = window_runs.inside[window_run] != 0u;
                if (span_end > window_runs.edges[window_run + 1] - x)
                    span_end = window_runs.edges[window_run + 1] - x;
            }
            {
                bool show_main = main_always || (main_variable && !inside);
                bool show_sub = sub_always || (sub_variable && !inside);
                if (!show_main && !show_sub) {
                    offset = span_end;
                    continue;
                }
                for (; offset < span_end; ++offset) {
                    int source_x = (world_x + offset) & (tile_size - 1);
                    unsigned pixel, palette;
                    uint16_t packed;
                    if ((entry & 0x4000u) != 0u)
                        source_x = tile_size - 1 - source_x;
                    pixel = (unsigned)(decoded_pixels >>
                            ((source_x & 7) * bpp)) & pixel_mask;
                    if (pixel == 0u) continue;
                    palette = palette_base + pixel;
                    packed = native_pack_pixel(
                        palette, rank, (unsigned)layer);
                    if (show_main && packed > main_pixels[x + offset])
                        main_pixels[x + offset] = packed;
                    if (show_sub && packed > sub_pixels[x + offset])
                        sub_pixels[x + offset] = packed;
                }
            }
        }
        x += run;
    }
}

static void native_resolve_obj(Ppu *SR_RESTRICT ppu, int screen_y,
                               int obj_offset, bool want_sub,
                               uint16_t *SR_RESTRICT main_pixels,
                               uint16_t *SR_RESTRICT sub_pixels) {
    const unsigned obj_bit = 1u << kPpuOverlaySource_Obj;
    bool main_enabled = (ppu->screenEnabled[0] & obj_bit) != 0u;
    bool sub_enabled = want_sub &&
        (ppu->screenEnabled[1] & obj_bit) != 0u;
    bool main_windowed = (ppu->screenWindowed[0] & obj_bit) != 0u;
    bool sub_windowed = (ppu->screenWindowed[1] & obj_bit) != 0u;
    int uniform_window = main_windowed || sub_windowed
        ? native_window_uniform(ppu, kPpuOverlaySource_Obj) : 0;
    bool main_always = main_enabled &&
        (!main_windowed || uniform_window == 0);
    bool sub_always = sub_enabled &&
        (!sub_windowed || uniform_window == 0);
    bool main_variable = main_enabled && main_windowed && uniform_window < 0;
    bool sub_variable = sub_enabled && sub_windowed && uniform_window < 0;
    NativeWindowRuns window_runs;
    PpuObjSampleCache *cache;
    if ((!main_always && !main_variable) &&
        (!sub_always && !sub_variable)) return;
    cache = get_obj_sample_cache(ppu, screen_y, obj_offset, 0, 0, 0, 0);
    if (cache == NULL) return;
    if (main_variable || sub_variable)
        native_window_runs(ppu, kPpuOverlaySource_Obj, &window_runs);
    for (int run = 0;
         run < (main_variable || sub_variable ? window_runs.count : 1);
         ++run) {
        bool inside = (main_variable || sub_variable) &&
            window_runs.inside[run] != 0u;
        bool show_main = main_always || (main_variable && !inside);
        bool show_sub = sub_always || (sub_variable && !inside);
        int left = main_variable || sub_variable ? window_runs.edges[run] : 0;
        int right = main_variable || sub_variable
            ? window_runs.edges[run + 1] : kPpuXPixels;
        if (!show_main && !show_sub) continue;
        for (unsigned word = (unsigned)left / kPpuBitWordBits;
             word <= ((unsigned)right - 1u) / kPpuBitWordBits; ++word) {
            unsigned word_x = word * kPpuBitWordBits;
            PpuBitWord opaque = cache->opaque[word];
            if ((unsigned)left > word_x)
                opaque &= (PpuBitWord)~(PpuBitWord)0u <<
                    ((unsigned)left - word_x);
            if ((unsigned)right < word_x + kPpuBitWordBits)
                opaque &= ((PpuBitWord)1u <<
                    ((unsigned)right - word_x)) - 1u;
            while (opaque != 0u) {
                int x = (int)(word_x + lowest_set_bit_index(opaque));
                PpuZbufType encoded =
                    cache->pixels.data[x + kPpuExtraLeftRight];
                unsigned palette = encoded & 0xffu;
                unsigned rank = encoded >> 8;
                uint16_t packed = native_pack_pixel(
                    palette, rank, kPpuOverlaySource_Obj);
                if (show_main && packed > main_pixels[x])
                    main_pixels[x] = packed;
                if (show_sub && packed > sub_pixels[x])
                    sub_pixels[x] = packed;
                opaque &= opaque - 1u;
            }
        }
    }
}

static uint32_t native_final_rgb(Ppu *ppu, uint16_t main, uint16_t sub,
                                 bool clipped, unsigned math_enabled,
                                 bool add_subscreen, bool subtract, bool half) {
    unsigned main_palette = main & 0xffu;
    unsigned main_layer = native_pixel_layer(main);
    uint16_t color = ppu->cgram[main_palette];
    bool eligible = main_layer < 5u &&
        (math_enabled & (1u << main_layer)) != 0u;
    if (main_layer == kPpuOverlaySource_Obj && main_palette < 0xc0u)
        eligible = false;
    if (clipped) color = 0u;
    if (!eligible) return color_rgb(ppu, color);
    {
        unsigned sub_layer = native_pixel_layer(sub);
        bool use_sub = add_subscreen && sub_layer != 5u;
        uint16_t second = use_sub
            ? ppu->cgram[sub & 0xffu] : ppu->fixedColor;
        int r = color & 31;
        int g = (color >> 5) & 31;
        int b = (color >> 10) & 31;
        int r2 = second & 31;
        int g2 = (second >> 5) & 31;
        int b2 = (second >> 10) & 31;
        const uint8_t *map = half && (use_sub || !add_subscreen)
            ? ppu->brightnessMultHalf : ppu->brightnessMult;
        if (subtract) {
            r -= r2; g -= g2; b -= b2;
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
        } else {
            r += r2; g += g2; b += b2;
        }
        return ((uint32_t)map[r] << 16) |
               ((uint32_t)map[g] << 8) | map[b];
    }
}

static uint16_t native_obj_cache_pixel(const PpuObjSampleCache *cache, int x) {
    PpuZbufType encoded;
    unsigned palette, rank;
    if (cache == NULL || x < -kPpuExtraLeftRight ||
        x >= kPpuXPixels + kPpuExtraLeftRight) return 0u;
    encoded = cache->pixels.data[x + kPpuExtraLeftRight];
    palette = encoded & 0xffu;
    if (palette == 0u) return 0u;
    rank = encoded >> 8;
    return native_pack_pixel(palette, rank, kPpuOverlaySource_Obj);
}

static void native_merge_packed_span(
        uint16_t *SR_RESTRICT main_pixels,
        uint16_t *SR_RESTRICT sub_pixels,
        const uint16_t *SR_RESTRICT source_main,
        const uint16_t *SR_RESTRICT source_sub, int count,
        bool merge_sub) {
    if (!merge_sub) {
        for (int offset = 0; offset < count; ++offset)
            if (source_main[offset] > main_pixels[offset])
                main_pixels[offset] = source_main[offset];
        return;
    }
    for (int offset = 0; offset < count; ++offset) {
        if (source_main[offset] > main_pixels[offset])
            main_pixels[offset] = source_main[offset];
        if (source_sub[offset] > sub_pixels[offset])
            sub_pixels[offset] = source_sub[offset];
    }
}

typedef struct NativeOverlayLinePlan {
    PpuOverlayCapture *capture;
    uint32_t *primary;
    uint32_t *bands[3];
    uint8_t priority_for_rank[16];
    int origin;
} NativeOverlayLinePlan;

static void native_overlay_line_plan(Ppu *ppu, int source, int screen_y,
                                     NativeOverlayLinePlan *plan) {
    PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    int row;
    memset(plan, 0, sizeof(*plan));
    plan->capture = capture;
    for (uint8_t priority = 0u; priority < 4u; ++priority)
        plan->priority_for_rank[
            layer_rank(ppu, source, priority) & 15u] = priority;
    if (ppu->overlayRenderBuffer[source] == NULL ||
        screen_y < capture->y0 || screen_y >= capture->y1) return;
    row = overlay_row(capture, screen_y);
    if (row < 0 || row >= kPpuBufHeight) return;
    plan->origin = surface_origin_x(
        ppu, ppu->overlayRenderPitch[source]);
    plan->primary = (uint32_t *)(ppu->overlayRenderBuffer[source] +
        (size_t)row * ppu->overlayRenderPitch[source]);
    for (int band = 0; band < 3; ++band) {
        if (ppu->overlayRenderBands[source][band] != NULL)
            plan->bands[band] = (uint32_t *)(
                ppu->overlayRenderBands[source][band] +
                (size_t)row * ppu->overlayRenderPitch[source]);
    }
}

static void native_write_overlay_packed(
        Ppu *ppu, int source, int x, uint16_t packed,
        uint8_t semantic_band, NativeOverlayLinePlan *plan) {
    unsigned palette = packed & 0xffu;
    unsigned rank = native_pixel_rank(packed);
    unsigned priority = plan->priority_for_rank[rank & 15u];
    int band;
    uint32_t *destination;
    uint16_t color;
    uint32_t argb;
    if (plan->primary == NULL) return;
    if (source == kPpuOverlaySource_Obj) {
        band = (int)priority;
    } else if (semantic_band != 0xffu && source < 2) {
        band = semantic_band == 1u ? 0 : semantic_band == 2u ? 1 : 2;
    } else if (semantic_band != 0xffu) {
        band = semantic_band;
    } else {
        band = (int)priority;
    }
    destination = band > 0 && band <= 3 && plan->bands[band - 1] != NULL
        ? plan->bands[band - 1] : plan->primary;
    if (destination == plan->primary) band = 0;
    color = ppu->cgram[palette];
    if ((plan->capture->flags &
         kPpuOverlayFlag_ApplyBgFixedColorSubtract) != 0u &&
        source < kPpuOverlaySource_Obj) {
        color = color_math(color, ppu->fixedColor, true, false);
        argb = color_argb(ppu, color);
    } else {
        argb = 0xff000000u | ppu->cgramRgb[palette];
    }
    if ((plan->capture->flags &
         kPpuOverlayFlag_MarkObjColorMath) != 0u &&
        source == kPpuOverlaySource_Obj &&
        ((palette - 0x80u) >> 4) >= 4u)
        argb = (argb & 0x00ffffffu) | 0x80000000u;
    if ((plan->capture->flags &
         kPpuOverlayFlag_MarkBgHalfAdd) != 0u &&
        source < kPpuOverlaySource_Obj)
        argb = (argb & 0x00ffffffu) | 0x80000000u;
    destination[plan->origin + x] = argb;
    ppu->overlayRenderContentMask[source] |= (uint8_t)(1u << band);
}

static void native_write_obj_range_capture(Ppu *ppu, int screen_y,
                                           int obj_offset) {
    PpuObjRangeCapture *capture = &ppu->objRangeCapture;
    PpuObjSampleCache *cache;
    uint32_t *row;
    int origin, left, right;
    if (capture->count == 0u || capture->pixels == NULL ||
        screen_y < capture->y0 || screen_y >= capture->y1 ||
        capture->x1 <= 0 || capture->x0 >= kPpuXPixels) return;
    cache = get_obj_sample_cache(
        ppu, screen_y, obj_offset,
        capture->first, capture->count, 0, 0);
    if (cache == NULL) return;
    row = (uint32_t *)(capture->pixels +
        (size_t)screen_y * capture->pitch);
    origin = surface_origin_x(ppu, capture->pitch);
    left = capture->x0 < 0 ? 0 : capture->x0;
    right = capture->x1 > kPpuXPixels ? kPpuXPixels : capture->x1;
    for (int x = left; x < right; ++x) {
        uint16_t packed = native_obj_cache_pixel(cache, x);
        if (packed != 0u)
            row[origin + x] = color_argb(ppu, ppu->cgram[packed & 0xffu]);
    }
}

/* Widescreen Mode 7 commonly has no extraction policy at all.  Resolve that
 * case directly into shared main/sub winners instead of allocating ten source
 * planes and merging five candidates for every output pixel. */
static bool render_native_mode7_wide_line(Ppu *ppu, int screen_y,
        uint32_t *row, int origin, bool dual_authentic,
        uint32_t *authentic_row, int authentic_origin) {
    uint16_t *main_pixels = ppu->nativeLineScratch.mainPixels;
    uint16_t *sub_pixels = ppu->nativeLineScratch.subPixels;
    uint16_t backdrop = native_pack_pixel(0u, 1u, 5u);
    bool want_sub = ppu->screenEnabled[1] != 0u ||
        PPU_addSubscreen(ppu) || PPU_pseudoHires(ppu);
    bool authentic_y = screen_y >= 0 && screen_y < kPpuYPixels;
    int left = -ppu->extraLeftCur;
    int right = kPpuXPixels + ppu->extraRightCur;
    if (!ppu->cgramRgbValid) rebuild_cgram_rgb(ppu);
    for (int x = left; x < right; ++x) {
        int index = x + kPpuExtraLeftRight;
        main_pixels[index] = backdrop;
        if (want_sub) sub_pixels[index] = backdrop;
    }
    native_resolve_mode7_layer_span(
        ppu, 0, screen_y, want_sub, left, right,
        kPpuExtraLeftRight, main_pixels, sub_pixels);
    if (PPU_m7extBg(ppu))
        native_resolve_mode7_layer_span(
            ppu, 1, screen_y, want_sub, left, right,
            kPpuExtraLeftRight, main_pixels, sub_pixels);
    if (authentic_y)
        native_resolve_obj(
            ppu, screen_y, 0, want_sub,
            main_pixels + kPpuExtraLeftRight,
            sub_pixels + kPpuExtraLeftRight);
    if (left < 0 || right > kPpuXPixels) {
        PpuObjSampleCache *obj_cache = get_obj_sample_cache(
            ppu, screen_y, 0, 0, 0, 0, 0);
        for (int side = 0; side < 2; ++side) {
            int span_left = side == 0 ? left : kPpuXPixels;
            int span_right = side == 0 ? 0 : right;
            for (int x = span_left; x < span_right; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t packed = native_obj_cache_pixel(obj_cache, x);
                if (packed != 0u &&
                    source_visible_on_screen(
                        ppu, kPpuOverlaySource_Obj, false, x) &&
                    packed > main_pixels[index])
                    main_pixels[index] = packed;
                if (packed != 0u && want_sub &&
                    source_visible_on_screen(
                        ppu, kPpuOverlaySource_Obj, true, x) &&
                    packed > sub_pixels[index])
                    sub_pixels[index] = packed;
            }
        }
    }
    {
        NativeWindowRuns color_runs;
        unsigned clip = PPU_clipMode(ppu);
        unsigned prevent = PPU_preventMathMode(ppu);
        unsigned math_enabled = PPU_mathEnabled(ppu);
        bool add_subscreen = PPU_addSubscreen(ppu);
        bool subtract = PPU_subtractColor(ppu);
        bool half = PPU_halfColor(ppu);
        native_window_runs(ppu, 5, &color_runs);
        for (int run = 0; run < color_runs.count; ++run) {
            bool inside = color_runs.inside[run] != 0u;
            bool clipped = clip == 3u || (clip == 2u && inside) ||
                           (clip == 1u && !inside);
            bool prevented = prevent == 3u || (prevent == 2u && inside) ||
                             (prevent == 1u && !inside);
            bool simple_color = prevented || math_enabled == 0u ||
                (!add_subscreen && ppu->fixedColor == 0u && !half);
            for (int x = color_runs.edges[run];
                 x < color_runs.edges[run + 1]; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t main = main_pixels[index];
                uint16_t sub = add_subscreen ? sub_pixels[index] : backdrop;
                uint32_t color = simple_color
                    ? (clipped ? 0u : ppu->cgramRgb[main & 0xffu])
                    : native_final_rgb(
                        ppu, main, sub, clipped, math_enabled,
                        add_subscreen, subtract, half);
                unsigned palette = main & 0xffu;
                if (native_pixel_layer(main) == kPpuOverlaySource_Obj)
                    palette = (palette - 0x80u) >> 4;
                row[origin + x] = color;
                ppu->bgBuffers[0].data[index] = (PpuZbufType)(
                    (native_pixel_rank(main) << 8) | palette);
                if (dual_authentic)
                    authentic_row[authentic_origin + x] = color;
            }
        }
        for (int side = 0; side < 2; ++side) {
            int span_left = side == 0 ? left : kPpuXPixels;
            int span_right = side == 0 ? 0 : right;
            bool clipped = clip == 3u || clip == 1u;
            bool prevented = prevent == 3u || prevent == 1u;
            bool simple_color = prevented || math_enabled == 0u ||
                (!add_subscreen && ppu->fixedColor == 0u && !half);
            for (int x = span_left; x < span_right; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t main = main_pixels[index];
                uint16_t sub = add_subscreen ? sub_pixels[index] : backdrop;
                unsigned palette = main & 0xffu;
                row[origin + x] = simple_color
                    ? (clipped ? 0u : ppu->cgramRgb[palette])
                    : native_final_rgb(
                        ppu, main, sub, clipped, math_enabled,
                        add_subscreen, subtract, half);
                if (native_pixel_layer(main) == kPpuOverlaySource_Obj)
                    palette = (palette - 0x80u) >> 4;
                ppu->bgBuffers[0].data[index] = (PpuZbufType)(
                    (native_pixel_rank(main) << 8) | palette);
            }
        }
    }
    return true;
}

/* Capture-aware native scanout.  Resolve each source once into a packed
 * scanline, export requested source pixels, then merge the surviving sources.
 * This preserves overlay/removal ownership while keeping tile and OAM walks
 * outside the destination-pixel loop. */
static bool render_native_capture_line(Ppu *ppu, int screen_y,
        uint32_t *row, int origin, bool authentic, bool dual_authentic,
        uint32_t *authentic_row, int authentic_origin) {
    PpuNativeLineScratch *scratch = &ppu->nativeLineScratch;
    uint16_t (*layer_main)[kPpuBufWidth] = scratch->layerMain;
    uint16_t (*layer_sub)[kPpuBufWidth] = scratch->layerSub;
    uint16_t *main_pixels = scratch->mainPixels;
    uint16_t *sub_pixels = scratch->subPixels;
    uint16_t *original_main = scratch->originalMain;
    uint16_t *original_sub = scratch->originalSub;
    uint8_t (*bands)[kPpuBufWidth] = scratch->bands;
    bool bg_active[4] = {false};
    bool resolved_span[kPpuOverlaySource_Count] = {false};
    uint8_t source_mask = (uint8_t)(1u << kPpuOverlaySource_Obj);
    NativeOverlayLinePlan overlay_plans[kPpuOverlaySource_Count];
    uint16_t backdrop = native_pack_pixel(0u, 1u, 5u);
    /* The composed subscreen is read only where colour math or a hires mode
     * can consume it.  A single source additionally needs its own subscreen
     * rendering when the overlay capture owns it there: Marahna authors BG1
     * subscreen-only and adds it to main-screen BG2 with colour math, so the
     * capture contract cannot equate "capture" with "the main pass" (see the
     * owner_sub export below).  Tracking that need per source keeps ordinary
     * main-only layers on the single-store path across a line where one other
     * source is subscreen-owned. */
    bool output_needs_sub = PPU_addSubscreen(ppu) || PPU_pseudoHires(ppu) ||
        PPU_mode(ppu) == 5 || PPU_mode(ppu) == 6;
    bool source_needs_sub[kPpuOverlaySource_Count];
    uint8_t full_add_mask = 0u;
    int obj_offset = authentic ? ppu->authenticObjOffsetX : 0;
    int left = authentic ? 0 : -ppu->extraLeftCur;
    int right = authentic ? kPpuXPixels
                            : kPpuXPixels + ppu->extraRightCur;
    PpuObjSampleCache *obj_capture_cache = NULL;
    PpuObjSampleCache *obj_removed_cache = NULL;
    PpuOverlayCapture *obj_capture =
        &ppu->overlayCaptures[kPpuOverlaySource_Obj];
    NativeLayerWindowPlan obj_visibility;
    bool authentic_y = screen_y >= 0 && screen_y < kPpuYPixels;
    if (PPU_mode(ppu) == 7) {
        source_mask |= 1u;
        if (PPU_m7extBg(ppu)) source_mask |= 2u;
    } else {
        for (int layer = 0; layer < 4; ++layer)
            if (bpp_for_mode(PPU_mode(ppu), layer) != 0)
                source_mask |= (uint8_t)(1u << layer);
    }
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        const PpuOverlayCapture *source_capture =
            &ppu->overlayCaptures[source];
        bool source_capture_line = capture_surface_bound(ppu, source) &&
            source_capture->x1 > source_capture->x0 &&
            screen_y >= source_capture->y0 &&
            screen_y < source_capture->y1 &&
            (source != kPpuOverlaySource_Obj ||
             source_capture->oamCount != 0u);
        bool source_owner_sub =
            (ppu->screenEnabled[0] & (1u << source)) == 0u;
        source_needs_sub[source] = output_needs_sub ||
            (source_capture_line && source_owner_sub);
        if (source_capture_line && (source_capture->flags &
                kPpuOverlayFlag_MarkFullAddSubscreen) != 0u)
            full_add_mask |= (uint8_t)(1u << source);
    }
    /* The full-add export compares the complete pre-removal subscreen winner
     * against the main-screen winner, so every source on such a line needs its
     * subscreen plane resolved regardless of what composition will read. */
    if (full_add_mask != 0u)
        for (int source = 0; source < kPpuOverlaySource_Count; ++source)
            source_needs_sub[source] = true;
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        if ((source_mask & (1u << source)) == 0u) continue;
        memset(layer_main[source], 0, sizeof(layer_main[source]));
        if (source_needs_sub[source])
            memset(layer_sub[source], 0, sizeof(layer_sub[source]));
    }
    memset(bands, 0xff, sizeof(scratch->bands));
    if (!ppu->cgramRgbValid) rebuild_cgram_rgb(ppu);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source)
        native_overlay_line_plan(
            ppu, source, screen_y, &overlay_plans[source]);
    native_layer_window_plan(
        ppu, kPpuOverlaySource_Obj,
        source_needs_sub[kPpuOverlaySource_Obj], &obj_visibility);
    if (PPU_mode(ppu) == 7) {
        bg_active[0] = true;
        resolved_span[0] = true;
        native_resolve_mode7_layer_span(
            ppu, 0, screen_y, source_needs_sub[0], left, right,
            kPpuExtraLeftRight, layer_main[0], layer_sub[0]);
        if (PPU_m7extBg(ppu)) {
            bg_active[1] = true;
            resolved_span[1] = true;
            native_resolve_mode7_layer_span(
                ppu, 1, screen_y, source_needs_sub[1], left, right,
                kPpuExtraLeftRight, layer_main[1], layer_sub[1]);
        }
    } else {
        for (int layer = 0; layer < 4; ++layer) {
            if (bpp_for_mode(PPU_mode(ppu), layer) == 0) continue;
            bg_active[layer] = true;
            if (native_virtual_bg_span_eligible(ppu, layer)) {
                native_resolve_virtual_bg_span(
                    ppu, layer, screen_y, source_needs_sub[layer],
                    left, right,
                    kPpuExtraLeftRight, layer_main[layer], layer_sub[layer],
                    layer < 2 ? bands[layer] : NULL);
                resolved_span[layer] = true;
            } else if (ppu->virtualTilemap[layer].lookup == NULL) {
                bool mosaic = PPU_mosaicEnabled(ppu, layer) &&
                    PPU_mosaicSize(ppu) > 1;
                if (authentic_y) {
                    native_resolve_bg(
                        ppu, layer, screen_y, source_needs_sub[layer],
                        layer_main[layer] + kPpuExtraLeftRight,
                        layer_sub[layer] + kPpuExtraLeftRight,
                        layer < 2 ? bands[layer] + kPpuExtraLeftRight : NULL);
                    if (!mosaic) {
                        native_resolve_vram_bg_span(
                            ppu, layer, screen_y, source_needs_sub[layer],
                            left, 0,
                            kPpuExtraLeftRight, layer_main[layer],
                            layer_sub[layer]);
                        native_resolve_vram_bg_span(
                            ppu, layer, screen_y, source_needs_sub[layer],
                            kPpuXPixels, right, kPpuExtraLeftRight,
                            layer_main[layer], layer_sub[layer]);
                    }
                } else if (!mosaic) {
                    native_resolve_vram_bg_span(
                        ppu, layer, screen_y, source_needs_sub[layer],
                        left, right,
                        kPpuExtraLeftRight, layer_main[layer],
                        layer_sub[layer]);
                }
                /* Margin policy can remap a destination to a different
                 * display-space mosaic group.  Until the arbitrary-span
                 * resolver understands that phase, leave only those margins
                 * to the reference sampler. */
                resolved_span[layer] = !mosaic;
            } else if (authentic_y) {
                native_resolve_bg(
                    ppu, layer, screen_y, source_needs_sub[layer],
                    layer_main[layer] + kPpuExtraLeftRight,
                    layer_sub[layer] + kPpuExtraLeftRight,
                    layer < 2 ? bands[layer] + kPpuExtraLeftRight : NULL);
            }
        }
    }
    if (authentic_y) {
        native_resolve_obj(ppu, screen_y, obj_offset,
                           source_needs_sub[kPpuOverlaySource_Obj],
                           layer_main[kPpuOverlaySource_Obj] +
                               kPpuExtraLeftRight,
                           layer_sub[kPpuOverlaySource_Obj] +
                               kPpuExtraLeftRight);
    }
    /* The tile-oriented resolvers above own the authentic span.  Margins can
     * have per-band clamp/repeat/virtual policies, so sample each BG source
     * once there, then keep composition/capture in the packed scanline.  This
     * is still one source fetch per pixel rather than repeating it for main,
     * sub, capture, removal, and authentic views. */
    {
        bool bg_fallback = false;
        PpuObjSampleCache *obj_cache = NULL;
        NativeLayerWindowPlan obj_margin_visibility = obj_visibility;
        for (int layer = 0; layer < 4; ++layer)
            if (bg_active[layer] && !resolved_span[layer])
                bg_fallback = true;
        if (bg_fallback) {
            int span_left[2] = {left, authentic_y ? kPpuXPixels : right};
            int span_right[2] = {authentic_y ? 0 : right, right};
            int span_count = authentic_y ? 2 : 1;
            for (int span = 0; span < span_count; ++span)
                for (int x = span_left[span]; x < span_right[span]; ++x) {
                    int index = x + kPpuExtraLeftRight;
                    for (int layer = 0; layer < 4; ++layer) {
                        SrPpuPixel pixel = {0};
                        uint16_t packed;
                        if (!bg_active[layer] || resolved_span[layer])
                            continue;
                        if (!sample_bg(
                                ppu, layer, x, screen_y, true, &pixel))
                            continue;
                        packed = native_pack_pixel(
                            pixel.palette, pixel.rank, pixel.layer);
                        if (source_visible_on_screen(
                                ppu, layer, false, x))
                            layer_main[layer][index] = packed;
                        if (source_needs_sub[layer] &&
                            source_visible_on_screen(
                                ppu, layer, true, x))
                            layer_sub[layer][index] = packed;
                        if (layer < 2) bands[layer][index] = pixel.band;
                    }
                }
        }
        if (obj_margin_visibility.main_mode != 0u ||
            obj_margin_visibility.sub_mode != 0u)
            obj_cache = get_obj_sample_cache(
                ppu, screen_y, obj_offset, 0, 0, 0, 0);
#define RESOLVE_OBJ_MARGIN_SPAN(begin_, end_) do {                         \
            for (int x = (begin_); x < (end_); ++x) {                     \
                int index = x + kPpuExtraLeftRight;                       \
                uint16_t packed = native_obj_cache_pixel(obj_cache, x);   \
                if (packed != 0u) {                                       \
                    bool inside = native_window_plan_inside(              \
                        &obj_margin_visibility, x);                        \
                    if (obj_margin_visibility.main_mode == 1u ||          \
                        (obj_margin_visibility.main_mode == 2u &&         \
                         !inside))                                        \
                        layer_main[kPpuOverlaySource_Obj][index] = packed;\
                    if (obj_margin_visibility.sub_mode == 1u ||           \
                        (obj_margin_visibility.sub_mode == 2u &&          \
                         !inside))                                        \
                        layer_sub[kPpuOverlaySource_Obj][index] = packed; \
                }                                                         \
            }                                                             \
        } while (0)
        if (obj_cache != NULL) {
            if (authentic_y) {
                RESOLVE_OBJ_MARGIN_SPAN(left, 0);
                RESOLVE_OBJ_MARGIN_SPAN(kPpuXPixels, right);
            } else {
                RESOLVE_OBJ_MARGIN_SPAN(left, right);
            }
        }
#undef RESOLVE_OBJ_MARGIN_SPAN
    }
    native_write_obj_range_capture(ppu, screen_y, obj_offset);
    if (capture_surface_bound(ppu, kPpuOverlaySource_Obj) &&
        native_capture_intersects(obj_capture, screen_y) &&
        obj_capture->oamCount != 0u) {
        if (obj_capture->oamFirst == 0u && obj_capture->oamCount == 128u) {
            obj_capture_cache = get_obj_sample_cache(
                ppu, screen_y, obj_offset, 0, 0, 0, 0);
        } else {
            obj_capture_cache = get_obj_sample_cache(
                ppu, screen_y, obj_offset,
                obj_capture->oamFirst, obj_capture->oamCount, 0, 0);
            if ((obj_capture->flags & kPpuOverlayFlag_RemoveFromGame) != 0u)
                obj_removed_cache = get_obj_sample_cache(
                    ppu, screen_y, obj_offset, 0, 0,
                    obj_capture->oamFirst, obj_capture->oamCount);
        }
    }
    for (int x = left; x < right; ++x)
        main_pixels[x + kPpuExtraLeftRight] = backdrop;
    if (output_needs_sub) {
        for (int x = left; x < right; ++x)
            sub_pixels[x + kPpuExtraLeftRight] = backdrop;
    }
    for (int layer = 0; layer < kPpuOverlaySource_Obj; ++layer) {
        PpuOverlayCapture *capture;
        bool capture_line, owner_sub, remove;
        int capture_left, capture_right;
        if ((source_mask & (1u << layer)) == 0u) continue;
        capture = &ppu->overlayCaptures[layer];
        capture_line = capture_surface_bound(ppu, layer) &&
            capture->x1 > capture->x0 &&
            screen_y >= capture->y0 && screen_y < capture->y1;
        owner_sub = (ppu->screenEnabled[0] & (1u << layer)) == 0u;
        remove = (capture->flags &
                  kPpuOverlayFlag_RemoveFromGame) != 0u;
        capture_left = capture_line && capture->x0 > left
            ? capture->x0 : left;
        capture_right = capture_line && capture->x1 < right
            ? capture->x1 : right;
        if (!capture_line || capture_left >= capture_right)
            capture_left = capture_right = right;
#define MERGE_NATIVE_BG_PIXEL(x_) do {                                    \
            int merge_index = (x_) + kPpuExtraLeftRight;                  \
            uint16_t merge_main = layer_main[layer][merge_index];         \
            if (merge_main > main_pixels[merge_index])                    \
                main_pixels[merge_index] = merge_main;                    \
            if (output_needs_sub) {                                       \
                uint16_t merge_sub = layer_sub[layer][merge_index];       \
                if (merge_sub > sub_pixels[merge_index])                  \
                    sub_pixels[merge_index] = merge_sub;                  \
            }                                                             \
        } while (0)
        for (int x = left; x < capture_left; ++x)
            MERGE_NATIVE_BG_PIXEL(x);
        for (int x = capture_left; x < capture_right; ++x) {
            int index = x + kPpuExtraLeftRight;
            uint16_t source_main = layer_main[layer][index];
            uint16_t source_sub = source_needs_sub[layer]
                ? layer_sub[layer][index] : 0u;
            if ((full_add_mask & (1u << layer)) == 0u) {
                uint16_t captured = owner_sub ? source_sub : source_main;
                if (captured != 0u)
                    native_write_overlay_packed(
                        ppu, layer, x, captured,
                        layer < 2 ? bands[layer][index] : 0xffu,
                        &overlay_plans[layer]);
            }
            if (!remove) {
                if (source_main > main_pixels[index])
                    main_pixels[index] = source_main;
                if (output_needs_sub && source_sub > sub_pixels[index])
                    sub_pixels[index] = source_sub;
            }
        }
        for (int x = capture_right; x < right; ++x)
            MERGE_NATIVE_BG_PIXEL(x);
#undef MERGE_NATIVE_BG_PIXEL
    }
    {
        const int layer = kPpuOverlaySource_Obj;
        PpuOverlayCapture *capture = &ppu->overlayCaptures[layer];
        bool capture_line = capture_surface_bound(ppu, layer) &&
            capture->x1 > capture->x0 &&
            screen_y >= capture->y0 && screen_y < capture->y1 &&
            capture->oamCount != 0u;
        bool owner_sub =
            (ppu->screenEnabled[0] & (1u << layer)) == 0u;
        bool remove = (capture->flags &
                       kPpuOverlayFlag_RemoveFromGame) != 0u;
        NativeLayerWindowPlan obj_capture_visibility = obj_visibility;
        if (!capture_line) {
            int index = left + kPpuExtraLeftRight;
            native_merge_packed_span(
                main_pixels + index, sub_pixels + index,
                layer_main[layer] + index, layer_sub[layer] + index,
                right - left, output_needs_sub);
        } else {
            for (int x = left; x < right; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t source_main = layer_main[layer][index];
                uint16_t source_sub = source_needs_sub[layer]
                    ? layer_sub[layer][index] : 0u;
                if (x >= capture->x0 && x < capture->x1) {
                    bool inside = native_window_plan_inside(
                        &obj_capture_visibility, x);
                    bool show_main =
                        obj_capture_visibility.main_mode == 1u ||
                        (obj_capture_visibility.main_mode == 2u && !inside);
                    bool show_sub =
                        obj_capture_visibility.sub_mode == 1u ||
                        (obj_capture_visibility.sub_mode == 2u && !inside);
                    uint16_t captured = (owner_sub ? show_sub : show_main)
                        ? native_obj_cache_pixel(obj_capture_cache, x) : 0u;
                    if (captured != 0u &&
                        (full_add_mask & (1u << layer)) == 0u)
                        native_write_overlay_packed(
                            ppu, layer, x, captured, 0xffu,
                            &overlay_plans[layer]);
                    if (remove) {
                        if (capture->oamFirst == 0u &&
                            capture->oamCount == 128u) {
                            source_main = source_sub = 0u;
                        } else {
                            uint16_t remaining =
                                native_obj_cache_pixel(obj_removed_cache, x);
                            source_main = show_main ? remaining : 0u;
                            source_sub = show_sub ? remaining : 0u;
                        }
                    }
                }
                if (source_main > main_pixels[index])
                    main_pixels[index] = source_main;
                if (output_needs_sub && source_sub > sub_pixels[index])
                    sub_pixels[index] = source_sub;
            }
        }
    }
    /* MarkFullAddSubscreen: export whichever source wins the COMPLETE
     * pre-removal subscreen, wherever the main-screen winner is a
     * math-bearing layer.  This is the same rule the reference sampler
     * applies in post_capture_masks, but read off the packed per-source
     * planes this line already resolved instead of re-sampling every pixel
     * through resolve_screen.  BG3 is excluded from the main winner inside
     * its own capture rect: it is a separately reinserted foreground plane,
     * and leaving it in punches HUD-glyph-shaped holes out of the addend.
     * The subscreen winner excludes relocated OBJ slots for the same reason
     * the reference pass does.  Only sources in source_mask are considered:
     * a plane outside it was neither cleared nor resolved on this line and
     * still holds whatever the last line that did use it left behind, which a
     * mid-frame BG-mode change makes reachable. */
    if (full_add_mask != 0u) {
        unsigned math_enabled = PPU_mathEnabled(ppu);
        const PpuOverlayCapture *bg3_capture =
            &ppu->overlayCaptures[kPpuOverlaySource_Bg3];
        bool bg3_bound = capture_surface_bound(ppu, kPpuOverlaySource_Bg3);
        bool obj_sub_enabled =
            (ppu->screenEnabled[1] & (1u << kPpuOverlaySource_Obj)) != 0u;
        PpuObjSampleCache *sub_obj_cache = obj_sub_enabled
            ? get_obj_sample_cache(
                  ppu, screen_y, 0, 0, 0,
                  ppu->overlayObjRelocatedCount != 0u
                      ? ppu->overlayObjRelocatedFirst : 0u,
                  ppu->overlayObjRelocatedCount)
            : NULL;
        for (int x = left; x < right; ++x) {
            int index = x + kPpuExtraLeftRight;
            uint16_t full_main = backdrop;
            uint16_t full_sub = backdrop;
            unsigned main_layer, sub_layer;
            bool exclude_bg3 = bg3_bound &&
                capture_active(bg3_capture, x, screen_y);
            for (int layer = 0; layer < kPpuOverlaySource_Obj; ++layer) {
                if ((source_mask & (1u << layer)) == 0u) continue;
                if (exclude_bg3 && layer == kPpuOverlaySource_Bg3) continue;
                if (layer_main[layer][index] > full_main)
                    full_main = layer_main[layer][index];
            }
            if (layer_main[kPpuOverlaySource_Obj][index] > full_main)
                full_main = layer_main[kPpuOverlaySource_Obj][index];
            main_layer = native_pixel_layer(full_main);
            if (main_layer >= 5u ||
                (math_enabled & (1u << main_layer)) == 0u) continue;
            for (int layer = 0; layer < kPpuOverlaySource_Obj; ++layer) {
                if ((source_mask & (1u << layer)) == 0u) continue;
                if (layer_sub[layer][index] > full_sub)
                    full_sub = layer_sub[layer][index];
            }
            if (sub_obj_cache != NULL &&
                source_visible_on_screen(
                    ppu, kPpuOverlaySource_Obj, true, x)) {
                uint16_t obj = native_obj_cache_pixel(sub_obj_cache, x);
                if (obj > full_sub) full_sub = obj;
            }
            sub_layer = native_pixel_layer(full_sub);
            if (sub_layer >= (unsigned)kPpuOverlaySource_Count ||
                (full_add_mask & (1u << sub_layer)) == 0u) continue;
            if (!capture_active(&ppu->overlayCaptures[sub_layer], x,
                                screen_y)) continue;
            native_write_overlay_packed(
                ppu, (int)sub_layer, x, full_sub,
                sub_layer < 2u ? bands[sub_layer][index] : 0xffu,
                &overlay_plans[sub_layer]);
        }
    }
    if (dual_authentic) {
        for (int x = 0; x < kPpuXPixels; ++x) {
            int index = x + kPpuExtraLeftRight;
            original_main[index] = backdrop;
            if (output_needs_sub) original_sub[index] = backdrop;
        }
        for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
            if ((source_mask & (1u << source)) == 0u) continue;
            for (int x = 0; x < kPpuXPixels; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t source_main = layer_main[source][index];
                if (source_main > original_main[index])
                    original_main[index] = source_main;
                if (output_needs_sub) {
                    uint16_t source_sub = layer_sub[source][index];
                    if (source_sub > original_sub[index])
                        original_sub[index] = source_sub;
                }
            }
        }
    }
    {
        NativeWindowRuns color_runs;
        unsigned clip = PPU_clipMode(ppu);
        unsigned prevent = PPU_preventMathMode(ppu);
        unsigned math_enabled = PPU_mathEnabled(ppu);
        bool add_subscreen = PPU_addSubscreen(ppu);
        bool subtract = PPU_subtractColor(ppu);
        bool half = PPU_halfColor(ppu);
        native_window_runs(ppu, 5, &color_runs);
        for (int run = 0; run < color_runs.count; ++run) {
            bool inside = color_runs.inside[run] != 0u;
            bool clipped = clip == 3u || (clip == 2u && inside) ||
                           (clip == 1u && !inside);
            bool prevented = prevent == 3u || (prevent == 2u && inside) ||
                             (prevent == 1u && !inside);
            bool simple_color = prevented || math_enabled == 0u ||
                (!add_subscreen && ppu->fixedColor == 0u && !half);
            int run_left = color_runs.edges[run] > left
                ? color_runs.edges[run] : left;
            int run_right = color_runs.edges[run + 1] < right
                ? color_runs.edges[run + 1] : right;
            for (int x = run_left; x < run_right; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t main = main_pixels[index];
                uint16_t sub = add_subscreen ? sub_pixels[index] : backdrop;
                row[origin + x] = simple_color
                    ? (clipped ? 0u : ppu->cgramRgb[main & 0xffu])
                    : native_final_rgb(
                        ppu, main, sub, clipped, math_enabled,
                        add_subscreen, subtract, half);
                {
                    unsigned palette = main & 0xffu;
                    if (native_pixel_layer(main) ==
                        kPpuOverlaySource_Obj)
                        palette = (palette - 0x80u) >> 4;
                    ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] =
                        (PpuZbufType)(
                            (native_pixel_rank(main) << 8) | palette);
                }
                if (dual_authentic) {
                    uint16_t authentic_main = original_main[index];
                    uint16_t authentic_sub = add_subscreen
                        ? original_sub[index] : backdrop;
                    authentic_row[authentic_origin + x] = simple_color
                        ? (clipped ? 0u
                                   : ppu->cgramRgb[
                                       authentic_main & 0xffu])
                        : native_final_rgb(
                            ppu, authentic_main, authentic_sub, clipped,
                            math_enabled, add_subscreen, subtract, half);
                }
            }
        }
        /* Color-window runs cover authentic X.  SNES windows do not extend
         * into synthetic margins, so emit those two spans with outside-window
         * state while retaining the same color-math mode. */
        for (int side = 0; side < 2; ++side) {
            int span_left = side == 0 ? left : kPpuXPixels;
            int span_right = side == 0 ? 0 : right;
            bool inside = false;
            bool clipped = clip == 3u || (clip == 2u && inside) ||
                           (clip == 1u && !inside);
            bool prevented = prevent == 3u || (prevent == 2u && inside) ||
                             (prevent == 1u && !inside);
            bool simple_color = prevented || math_enabled == 0u ||
                (!add_subscreen && ppu->fixedColor == 0u && !half);
            for (int x = span_left; x < span_right; ++x) {
                int index = x + kPpuExtraLeftRight;
                uint16_t main = main_pixels[index];
                uint16_t sub = add_subscreen ? sub_pixels[index] : backdrop;
                row[origin + x] = simple_color
                    ? (clipped ? 0u : ppu->cgramRgb[main & 0xffu])
                    : native_final_rgb(
                        ppu, main, sub, clipped, math_enabled,
                        add_subscreen, subtract, half);
                {
                    unsigned palette = main & 0xffu;
                    if (native_pixel_layer(main) ==
                        kPpuOverlaySource_Obj)
                        palette = (palette - 0x80u) >> 4;
                    ppu->bgBuffers[0].data[index] = (PpuZbufType)(
                        (native_pixel_rank(main) << 8) | palette);
                }
            }
        }
    }
    return true;
}

static bool render_native_fast_line(Ppu *ppu, int screen_y,
        uint32_t *row, int origin, bool capture, bool authentic,
        bool dual_authentic, uint32_t *authentic_row, int authentic_origin) {
    uint16_t *main_pixels = ppu->nativeLineScratch.mainPixels;
    uint16_t *sub_pixels = ppu->nativeLineScratch.subPixels;
    uint16_t backdrop = native_pack_pixel(0u, 1u, 5u);
    bool want_sub;
    NativeWindowRuns color_runs;
    int obj_offset = authentic ? ppu->authenticObjOffsetX : 0;
    if (!native_fast_eligible(ppu, screen_y, capture)) return false;
    if (capture && PPU_mode(ppu) == 7 &&
        !native_capture_policy_on_line(ppu, screen_y) &&
        (ppu->extraLeftCur != 0u || ppu->extraRightCur != 0u ||
         screen_y < 0 || screen_y >= kPpuYPixels))
        return render_native_mode7_wide_line(
            ppu, screen_y, row, origin, dual_authentic,
            authentic_row, authentic_origin);
    if (capture && (native_capture_line_needed(ppu, screen_y) ||
                    ppu->extraLeftCur != 0u ||
                    ppu->extraRightCur != 0u ||
                    screen_y < 0 || screen_y >= kPpuYPixels))
        return render_native_capture_line(
            ppu, screen_y, row, origin, authentic, dual_authentic,
            authentic_row, authentic_origin);
    want_sub = PPU_addSubscreen(ppu) || PPU_pseudoHires(ppu) ||
        PPU_mode(ppu) == 5 || PPU_mode(ppu) == 6;
    for (int x = 0; x < kPpuXPixels; ++x)
        main_pixels[x] = backdrop;
    if (want_sub) {
        for (int x = 0; x < kPpuXPixels; ++x)
            sub_pixels[x] = backdrop;
    }
    if (PPU_mode(ppu) == 7) {
        native_resolve_mode7(ppu, screen_y, want_sub,
                             main_pixels, sub_pixels);
    } else {
        for (int layer = 0; layer < 4; ++layer) {
            if (bpp_for_mode(PPU_mode(ppu), layer) != 0)
                native_resolve_bg(ppu, layer, screen_y, want_sub,
                                  main_pixels, sub_pixels, NULL);
        }
    }
    native_resolve_obj(ppu, screen_y, obj_offset, want_sub,
                       main_pixels, sub_pixels);
    native_window_runs(ppu, 5, &color_runs);
    {
        unsigned clip = PPU_clipMode(ppu);
        unsigned prevent = PPU_preventMathMode(ppu);
        unsigned math_enabled = PPU_mathEnabled(ppu);
        bool add_subscreen = PPU_addSubscreen(ppu);
        bool subtract = PPU_subtractColor(ppu);
        bool half = PPU_halfColor(ppu);
        for (int run = 0; run < color_runs.count; ++run) {
            bool inside = color_runs.inside[run] != 0u;
            bool clipped = clip == 3u || (clip == 2u && inside) ||
                           (clip == 1u && !inside);
            bool prevented = prevent == 3u || (prevent == 2u && inside) ||
                             (prevent == 1u && !inside);
            bool simple_color = prevented || math_enabled == 0u ||
                (!add_subscreen && ppu->fixedColor == 0u && !half);
            if (simple_color) {
                if (!ppu->cgramRgbValid) rebuild_cgram_rgb(ppu);
                for (int x = color_runs.edges[run];
                     x < color_runs.edges[run + 1]; ++x) {
                    uint16_t main = main_pixels[x];
                    row[origin + x] = clipped
                        ? 0u : ppu->cgramRgb[main & 0xffu];
                    if (capture) {
                        unsigned palette = main & 0xffu;
                        if (native_pixel_layer(main) ==
                            kPpuOverlaySource_Obj)
                            palette = (palette - 0x80u) >> 4;
                        ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] =
                            (PpuZbufType)(
                                (native_pixel_rank(main) << 8) | palette);
                    }
                }
            } else {
                for (int x = color_runs.edges[run];
                     x < color_runs.edges[run + 1]; ++x) {
                    uint16_t main = main_pixels[x];
                    uint32_t color = native_final_rgb(
                        ppu, main,
                        add_subscreen ? sub_pixels[x] : backdrop,
                        clipped, math_enabled,
                        add_subscreen, subtract, half);
                    row[origin + x] = color;
                    if (capture) {
                        unsigned palette = main & 0xffu;
                        if (native_pixel_layer(main) ==
                            kPpuOverlaySource_Obj)
                            palette = (palette - 0x80u) >> 4;
                        ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] =
                            (PpuZbufType)(
                                (native_pixel_rank(main) << 8) | palette);
                    }
                }
            }
        }
    }
    if (dual_authentic) {
        memcpy(authentic_row + authentic_origin, row + origin,
               kPpuXPixels * sizeof(*row));
    }
    return true;
}

static void post_capture_masks(Ppu *ppu, int x, int y,
        const SrPpuPixel *main, const SrPpuPixel *sub,
        const SrPpuPixel *full_main, const SrPpuPixel *full_sub) {
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        if (!capture_surface_bound(ppu, source) ||
            !capture_active(capture, x, y)) continue;
        if ((capture->flags & kPpuOverlayFlag_MarkMainScreenWinner) != 0u) {
            if (main->layer == source) write_overlay(ppu, source, x, y, main,
                                                     0xffffffffu);
        } else if ((capture->flags & kPpuOverlayFlag_MarkOwningScreenWinner) != 0u) {
            const SrPpuPixel *owner = (ppu->screenEnabled[0] & (1u << source))
                ? main : sub;
            if (owner->layer == source) write_overlay(ppu, source, x, y, owner,
                                                      0xffffffffu);
        } else if ((capture->flags & kPpuOverlayFlag_MarkFullAddSubscreen) != 0u) {
            if (full_sub->layer == source && full_main->layer < 5u &&
                (PPU_mathEnabled(ppu) & (1u << full_main->layer)) != 0u)
                write_overlay(ppu, source, x, y, full_sub, 0u);
        }
    }
}

static bool authentic_sampling_matches(const Ppu *ppu, int screen_y) {
    int row_index = output_row(ppu, screen_y);
    if (!PpuAuthenticSurfaceReady(ppu) || row_index < 0 ||
        row_index >= (int)ppu->authenticRenderHeight || screen_y < 0 ||
        screen_y >= kPpuYPixels || ppu->authenticObjOffsetX != 0)
        return false;
    for (int layer = 0; layer < 2; ++layer) {
        if ((ppu->authenticHScrollMask & (1u << layer)) != 0u &&
            ppu->authenticHScroll[layer][screen_y] != ppu->hScroll[layer])
            return false;
    }
    return true;
}

static bool render_line_to(Ppu *ppu, int screen_y, uint8_t *buffer,
                           size_t pitch, uint32_t height,
                           bool capture, bool authentic) {
    int origin = surface_origin_x(ppu, pitch);
    int row_index = output_row(ppu, screen_y);
    uint32_t *row;
    bool deferred_capture = false;
    bool full_add_capture = false;
    bool dual_authentic = capture && !authentic &&
        authentic_sampling_matches(ppu, screen_y);
    uint32_t *authentic_row = NULL;
    int authentic_origin = 0;
    int left = authentic ? 0 : -ppu->extraLeftCur;
    int right = authentic ? kPpuXPixels : kPpuXPixels + ppu->extraRightCur;
    bool native_center = false;
    if (buffer == NULL || pitch == 0u || row_index < 0 ||
        row_index >= (int)height || row_index >= kPpuBufHeight)
        return false;
    row = (uint32_t *)(buffer + (size_t)row_index * pitch);
    if (PPU_forcedBlank(ppu) || origin + left > 0 ||
        origin + right < (int)(pitch / sizeof(uint32_t)))
        memset(row, 0, pitch);
    if (dual_authentic) {
        authentic_row = (uint32_t *)(ppu->authenticRenderBuffer +
            (size_t)row_index * ppu->authenticRenderPitch);
        authentic_origin = surface_origin_x(ppu, ppu->authenticRenderPitch);
        memset(authentic_row, 0, ppu->authenticRenderPitch);
    }
    if (PPU_forcedBlank(ppu)) return dual_authentic;
    if (capture) {
        for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
            const PpuOverlayCapture *cap = &ppu->overlayCaptures[source];
            if (!capture_surface_bound(ppu, source) ||
                screen_y < cap->y0 || screen_y >= cap->y1) continue;
            if (capture_is_deferred(cap)) deferred_capture = true;
            if ((cap->flags & kPpuOverlayFlag_MarkFullAddSubscreen) != 0u)
                full_add_capture = true;
        }
    }
    native_center = render_native_fast_line(
        ppu, screen_y, row, origin, capture,
        authentic, dual_authentic, authentic_row, authentic_origin);
    if (native_center &&
        (capture || (left == 0 && right == kPpuXPixels)))
        return dual_authentic;
    for (int x = left; x < right; ++x) {
        int obj_offset = authentic ? ppu->authenticObjOffsetX : 0;
        if (native_center && x >= 0 && x < kPpuXPixels) continue;
        SrPpuPixel main, sub;
        SrPpuPixel original_main, original_sub, full_main, full_sub;
        bool want_sub = (capture && ppu->screenEnabled[1] != 0u) ||
            PPU_addSubscreen(ppu) || PPU_pseudoHires(ppu) ||
            PPU_mode(ppu) == 5 || PPU_mode(ppu) == 6;
        if (capture) capture_obj_sources(ppu, x, screen_y, obj_offset);
        resolve_screen_pair(ppu, x, screen_y, capture, obj_offset, capture,
                            want_sub, dual_authentic, &main, &sub,
                            &original_main, &original_sub);
        row[origin + x] = color_rgb(ppu, final_color(ppu, x, &main, &sub));
        if (dual_authentic && x >= 0 && x < kPpuXPixels)
            authentic_row[authentic_origin + x] = color_rgb(
                ppu, final_color(ppu, x, &original_main, &original_sub));
        if (capture) {
            int index = x + kPpuExtraLeftRight;
            if (index >= 0 && index < kPpuBufWidth)
                ppu->bgBuffers[0].data[index] =
                    (PpuZbufType)((main.rank << 8) | main.palette);
            if (!dual_authentic) {
                original_main = main;
                original_sub = sub;
            }
            full_main = main;
            full_sub = sub;
            if (deferred_capture && !dual_authentic) {
                original_main = resolve_screen(ppu, x, screen_y, false, false,
                                               obj_offset, false, -1, false,
                                               NULL);
                original_sub = resolve_screen(ppu, x, screen_y, true, false,
                                              0, false, -1, false, NULL);
                full_main = original_main;
                full_sub = original_sub;
            } else if (deferred_capture) {
                full_main = original_main;
                full_sub = original_sub;
            }
            if (full_add_capture) {
                int excluded = capture_surface_bound(
                    ppu, kPpuOverlaySource_Bg3) && capture_active(
                    &ppu->overlayCaptures[kPpuOverlaySource_Bg3], x, screen_y)
                    ? kPpuOverlaySource_Bg3 : -1;
                full_main = resolve_screen(ppu, x, screen_y, false, false,
                                           obj_offset, false, excluded, false,
                                           NULL);
                full_sub = resolve_screen(ppu, x, screen_y, true, false,
                                          0, false, -1, true, NULL);
            }
            post_capture_masks(ppu, x, screen_y,
                               &original_main, &original_sub,
                               &full_main, &full_sub);
        }
    }
    return dual_authentic;
}

static void render_authentic(Ppu *ppu, int screen_y) {
    uint16_t saved_h0, saved_h1;
    uint8_t saved_left, saved_right, saved_budget;
    if (!PpuAuthenticSurfaceReady(ppu) || screen_y < 0 || screen_y >= kPpuYPixels)
        return;
    saved_h0 = ppu->hScroll[0]; saved_h1 = ppu->hScroll[1];
    saved_left = ppu->extraLeftCur; saved_right = ppu->extraRightCur;
    saved_budget = ppu->extraLeftRight;
    if ((ppu->authenticHScrollMask & kPpuAuthenticCameraLayer_Bg1) != 0u)
        ppu->hScroll[0] = ppu->authenticHScroll[0][screen_y];
    if ((ppu->authenticHScrollMask & kPpuAuthenticCameraLayer_Bg2) != 0u)
        ppu->hScroll[1] = ppu->authenticHScroll[1][screen_y];
    ppu->extraLeftCur = ppu->extraRightCur = 0u;
    (void)render_line_to(ppu, screen_y, ppu->authenticRenderBuffer,
                         ppu->authenticRenderPitch,
                         ppu->authenticRenderHeight, false, true);
    ppu->hScroll[0] = saved_h0; ppu->hScroll[1] = saved_h1;
    ppu->extraLeftCur = saved_left; ppu->extraRightCur = saved_right;
    ppu->extraLeftRight = saved_budget;
}

static void render_line(Ppu *ppu, int line) {
    int screen_y = line - 1;
    for (int slot = 0; slot < kPpuObjSampleCacheCount; ++slot)
        ppu->objSampleCache[slot].valid = false;
    for (int layer = 0; layer < 2; ++layer)
        ppu->virtualSampleCache[layer].valid = false;
    ppu->mode7SampleCache.valid = false;
    update_brightness(ppu);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source)
        clear_overlay_row(ppu, source, screen_y);
    if (ppu->objRangeCapture.count != 0u && screen_y >= ppu->objRangeCapture.y0 &&
        screen_y < ppu->objRangeCapture.y1)
        memset(ppu->objRangeCapture.pixels +
               (size_t)screen_y * ppu->objRangeCapture.pitch, 0,
               ppu->objRangeCapture.pitch);
    bool authentic_done = render_line_to(
        ppu, screen_y, ppu->renderBuffer, ppu->renderPitch,
        ppu->renderHeight, true, false);
    /* Captured synthesized padding may extend beyond the live game margin. */
    if (ppu->wsPadCapturedToBudget) {
        for (int source = 0; source < 4; ++source) {
            PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
            PpuWidescreenLayerPolicy policy =
                PpuResolveWidescreenLayerPolicy(
                    ppu, (uint8_t)source, screen_y);
            if (ppu->overlayRenderBuffer[source] == NULL) continue;
            /* The fixed capture budget widens synthesized mirror/repeat
             * padding only.  Raw-wrap, transparent and clamped layers retain
             * the live finite-world margin.  In particular, a BG3 HUD split
             * may legitimately occupy the live margin but must not leak
             * wrapped HUD tiles into a side that has collapsed to black. */
            if (policy.fill != kPpuWidescreenBandFill_Mirror &&
                policy.fill != kPpuWidescreenBandFill_Repeat) continue;
            for (int x = -ppu->extraLeftRight; x < -ppu->extraLeftCur; ++x) {
                SrPpuPixel pixel = {0};
                if (capture_active(capture, x, screen_y) &&
                    sample_bg(ppu, source, x, screen_y, true, &pixel))
                    write_overlay(ppu, source, x, screen_y, &pixel, 0u);
            }
            for (int x = kPpuXPixels + ppu->extraRightCur;
                 x < kPpuXPixels + ppu->extraLeftRight; ++x) {
                SrPpuPixel pixel = {0};
                if (capture_active(capture, x, screen_y) &&
                    sample_bg(ppu, source, x, screen_y, true, &pixel))
                    write_overlay(ppu, source, x, screen_y, &pixel, 0u);
            }
        }
    }
    if (!authentic_done) render_authentic(ppu, screen_y);
}

void ppu_runLine(Ppu *ppu, int line) {
    if (ppu == NULL) return;
    if (line == 0) {
        /* Public component users may populate exposed OAM storage directly
         * between frames.  Lazy invalidation here preserves that ABI while
         * avoiding a rebuild on frames that never request OBJ scanout. */
        ppu->objScanlineMasksValid = false;
        /* CGRAM is exposed by the same component ABI.  The native output
         * paths cache its brightness-expanded form, so invalidate it at the
         * frame boundary to make direct palette edits visible on the next
         * scanline.  Register writes still update a valid cache in place. */
        ppu->cgramRgbValid = false;
        memset(ppu->overlayRenderContentMask, 0,
               sizeof(ppu->overlayRenderContentMask));
        ppu->mosaicStartLine = 1u;
        ppu->rangeOver = ppu->timeOver = false;
        ppu->evenFrame = !ppu->evenFrame;
        return;
    }
    render_line(ppu, line);
}

void ppu_runMarginLine(Ppu *ppu, int line) {
    if (ppu != NULL) render_line(ppu, line);
}

bool ppu_checkOverscan(Ppu *ppu) {
    if (ppu == NULL) return false;
    ppu->frameOverscan = PPU_overscan(ppu);
    return ppu->frameOverscan;
}

void ppu_handleVblank(Ppu *ppu) {
    if (ppu == NULL) return;
    if (!PPU_forcedBlank(ppu)) {
        ppu->oamAdr = ppu->oamaddl;
        ppu->oamInHigh = (ppu->oamaddh & 1u) != 0u;
        ppu->oamSecondWrite = false;
    }
    ppu->frameInterlace = PPU_interlace(ppu);
}

void ppu_latchCounters(Ppu *ppu, uint16_t h_count, uint16_t v_count) {
    if (ppu == NULL) return;
    ppu->hCount = h_count;
    ppu->vCount = v_count;
    ppu->hCountSecond = false;
    ppu->vCountSecond = false;
    ppu->countersLatched = true;
}

uint8_t ppu_read(Ppu *ppu, uint8_t address) {
    uint8_t result = 0u;
    if (ppu == NULL) return 0u;
    switch (address) {
        case 0x34: case 0x35: case 0x36: {
            int product = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
            return (uint8_t)(product >> ((address - 0x34) * 8));
        }
        case 0x37:
            /* The owning Snes advances and supplies the live beam position.
             * A direct PPU-only caller re-latches the most recent position. */
            ppu_latchCounters(ppu, ppu->hCount, ppu->vCount);
            return 0u;
        case 0x38:
            if (ppu->oamInHigh) {
                result = ppu->highOam[((ppu->oamAdr & 15u) << 1) |
                                      ppu->oamSecondWrite];
                if (ppu->oamSecondWrite && ++ppu->oamAdr == 0u) ppu->oamInHigh = false;
            } else if (!ppu->oamSecondWrite) result = (uint8_t)ppu->oam[ppu->oamAdr];
            else { result = (uint8_t)(ppu->oam[ppu->oamAdr++] >> 8);
                   if (ppu->oamAdr == 0u) ppu->oamInHigh = true; }
            ppu->oamSecondWrite = !ppu->oamSecondWrite; return result;
        case 0x39: {
            uint16_t value = ppu->vramReadBuffer;
            if (!ppu->vramIncrementOnHigh) {
                ppu->vramReadBuffer = ppu->vram[remap_vram(ppu) & 0x7fffu];
                ppu->vramPointer += ppu->vramIncrement;
            }
            return (uint8_t)value;
        }
        case 0x3a: {
            uint16_t value = ppu->vramReadBuffer;
            if (ppu->vramIncrementOnHigh) {
                ppu->vramReadBuffer = ppu->vram[remap_vram(ppu) & 0x7fffu];
                ppu->vramPointer += ppu->vramIncrement;
            }
            return (uint8_t)(value >> 8);
        }
        case 0x3b:
            result = !ppu->cgramSecondWrite ? (uint8_t)ppu->cgram[ppu->cgramPointer]
                : (uint8_t)((ppu->cgram[ppu->cgramPointer++] >> 8) & 0x7fu);
            ppu->cgramSecondWrite = !ppu->cgramSecondWrite; return result;
        case 0x3c:
            result = ppu->hCountSecond ? (uint8_t)((ppu->hCount >> 8) & 1u)
                                       : (uint8_t)ppu->hCount;
            ppu->hCountSecond = !ppu->hCountSecond; return result;
        case 0x3d:
            result = ppu->vCountSecond ? (uint8_t)((ppu->vCount >> 8) & 1u)
                                       : (uint8_t)ppu->vCount;
            ppu->vCountSecond = !ppu->vCountSecond; return result;
        case 0x3e: return (uint8_t)(1u | (ppu->rangeOver ? 0x40u : 0u) |
                                     (ppu->timeOver ? 0x80u : 0u));
        case 0x3f:
            result = (uint8_t)(3u | (ppu->countersLatched ? 0x40u : 0u) |
                               (ppu->evenFrame ? 0x80u : 0u));
            ppu->countersLatched = false;
            ppu->hCountSecond = ppu->vCountSecond = false;
            return result;
        default: return 0u;
    }
}

void ppu_write(Ppu *ppu, uint8_t address, uint8_t value) {
    if (ppu == NULL) return;
    switch (address) {
        case 0x00: ppu->inidisp = value; break;
        case 0x01:
            if (((ppu->obsel ^ value) & 0xe0u) != 0u)
                ppu->objScanlineMasksValid = false;
            ppu->obsel = value; break;
        case 0x02: ppu->oamaddl = value; ppu->oamAdr = value;
                   ppu->oamInHigh = (ppu->oamaddh & 1u) != 0u;
                   ppu->oamSecondWrite = false; break;
        case 0x03: ppu->oamaddh = value; ppu->oamAdr = ppu->oamaddl;
                   ppu->oamInHigh = (value & 1u) != 0u;
                   ppu->oamSecondWrite = false; break;
        case 0x04:
            if (ppu->oamInHigh) {
                unsigned index = ((ppu->oamAdr & 15u) << 1) | ppu->oamSecondWrite;
                if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
                    uint8_t old_value = ppu->highOam[index];
                    ppu->highOam[index] = value;
                    sr_runner_emit_ppu_memory_write(
                        ppu, SR_MEMORY_HIGH_OAM, index,
                        old_value, value, 1u);
                } else {
                    ppu->highOam[index] = value;
                }
                ppu->objScanlineMasksValid = false;
                if (ppu->oamSecondWrite && ++ppu->oamAdr == 0u) ppu->oamInHigh = false;
            } else if (!ppu->oamSecondWrite) ppu->oamBuffer = value;
            else {
                uint16_t word = (uint16_t)(ppu->oamBuffer | ((uint16_t)value << 8));
                if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
                    uint16_t old_value = ppu->oam[ppu->oamAdr];
                    ppu->oam[ppu->oamAdr++] = word;
                    sr_runner_emit_ppu_memory_write(
                        ppu, SR_MEMORY_OAM,
                        (uint32_t)(uint8_t)(ppu->oamAdr - 1u) * 2u,
                        old_value, word, 2u);
                } else {
                    ppu->oam[ppu->oamAdr++] = word;
                }
                ppu->objScanlineMasksValid = false;
                if (ppu->oamAdr == 0u) ppu->oamInHigh = true;
            }
            ppu->oamSecondWrite = !ppu->oamSecondWrite; break;
        case 0x05: ppu->bgmode = value; break;
        case 0x06: ppu->mosaic = value; ppu->mosaicStartLine = 0u; break;
        case 0x07: case 0x08: case 0x09: case 0x0a:
            ppu->bgXsc[address - 7u] = value; break;
        case 0x0b: ppu->bgTileAdr = (ppu->bgTileAdr & 0xff00u) | value; break;
        case 0x0c: ppu->bgTileAdr = (ppu->bgTileAdr & 0x00ffu) |
                                       ((uint16_t)value << 8); break;
        case 0x0d: ppu->m7matrix[6] = (int16_t)(((uint16_t)value << 8 |
                         ppu->m7prev) & 0x1fffu); ppu->m7prev = value;
                   /* fall through */
        case 0x0f: case 0x11: case 0x13:
            ppu->hScroll[(address - 0x0d) / 2] =
                (uint16_t)(((uint16_t)value << 8 | (ppu->scrollPrev & 0xf8u) |
                            (ppu->scrollPrev2 & 7u)) & 0x3ffu);
            ppu->scrollPrev = ppu->scrollPrev2 = value; break;
        case 0x0e: ppu->m7matrix[7] = (int16_t)(((uint16_t)value << 8 |
                         ppu->m7prev) & 0x1fffu); ppu->m7prev = value;
                   /* fall through */
        case 0x10: case 0x12: case 0x14:
            ppu->vScroll[(address - 0x0e) / 2] =
                (uint16_t)(((uint16_t)value << 8 | ppu->scrollPrev) & 0x3ffu);
            ppu->scrollPrev = value; break;
        case 0x15:
            ppu->vramIncrement = (value & 3u) == 0u ? 1u :
                                 (value & 3u) == 1u ? 32u : 128u;
            ppu->vramRemapMode = (value >> 2) & 3u;
            ppu->vramIncrementOnHigh = (value & 0x80u) != 0u; break;
        case 0x16: ppu->vramPointer = (ppu->vramPointer & 0xff00u) | value;
                   ppu->vramReadBuffer = ppu->vram[remap_vram(ppu) & 0x7fffu]; break;
        case 0x17: ppu->vramPointer = (ppu->vramPointer & 0x00ffu) |
                                           ((uint16_t)value << 8);
                   ppu->vramReadBuffer = ppu->vram[remap_vram(ppu) & 0x7fffu]; break;
        case 0x18: {
            uint16_t index = remap_vram(ppu) & 0x7fffu;
            uint8_t old_value = (uint8_t)ppu->vram[index];
            ppu->vram[index] = (ppu->vram[index] & 0xff00u) | value;
            if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE))
                sr_runner_emit_ppu_memory_write(
                    ppu, SR_MEMORY_VRAM, (uint32_t)index * 2u,
                    old_value, value, 1u);
            if (!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
            break;
        }
        case 0x19: {
            uint16_t index = remap_vram(ppu) & 0x7fffu;
            uint8_t old_value = (uint8_t)(ppu->vram[index] >> 8);
            ppu->vram[index] = (ppu->vram[index] & 0x00ffu) |
                               ((uint16_t)value << 8);
            if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE))
                sr_runner_emit_ppu_memory_write(
                    ppu, SR_MEMORY_VRAM,
                    (uint32_t)index * 2u + 1u,
                    old_value, value, 1u);
            if (ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
            break;
        }
        case 0x1a: ppu->m7sel = value; break;
        case 0x1b: case 0x1c: case 0x1d: case 0x1e:
            ppu->m7matrix[address - 0x1b] =
                (int16_t)((uint16_t)value << 8 | ppu->m7prev);
            ppu->m7prev = value; break;
        case 0x1f: case 0x20:
            ppu->m7matrix[address - 0x1b] =
                (int16_t)(((uint16_t)value << 8 | ppu->m7prev) & 0x1fffu);
            ppu->m7prev = value; break;
        case 0x21: ppu->cgramPointer = value; ppu->cgramSecondWrite = false; break;
        case 0x22:
            if (!ppu->cgramSecondWrite) ppu->cgramBuffer = value;
            else {
                unsigned index = ppu->cgramPointer++;
                uint16_t new_value = (uint16_t)(ppu->cgramBuffer |
                    ((uint16_t)(value & 0x7fu) << 8));
                if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
                    uint16_t old_value = ppu->cgram[index];
                    ppu->cgram[index] = new_value;
                    sr_runner_emit_ppu_memory_write(
                        ppu, SR_MEMORY_CGRAM, (uint32_t)index * 2u,
                        old_value, new_value, 2u);
                } else {
                    ppu->cgram[index] = new_value;
                }
                if (ppu->cgramRgbValid)
                    ppu->cgramRgb[index] = color_rgb(ppu, ppu->cgram[index]);
            }
            ppu->cgramSecondWrite = !ppu->cgramSecondWrite; break;
        case 0x23: ppu->windowsel = (ppu->windowsel & ~0xffu) | value; break;
        case 0x24: ppu->windowsel = (ppu->windowsel & ~0xff00u) |
                                       ((uint32_t)value << 8); break;
        case 0x25: ppu->windowsel = (ppu->windowsel & ~0xff0000u) |
                                       ((uint32_t)value << 16); break;
        case 0x26: ppu->window1left = value; break;
        case 0x27: ppu->window1right = value; break;
        case 0x28: ppu->window2left = value; break;
        case 0x29: ppu->window2right = value; break;
        case 0x2a: ppu->wbgobjlog = (ppu->wbgobjlog & 0xff00u) | value; break;
        case 0x2b: ppu->wbgobjlog = (ppu->wbgobjlog & 0x00ffu) |
                                       ((uint16_t)value << 8); break;
        case 0x2c: ppu->screenEnabled[0] = value; break;
        case 0x2d: ppu->screenEnabled[1] = value; break;
        case 0x2e: ppu->screenWindowed[0] = value; break;
        case 0x2f: ppu->screenWindowed[1] = value; break;
        case 0x30: ppu->cgwsel = value; break;
        case 0x31: ppu->cgadsub = value; break;
        case 0x32:
            if (value & 0x20u) ppu->fixedColor =
                (ppu->fixedColor & ~0x001fu) | (value & 0x1fu);
            if (value & 0x40u) ppu->fixedColor =
                (ppu->fixedColor & ~0x03e0u) | ((uint16_t)(value & 0x1fu) << 5);
            if (value & 0x80u) ppu->fixedColor =
                (ppu->fixedColor & ~0x7c00u) | ((uint16_t)(value & 0x1fu) << 10);
            break;
        case 0x33: ppu->setini = value; break;
        default: break;
    }
}

int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags) {
    (void)ppu; (void)render_flags; return 1;
}

void PpuNoteSurfaceViewChange(Ppu *ppu) {
    if (ppu != NULL) note_surface_binding(ppu);
}
