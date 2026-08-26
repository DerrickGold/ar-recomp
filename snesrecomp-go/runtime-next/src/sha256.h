#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sha256_compute(const uint8_t *data, size_t length, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
