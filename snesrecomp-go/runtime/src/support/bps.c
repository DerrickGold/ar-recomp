#include "util.h"

#include "snesrecomp/support/crc32.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool decode_bps_integer(const uint8 **cursor, const uint8 *end,
                               uint64_t *result) {
    uint64_t value = 0u;
    uint64_t shift = 1u;
    while (*cursor < end) {
        uint8_t byte = *(*cursor)++;
        uint64_t digit = byte & 0x7fu;
        if (digit != 0u && shift > UINT64_MAX / digit) {
            return false;
        }
        if (value > UINT64_MAX - digit * shift) {
            return false;
        }
        value += digit * shift;
        if ((byte & 0x80u) != 0u) {
            *result = value;
            return true;
        }
        if (shift > UINT64_MAX / 128u) {
            return false;
        }
        shift <<= 7;
        if (value > UINT64_MAX - shift) {
            return false;
        }
        value += shift;
    }
    return false;
}

static bool apply_relative_delta(uint64_t encoded, int64_t *position) {
    uint64_t magnitude = encoded >> 1;
    if ((encoded & 1u) != 0u) {
        if (magnitude > (uint64_t)INT64_MAX ||
            *position < INT64_MIN + (int64_t)magnitude) {
            return false;
        }
        *position -= (int64_t)magnitude;
    } else {
        if (magnitude > (uint64_t)INT64_MAX ||
            *position > INT64_MAX - (int64_t)magnitude) {
            return false;
        }
        *position += (int64_t)magnitude;
    }
    return true;
}

uint8 *ApplyBps(const uint8 *source, size_t source_size,
                const uint8 *patch, size_t patch_size, size_t *output_size) {
    const uint8 *cursor;
    const uint8 *command_end;
    uint64_t declared_source;
    uint64_t declared_target;
    uint64_t metadata_size;
    size_t output_offset = 0u;
    int64_t source_relative = 0;
    int64_t target_relative = 0;
    uint8 *output = NULL;
    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (source == NULL || patch == NULL || output_size == NULL ||
        patch_size < 19u || memcmp(patch, "BPS1", 4u) != 0) {
        return NULL;
    }
    command_end = patch + patch_size - 12u;
    if (crc32_compute(source, source_size) != read_le32(command_end) ||
        crc32_compute(patch, patch_size - 4u) != read_le32(patch + patch_size - 4u)) {
        return NULL;
    }
    cursor = patch + 4u;
    if (!decode_bps_integer(&cursor, command_end, &declared_source) ||
        !decode_bps_integer(&cursor, command_end, &declared_target) ||
        !decode_bps_integer(&cursor, command_end, &metadata_size) ||
        declared_source != source_size || declared_target > SIZE_MAX ||
        metadata_size > (uint64_t)(command_end - cursor)) {
        return NULL;
    }
    cursor += (size_t)metadata_size;
    output = (uint8 *)malloc(declared_target == 0u ? 1u : (size_t)declared_target);
    if (output == NULL) {
        return NULL;
    }
    while (cursor < command_end && output_offset < (size_t)declared_target) {
        uint64_t encoded_command;
        uint64_t encoded_length;
        size_t length;
        unsigned action;
        if (!decode_bps_integer(&cursor, command_end, &encoded_command)) {
            goto invalid;
        }
        encoded_length = (encoded_command >> 2) + 1u;
        if (encoded_length > SIZE_MAX ||
            (size_t)encoded_length > (size_t)declared_target - output_offset) {
            goto invalid;
        }
        length = (size_t)encoded_length;
        action = (unsigned)(encoded_command & 3u);
        if (action == 0u) {
            if (output_offset > source_size || length > source_size - output_offset) {
                goto invalid;
            }
            memcpy(output + output_offset, source + output_offset, length);
            output_offset += length;
        } else if (action == 1u) {
            if (length > (size_t)(command_end - cursor)) {
                goto invalid;
            }
            memcpy(output + output_offset, cursor, length);
            cursor += length;
            output_offset += length;
        } else {
            uint64_t delta;
            int64_t *relative = action == 2u ? &source_relative : &target_relative;
            if (!decode_bps_integer(&cursor, command_end, &delta) ||
                !apply_relative_delta(delta, relative) || *relative < 0) {
                goto invalid;
            }
            if (action == 2u) {
                if ((uint64_t)*relative > source_size ||
                    length > source_size - (size_t)*relative) {
                    goto invalid;
                }
                memcpy(output + output_offset, source + (size_t)*relative, length);
            } else {
                size_t index;
                if ((uint64_t)*relative >= output_offset && length != 0u) {
                    goto invalid;
                }
                for (index = 0u; index < length; ++index) {
                    size_t read_offset = (size_t)*relative + index;
                    if (read_offset >= output_offset + index) {
                        goto invalid;
                    }
                    output[output_offset + index] = output[read_offset];
                }
            }
            *relative += (int64_t)length;
            output_offset += length;
        }
    }
    if (cursor != command_end || output_offset != (size_t)declared_target ||
        crc32_compute(output, output_offset) != read_le32(command_end + 4u)) {
        goto invalid;
    }
    *output_size = output_offset;
    return output;

invalid:
    free(output);
    return NULL;
}
