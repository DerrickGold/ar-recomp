#include "runner_internal.h"

#include "snesrecomp/game/runtime_constants.h"
#include "snes/cart.h"
#include "snes/cart_map_internal.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "support/sha256.h"

#include <stdint.h>
#include <string.h>

typedef struct PpuScanoutHdma {
    const uint8_t *table;
    const uint8_t *indirect;
    uint8_t repeat_count;
    uint8_t mode;
    uint8_t b_address;
    uint8_t indirect_bank;
} PpuScanoutHdma;

static void presentation_u16(Sha256Context *sha, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    sha256_update(sha, bytes, sizeof(bytes));
}

static void presentation_u32(Sha256Context *sha, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    sha256_update(sha, bytes, sizeof(bytes));
}

static void presentation_u64(Sha256Context *sha, uint64_t value) {
    uint8_t bytes[8];
    unsigned index;
    for (index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    sha256_update(sha, bytes, sizeof(bytes));
}

static bool capture_presentation_digest(
        const Snes *snes, SrPpuScanoutResult *out_result) {
    static const uint8_t domain[] = "snesrecomp-presentation-v1";
    const Ppu *ppu;
    SrPpuPresentationDigest *digest;
    Sha256Context sha;
    uint32_t width;
    uint32_t height;
    int start_x;
    uint32_t row;
    if (snes == NULL || out_result == NULL || snes->ppu == NULL)
        return false;
    ppu = snes->ppu;
    digest = &out_result->presentation;
    digest->struct_size = SR_PPU_PRESENTATION_DIGEST_V2_SIZE;
    if (ppu->renderBuffer == NULL || ppu->renderPitch == 0u) return false;
    width = SR_PPU_NATIVE_WIDTH + ppu->extraLeftCur + ppu->extraRightCur;
    height = SR_PPU_NATIVE_HEIGHT + ppu->extraTopCur + ppu->extraBottomCur;
    start_x = PpuSurfaceApron(ppu, ppu->renderPitch) +
        ppu->extraLeftRight - ppu->extraLeftCur;
    if (start_x < 0 || width > ppu->renderPitch / sizeof(uint32_t) ||
        (uint64_t)(uint32_t)start_x + width >
            ppu->renderPitch / sizeof(uint32_t) ||
        height > ppu->renderHeight)
        return false;
    sha256_init(&sha);
    sha256_update(&sha, domain, sizeof(domain) - 1u);
    presentation_u32(&sha, SR_PPU_PRESENTATION_SCHEMA_VERSION);
    presentation_u64(&sha, snes->abiFrameCounter);
    presentation_u32(&sha, SR_PPU_PRESENTATION_PIXEL_XRGB8888_LE);
    presentation_u32(&sha, width);
    presentation_u32(&sha, height);
    presentation_u16(&sha, ppu->extraLeftCur);
    presentation_u16(&sha, ppu->extraRightCur);
    presentation_u16(&sha, ppu->extraTopCur);
    presentation_u16(&sha, ppu->extraBottomCur);
    for (row = 0u; row < height; ++row) {
        const uint8_t *source = ppu->renderBuffer +
            (size_t)row * ppu->renderPitch +
            (size_t)start_x * sizeof(uint32_t);
        uint32_t column;
        for (column = 0u; column < width; ++column) {
            uint32_t pixel;
            memcpy(&pixel, source + (size_t)column * sizeof(pixel),
                   sizeof(pixel));
            presentation_u32(&sha, pixel);
        }
    }
    sha256_final(&sha, digest->sha256);
    digest->schema_version = SR_PPU_PRESENTATION_SCHEMA_VERSION;
    digest->frame_counter = snes->abiFrameCounter;
    digest->width_pixels = width;
    digest->height_pixels = height;
    digest->margin_left_pixels = ppu->extraLeftCur;
    digest->margin_right_pixels = ppu->extraRightCur;
    digest->margin_top_pixels = ppu->extraTopCur;
    digest->margin_bottom_pixels = ppu->extraBottomCur;
    digest->pixel_format = SR_PPU_PRESENTATION_PIXEL_XRGB8888_LE;
    out_result->flags |= SR_PPU_SCANOUT_PRESENTATION_DIGEST_VALID;
    return true;
}

static const uint8_t *scanout_hdma_pointer(
        const Snes *snes, uint32_t address) {
    const uint8_t bank = (uint8_t)(address >> 16);
    const uint16_t offset = (uint16_t)address;
    SrCartAddress mapped;
    if (snes == NULL) return NULL;
    if (snes->ram != NULL) {
        if (bank == 0x7eu) return snes->ram + offset;
        if (bank == 0x7fu) return snes->ram + 0x10000u + offset;
        if ((bank < 0x40u || (bank >= 0x80u && bank < 0xc0u)) &&
            offset < 0x2000u)
            return snes->ram + offset;
    }
    if (snes->cart == NULL || snes->cart->rom == NULL ||
        snes->cart->romSize == 0u)
        return NULL;
    mapped = sr_cart_map_read_inline(
        (SrCartMapping)snes->cart->type, bank, offset,
        snes->cart->romSize, 0u);
    if (mapped.region != SR_CART_REGION_ROM) {
        mapped = sr_cart_map_read_inline(
            SR_CART_MAPPING_LOROM, bank, (uint16_t)(offset | 0x8000u),
            snes->cart->romSize, 0u);
    }
    return mapped.region == SR_CART_REGION_ROM
        ? snes->cart->rom + mapped.offset : NULL;
}

static bool pointer_in_range(
        const uint8_t *pointer, const uint8_t *base, size_t size) {
    const uintptr_t address = (uintptr_t)pointer;
    const uintptr_t first = (uintptr_t)base;
    return pointer != NULL && base != NULL && address >= first &&
        address - first < size;
}

static bool scanout_hdma_pointer_valid(
        const Snes *snes, const uint8_t *pointer) {
    if (snes == NULL || pointer == NULL) return false;
    return pointer_in_range(pointer, snes->ram, kSnesWramSize) ||
        (snes->cart != NULL && pointer_in_range(
            pointer, snes->cart->rom, snes->cart->romSize));
}

static void scanout_hdma_init(
        const Snes *snes, PpuScanoutHdma *scanout,
        const DmaChannel *channel) {
    if (scanout == NULL || channel == NULL) return;
    memset(scanout, 0, sizeof(*scanout));
    if (!channel->hdmaActive) return;
    scanout->table = scanout_hdma_pointer(
        snes, ((uint32_t)channel->aBank << 16) | channel->aAdr);
    scanout->mode = (uint8_t)(channel->mode |
        (channel->indirect ? 0x40u : 0u));
    scanout->b_address = channel->bAdr;
    scanout->indirect_bank = channel->indBank;
}

static void scanout_hdma_line(Snes *snes, PpuScanoutHdma *channel) {
    static const uint8_t offsets[8][4] = {
        {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
        {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
    };
    static const uint8_t lengths[8] = {1, 2, 2, 4, 4, 4, 2, 4};
    bool transfer = false;
    uint8_t mode;
    uint8_t index;
    if (snes == NULL || snes->ppu == NULL || channel == NULL ||
        channel->table == NULL)
        return;
    mode = channel->mode & 7u;
    if ((channel->repeat_count & 0x7fu) == 0u) {
        uint16_t indirect;
        if (!scanout_hdma_pointer_valid(snes, channel->table)) {
            channel->table = NULL;
            return;
        }
        channel->repeat_count = *channel->table++;
        if (channel->repeat_count == 0u) {
            channel->table = NULL;
            return;
        }
        if ((channel->mode & 0x40u) != 0u) {
            if (!scanout_hdma_pointer_valid(snes, channel->table) ||
                !scanout_hdma_pointer_valid(snes, channel->table + 1)) {
                channel->table = NULL;
                return;
            }
            indirect = (uint16_t)channel->table[0] |
                ((uint16_t)channel->table[1] << 8);
            channel->indirect = scanout_hdma_pointer(
                snes, ((uint32_t)channel->indirect_bank << 16) | indirect);
            channel->table += 2;
        }
        transfer = true;
    }
    if (transfer || (channel->repeat_count & 0x80u) != 0u) {
        for (index = 0u; index < lengths[mode]; ++index) {
            const uint8_t *source = (channel->mode & 0x40u) != 0u
                ? channel->indirect : channel->table;
            const uint16_t reg = (uint16_t)(
                0x2100u + channel->b_address + offsets[mode][index]);
            uint8_t value;
            if (!scanout_hdma_pointer_valid(snes, source)) {
                channel->table = NULL;
                break;
            }
            value = *source;
            if ((channel->mode & 0x40u) != 0u)
                ++channel->indirect;
            else
                ++channel->table;
            ppu_write(snes->ppu, (uint8_t)reg, value);
            if (sr_runner_event_enabled(SR_EVENT_MASK_REGISTER_ACCESS)) {
                sr_runner_emit_register_access(
                    snes, true, reg, value, 1u);
            }
        }
    }
    --channel->repeat_count;
}

static void scanout_start_hdma(
        Snes *snes, Dma *dma, uint8_t suppress_mask,
        PpuScanoutHdma scanout[SR_DMA_CHANNEL_COUNT]) {
    unsigned index;
    if (snes == NULL || dma == NULL || scanout == NULL) return;
    for (index = 0u; index < SR_DMA_CHANNEL_COUNT; ++index) {
        DmaChannel *channel = &dma->channel[index];
        const bool selected = channel->hdmaActive &&
            (suppress_mask & (uint8_t)(1u << index)) == 0u;
        memset(&scanout[index], 0, sizeof(scanout[index]));
        if (selected) scanout_hdma_init(snes, &scanout[index], channel);
        if (selected && sr_runner_event_enabled(SR_EVENT_MASK_DMA)) {
            SrRunnerEvent event = {0};
            event.type = SR_EVENT_DMA_BEGIN;
            event.frame_counter = snes->abiFrameCounter;
            event.flags = SR_EVENT_DMA_HDMA |
                (channel->fromB ? SR_EVENT_DMA_FROM_B_BUS : 0u) |
                (channel->fixed ? SR_EVENT_DMA_FIXED_A_BUS : 0u) |
                (channel->decrement ? SR_EVENT_DMA_DECREMENT_A_BUS : 0u) |
                (channel->indirect ? SR_EVENT_DMA_INDIRECT : 0u);
            event.address = ((uint32_t)channel->aBank << 16) | channel->aAdr;
            event.dma_a_address24 = event.address;
            event.dma_table_address = channel->aAdr;
            event.dma_channel = (uint8_t)index;
            event.dma_mode = channel->mode;
            event.dma_b_address = channel->bAdr;
            event.dma_indirect_bank = channel->indBank;
            event.label = "hdma";
            sr_runner_emit_event(snes, SR_EVENT_MASK_DMA, &event);
        }
    }
}

static void scanout_surface_view_init(
        SrPpuSurfaceView *view, const uint8_t *data, uint32_t pitch,
        uint32_t height, int32_t origin_x, int32_t origin_y) {
    if (view == NULL || data == NULL || pitch == 0u ||
        pitch % sizeof(uint32_t) != 0u || height == 0u)
        return;
    view->flags = SR_PPU_SURFACE_BOUND | SR_PPU_SURFACE_HAS_CONTENT;
    view->pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32;
    view->data = data;
    view->pitch_bytes = pitch;
    view->byte_size = (uint64_t)pitch * height;
    view->width_pixels = pitch / (uint32_t)sizeof(uint32_t);
    view->height_pixels = height;
    view->origin_x = origin_x;
    view->origin_y = origin_y;
    view->scale = 1u;
}

static void scanout_line_context_init(
        Snes *snes, Ppu *ppu, const PpuScanoutHdma *hdma,
        uint32_t line, uint32_t flags,
        SrPpuScanoutLineContext *context) {
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    const uint32_t rendered_height = (uint32_t)PpuRenderedHeight(ppu);
    unsigned channel;
    memset(context, 0, sizeof(*context));
    context->struct_size = SR_PPU_SCANOUT_LINE_CONTEXT_V2_SIZE;
    context->flags = flags;
    context->lifetime_generation = snes->abiLifetimeGeneration;
    context->line = line;
    context->channel_count = SR_DMA_CHANNEL_COUNT;
    context->state.struct_size = sizeof(context->state);
    (void)api->query_ppu_state(
        sr_runner_handle(snes), &context->state);
    scanout_surface_view_init(
        &context->main_surface, ppu->renderBuffer, ppu->renderPitch,
        rendered_height,
        PpuSurfaceApron(ppu, ppu->renderPitch) + ppu->extraLeftRight,
        PpuVerticalOrigin(ppu));
    scanout_surface_view_init(
        &context->authentic_surface, ppu->authenticRenderBuffer,
        ppu->authenticRenderPitch, rendered_height,
        PpuSurfaceApron(ppu, ppu->authenticRenderPitch) +
            ppu->extraLeftRight,
        PpuVerticalOrigin(ppu));
    for (channel = 0u; channel < SR_DMA_CHANNEL_COUNT; ++channel) {
        const PpuScanoutHdma *source = &hdma[channel];
        SrPpuScanoutHdmaState *target = &context->channels[channel];
        target->flags =
            (source->table != NULL ? SR_PPU_SCANOUT_HDMA_ACTIVE : 0u) |
            ((source->mode & 0x40u) != 0u
                ? SR_PPU_SCANOUT_HDMA_INDIRECT : 0u);
        target->repeat_count = source->repeat_count;
        target->mode = source->mode & 7u;
        target->b_address = source->b_address;
        target->indirect_bank = source->indirect_bank;
    }
}

static SrResult run_ppu_scanout(
        Snes *snes, const SrPpuScanoutRequest *request,
        SrPpuScanoutResult *out_result) {
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    Ppu *ppu = snes->ppu;
    Dma *dma = snes->dma;
    PpuScanoutHdma hdma[SR_DMA_CHANNEL_COUNT] = {{0}};
    int trigger;
    int line;
    unsigned channel;
    if (ppu == NULL || dma == NULL || api == NULL)
        return SR_RESULT_UNAVAILABLE;
    if (!PpuOutputSurfacesFitGeometry(
            ppu, ppu->extraLeftRight,
            ppu->extraTopCur, ppu->extraBottomCur))
        return SR_RESULT_INVALID_ARGUMENT;

    if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
        sr_runner_emit_frame_boundary(
            snes, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_SCANOUT,
            "scanout-begin");
    }
    snes->diagnosticScanoutObserved = true;
    _Static_assert(SR_DMA_CHANNEL_COUNT == kDmaChannelCount,
                   "scanout HDMA channel count must match the runner");
    scanout_start_hdma(
        snes, dma, (uint8_t)request->hdma_suppress_mask, hdma);

    trigger = snes->vIrqEnabled ? (int)snes->vTimer + 1 : -1;
    for (line = 0; line <= (int)SR_PPU_NATIVE_HEIGHT; ++line) {
        snes_setBeamPosition(snes, 0u, (uint16_t)line);
        if (request->line_callback != NULL) {
            SrPpuScanoutLineContext context;
            scanout_line_context_init(
                snes, ppu, hdma, (uint32_t)line,
                SR_PPU_SCANOUT_LINE_BEFORE, &context);
            request->line_callback(request->user_data, &context);
        }
        ppu_runLine(ppu, line);
        if (line == 0) {
            int margin;
            for (margin = ppu->extraTopCur; margin >= 1; --margin)
                ppu_runMarginLine(ppu, 1 - margin);
        }
        for (channel = 0u; channel < SR_DMA_CHANNEL_COUNT; ++channel)
            scanout_hdma_line(snes, &hdma[channel]);
        if (request->line_callback != NULL) {
            SrPpuScanoutLineContext context;
            scanout_line_context_init(
                snes, ppu, hdma, (uint32_t)line,
                SR_PPU_SCANOUT_LINE_AFTER_HDMA, &context);
            request->line_callback(request->user_data, &context);
        }
        if (line == trigger) {
            const bool observe_irq =
                sr_runner_event_enabled(SR_EVENT_MASK_INTERRUPT);
            snes->inIrq = true;
            if (observe_irq) {
                sr_runner_emit_interrupt(
                    snes, SR_INTERRUPT_IRQ,
                    SR_EVENT_INTERRUPT_ENTER |
                        SR_EVENT_INTERRUPT_CALLBACK,
                    0u, 0u, (int32_t)line,
                    "scanout-irq-callback");
            }
            request->irq_callback(request->user_data, (uint32_t)line);
            if (observe_irq) {
                sr_runner_emit_interrupt(
                    snes, SR_INTERRUPT_IRQ,
                    SR_EVENT_INTERRUPT_EXIT |
                        SR_EVENT_INTERRUPT_CALLBACK,
                    0u, 0u, (int32_t)line,
                    "scanout-irq-callback");
            }
            trigger = snes->vIrqEnabled ? (int)snes->vTimer + 1 : -1;
        }
    }
    for (line = 1; line <= ppu->extraBottomCur; ++line)
        ppu_runMarginLine(ppu, (int)SR_PPU_NATIVE_HEIGHT + line);
    snes_beginVblank(snes);

    out_result->final_state.struct_size = sizeof(out_result->final_state);
    (void)api->query_ppu_state(
        sr_runner_handle(snes), &out_result->final_state);
    out_result->flags =
        (PpuAuthenticSurfaceReady(ppu)
            ? SR_PPU_SCANOUT_AUTHENTIC_SURFACE_READY : 0u) |
        (PpuAuthenticCameraFrameReady(
             ppu, kPpuAuthenticCameraLayer_Bg1)
            ? SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG1 : 0u) |
        (PpuAuthenticCameraFrameReady(
             ppu, kPpuAuthenticCameraLayer_Bg2)
            ? SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG2 : 0u);
    if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
        sr_runner_emit_frame_boundary(
            snes,
            SR_EVENT_FRAME_END | SR_EVENT_FRAME_VBLANK |
                SR_EVENT_FRAME_SCANOUT,
            "scanout-end");
    }
    if ((request->flags &
         SR_PPU_SCANOUT_CAPTURE_PRESENTATION_DIGEST) != 0u)
        (void)capture_presentation_digest(snes, out_result);
    return SR_RESULT_OK;
}

static SrResult rasterize_obj_range(
        Snes *snes, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    PpuObjPart parts[128];
    PpuObjRangeBounds bounds;
    uint64_t row_bytes;
    uint64_t required_bytes;
    int part_count = 0;
    int width;
    int height;
    if (request->flags != 0u ||
        request->pixel_format != SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32)
        return SR_RESULT_UNSUPPORTED;
    if (snes->ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (request->first_sprite >= 128u || request->sprite_count == 0u ||
        request->sprite_count > 128u - request->first_sprite ||
        request->priority >= 4u || request->pixels == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (!PpuResolveObjSlots(
            snes->ppu, (uint8_t)request->first_sprite,
            (uint8_t)request->sprite_count, (uint8_t)request->priority,
            parts, 128, &part_count) ||
        !PpuGetPartBounds(parts, part_count, &bounds))
        return SR_RESULT_UNAVAILABLE;
    width = bounds.x1 - bounds.x0;
    height = bounds.y1 - bounds.y0;
    if (width <= 0 || height <= 0) return SR_RESULT_UNAVAILABLE;
    row_bytes = (uint64_t)(unsigned)width * sizeof(uint32_t);
    if (request->pitch_bytes < row_bytes ||
        request->pitch_bytes > (uint64_t)SIZE_MAX ||
        (request->pitch_bytes % sizeof(uint32_t)) != 0u ||
        ((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if ((uint64_t)(unsigned)(height - 1) >
        (UINT64_MAX - row_bytes) / request->pitch_bytes)
        return SR_RESULT_INVALID_ARGUMENT;
    required_bytes = (uint64_t)(unsigned)(height - 1) *
        request->pitch_bytes + row_bytes;
    if (request->pixel_byte_size < required_bytes)
        return SR_RESULT_INVALID_ARGUMENT;
    if (!PpuRasterizeParts(snes->ppu, parts, part_count, &bounds,
                           request->pixels, width, height,
                           (size_t)request->pitch_bytes))
        return SR_RESULT_UNAVAILABLE;
    out_result->x0 = bounds.x0;
    out_result->y0 = bounds.y0;
    out_result->x1 = bounds.x1;
    out_result->y1 = bounds.y1;
    out_result->width = (uint32_t)width;
    out_result->height = (uint32_t)height;
    return SR_RESULT_OK;
}

static SrResult resolve_obj_range(
        Snes *snes, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result) {
    PpuObjRangeBounds bounds;
    int part_count = 0;
    if (request->flags != 0u || snes->ppu == NULL)
        return request->flags != 0u
            ? SR_RESULT_UNSUPPORTED : SR_RESULT_UNAVAILABLE;
    if (request->first_sprite >= 128u || request->sprite_count == 0u ||
        request->sprite_count > 128u - request->first_sprite ||
        request->priority >= 4u || request->parts == NULL ||
        request->part_capacity < request->sprite_count ||
        request->part_capacity > 128u ||
        ((uintptr_t)request->parts % _Alignof(SrPpuObjPart)) != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (!PpuResolveObjSlots(
            snes->ppu, (uint8_t)request->first_sprite,
            (uint8_t)request->sprite_count, (uint8_t)request->priority,
            request->parts, (int)request->part_capacity, &part_count) ||
        !PpuGetPartBounds(request->parts, part_count, &bounds))
        return SR_RESULT_UNAVAILABLE;
    out_result->part_count = (uint32_t)part_count;
    out_result->x0 = bounds.x0;
    out_result->y0 = bounds.y0;
    out_result->x1 = bounds.x1;
    out_result->y1 = bounds.y1;
    return SR_RESULT_OK;
}

static SrResult rasterize_obj_parts(
        Snes *snes, const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    PpuObjRangeBounds bounds;
    uint64_t row_bytes;
    uint64_t required_bytes;
    int width;
    int height;
    if (request->flags != 0u || request->reserved != 0u ||
        request->pixel_format != SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32)
        return SR_RESULT_UNSUPPORTED;
    if (snes->ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (request->parts == NULL || request->part_count == 0u ||
        request->part_count > 128u || request->pixels == NULL ||
        ((uintptr_t)request->parts % _Alignof(SrPpuObjPart)) != 0u ||
        ((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u ||
        request->x0 < INT16_MIN || request->x0 > INT16_MAX ||
        request->y0 < INT16_MIN || request->y0 > INT16_MAX ||
        request->x1 < INT16_MIN || request->x1 > INT16_MAX ||
        request->y1 < INT16_MIN || request->y1 > INT16_MAX ||
        request->x1 <= request->x0 || request->y1 <= request->y0)
        return SR_RESULT_INVALID_ARGUMENT;
    for (uint64_t index = 0u; index < request->part_count; ++index) {
        const SrPpuObjPart *part = &request->parts[index];
        if (part->reserved != 0u || part->size == 0u || part->size > 64u ||
            (part->size & 7u) != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
    }
    width = request->x1 - request->x0;
    height = request->y1 - request->y0;
    row_bytes = (uint64_t)(unsigned)width * sizeof(uint32_t);
    if (request->pitch_bytes < row_bytes ||
        request->pitch_bytes > (uint64_t)SIZE_MAX ||
        (request->pitch_bytes % sizeof(uint32_t)) != 0u ||
        (uint64_t)(unsigned)(height - 1) >
            (UINT64_MAX - row_bytes) / request->pitch_bytes)
        return SR_RESULT_INVALID_ARGUMENT;
    required_bytes = (uint64_t)(unsigned)(height - 1) *
        request->pitch_bytes + row_bytes;
    if (request->pixel_byte_size < required_bytes)
        return SR_RESULT_INVALID_ARGUMENT;
    bounds.x0 = (int16_t)request->x0;
    bounds.y0 = (int16_t)request->y0;
    bounds.x1 = (int16_t)request->x1;
    bounds.y1 = (int16_t)request->y1;
    if (!PpuRasterizeParts(
            snes->ppu, request->parts, (int)request->part_count, &bounds,
            request->pixels, width, height, (size_t)request->pitch_bytes))
        return SR_RESULT_UNAVAILABLE;
    out_result->x0 = request->x0;
    out_result->y0 = request->y0;
    out_result->x1 = request->x1;
    out_result->y1 = request->y1;
    out_result->width = (uint32_t)width;
    out_result->height = (uint32_t)height;
    return SR_RESULT_OK;
}

void sr_runner_bind_ppu_services(Snes *snes, bool enabled) {
    sr_runner_set_ppu_obj_raster_provider(
        snes, enabled ? rasterize_obj_range : NULL);
    sr_runner_set_ppu_obj_resolve_provider(
        snes, enabled ? resolve_obj_range : NULL);
    sr_runner_set_ppu_obj_parts_raster_provider(
        snes, enabled ? rasterize_obj_parts : NULL);
    sr_runner_set_ppu_scanout_provider(
        snes, enabled ? run_ppu_scanout : NULL);
}
