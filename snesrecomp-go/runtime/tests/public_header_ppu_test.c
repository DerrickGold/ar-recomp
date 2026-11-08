#include "snesrecomp/runner/ppu.h"

/* Mirror only the established request prefix, so appending a future optional
 * tail cannot silently change the version boundary used by old callers. */
typedef struct LegacyGuardScanoutRequest {
    uint32_t struct_size, flags;
    uint64_t lifetime_generation;
    uint32_t hdma_suppress_mask, reserved;
    SrPpuScanoutLineCallback line_callback;
    SrPpuScanoutIrqCallback irq_callback;
    void *user_data;
    SrPpuBgPacket *background_packet;
    const SrPpuBackgroundGuardRequest *background_guards;
    uint32_t background_guard_count, background_guard_reserved;
} LegacyGuardScanoutRequest;
_Static_assert(sizeof(LegacyGuardScanoutRequest) == SR_PPU_SCANOUT_REQUEST_BACKGROUND_GUARDS_SIZE,
    "legacy guard request prefix changed");

int snesrecomp_public_header_ppu_probe(void) {
    SrPpuBackgroundGuardRequest guard = {.struct_size = sizeof(guard)};
    SrPpuBackgroundGuardBands bands = {.struct_size = sizeof(bands)};
    SrPpuScanoutRequest request = {.struct_size = sizeof(request),
        .background_guards = &guard, .background_guard_count = 1,
        .background_guard_bands = {&bands, NULL}};
    return SR_PPU_NATIVE_WIDTH == 256u &&
        SR_PPU_SURFACE_MAX_HEIGHT == 352u && request.background_guard_count == 1 ? 0 : 1;
}
