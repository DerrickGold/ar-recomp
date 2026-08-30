#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sha256Context {
    uint32_t state[8];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_used;
} Sha256Context;

void sha256_init(Sha256Context *context);
void sha256_update(Sha256Context *context, const uint8_t *data,
                   size_t length);
void sha256_final(Sha256Context *context, uint8_t digest[32]);
void sha256_compute(const uint8_t *data, size_t length, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
