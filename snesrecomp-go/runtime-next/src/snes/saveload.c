#include "saveload.h"

#include <string.h>

void saveload_bytes(SaveLoadInfo *info, void *data, size_t size) {
    if (info == NULL || info->func == NULL || info->failed) return;
    info->func(info, data, size);
}

void saveload_u8(SaveLoadInfo *info, uint8_t *value) {
    saveload_bytes(info, value, sizeof(*value));
}

void saveload_i8(SaveLoadInfo *info, int8_t *value) {
    saveload_bytes(info, value, sizeof(*value));
}

void saveload_bool(SaveLoadInfo *info, bool *value) {
    uint8_t encoded = info->saving && *value ? 1u : 0u;
    saveload_u8(info, &encoded);
    if (!info->saving && !info->failed) *value = encoded != 0u;
}

void saveload_u16(SaveLoadInfo *info, uint16_t *value) {
    uint8_t encoded[2];
    if (info->saving) {
        encoded[0] = (uint8_t)*value;
        encoded[1] = (uint8_t)(*value >> 8);
    }
    saveload_bytes(info, encoded, sizeof(encoded));
    if (!info->saving && !info->failed)
        *value = (uint16_t)encoded[0] | ((uint16_t)encoded[1] << 8);
}

void saveload_i16(SaveLoadInfo *info, int16_t *value) {
    uint16_t bits = 0u;
    if (info->saving) memcpy(&bits, value, sizeof(bits));
    saveload_u16(info, &bits);
    if (!info->saving && !info->failed) memcpy(value, &bits, sizeof(bits));
}

void saveload_u32(SaveLoadInfo *info, uint32_t *value) {
    uint8_t encoded[4];
    if (info->saving) {
        encoded[0] = (uint8_t)*value;
        encoded[1] = (uint8_t)(*value >> 8);
        encoded[2] = (uint8_t)(*value >> 16);
        encoded[3] = (uint8_t)(*value >> 24);
    }
    saveload_bytes(info, encoded, sizeof(encoded));
    if (!info->saving && !info->failed) {
        *value = (uint32_t)encoded[0] | ((uint32_t)encoded[1] << 8) |
                 ((uint32_t)encoded[2] << 16) | ((uint32_t)encoded[3] << 24);
    }
}

void saveload_i32(SaveLoadInfo *info, int32_t *value) {
    uint32_t bits = 0u;
    if (info->saving) memcpy(&bits, value, sizeof(bits));
    saveload_u32(info, &bits);
    if (!info->saving && !info->failed) memcpy(value, &bits, sizeof(bits));
}

void saveload_u64(SaveLoadInfo *info, uint64_t *value) {
    uint8_t encoded[8];
    if (info->saving) {
        for (unsigned byte = 0; byte < 8u; ++byte)
            encoded[byte] = (uint8_t)(*value >> (byte * 8u));
    }
    saveload_bytes(info, encoded, sizeof(encoded));
    if (!info->saving && !info->failed) {
        *value = 0u;
        for (unsigned byte = 0; byte < 8u; ++byte)
            *value |= (uint64_t)encoded[byte] << (byte * 8u);
    }
}

void saveload_f64(SaveLoadInfo *info, double *value) {
    uint64_t bits = 0u;
    _Static_assert(sizeof(bits) == sizeof(*value),
                   "snapshot format requires 64-bit doubles");
    if (info->saving) memcpy(&bits, value, sizeof(bits));
    saveload_u64(info, &bits);
    if (!info->saving && !info->failed) memcpy(value, &bits, sizeof(bits));
}

void saveload_u16_array(SaveLoadInfo *info, uint16_t *values, size_t count) {
    for (size_t index = 0; index < count; ++index)
        saveload_u16(info, &values[index]);
}

void saveload_i16_array(SaveLoadInfo *info, int16_t *values, size_t count) {
    for (size_t index = 0; index < count; ++index)
        saveload_i16(info, &values[index]);
}

void saveload_u32_array(SaveLoadInfo *info, uint32_t *values, size_t count) {
    for (size_t index = 0; index < count; ++index)
        saveload_u32(info, &values[index]);
}

bool saveload_decode_snapshot_header(const uint8_t bytes[8], uint32_t magic,
                                     uint32_t portable_version,
                                     uint32_t legacy_version,
                                     bool *portable) {
    uint32_t little_magic;
    uint32_t little_version;
    uint32_t native[2];
    if (bytes == NULL || portable == NULL) return false;
    little_magic = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                   ((uint32_t)bytes[2] << 16) |
                   ((uint32_t)bytes[3] << 24);
    little_version = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) |
                     ((uint32_t)bytes[6] << 16) |
                     ((uint32_t)bytes[7] << 24);
    if (little_magic == magic && little_version == portable_version) {
        *portable = true;
        return true;
    }
    memcpy(native, bytes, sizeof(native));
    if (native[0] == magic && native[1] == legacy_version) {
        *portable = false;
        return true;
    }
    return false;
}
