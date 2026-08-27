#include "runner_next_internal.h"

#include "snes/ppu.h"
#include "snes/snes.h"

#include <stdint.h>

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
}
