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

void sr_runner_bind_ppu_services(Snes *snes, bool enabled) {
    sr_runner_set_ppu_obj_raster_provider(
        snes, enabled ? rasterize_obj_range : NULL);
}
