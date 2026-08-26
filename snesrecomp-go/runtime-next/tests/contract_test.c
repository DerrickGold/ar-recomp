#include "crc32.h"
#include "runner_next.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int check(int condition, const char *message) {
    if (condition) {
        return 0;
    }
    fprintf(stderr, "runtime-next contract failed: %s\n", message);
    return 1;
}

int main(void) {
    static const uint8_t known_vector[] = "123456789";
    static const uint8_t first[] = "1234";
    static const uint8_t second[] = "56789";
    const SrRunnerDescriptor *descriptor = sr_runner_descriptor();
    uint32_t running = crc32_update(0xFFFFFFFFu, first, strlen((const char *)first));
    running = crc32_update(running, second, strlen((const char *)second)) ^ 0xFFFFFFFFu;

    int failed = 0;
    failed |= check(descriptor != NULL, "descriptor is null");
    failed |= check(descriptor != NULL && descriptor->abi_version == SR_RUNNER_ABI_VERSION,
                    "ABI version mismatch");
    failed |= check(descriptor != NULL && strcmp(descriptor->variant, "next") == 0,
                    "variant mismatch");
    failed |= check(descriptor != NULL && descriptor->legacy_source_count == 0u,
                    "legacy source count mismatch");
    failed |= check(descriptor != NULL &&
                        descriptor->struct_size == sizeof(*descriptor),
                    "descriptor size mismatch");
    failed |= check(descriptor != NULL &&
                        (descriptor->capabilities &
                         SR_RUNNER_CAP_GENERATION_COUNTERS) != 0u,
                    "descriptor capability mismatch");
    failed |= check(descriptor != NULL &&
                        (descriptor->capabilities &
                         SR_RUNNER_CAP_CPU_STATE) != 0u,
                    "descriptor CPU-state capability mismatch");
    failed |= check(descriptor != NULL &&
                        (descriptor->capabilities &
                         SR_RUNNER_CAP_SAFE_POINT_MUTATIONS) != 0u,
                    "descriptor mutation capability mismatch");
    failed |= check(descriptor != NULL &&
                        (descriptor->capabilities &
                         (SR_RUNNER_CAP_PPU_STATE |
                          SR_RUNNER_CAP_BORROWED_U16_SPANS |
                          SR_RUNNER_CAP_PPU_FRAME_STATE |
                          SR_RUNNER_CAP_PPU_OBJ_RASTER)) ==
                            (SR_RUNNER_CAP_PPU_STATE |
                             SR_RUNNER_CAP_BORROWED_U16_SPANS |
                             SR_RUNNER_CAP_PPU_FRAME_STATE |
                             SR_RUNNER_CAP_PPU_OBJ_RASTER),
                    "descriptor PPU capability mismatch");
    failed |= check(crc32_compute(NULL, 0) == 0u, "empty CRC32 mismatch");
    failed |= check(crc32_compute(known_vector, sizeof(known_vector) - 1) == 0xCBF43926u,
                    "known CRC32 vector mismatch");
    failed |= check(running == 0xCBF43926u, "incremental CRC32 mismatch");
    return failed;
}
