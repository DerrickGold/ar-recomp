#include "crc32.h"

/*
 * A 16-entry nibble table keeps the hot loop branch-free without the mutable
 * initialization state or 1 KiB footprint of a byte table. The table is
 * read-only, so calls are naturally thread-safe on every C11 target.
 */
static const uint32_t k_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t crc32_update(uint32_t state, const uint8_t *data, size_t len) {
    for (size_t index = 0; index < len; ++index) {
        state ^= data[index];
        state = (state >> 4) ^ k_crc32_nibble[state & 0x0Fu];
        state = (state >> 4) ^ k_crc32_nibble[state & 0x0Fu];
    }
    return state;
}

uint32_t crc32_compute(const uint8_t *data, size_t len) {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}
