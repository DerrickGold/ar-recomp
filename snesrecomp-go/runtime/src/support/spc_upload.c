#include "snesrecomp/spc_upload.h"

#include <string.h>

enum {
    SR_SPC_MAX_BLOCKS = 512,
    SR_SPC_MAX_POOL_SCAN = 0x60000
};

#if defined(_MSC_VER)
#define SR_UPLOAD_THREAD_LOCAL __declspec(thread)
#else
#define SR_UPLOAD_THREAD_LOCAL _Thread_local
#endif
static SR_UPLOAD_THREAD_LOCAL uint8_t *s_write_bitmap;
static SR_UPLOAD_THREAD_LOCAL size_t s_write_bitmap_bytes;

void sr_spc_upload_begin_write_tracking(uint8_t *bitmap,
                                        size_t bitmap_byte_size) {
    s_write_bitmap = bitmap;
    s_write_bitmap_bytes = bitmap_byte_size;
}

void sr_spc_upload_end_write_tracking(void) {
    s_write_bitmap = NULL;
    s_write_bitmap_bytes = 0u;
}

static void track_write(uint16_t destination, size_t length) {
    size_t index;
    if (s_write_bitmap == NULL || s_write_bitmap_bytes < 0x2000u) return;
    for (index = 0u; index < length; ++index) {
        const uint16_t address = (uint16_t)(destination + index);
        s_write_bitmap[address >> 3] |=
            (uint8_t)(1u << (address & 7u));
    }
}

static uint8_t rom_byte(const uint8_t *rom, size_t size, size_t offset) {
    return rom[offset % size];
}

static uint16_t rom_word(const uint8_t *rom, size_t size, size_t offset) {
    return (uint16_t)rom_byte(rom, size, offset) |
           ((uint16_t)rom_byte(rom, size, offset + 1u) << 8);
}

bool sr_spc_upload_image(const uint8_t *rom, size_t rom_size,
                         size_t source_offset, uint8_t aram[0x10000],
                         SrSpcUploadResult *result) {
    size_t cursor = source_offset;
    unsigned blocks = 0u;
    if (rom == NULL || rom_size == 0u || aram == NULL || result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    for (;;) {
        uint16_t length = rom_word(rom, rom_size, cursor);
        uint16_t destination = rom_word(rom, rom_size, cursor + 2u);
        cursor += 4u;
        if (length == 0u) {
            result->entry_point = destination;
            result->block_count = (uint16_t)blocks;
            result->script_offset = (uint64_t)(cursor - 1u);
            return true;
        }
        if (++blocks > SR_SPC_MAX_BLOCKS) return false;
        track_write(destination, length);
        for (uint32_t index = 0u; index < length; ++index) {
            aram[(uint16_t)(destination + index)] =
                rom_byte(rom, rom_size, cursor + index);
        }
        cursor += length;
    }
}

bool sr_spc_upload_samples(const uint8_t *rom, size_t rom_size,
                           size_t script_offset, uint8_t segment_count,
                           size_t pool_offset, uint16_t first_destination,
                           uint8_t aram[0x10000], uint16_t *last_destination,
                           uint16_t *last_length) {
    uint16_t destination = first_destination;
    uint16_t previous_length = 0u;
    uint16_t final_length = 0u;
    uint16_t final_destination = first_destination;
    if (rom == NULL || rom_size == 0u || aram == NULL) return false;

    for (uint32_t segment = 0u; segment < segment_count; ++segment) {
        uint8_t chunk_index = rom_byte(rom, rom_size, script_offset + segment);
        size_t chunk = pool_offset;
        size_t scanned = 0u;
        for (uint32_t index = 0u; index < chunk_index; ++index) {
            uint16_t skip = rom_word(rom, rom_size, chunk);
            size_t advance = (size_t)skip + 2u;
            if (advance > SR_SPC_MAX_POOL_SCAN - scanned) return false;
            scanned += advance;
            chunk += advance;
        }
        uint16_t length = rom_word(rom, rom_size, chunk);
        chunk += 2u;
        destination = (uint16_t)(destination + previous_length);
        track_write(destination, length);
        for (uint32_t index = 0u; index < length; ++index) {
            aram[(uint16_t)(destination + index)] =
                rom_byte(rom, rom_size, chunk + index);
        }
        previous_length = length;
        final_length = length;
        final_destination = destination;
    }
    if (last_destination != NULL) *last_destination = final_destination;
    if (last_length != NULL) *last_length = final_length;
    return true;
}
