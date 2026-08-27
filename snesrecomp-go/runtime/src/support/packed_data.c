#include "snesrecomp/game/types.h"

#include <stddef.h>
#include <stdint.h>

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le24(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16);
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

MemBlk FindIndexInMemblk(MemBlk data, size_t index) {
    size_t table_end;
    size_t item_count;
    size_t payload_start;
    size_t left;
    size_t right;
    uint16_t descriptor;
    MemBlk empty = {NULL, 0u};
    if (data.ptr == NULL || data.size < 2u) {
        return empty;
    }
    table_end = data.size - 2u;
    descriptor = read_le16(data.ptr + table_end);
    item_count = descriptor & 0x0fffu;
    if ((descriptor & 0x4000u) != 0u) {
        size_t mapped_count;
        if (table_end < 2u) {
            return empty;
        }
        table_end -= 2u;
        mapped_count = read_le16(data.ptr + table_end);
        if (index > item_count) {
            return empty;
        }
        if (mapped_count >= 256u) {
            size_t map_bytes;
            if (item_count > (SIZE_MAX / 2u) - 1u) {
                return empty;
            }
            map_bytes = (item_count + 1u) * 2u;
            if (map_bytes > table_end) {
                return empty;
            }
            table_end -= map_bytes;
            index = read_le16(data.ptr + table_end + index * 2u);
        } else {
            size_t map_bytes = item_count + 1u;
            if (map_bytes > table_end) {
                return empty;
            }
            table_end -= map_bytes;
            index = data.ptr[table_end + index];
        }
        item_count = mapped_count;
    }
    if (index > item_count) {
        return empty;
    }
    if ((descriptor & 0x8000u) != 0u) {
        size_t offset_bytes;
        if (item_count > SIZE_MAX / 2u) {
            return empty;
        }
        offset_bytes = item_count * 2u;
        if (offset_bytes > table_end) {
            return empty;
        }
        payload_start = offset_bytes;
        left = index == 0u ? payload_start
                           : payload_start +
                                 read_le16(data.ptr + (index - 1u) * 2u);
        right = index == item_count
                    ? table_end
                    : payload_start + read_le16(data.ptr + index * 2u);
    } else {
        size_t offset_bytes;
        if (item_count > SIZE_MAX / 4u) {
            return empty;
        }
        offset_bytes = item_count * 4u;
        if (offset_bytes > table_end) {
            return empty;
        }
        payload_start = offset_bytes;
        left = index == 0u ? payload_start
                           : payload_start +
                                 read_le32(data.ptr + (index - 1u) * 4u);
        right = index == item_count
                    ? table_end
                    : payload_start + read_le32(data.ptr + index * 4u);
    }
    if (left > right || right > table_end) {
        return empty;
    }
    return (MemBlk){data.ptr + left, right - left};
}

const uint8 *FindAddrInMemblk(MemBlk data, uint32 addr) {
    size_t count;
    size_t addresses_end;
    size_t offsets_end;
    size_t low;
    size_t high;
    size_t selected;
    uint32 source_address;
    uint32 payload_offset;
    uint64_t resolved;
    if (data.ptr == NULL || data.size < 2u) {
        return NULL;
    }
    count = read_le16(data.ptr);
    if (count == 0u || count > (SIZE_MAX - 2u) / 6u) {
        return NULL;
    }
    addresses_end = 2u + count * 3u;
    offsets_end = addresses_end + count * 3u;
    if (offsets_end > data.size || addr < read_le24(data.ptr + 2u)) {
        return NULL;
    }
    low = 0u;
    high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uint32 candidate = read_le24(data.ptr + 2u + middle * 3u);
        if (candidate <= addr) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    selected = low - 1u;
    source_address = read_le24(data.ptr + 2u + selected * 3u);
    payload_offset = read_le24(data.ptr + addresses_end + selected * 3u);
    resolved = (uint64_t)payload_offset + (uint64_t)(addr - source_address);
    if (resolved >= data.size) {
        return NULL;
    }
    return data.ptr + (size_t)resolved;
}
