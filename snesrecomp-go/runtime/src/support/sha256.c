#include "sha256.h"

#include <string.h>

static const uint32_t k_round_constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static uint32_t load_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t schedule[16];
    for (unsigned index = 0; index < 16u; ++index) {
        schedule[index] = load_be32(block + index * 4u);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (unsigned round = 0; round < 64u; ++round) {
        uint32_t word;
        if (round < 16u) {
            word = schedule[round];
        } else {
            const uint32_t x = schedule[(round + 1u) & 15u];
            const uint32_t y = schedule[(round + 14u) & 15u];
            const uint32_t small0 = rotate_right(x, 7u) ^ rotate_right(x, 18u) ^ (x >> 3u);
            const uint32_t small1 = rotate_right(y, 17u) ^ rotate_right(y, 19u) ^ (y >> 10u);
            word = schedule[round & 15u] + small0 +
                   schedule[(round + 9u) & 15u] + small1;
            schedule[round & 15u] = word;
        }

        const uint32_t big1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^
                              rotate_right(e, 25u);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t first = h + big1 + choose + k_round_constants[round] + word;
        const uint32_t big0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^
                              rotate_right(a, 22u);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = big0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(Sha256Context *context) {
    static const uint32_t initial_state[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    if (context == NULL) return;
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->byte_count = 0u;
    context->block_used = 0u;
}

void sha256_update(Sha256Context *context, const uint8_t *data,
                   size_t length) {
    size_t consumed = 0u;
    if (context == NULL || (data == NULL && length != 0u)) return;
    context->byte_count += length;
    if (context->block_used != 0u) {
        size_t available = sizeof(context->block) - context->block_used;
        size_t take = length < available ? length : available;
        if (take != 0u)
            memcpy(context->block + context->block_used, data, take);
        context->block_used += take;
        consumed += take;
        if (context->block_used == sizeof(context->block)) {
            compress(context->state, context->block);
            context->block_used = 0u;
        }
    }
    while (length - consumed >= 64u) {
        compress(context->state, data + consumed);
        consumed += 64u;
    }
    if (consumed != length) {
        context->block_used = length - consumed;
        memcpy(context->block, data + consumed, context->block_used);
    }
}

void sha256_final(Sha256Context *context, uint8_t digest[32]) {
    uint8_t final_blocks[128] = {0};
    size_t remaining;
    size_t final_size;
    uint64_t bit_length;
    unsigned index;
    if (context == NULL || digest == NULL) return;
    remaining = context->block_used;
    if (remaining != 0u) {
        memcpy(final_blocks, context->block, remaining);
    }
    final_blocks[remaining] = 0x80u;
    final_size = remaining < 56u ? 64u : 128u;
    bit_length = context->byte_count * 8u;
    for (index = 0u; index < 8u; ++index) {
        final_blocks[final_size - 1u - index] =
            (uint8_t)(bit_length >> (index * 8u));
    }
    compress(context->state, final_blocks);
    if (final_size == 128u) {
        compress(context->state, final_blocks + 64u);
    }

    for (index = 0u; index < 8u; ++index) {
        digest[index * 4u] = (uint8_t)(context->state[index] >> 24u);
        digest[index * 4u + 1u] =
            (uint8_t)(context->state[index] >> 16u);
        digest[index * 4u + 2u] =
            (uint8_t)(context->state[index] >> 8u);
        digest[index * 4u + 3u] = (uint8_t)context->state[index];
    }
    memset(context, 0, sizeof(*context));
}

void sha256_compute(const uint8_t *data, size_t length, uint8_t digest[32]) {
    Sha256Context context;
    sha256_init(&context);
    sha256_update(&context, data, length);
    sha256_final(&context, digest);
}
