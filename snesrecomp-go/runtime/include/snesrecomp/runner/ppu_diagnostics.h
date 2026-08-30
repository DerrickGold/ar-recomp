/**
 * @file ppu_diagnostics.h
 * @brief Optional caller-side helpers for virtual-tile provider diagnostics.
 * @ingroup sr_runner_ppu
 */
#pragma once

#include "snesrecomp/runner/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-frame census populated by a game-owned virtual tile callback. This is
 * caller-side diagnostic state, not a runner snapshot or semantic camera.
 * Minimum and maximum values are observed request coordinates only; HDMA,
 * bands, mirroring, and finite extents may make them differ from the game's
 * camera or authored world bounds. */
typedef struct SrPpuVirtualTileCensus {
    uint64_t request_count;
    uint64_t found_count;
    uint64_t transparent_count;
    uint64_t authentic_fallback_count;
    uint64_t invalid_result_count;
    int32_t minimum_tile_x;
    int32_t maximum_tile_x;
    int32_t minimum_tile_y;
    int32_t maximum_tile_y;
} SrPpuVirtualTileCensus;

/** Begin a new target frame. Call finish after scanout even when no provider
 * request is expected; SR_RESULT_UNAVAILABLE is the intentional loud
 * "diagnostic did not run" result. */
static inline void sr_ppu_virtual_tile_census_reset(
        SrPpuVirtualTileCensus *census) {
    if (census == NULL) return;
    census->request_count = 0u;
    census->found_count = 0u;
    census->transparent_count = 0u;
    census->authentic_fallback_count = 0u;
    census->invalid_result_count = 0u;
    census->minimum_tile_x = 0;
    census->maximum_tile_x = 0;
    census->minimum_tile_y = 0;
    census->maximum_tile_y = 0;
}

/** Record one scalar lookup result from the provider callback. Reserved
 * results are counted separately and as transparent, matching the runner's
 * fail-closed behavior. */
static inline void sr_ppu_virtual_tile_census_record(
        SrPpuVirtualTileCensus *census, int32_t tile_x, int32_t tile_y,
        SrPpuVirtualTileLookupResult result) {
    if (census == NULL) return;
    if (census->request_count == 0u) {
        census->minimum_tile_x = census->maximum_tile_x = tile_x;
        census->minimum_tile_y = census->maximum_tile_y = tile_y;
    } else {
        if (tile_x < census->minimum_tile_x) census->minimum_tile_x = tile_x;
        if (tile_x > census->maximum_tile_x) census->maximum_tile_x = tile_x;
        if (tile_y < census->minimum_tile_y) census->minimum_tile_y = tile_y;
        if (tile_y > census->maximum_tile_y) census->maximum_tile_y = tile_y;
    }
    ++census->request_count;
    if (result == SR_PPU_VIRTUAL_TILE_FOUND) {
        ++census->found_count;
    } else if (result == SR_PPU_VIRTUAL_TILE_FALLBACK_AUTHENTIC) {
        ++census->authentic_fallback_count;
    } else {
        ++census->transparent_count;
        if (result != SR_PPU_VIRTUAL_TILE_TRANSPARENT)
            ++census->invalid_result_count;
    }
}

/** Complete a target frame. Callers must handle SR_RESULT_UNAVAILABLE as a
 * failed diagnostic rather than printing a successful-looking set of zeros. */
static inline SrResult sr_ppu_virtual_tile_census_finish(
        const SrPpuVirtualTileCensus *census) {
    if (census == NULL) return SR_RESULT_INVALID_ARGUMENT;
    return census->request_count != 0u
        ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
