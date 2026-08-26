#include "ppu.h"

#include "../debug_server.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
}

Ppu *ppu_init(void) {
    Ppu *ppu = (Ppu *)calloc(1u, sizeof(Ppu));
    if (ppu != NULL) {
        ppu->lastBrightnessMult = 0xffu;
        ppu->vramIncrement = 1u;
        reset_layer_policy(ppu);
    }
    return ppu;
}

void ppu_free(Ppu *ppu) { free(ppu); }

void ppu_reset(Ppu *ppu) {
    uint8_t *render;
    uint32_t render_pitch, flags;
    uint8_t *authentic;
    uint32_t authentic_pitch;
    uint8_t *overlays[kPpuOverlaySource_Count];
    uint32_t overlay_pitch[kPpuOverlaySource_Count];
    uint8_t *bands[kPpuOverlaySource_Count][3];
    uint8_t *m7;
    uint32_t m7_pitch;
    uint8_t m7_scale;
    if (ppu == NULL) return;
    render = ppu->renderBuffer;
    render_pitch = ppu->renderPitch;
    flags = ppu->renderFlags;
    authentic = ppu->authenticRenderBuffer;
    authentic_pitch = ppu->authenticRenderPitch;
    memcpy(overlays, ppu->overlayRenderBuffer, sizeof(overlays));
    memcpy(overlay_pitch, ppu->overlayRenderPitch, sizeof(overlay_pitch));
    memcpy(bands, ppu->overlayRenderBands, sizeof(bands));
    m7 = ppu->m7OverlayBuffer;
    m7_pitch = ppu->m7OverlayPitch;
    m7_scale = ppu->m7OverlayScale;
    memset(ppu, 0, sizeof(*ppu));
    ppu->renderBuffer = render;
    ppu->renderPitch = render_pitch;
    ppu->renderFlags = flags;
    ppu->authenticRenderBuffer = authentic;
    ppu->authenticRenderPitch = authentic_pitch;
    memcpy(ppu->overlayRenderBuffer, overlays, sizeof(overlays));
    memcpy(ppu->overlayRenderPitch, overlay_pitch, sizeof(overlay_pitch));
    memcpy(ppu->overlayRenderBands, bands, sizeof(bands));
    ppu->m7OverlayBuffer = m7;
    ppu->m7OverlayPitch = m7_pitch;
    ppu->m7OverlayScale = m7_scale;
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
    info->func(info, version, sizeof(version));
    info->func(info, &ppu->inidisp, PPU_SAVESTATE_REGS_SIZE);
    info->func(info, &ppu->cgram, PPU_SAVESTATE_MEM_SIZE);
    /* Derived geometry is deliberately outside the serialized contract. */
    ppu->objScanlineMasksValid = false;
    ppu->cgramRgbValid = false;
}

void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch,
                     uint32_t render_flags) {
    if (ppu == NULL) return;
    ppu->renderBuffer = pixels;
    ppu->renderPitch = (uint32_t)pitch;
    ppu->renderFlags = render_flags;
    /* The struct remains intentionally inspectable to host enhancements.
     * Refresh the derived palette at output binding so direct CGRAM edits are
     * visible without a write barrier in the scanline hot path. */
    ppu->cgramRgbValid = false;
}

bool PpuBindAuthenticSurface(Ppu *ppu, uint8_t *pixels, size_t pitch) {
    size_t minimum;
    if (ppu == NULL) return false;
    minimum = kPpuXPixels + (size_t)ppu->extraLeftRight * 2u;
    if (pixels != NULL && (pitch == 0u || pitch % 4u != 0u ||
        pitch / 4u < minimum || pitch / 4u > kPpuSurfaceWidth)) return false;
    ppu->authenticRenderBuffer = pixels;
    ppu->authenticRenderPitch = pixels != NULL ? (uint32_t)pitch : 0u;
    return true;
}

bool PpuAuthenticSurfaceBound(const Ppu *ppu) {
    return ppu != NULL && ppu->authenticRenderBuffer != NULL &&
           ppu->authenticRenderPitch != 0u;
}

bool PpuAuthenticSurfaceReady(const Ppu *ppu) {
    size_t width, required;
    if (!PpuAuthenticSurfaceBound(ppu)) return false;
    width = ppu->authenticRenderPitch / 4u;
    required = kPpuXPixels + (size_t)ppu->extraLeftRight * 2u;
    return width >= required && width <= kPpuSurfaceWidth;
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
    memset(ppu->overlayRenderBands, 0, sizeof(ppu->overlayRenderBands));
    memset(ppu->overlayRenderContentMask, 0,
           sizeof(ppu->overlayRenderContentMask));
    PpuClearOverlayCaptures(ppu);
}

bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch) {
    if (ppu == NULL || (unsigned)source >= kPpuOverlaySource_Count ||
        (pixels != NULL && (pitch == 0u || pitch % 4u != 0u ||
         pitch / 4u < kPpuXPixels || pitch / 4u > kPpuSurfaceWidth))) return false;
    ppu->overlayRenderBuffer[source] = pixels;
    ppu->overlayRenderPitch[source] = pixels != NULL ? (uint32_t)pitch : 0u;
    memset(ppu->overlayRenderBands[source], 0,
           sizeof(ppu->overlayRenderBands[source]));
    ppu->overlayRenderContentMask[source] = 0u;
    ppu->overlayRenderMaybeDirty[source] = pixels != NULL;
    if (pixels == NULL) memset(&ppu->overlayCaptures[source], 0,
                               sizeof(ppu->overlayCaptures[source]));
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
        uint64_t bit = UINT64_C(1) << (slot & 63u);
        unsigned word = slot >> 6;
        first = clamp_int(first, -kPpuExtraTopBottom,
                          kPpuYPixels + kPpuExtraTopBottom);
        end = clamp_int(end, -kPpuExtraTopBottom,
                        kPpuYPixels + kPpuExtraTopBottom);
        for (int y = first; y < end; ++y)
            ppu->objScanlineMasks[y + kPpuExtraTopBottom][word] |= bit;
    }
    ppu->objScanlineMasksValid = true;
}

static const uint64_t *obj_scanline_masks(Ppu *ppu, int screen_y) {
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
        (ppu->wsBg3WidenY == 0u || row < ppu->wsBg3WidenY) &&
        ppu->wsHudSplitHeight == 0u) return false;
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

static bool sample_mode7(Ppu *ppu, int layer, int x, int y, SrPpuPixel *out) {
    int sx = x - 128;
    int sy = y - 128;
    int a = ppu->m7matrix[0], b = ppu->m7matrix[1];
    int c = ppu->m7matrix[2], d = ppu->m7matrix[3];
    int cx = sign13((uint16_t)ppu->m7matrix[4]);
    int cy = sign13((uint16_t)ppu->m7matrix[5]);
    int h = sign13((uint16_t)ppu->m7matrix[6]);
    int v = sign13((uint16_t)ppu->m7matrix[7]);
    int px, py, tile, pixel;
    if (a == 0 && d == 0) a = d = 0x100;
    if (PPU_m7xFlip(ppu)) sx = -sx;
    if (PPU_m7yFlip(ppu)) sy = -sy;
    px = ((a * sx + b * sy) >> 8) + cx + h;
    py = ((c * sx + d * sy) >> 8) + cy + v;
    if (PPU_m7largeField(ppu) && (px < 0 || py < 0 || px >= 1024 || py >= 1024)) {
        if (!PPU_m7charFill(ppu)) return false;
        px &= 7; py &= 7; tile = 0;
    } else {
        px &= 1023; py &= 1023;
        tile = ppu->vram[((py >> 3) * 128 + (px >> 3)) & 0x7fff] & 0xff;
    }
    pixel = ppu->vram[(tile * 64 + (py & 7) * 8 + (px & 7)) & 0x7fff] >> 8;
    if (pixel == 0) return false;
    out->valid = true; out->color = ppu->cgram[pixel]; out->layer = (uint8_t)layer;
    out->priority = layer == 1 ? (pixel >> 7) : 0;
    out->rank = layer_rank(ppu, layer, out->priority);
    out->palette = (uint8_t)pixel; out->band = 0xffu;
    return true;
}

static bool sample_bg(Ppu *ppu, int layer, int screen_x, int screen_y,
                      bool overlay_padding, SrPpuPixel *out) {
    int source_x, bpp, tile_size, map_x, map_y, in_x, in_y;
    int map_address, tile_address, pixel, palette;
    uint16_t entry;
    PpuVirtualTilemapBinding *binding;
    bool use_virtual;
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
    use_virtual = PPU_mode(ppu) == 1 && binding->lookup != NULL &&
        (screen_x < 0 || screen_x >= kPpuXPixels || screen_y < 0 ||
         screen_y >= kPpuYPixels ||
         (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u);
    if (use_virtual) {
        int world_x = binding->camera_x + source_x +
            wrapped_delta10(ppu->hScroll[layer], binding->hscroll_anchor);
        int world_y = binding->camera_y +
            clamp_int(sample_y, 0, kPpuYPixels - 1) +
            wrapped_delta10(ppu->vScroll[layer], binding->vscroll_anchor);
        map_x = floor_div8(world_x); map_y = floor_div8(world_y);
        in_x = world_x - map_x * 8; in_y = world_y - map_y * 8;
        if (!binding->lookup(binding->context, map_x, map_y, &entry)) return false;
        if (binding->band_lookup != NULL)
            (void)binding->band_lookup(binding->context, map_x, map_y, entry, &band);
    } else {
        int world_x = source_x + ppu->hScroll[layer];
        int world_y = sample_y + ppu->vScroll[layer];
        tile_size = PPU_bigTiles(ppu, layer) ? 16 : 8;
        if (world_x >= 0 && world_y >= 0) {
            int shift = tile_size == 16 ? 4 : 3;
            map_x = world_x >> shift;
            map_y = world_y >> shift;
            in_x = world_x & (tile_size - 1);
            in_y = world_y & (tile_size - 1);
        } else {
            map_x = floor_div8(world_x) / (tile_size / 8);
            map_y = floor_div8(world_y) / (tile_size / 8);
            in_x = ((world_x % tile_size) + tile_size) % tile_size;
            in_y = ((world_y % tile_size) + tile_size) % tile_size;
        }
        map_address = PPU_bgTilemapAdr(ppu, layer) + (map_x & 31) +
                      ((map_y & 31) << 5);
        if ((map_x & 32) != 0) map_address += 0x400;
        if ((map_y & 32) != 0)
            map_address += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
        entry = ppu->vram[map_address & 0x7fff];
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
static unsigned lowest_set_bit_index64(uint64_t value) {
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

static unsigned append_obj_slots(uint64_t mask, unsigned base,
                                 uint8_t *slots, unsigned count) {
    while (mask != 0u) {
        unsigned bit = lowest_set_bit_index64(mask);
        slots[count++] = (uint8_t)(base + bit);
        mask &= mask - 1u;
    }
    return count;
}

static void build_obj_sample_cache(Ppu *ppu, PpuObjSampleCache *cache,
                                   int screen_y, int x_offset) {
    unsigned start = (ppu->oamaddh & 0x80u) != 0u
        ? ppu->oamaddl >> 1 : 0u;
    const uint64_t *eligible = obj_scanline_masks(ppu, screen_y);
    uint8_t ordered_slots[128];
    unsigned slot_count = 0u;
    bool margin = screen_y < 0 || screen_y >= kPpuYPixels;
    memset(cache->pixels.data, 0, sizeof(cache->pixels.data));
    memset(cache->opaque, 0, sizeof(cache->opaque));
    if (eligible == NULL) goto finish;
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
    for (unsigned step = 0; step < slot_count; ++step) {
        unsigned slot = ordered_slots[step];
        PpuObjPart part;
        uint8_t rank;
        int palette_base, local_y, tile_y, tile_row;
        int tile_columns, tile_address, base_tile;
        bool hflip;
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
                        cache->opaque[(unsigned)x >> 6] |=
                            UINT64_C(1) << ((unsigned)x & 63u);
                }
            }
        }
    }
finish:
    cache->screen_y = (int16_t)screen_y;
    cache->x_offset = (int16_t)x_offset;
    cache->valid = true;
}

static PpuObjSampleCache *get_obj_sample_cache(Ppu *ppu, int screen_y,
                                                int x_offset) {
    PpuObjSampleCache *cache = NULL;
    for (int slot = 0; slot < kPpuObjSampleCacheCount; ++slot) {
        PpuObjSampleCache *candidate = &ppu->objSampleCache[slot];
        if (candidate->valid && candidate->screen_y == screen_y &&
            candidate->x_offset == x_offset) {
            cache = candidate;
            break;
        }
        if (!candidate->valid && cache == NULL) cache = candidate;
    }
    if (cache != NULL && !cache->valid)
        build_obj_sample_cache(ppu, cache, screen_y, x_offset);
    return cache;
}

static bool sample_obj_cached(Ppu *ppu, int screen_x, int screen_y,
                              int x_offset, SrPpuPixel *out, bool *handled) {
    PpuObjSampleCache *cache;
    int index = screen_x + kPpuExtraLeftRight;
    *handled = true;
    if (index < 0 || index >= kPpuBufWidth) return false;
    cache = get_obj_sample_cache(ppu, screen_y, x_offset);
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
    if (include_count == 0 && exclude_count == 0 && out_slot == NULL &&
        (ppu->renderFlags & kPpuRenderFlags_ReferencePixelRenderer) == 0u) {
        bool handled;
        bool found = sample_obj_cached(ppu, screen_x, screen_y, x_offset,
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

static void clear_overlay_row(Ppu *ppu, int source, int screen_y) {
    PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
    int row = overlay_row(capture, screen_y);
    uint32_t fill;
    if (ppu->overlayRenderBuffer[source] == NULL || row < 0 ||
        row >= kPpuBufHeight) return;
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
    if (capture->oamCount == 0u || !capture_active(capture, x, y) ||
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
        removed = honor_removal && capture_active(cap, x, y) &&
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
        } else if (honor_removal && capture_active(cap, x, y) &&
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
        removed = honor_removal && capture_active(cap, x, y) &&
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
            if (honor_removal && capture_active(cap, x, y) &&
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

static bool native_capture_intersects(const PpuOverlayCapture *capture,
                                      int screen_y) {
    return capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
           capture->x1 > 0 && capture->x0 < kPpuXPixels &&
           screen_y >= capture->y0 && screen_y < capture->y1;
}

static bool native_fast_eligible(const Ppu *ppu, int screen_y,
                                 bool capture) {
    uint8_t visible_layers = ppu->screenEnabled[0] | ppu->screenEnabled[1];
    int mode = PPU_mode(ppu);
    if ((ppu->renderFlags & kPpuRenderFlags_ReferencePixelRenderer) != 0u ||
        screen_y < 0 || screen_y >= kPpuYPixels)
        return false;
    for (int layer = 0; layer < 4; ++layer) {
        const PpuVirtualTilemapBinding *binding = &ppu->virtualTilemap[layer];
        if (bpp_for_mode(mode, layer) == 0 &&
            !(mode == 7 && layer == 1 && PPU_m7extBg(ppu))) continue;
        if ((visible_layers & (1u << layer)) != 0u &&
            PPU_mosaicEnabled(ppu, layer) && PPU_mosaicSize(ppu) > 1)
            return false;
        if (mode == 1 && binding->lookup != NULL &&
            (binding->flags & kPpuVirtualTilemapFlag_IncludeAuthentic) != 0u)
            return false;
    }
    if (!capture) return true;
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        if (native_capture_intersects(&ppu->overlayCaptures[source], screen_y))
            return false;
    }
    return ppu->objRangeCapture.count == 0u ||
           ppu->objRangeCapture.x1 <= 0 ||
           ppu->objRangeCapture.x0 >= kPpuXPixels ||
           screen_y < ppu->objRangeCapture.y0 ||
           screen_y >= ppu->objRangeCapture.y1;
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

/* Mode 7's transform changes linearly across a scanline.  Advance the two
 * affine numerators instead of recomputing four multiplies for every layer,
 * screen, and pixel as the general reference sampler must. */
static void native_resolve_mode7(Ppu *SR_RESTRICT ppu, int screen_y,
                                 bool want_sub,
                                 uint16_t *SR_RESTRICT main_pixels,
                                 uint16_t *SR_RESTRICT sub_pixels) {
    NativeLayerWindowPlan plans[2];
    int a = ppu->m7matrix[0], b = ppu->m7matrix[1];
    int c = ppu->m7matrix[2], d = ppu->m7matrix[3];
    int cx = sign13((uint16_t)ppu->m7matrix[4]);
    int cy = sign13((uint16_t)ppu->m7matrix[5]);
    int h = sign13((uint16_t)ppu->m7matrix[6]);
    int v = sign13((uint16_t)ppu->m7matrix[7]);
    int sx = -128;
    int sy = screen_y + 1 - 128;
    int x_step, y_step, x_numerator, y_numerator;
    unsigned rank0 = layer_rank(ppu, 0, 0);
    unsigned rank1[2];
    bool extbg = PPU_m7extBg(ppu);
    bool layer0_visible, layer1_visible;
    if (a == 0 && d == 0) a = d = 0x100;
    if (PPU_m7xFlip(ppu)) sx = -sx;
    if (PPU_m7yFlip(ppu)) sy = -sy;
    x_step = PPU_m7xFlip(ppu) ? -a : a;
    y_step = PPU_m7xFlip(ppu) ? -c : c;
    x_numerator = a * sx + b * sy;
    y_numerator = c * sx + d * sy;
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
         ++x, x_numerator += x_step, y_numerator += y_step) {
        int px = (x_numerator >> 8) + cx + h;
        int py = (y_numerator >> 8) + cy + v;
        int tile, pixel;
        bool inside;
        uint16_t packed;
        if (PPU_m7largeField(ppu) &&
            (px < 0 || py < 0 || px >= 1024 || py >= 1024)) {
            if (!PPU_m7charFill(ppu)) continue;
            px &= 7;
            py &= 7;
            tile = 0;
        } else {
            px &= 1023;
            py &= 1023;
            tile = ppu->vram[
                ((py >> 3) * 128 + (px >> 3)) & 0x7fff] & 0xff;
        }
        pixel = ppu->vram[
            (tile * 64 + (py & 7) * 8 + (px & 7)) & 0x7fff] >> 8;
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

static void native_resolve_bg(Ppu *SR_RESTRICT ppu, int layer, int screen_y,
                              bool want_sub,
                              uint16_t *SR_RESTRICT main_pixels,
                              uint16_t *SR_RESTRICT sub_pixels) {
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
    int x = 0;
    if ((!main_always && !main_variable) &&
        (!sub_always && !sub_variable)) return;
    if (main_variable || sub_variable)
        native_window_runs(ppu, layer, &window_runs);
    while (x < kPpuXPixels) {
        int world_x = x + ppu->hScroll[layer];
        int map_x = world_x >> tile_shift;
        int in_x = world_x & (tile_size - 1);
        int map_address = PPU_bgTilemapAdr(ppu, layer) + (map_x & 31) +
                          ((map_y & 31) << 5);
        int sample_y = in_y;
        int run = 8 - (world_x & 7);
        uint16_t entry;
        uint64_t decoded_pixels = 0u;
        unsigned palette_base, rank;
        int tile, tile_base;
        if ((map_x & 32) != 0) map_address += 0x400;
        if ((map_y & 32) != 0)
            map_address += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
        entry = ppu->vram[map_address & 0x7fff];
        if ((entry & 0x8000u) != 0u) sample_y = tile_size - 1 - sample_y;
        {
            int sample_x = (entry & 0x4000u) != 0u
                ? tile_size - 1 - in_x : in_x;
            tile = (entry & 0x3ffu) + (sample_x >> 3) +
                   ((sample_y >> 3) << 4);
        }
        tile_base = PPU_bgTileAdr(ppu, layer) + tile * bpp * 4;
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
            x += 8;
            continue;
        }
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
    cache = get_obj_sample_cache(ppu, screen_y, obj_offset);
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
        for (unsigned word = (unsigned)left >> 6;
             word <= ((unsigned)right - 1u) >> 6; ++word) {
            unsigned word_x = word << 6;
            uint64_t opaque = cache->opaque[word];
            if ((unsigned)left > word_x)
                opaque &= ~UINT64_C(0) << ((unsigned)left - word_x);
            if ((unsigned)right < word_x + 64u)
                opaque &= (UINT64_C(1) << ((unsigned)right - word_x)) - 1u;
            while (opaque != 0u) {
                int x = (int)(word_x + lowest_set_bit_index64(opaque));
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

static bool render_native_fast_line(Ppu *ppu, int screen_y,
        uint32_t *row, int origin, bool capture, bool authentic,
        bool dual_authentic, uint32_t *authentic_row, int authentic_origin) {
    uint16_t main_pixels[kPpuXPixels];
    uint16_t sub_pixels[kPpuXPixels];
    uint16_t backdrop = native_pack_pixel(0u, 1u, 5u);
    bool want_sub;
    NativeWindowRuns color_runs;
    int obj_offset = authentic ? ppu->authenticObjOffsetX : 0;
    if (!native_fast_eligible(ppu, screen_y, capture)) return false;
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
                                  main_pixels, sub_pixels);
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
        if (!capture_active(capture, x, y)) continue;
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
    if (!PpuAuthenticSurfaceReady(ppu) || screen_y < 0 ||
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
                           size_t pitch, bool capture, bool authentic) {
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
    if (buffer == NULL || pitch == 0u || row_index < 0 || row_index >= kPpuBufHeight)
        return false;
    row = (uint32_t *)(buffer + (size_t)row_index * pitch);
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
            if (screen_y < cap->y0 || screen_y >= cap->y1) continue;
            if (capture_is_deferred(cap)) deferred_capture = true;
            if ((cap->flags & kPpuOverlayFlag_MarkFullAddSubscreen) != 0u)
                full_add_capture = true;
        }
    }
    if (left == 0 && right == kPpuXPixels &&
        render_native_fast_line(ppu, screen_y, row, origin, capture,
                                authentic, dual_authentic, authentic_row,
                                authentic_origin))
        return dual_authentic;
    for (int x = left; x < right; ++x) {
        int obj_offset = authentic ? ppu->authenticObjOffsetX : 0;
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
                int excluded = capture_active(
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
                         ppu->authenticRenderPitch, false, true);
    ppu->hScroll[0] = saved_h0; ppu->hScroll[1] = saved_h1;
    ppu->extraLeftCur = saved_left; ppu->extraRightCur = saved_right;
    ppu->extraLeftRight = saved_budget;
}

static void render_line(Ppu *ppu, int line) {
    int screen_y = line - 1;
    for (int slot = 0; slot < kPpuObjSampleCacheCount; ++slot)
        ppu->objSampleCache[slot].valid = false;
    update_brightness(ppu);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source)
        clear_overlay_row(ppu, source, screen_y);
    if (ppu->objRangeCapture.count != 0u && screen_y >= ppu->objRangeCapture.y0 &&
        screen_y < ppu->objRangeCapture.y1)
        memset(ppu->objRangeCapture.pixels +
               (size_t)screen_y * ppu->objRangeCapture.pitch, 0,
               ppu->objRangeCapture.pitch);
    bool authentic_done = render_line_to(
        ppu, screen_y, ppu->renderBuffer, ppu->renderPitch, true, false);
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
                if (sample_bg(ppu, source, x, screen_y, true, &pixel) &&
                    capture_active(capture, x, screen_y))
                    write_overlay(ppu, source, x, screen_y, &pixel, 0u);
            }
            for (int x = kPpuXPixels + ppu->extraRightCur;
                 x < kPpuXPixels + ppu->extraLeftRight; ++x) {
                SrPpuPixel pixel = {0};
                if (sample_bg(ppu, source, x, screen_y, true, &pixel) &&
                    capture_active(capture, x, screen_y))
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
        memset(ppu->overlayRenderContentMask, 0,
               sizeof(ppu->overlayRenderContentMask));
        debug_server_on_oam_render();
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

uint8_t ppu_read(Ppu *ppu, uint8_t address) {
    uint8_t result = 0u;
    if (ppu == NULL) return 0u;
    switch (address) {
        case 0x34: case 0x35: case 0x36: {
            int product = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
            return (uint8_t)(product >> ((address - 0x34) * 8));
        }
        case 0x37:
            ppu->hCount = 0u; ppu->vCount = 0xc0u;
            ppu->hCountSecond = ppu->vCountSecond = false;
            ppu->countersLatched = true; return 0u;
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
                ppu->highOam[index] = value;
                ppu->objScanlineMasksValid = false;
                debug_server_on_oam_write(1, (uint16_t)index, value);
                if (ppu->oamSecondWrite && ++ppu->oamAdr == 0u) ppu->oamInHigh = false;
            } else if (!ppu->oamSecondWrite) ppu->oamBuffer = value;
            else {
                uint16_t word = (uint16_t)(ppu->oamBuffer | ((uint16_t)value << 8));
                debug_server_on_oam_write(0, ppu->oamAdr, word);
                ppu->oam[ppu->oamAdr++] = word;
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
            ppu->vram[index] = (ppu->vram[index] & 0xff00u) | value;
            debug_server_on_vram_write((uint32_t)index * 2u, value);
            if (!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
            break;
        }
        case 0x19: {
            uint16_t index = remap_vram(ppu) & 0x7fffu;
            ppu->vram[index] = (ppu->vram[index] & 0x00ffu) |
                               ((uint16_t)value << 8);
            debug_server_on_vram_write((uint32_t)index * 2u + 1u, value);
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
                ppu->cgram[index] = (uint16_t)(ppu->cgramBuffer |
                    ((uint16_t)(value & 0x7fu) << 8));
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
