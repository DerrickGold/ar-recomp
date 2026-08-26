#include "rom.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    SR_ROM_MINIMUM_SIZE = 0x8000,
    SR_ROM_COPIER_HEADER_SIZE = 0x200,
    SR_ROM_HEADER_SIZE = 0x40
};

typedef struct HeaderCandidate {
    SrRomInfo info;
    uint8_t speed;
    uint8_t content_type;
    uint8_t coprocessor;
    uint8_t chips;
    uint16_t checksum;
    uint16_t complement;
} HeaderCandidate;

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t decode_capacity(uint8_t exponent) {
    if (exponent >= 22u) {
        return 0u;
    }
    return UINT32_C(0x400) << exponent;
}

static char printable_title_byte(uint8_t value) {
    return value >= 0x20u && value < 0x7fu ? (char)value : '.';
}

static int score_header(const uint8_t *data, size_t size, size_t location,
                        HeaderCandidate *candidate) {
    const uint8_t *header = data + location;
    int score = 0;

    memset(candidate, 0, sizeof(*candidate));
    candidate->info.mapping = location < 0x9000u
        ? SR_CART_MAPPING_LOROM : SR_CART_MAPPING_HIROM;
    candidate->info.header_offset = location;
    candidate->info.payload_offset =
        location == 0x81c0u || location == 0x101c0u
            ? SR_ROM_COPIER_HEADER_SIZE : 0u;
    candidate->info.payload_size = size - candidate->info.payload_offset;

    for (size_t index = 0; index < 21u; ++index) {
        candidate->info.title[index] = printable_title_byte(header[index]);
    }
    candidate->info.title[21] = '\0';

    candidate->speed = (uint8_t)(header[0x15] >> 4);
    candidate->content_type = (uint8_t)(header[0x15] & 0x0fu);
    candidate->coprocessor = (uint8_t)(header[0x16] >> 4);
    candidate->chips = (uint8_t)(header[0x16] & 0x0fu);
    candidate->info.declared_rom_size = decode_capacity(header[0x17]);
    candidate->info.ram_size = candidate->chips == 0u
        ? 0u : decode_capacity(header[0x18]);
    candidate->info.region = header[0x19];
    candidate->info.version = header[0x1b];
    candidate->complement = read_le16(header + 0x1c);
    candidate->checksum = read_le16(header + 0x1e);
    candidate->info.pal = (candidate->info.region >= 0x02u &&
                           candidate->info.region <= 0x0cu) ||
                          candidate->info.region == 0x11u;

    score += candidate->speed == 2u || candidate->speed == 3u ? 5 : -4;
    score += candidate->content_type <= 3u || candidate->content_type == 5u ? 5 : -2;
    score += candidate->coprocessor <= 5u || candidate->coprocessor >= 0x0eu ? 5 : -2;
    score += candidate->chips <= 6u || candidate->chips == 9u ||
             candidate->chips == 0x0au ? 5 : -2;
    score += candidate->info.region <= 0x14u ? 5 : -2;
    score += (uint16_t)(candidate->checksum + candidate->complement) == UINT16_MAX
        ? 8 : -6;

    const uint16_t reset_vector = read_le16(header + 0x3c);
    score += reset_vector >= 0x8000u ? 8 : -20;

    const size_t mapping_base = candidate->info.mapping == SR_CART_MAPPING_LOROM
        ? location + SR_ROM_HEADER_SIZE - 0x8000u
        : location + SR_ROM_HEADER_SIZE - 0x8000u;
    const size_t opcode_offset = mapping_base + (size_t)(reset_vector & 0x7fffu);
    uint8_t opcode = 0xffu;
    if (opcode_offset < size) {
        opcode = data[opcode_offset];
    } else {
        score -= 14;
    }
    if (opcode == 0x78u || opcode == 0x18u) {
        score += 6;
    }
    if (opcode == 0x4cu || opcode == 0x5cu || opcode == 0x9cu) {
        score += 3;
    }
    if (opcode == 0x00u || opcode == 0xffu || opcode == 0xdbu) {
        score -= 6;
    }

    candidate->info.header_score = (int16_t)score;
    return score;
}

SrRomStatus sr_rom_analyze(const uint8_t *data, size_t size, SrRomInfo *info) {
    static const size_t locations[] = {0x7fc0u, 0x81c0u, 0xffc0u, 0x101c0u};
    HeaderCandidate best;
    int best_score = 0;
    bool have_candidate = false;

    if (data == NULL || info == NULL) {
        return SR_ROM_INVALID_ARGUMENT;
    }
    if (size < SR_ROM_MINIMUM_SIZE) {
        return SR_ROM_TOO_SMALL;
    }

    memset(&best, 0, sizeof(best));
    for (size_t index = 0; index < sizeof(locations) / sizeof(locations[0]); ++index) {
        const size_t location = locations[index];
        if (location > size || size - location < SR_ROM_HEADER_SIZE) {
            continue;
        }
        HeaderCandidate current;
        const int score = score_header(data, size, location, &current);
        if (!have_candidate || score > best_score) {
            best = current;
            best_score = score;
            have_candidate = true;
        }
    }

    if (!have_candidate) {
        return SR_ROM_TOO_SMALL;
    }
    *info = best.info;
    return SR_ROM_OK;
}

static size_t expanded_size(size_t payload_size) {
    size_t result = SR_ROM_MINIMUM_SIZE;
    while (result < payload_size) {
        if (result > SIZE_MAX / 2u) {
            return 0u;
        }
        result *= 2u;
    }
    return result;
}

static void mirror_to_capacity(uint8_t *data, size_t used, size_t capacity) {
    size_t block = 1u;
    while (used < capacity) {
        if ((used & block) != 0u) {
            memcpy(data + used, data + used - block, block);
            used += block;
        }
        block *= 2u;
    }
}

SrRomStatus sr_rom_prepare(const uint8_t *data, size_t size, SrRomImage *image) {
    SrRomInfo info;
    if (image == NULL) {
        return SR_ROM_INVALID_ARGUMENT;
    }
    memset(image, 0, sizeof(*image));

    const SrRomStatus status = sr_rom_analyze(data, size, &info);
    if (status != SR_ROM_OK) {
        return status;
    }
    const size_t capacity = expanded_size(info.payload_size);
    if (capacity == 0u || capacity > (size_t)INT_MAX) {
        return SR_ROM_TOO_LARGE;
    }

    uint8_t *copy = (uint8_t *)malloc(capacity);
    if (copy == NULL) {
        return SR_ROM_OUT_OF_MEMORY;
    }
    memcpy(copy, data + info.payload_offset, info.payload_size);
    mirror_to_capacity(copy, info.payload_size, capacity);

    image->data = copy;
    image->size = capacity;
    image->info = info;
    return SR_ROM_OK;
}

void sr_rom_release(SrRomImage *image) {
    if (image == NULL) {
        return;
    }
    free(image->data);
    memset(image, 0, sizeof(*image));
}

const char *sr_rom_status_string(SrRomStatus status) {
    switch (status) {
        case SR_ROM_OK: return "ok";
        case SR_ROM_INVALID_ARGUMENT: return "invalid argument";
        case SR_ROM_TOO_SMALL: return "ROM is smaller than 32 KiB";
        case SR_ROM_TOO_LARGE: return "ROM is too large for the runner ABI";
        case SR_ROM_OUT_OF_MEMORY: return "out of memory";
        default: return "unknown ROM error";
    }
}
