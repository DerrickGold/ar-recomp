#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 802.3 CRC32. state uses the same running-state convention as zlib. */
uint32_t crc32_update(uint32_t state, const uint8_t *data, size_t len);
uint32_t crc32_compute(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
