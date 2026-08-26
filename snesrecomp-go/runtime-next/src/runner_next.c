#include "runner_next.h"

#include "runner_next_internal.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#include <string.h>

_Static_assert(SR_PPU_NATIVE_WIDTH == kPpuXPixels,
               "public ABI native width must match the PPU");
_Static_assert(SR_PPU_OBJ_X_WRAP == kPpuObjXWrap,
               "public ABI OBJ X wrap must match the PPU");
_Static_assert(SR_PPU_OBJ_Y_WRAP == kPpuObjYWrap,
               "public ABI OBJ Y wrap must match the PPU");
_Static_assert(SR_PPU_OBJ_Y_NEGATIVE_FROM == kPpuObjYNegativeFrom,
               "public ABI OBJ negative-Y band must match the PPU");
_Static_assert(SR_PPU_CGRAM_WORD_COUNT == kPpuCgramEntries,
               "public ABI CGRAM extent must match the PPU");
_Static_assert(SR_PPU_TILE_ID_COUNT == kPpuObjTileIds,
               "public ABI tile-id extent must match the PPU");
_Static_assert(SR_PPU_SURFACE_BAND_COUNT == 4u,
               "public ABI surface bands must cover primary plus 3 splits");

static SrRunnerCpuStateProvider *s_cpu_state_provider;
static const void *s_cpu_component;
static Snes *s_cpu_state_runner;
static SrRunnerPpuObjRasterProvider *s_ppu_obj_raster_provider;
static Snes *s_ppu_obj_raster_runner;

static Snes *runner_from_handle(SrRunnerHandle *runner) {
    return (Snes *)(void *)runner;
}

static const SrComponentHandle *component_handle(const void *component) {
    return (const SrComponentHandle *)component;
}

static SrResult get_component(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component) {
    Snes *snes = runner_from_handle(runner);
    const void *resolved = NULL;
    if (out_component == NULL) return SR_RESULT_INVALID_ARGUMENT;
    *out_component = NULL;
    if (snes == NULL) return SR_RESULT_INVALID_ARGUMENT;
    switch (component) {
        case SR_COMPONENT_RUNNER: resolved = snes; break;
        case SR_COMPONENT_CPU:
            resolved = s_cpu_state_runner == snes && s_cpu_component != NULL
                ? s_cpu_component : snes->cpu;
            break;
        case SR_COMPONENT_PPU: resolved = snes->ppu; break;
        case SR_COMPONENT_APU: resolved = snes->apu; break;
        case SR_COMPONENT_DSP:
            resolved = snes->apu != NULL ? snes->apu->dsp : NULL;
            break;
        case SR_COMPONENT_SPC:
            resolved = snes->apu != NULL ? snes->apu->spc : NULL;
            break;
        case SR_COMPONENT_DMA: resolved = snes->dma; break;
        case SR_COMPONENT_CARTRIDGE: resolved = snes->cart; break;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
    *out_component = component_handle(resolved);
    return resolved != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult query_generations(
        SrRunnerHandle *runner, SrGenerationSnapshot *out_generations) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_generations == NULL ||
        out_generations->struct_size < SR_GENERATION_SNAPSHOT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    out_generations->struct_size = SR_GENERATION_SNAPSHOT_V1_SIZE;
    out_generations->reserved = 0u;
    out_generations->lifetime_generation = snes->abiLifetimeGeneration;
    out_generations->tick_generation = snes->abiTickGeneration;
    out_generations->reset_generation = snes->abiResetGeneration;
    out_generations->load_generation = snes->abiLoadGeneration;
    out_generations->mutation_generation = snes->abiMutationGeneration;
    return SR_RESULT_OK;
}

static SrResult resolve_memory(Snes *snes, SrMemoryRegion region,
                               const uint8_t **data, uint64_t *byte_size) {
    if (snes == NULL || data == NULL || byte_size == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    switch (region) {
        case SR_MEMORY_WRAM:
            *data = snes->ram;
            *byte_size = kSnesWramSize;
            break;
        case SR_MEMORY_SRAM:
            if (snes->cart == NULL || snes->cart->ram == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->ram;
            *byte_size = snes->cart->ramSize;
            break;
        case SR_MEMORY_ROM:
            if (snes->cart == NULL || snes->cart->rom == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->rom;
            *byte_size = snes->cart->romSize;
            break;
        case SR_MEMORY_HIGH_OAM:
            if (snes->ppu == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->ppu->highOam;
            *byte_size = SR_PPU_HIGH_OAM_BYTE_COUNT;
            break;
        case SR_MEMORY_APU_RAM:
        case SR_MEMORY_DSP_REGISTERS:
            /* The audio thread can advance these components between main
             * runner ticks. They need an APU-lock-aware pinned snapshot, not
             * a borrowed pointer with a misleading main-thread generation. */
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
        default:
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return *data != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult borrow_memory(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_SPAN_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_memory(snes, region, &data, &byte_size);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_SPAN_V1_SIZE);
        out_span->struct_size = SR_BORROWED_SPAN_V1_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_SPAN_V1_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->byte_size = byte_size;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_is_valid(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_SPAN_V1_SIZE || span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_memory(snes, span->region, &data, &byte_size) != SR_RESULT_OK)
        return 0u;
    return span->data == data && span->byte_size == byte_size ? 1u : 0u;
}

static SrResult resolve_u16_memory(Snes *snes, SrMemoryRegion region,
                                   const uint16_t **data,
                                   uint64_t *element_count) {
    if (snes == NULL || data == NULL || element_count == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (snes->ppu == NULL) {
        *data = NULL;
        *element_count = 0u;
        return SR_RESULT_UNAVAILABLE;
    }
    switch (region) {
        case SR_MEMORY_VRAM:
            *data = snes->ppu->vram;
            *element_count = SR_PPU_VRAM_WORD_COUNT;
            break;
        case SR_MEMORY_CGRAM:
            *data = snes->ppu->cgram;
            *element_count = SR_PPU_CGRAM_WORD_COUNT;
            break;
        case SR_MEMORY_OAM:
            *data = snes->ppu->oam;
            *element_count = SR_PPU_OAM_WORD_COUNT;
            break;
        default:
            *data = NULL;
            *element_count = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return SR_RESULT_OK;
}

static SrResult borrow_u16_memory(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_U16_SPAN_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_u16_memory(snes, region, &data, &element_count);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_U16_SPAN_V1_SIZE);
        out_span->struct_size = SR_BORROWED_U16_SPAN_V1_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_U16_SPAN_V1_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->element_count = element_count;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_u16_is_valid(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_U16_SPAN_V1_SIZE ||
        span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_u16_memory(snes, span->region, &data, &element_count) !=
        SR_RESULT_OK)
        return 0u;
    return span->data == data && span->element_count == element_count ? 1u : 0u;
}

static SrResult query_cpu_state(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_CPU_STATE_SNAPSHOT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_CPU_STATE_SNAPSHOT_V1_SIZE);
    out_state->struct_size = SR_CPU_STATE_SNAPSHOT_V1_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_cpu_state_runner != snes || s_cpu_state_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_cpu_state_provider(snes, out_state);
}

static uint8_t ppu_background_bpp(uint8_t mode, unsigned layer,
                                  uint32_t flags) {
    static const uint8_t bpp[7][4] = {
        {2u, 2u, 2u, 2u}, {4u, 4u, 2u, 0u}, {4u, 4u, 0u, 0u},
        {8u, 4u, 0u, 0u}, {8u, 2u, 0u, 0u}, {4u, 2u, 0u, 0u},
        {4u, 0u, 0u, 0u},
    };
    if (layer >= 4u) return 0u;
    if (mode < 7u) return bpp[mode][layer];
    if (mode == 7u && layer == 0u) return 8u;
    if (mode == 7u && layer == 1u &&
        (flags & SR_PPU_STATE_MODE7_EXT_BG) != 0u) return 8u;
    return 0u;
}

static SrResult query_ppu_state(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned layer;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_STATE_SNAPSHOT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_STATE_SNAPSHOT_V1_SIZE);
    out_state->struct_size = SR_PPU_STATE_SNAPSHOT_V1_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_state->flags =
        (PPU_forcedBlank(ppu) ? SR_PPU_STATE_FORCED_BLANK : 0u) |
        (PPU_bg3priority(ppu) ? SR_PPU_STATE_BG3_PRIORITY : 0u) |
        (PPU_interlace(ppu) ? SR_PPU_STATE_INTERLACE : 0u) |
        (PPU_objInterlace(ppu) ? SR_PPU_STATE_OBJ_INTERLACE : 0u) |
        (PPU_overscan(ppu) ? SR_PPU_STATE_OVERSCAN : 0u) |
        (PPU_pseudoHires(ppu) ? SR_PPU_STATE_PSEUDO_HIRES : 0u) |
        (PPU_m7extBg(ppu) ? SR_PPU_STATE_MODE7_EXT_BG : 0u);
    out_state->display_control = ppu->inidisp;
    out_state->object_select = ppu->obsel;
    out_state->bg_mode_control = ppu->bgmode;
    out_state->mosaic_control = ppu->mosaic;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->brightness = (uint8_t)PPU_brightness(ppu);
    out_state->main_screen = ppu->screenEnabled[0];
    out_state->sub_screen = ppu->screenEnabled[1];
    out_state->main_windowed = ppu->screenWindowed[0];
    out_state->sub_windowed = ppu->screenWindowed[1];
    out_state->object_size_select = (uint8_t)PPU_objSize(ppu);
    out_state->margin_left = ppu->extraLeftCur;
    out_state->margin_right = ppu->extraRightCur;
    out_state->margin_top = ppu->extraTopCur;
    out_state->margin_bottom = ppu->extraBottomCur;
    out_state->object_tile_base_1_word = (uint32_t)PPU_objTileAdr1(ppu);
    out_state->object_tile_base_2_word = (uint32_t)PPU_objTileAdr2(ppu);
    for (layer = 0u; layer < 4u; ++layer) {
        SrPpuBackgroundState *background = &out_state->backgrounds[layer];
        background->h_scroll = ppu->hScroll[layer];
        background->v_scroll = ppu->vScroll[layer];
        background->tilemap_base_word =
            (uint16_t)PPU_bgTilemapAdr(ppu, layer);
        background->tile_base_word = (uint16_t)PPU_bgTileAdr(ppu, layer);
        background->tilemap_width_tiles =
            PPU_bgTilemapWider(ppu, layer) ? 64u : 32u;
        background->tilemap_height_tiles =
            PPU_bgTilemapHigher(ppu, layer) ? 64u : 32u;
        background->tile_size_pixels = PPU_bigTiles(ppu, layer) ? 16u : 8u;
        background->bits_per_pixel = ppu_background_bpp(
            out_state->bg_mode, layer, out_state->flags);
    }
    return SR_RESULT_OK;
}

static uint8_t ppu_frame_color_component(uint16_t value,
                                         uint8_t brightness) {
    uint32_t expanded = ((uint32_t)value << 3) | ((uint32_t)value >> 2);
    return (uint8_t)((expanded * brightness) / 15u);
}

static uint32_t ppu_frame_color_argb(const Ppu *ppu, uint16_t color) {
    uint8_t brightness = (uint8_t)PPU_brightness(ppu);
    return UINT32_C(0xff000000) |
        ((uint32_t)ppu_frame_color_component(color & 0x1fu, brightness) << 16) |
        ((uint32_t)ppu_frame_color_component((color >> 5) & 0x1fu,
                                             brightness) << 8) |
        ppu_frame_color_component((color >> 10) & 0x1fu, brightness);
}

static uint32_t ppu_overlay_content_mask(const Ppu *ppu, unsigned source) {
    uint32_t mask = 0u;
    unsigned band;
    if (ppu->overlayRenderBuffer[source] != NULL &&
        (ppu->overlayRenderContentMask[source] & 1u) != 0u)
        mask |= 1u;
    for (band = 1u; band <= 3u; ++band) {
        if (ppu->overlayRenderBands[source][band - 1u] != NULL &&
            (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u)
            mask |= 1u << band;
    }
    return mask;
}

static uint32_t ppu_overlay_fill_argb(
        const Ppu *ppu, const PpuOverlayCapture *capture) {
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Black)
        return UINT32_C(0xff000000);
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Cgram)
        return ppu_frame_color_argb(
            ppu, ppu->cgram[capture->transparentFillCgram]);
    return 0u;
}

static SrResult query_ppu_frame_state(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned source;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_FRAME_SNAPSHOT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_FRAME_SNAPSHOT_V1_SIZE);
    out_state->struct_size = SR_PPU_FRAME_SNAPSHOT_V1_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    _Static_assert(SR_PPU_OVERLAY_SOURCE_COUNT == kPpuOverlaySource_Count,
                   "ABI overlay count must match the PPU");
    _Static_assert(SR_PPU_OVERLAY_REMOVE_FROM_GAME ==
                       kPpuOverlayFlag_RemoveFromGame &&
                   SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH ==
                       kPpuOverlayFlag_MarkObjColorMath &&
                   SR_PPU_OVERLAY_MARK_BG_HALF_ADD ==
                       kPpuOverlayFlag_MarkBgHalfAdd &&
                   SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT ==
                       kPpuOverlayFlag_ApplyBgFixedColorSubtract &&
                   SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN ==
                       kPpuOverlayFlag_MarkFullAddSubscreen &&
                   SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkMainScreenWinner &&
                   SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkOwningScreenWinner,
                   "ABI overlay flags must match the PPU");
    out_state->display_control = ppu->inidisp;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->hud_split_height = ppu->wsHudSplitHeight;
    out_state->hud_left_end = ppu->wsHudLeftEnd;
    out_state->hud_right_start = ppu->wsHudRightStart;
    out_state->hud_player_row_y = ppu->wsHudPlayerRowY;
    out_state->hud_left_only_y = ppu->wsHudLeftOnlyY;
    out_state->margin_budget = ppu->extraLeftRight;
    out_state->mode7_override_active = ppu->m7Override.rgba != NULL ? 1u : 0u;
    out_state->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        SrPpuOverlayState *overlay = &out_state->overlays[source];
        overlay->x0 = capture->x0;
        overlay->x1 = capture->x1;
        overlay->y0 = capture->y0;
        overlay->y1 = capture->y1;
        overlay->flags = capture->flags;
        overlay->content_band_mask = ppu_overlay_content_mask(ppu, source);
        overlay->transparent_fill_argb =
            ppu_overlay_fill_argb(ppu, capture);
        overlay->transparent_fill_configured =
            capture->transparentFillConfigured != 0u ? 1u : 0u;
        overlay->oam_first = capture->oamFirst;
        overlay->oam_count = capture->oamCount;
    }
    return SR_RESULT_OK;
}

static void ppu_surface_view_init(SrPpuSurfaceView *view,
                                  const uint8_t *data, uint32_t pitch,
                                  uint32_t height, int32_t origin_x,
                                  int32_t origin_y, uint32_t scale,
                                  bool has_content) {
    if (view == NULL || data == NULL || pitch == 0u ||
        pitch % sizeof(uint32_t) != 0u || height == 0u) return;
    view->flags = SR_PPU_SURFACE_BOUND |
        (has_content ? SR_PPU_SURFACE_HAS_CONTENT : 0u);
    view->pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32;
    view->data = data;
    view->pitch_bytes = pitch;
    view->byte_size = (uint64_t)pitch * height;
    view->width_pixels = pitch / (uint32_t)sizeof(uint32_t);
    view->height_pixels = height;
    view->origin_x = origin_x;
    view->origin_y = origin_y;
    view->scale = scale;
}

static uint32_t ppu_overlay_surface_height(
        const PpuOverlayCapture *capture) {
    int height;
    if (capture == NULL || capture->x1 <= capture->x0 ||
        capture->y1 <= capture->y0) return 0u;
    height = capture->y0 < 0 ? capture->y1 - capture->y0 : capture->y1;
    if (height <= 0) return 0u;
    if (height > kPpuBufHeight) height = kPpuBufHeight;
    return (uint32_t)height;
}

static SrResult query_ppu_surfaces(
        SrRunnerHandle *runner, SrPpuSurfaceSnapshot *out_surfaces) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint32_t rendered_height;
    unsigned source, band;
    if (snes == NULL || out_surfaces == NULL ||
        out_surfaces->struct_size < SR_PPU_SURFACE_SNAPSHOT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_surfaces, 0, SR_PPU_SURFACE_SNAPSHOT_V1_SIZE);
    out_surfaces->struct_size = SR_PPU_SURFACE_SNAPSHOT_V1_SIZE;
    out_surfaces->lifetime_generation = snes->abiLifetimeGeneration;
    out_surfaces->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    out_surfaces->band_count = SR_PPU_SURFACE_BAND_COUNT;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_surfaces->binding_generation = ppu->surfaceBindingGeneration;
    rendered_height = (uint32_t)PpuRenderedHeight(ppu);
    ppu_surface_view_init(
        &out_surfaces->main, ppu->renderBuffer, ppu->renderPitch,
        rendered_height,
        PpuSurfaceApron(ppu, ppu->renderPitch) + ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    ppu_surface_view_init(
        &out_surfaces->authentic, ppu->authenticRenderBuffer,
        ppu->authenticRenderPitch, rendered_height,
        PpuSurfaceApron(ppu, ppu->authenticRenderPitch) +
            ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        uint32_t height = ppu_overlay_surface_height(capture);
        int32_t origin_y = capture->y0 < 0 ? -capture->y0 : 0;
        ppu_surface_view_init(
            &out_surfaces->overlays[source][0],
            ppu->overlayRenderBuffer[source],
            ppu->overlayRenderPitch[source], height,
            PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                ppu->extraLeftRight,
            origin_y, 1u,
            (ppu->overlayRenderContentMask[source] & 1u) != 0u);
        for (band = 1u; band < SR_PPU_SURFACE_BAND_COUNT; ++band)
            ppu_surface_view_init(
                &out_surfaces->overlays[source][band],
                ppu->overlayRenderBands[source][band - 1u],
                ppu->overlayRenderPitch[source], height,
                PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                    ppu->extraLeftRight,
                origin_y, 1u,
                (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u);
    }
    if (ppu->m7OverlayBuffer != NULL && ppu->m7OverlayScale != 0u) {
        uint32_t scale = ppu->m7OverlayScale;
        int32_t width = (int32_t)(ppu->m7OverlayPitch / sizeof(uint32_t));
        int32_t span = (kPpuXPixels + 2 * ppu->extraLeftRight) *
            (int32_t)scale;
        int32_t apron = (width - span) / 2;
        if (apron < 0) apron = 0;
        ppu_surface_view_init(
            &out_surfaces->mode7, ppu->m7OverlayBuffer,
            ppu->m7OverlayPitch, rendered_height * scale,
            apron + ppu->extraLeftRight * (int32_t)scale,
            PpuVerticalOrigin(ppu) * (int32_t)scale, scale,
            ppu->m7Override.rgba != NULL);
    }
    return SR_RESULT_OK;
}

static uint32_t ppu_surface_snapshot_is_valid(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces) {
    Snes *snes = runner_from_handle(runner);
    return snes != NULL && snes->ppu != NULL && surfaces != NULL &&
        surfaces->struct_size >= SR_PPU_SURFACE_SNAPSHOT_V1_SIZE &&
        surfaces->lifetime_generation == snes->abiLifetimeGeneration &&
        surfaces->binding_generation ==
            snes->ppu->surfaceBindingGeneration;
}

static SrResult rasterize_ppu_obj_range(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_RASTER_REQUEST_V1_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RASTER_RESULT_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RASTER_RESULT_V1_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RASTER_RESULT_V1_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_raster_runner != snes ||
        s_ppu_obj_raster_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_raster_provider(snes, request, out_result);
}

static const SnesRunnerApi k_runner_api = {
    SR_RUNNER_ABI_VERSION,
    (uint32_t)sizeof(SnesRunnerApi),
    SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS,
    get_component,
    query_generations,
    borrow_memory,
    borrow_is_valid,
    query_cpu_state,
    query_ppu_state,
    borrow_u16_memory,
    borrow_u16_is_valid,
    query_ppu_frame_state,
    rasterize_ppu_obj_range,
    query_ppu_surfaces,
    ppu_surface_snapshot_is_valid,
};

/* Keep synchronized with the source boundary in runner.cmake. */
static const SrRunnerDescriptor k_runner = {
    SR_RUNNER_ABI_VERSION,
    "next",
    0u,
    (uint32_t)sizeof(SrRunnerDescriptor),
    SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS,
};

const SrRunnerDescriptor *sr_runner_descriptor(void) {
    return &k_runner;
}

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
    if (requested_abi_version != SR_RUNNER_ABI_VERSION) return NULL;
    return &k_runner_api;
}

SrRunnerHandle *sr_runner_handle(Snes *snes) {
    return (SrRunnerHandle *)(void *)snes;
}

void sr_runner_set_cpu_state_provider(
        Snes *snes, SrRunnerCpuStateProvider *provider,
        const void *component_handle) {
    if (provider == NULL && s_cpu_state_runner != snes) return;
    s_cpu_state_runner = provider != NULL ? snes : NULL;
    s_cpu_state_provider = provider;
    s_cpu_component = provider != NULL ? component_handle : NULL;
}

void sr_runner_set_ppu_obj_raster_provider(
        Snes *snes, SrRunnerPpuObjRasterProvider *provider) {
    if (provider == NULL && s_ppu_obj_raster_runner != snes) return;
    s_ppu_obj_raster_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_raster_provider = provider;
}

static void invalidate_lifetime(Snes *snes) {
    if (snes != NULL) ++snes->abiLifetimeGeneration;
}

void sr_runner_note_tick(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiTickGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_reset(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiResetGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_load(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiLoadGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_mutation(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiMutationGeneration;
    invalidate_lifetime(snes);
}
