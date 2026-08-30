#include "snesrecomp/runner/ppu_diagnostics.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "ppu diagnostics check failed: %s:%d: %s\n",    \
                __FILE__, __LINE__, #condition);                          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

int main(void) {
    SrPpuVirtualTileCensus census;
    sr_ppu_virtual_tile_census_reset(&census);
    CHECK(sr_ppu_virtual_tile_census_finish(&census) ==
          SR_RESULT_UNAVAILABLE);
    sr_ppu_virtual_tile_census_record(
        &census, 12, 40, SR_PPU_VIRTUAL_TILE_FOUND);
    sr_ppu_virtual_tile_census_record(
        &census, -3, 42, SR_PPU_VIRTUAL_TILE_TRANSPARENT);
    sr_ppu_virtual_tile_census_record(
        &census, 19, 38, SR_PPU_VIRTUAL_TILE_FALLBACK_AUTHENTIC);
    sr_ppu_virtual_tile_census_record(&census, 7, 41, UINT32_MAX);
    CHECK(sr_ppu_virtual_tile_census_finish(&census) == SR_RESULT_OK);
    CHECK(census.request_count == 4u);
    CHECK(census.found_count == 1u);
    CHECK(census.transparent_count == 2u);
    CHECK(census.authentic_fallback_count == 1u);
    CHECK(census.invalid_result_count == 1u);
    CHECK(census.minimum_tile_x == -3 && census.maximum_tile_x == 19);
    CHECK(census.minimum_tile_y == 38 && census.maximum_tile_y == 42);
    return failures != 0;
}
