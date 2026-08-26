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
    failed |= check(crc32_compute(NULL, 0) == 0u, "empty CRC32 mismatch");
    failed |= check(crc32_compute(known_vector, sizeof(known_vector) - 1) == 0xCBF43926u,
                    "known CRC32 vector mismatch");
    failed |= check(running == 0xCBF43926u, "incremental CRC32 mismatch");
    return failed;
}
