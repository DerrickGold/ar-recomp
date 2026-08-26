#include "util.h"

#include "crc32.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

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

static int ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static char *duplicate_string(const char *source) {
    const size_t length = strlen(source) + 1u;
    char *copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

char *NextDelim(char **text, int separator) {
    char *start;
    char *end;
    if (text == NULL || *text == NULL) {
        return NULL;
    }
    start = *text;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    end = strchr(start, separator);
    if (end == NULL) {
        *text = NULL;
    } else {
        *end = '\0';
        *text = end + 1;
    }
    return start;
}

bool StringEqualsNoCase(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    while (*left != '\0' && *right != '\0') {
        if (ascii_lower((unsigned char)*left) !=
            ascii_lower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

const char *StringStartsWithNoCase(const char *text, const char *prefix) {
    if (text == NULL || prefix == NULL) {
        return NULL;
    }
    while (*prefix != '\0') {
        if (*text == '\0' || ascii_lower((unsigned char)*text) !=
                                ascii_lower((unsigned char)*prefix)) {
            return NULL;
        }
        ++text;
        ++prefix;
    }
    return text;
}

uint8 *ReadWholeFile(const char *name, size_t *length) {
    FILE *file;
    long file_size;
    uint8 *result;
    if (length != NULL) {
        *length = 0u;
    }
    if (name == NULL || (file = fopen(name, "rb")) == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0 || (uintmax_t)file_size > SIZE_MAX - 1u) {
        fclose(file);
        return NULL;
    }
    result = (uint8 *)malloc((size_t)file_size + 1u);
    if (result == NULL) {
        fclose(file);
        return NULL;
    }
    if ((size_t)file_size != 0u &&
        fread(result, 1u, (size_t)file_size, file) != (size_t)file_size) {
        free(result);
        fclose(file);
        return NULL;
    }
    result[file_size] = 0u;
    if (fclose(file) != 0) {
        free(result);
        return NULL;
    }
    if (length != NULL) {
        *length = (size_t)file_size;
    }
    return result;
}

char *NextLineStripComments(char **text) {
    char *line;
    char *end;
    char *comment;
    if (text == NULL || *text == NULL) {
        return NULL;
    }
    line = *text;
    end = strchr(line, '\n');
    if (end == NULL) {
        *text = NULL;
        end = line + strlen(line);
    } else {
        *text = end + 1;
    }
    comment = (char *)memchr(line, '#', (size_t)(end - line));
    if (comment != NULL) {
        end = comment;
    }
    while (end > line &&
           (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    return line;
}

char *NextPossiblyQuotedString(char **text) {
    char *start;
    char *end;
    if (text == NULL || *text == NULL) {
        return NULL;
    }
    start = *text;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (*start == '"') {
        ++start;
        end = strchr(start, '"');
        if (end == NULL) {
            end = start + strlen(start);
        }
    } else {
        end = start;
        while (*end != '\0' && *end != ' ' && *end != '\t') {
            ++end;
        }
    }
    if (*end != '\0') {
        *end++ = '\0';
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    *text = end;
    return start;
}

char *ReplaceFilenameWithNewPath(const char *old_path, const char *new_name) {
    size_t directory_length;
    size_t name_length;
    char *result;
    if (old_path == NULL || new_name == NULL) {
        return NULL;
    }
    directory_length = strlen(old_path);
    while (directory_length != 0u && old_path[directory_length - 1u] != '/' &&
           old_path[directory_length - 1u] != '\\') {
        --directory_length;
    }
    name_length = strlen(new_name);
    if (directory_length > SIZE_MAX - name_length - 1u) {
        return NULL;
    }
    result = (char *)malloc(directory_length + name_length + 1u);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, old_path, directory_length);
    memcpy(result + directory_length, new_name, name_length + 1u);
    return result;
}

char *SplitKeyValue(char *text) {
    char *equals;
    char *key_end;
    char *value;
    if (text == NULL || (equals = strchr(text, '=')) == NULL) {
        return NULL;
    }
    key_end = equals;
    while (key_end > text && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
        --key_end;
    }
    *key_end = '\0';
    value = equals + 1;
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    return value;
}

const char *SkipPrefix(const char *text, const char *prefix) {
    if (text == NULL || prefix == NULL) {
        return NULL;
    }
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return NULL;
        }
    }
    return text;
}

void StrSet(char **destination, const char *source) {
    char *replacement;
    if (destination == NULL) {
        return;
    }
    replacement = source == NULL ? NULL : duplicate_string(source);
    if (source != NULL && replacement == NULL) {
        return;
    }
    free(*destination);
    *destination = replacement;
}

bool ParseBool(const char *value, bool *result) {
    bool parsed;
    if (value == NULL) {
        return false;
    }
    if (strcmp(value, "1") == 0 || StringEqualsNoCase(value, "true") ||
        StringEqualsNoCase(value, "yes") || StringEqualsNoCase(value, "on")) {
        parsed = true;
    } else if (strcmp(value, "0") == 0 ||
               StringEqualsNoCase(value, "false") ||
               StringEqualsNoCase(value, "no") ||
               StringEqualsNoCase(value, "off")) {
        parsed = false;
    } else {
        return false;
    }
    if (result != NULL) {
        *result = parsed;
        return true;
    }
    return parsed;
}

void ByteArray_Resize(ByteArray *array, size_t new_size) {
    uint8 *replacement;
    size_t capacity;
    if (array == NULL) {
        return;
    }
    if (new_size <= array->capacity) {
        array->size = new_size;
        return;
    }
    capacity = array->capacity;
    if (capacity < 8u) {
        capacity = 8u;
    }
    while (capacity < new_size) {
        size_t growth = capacity / 2u + 1u;
        if (capacity > SIZE_MAX - growth) {
            capacity = new_size;
            break;
        }
        capacity += growth;
    }
    replacement = (uint8 *)realloc(array->data, capacity);
    if (replacement == NULL) {
        abort();
    }
    array->data = replacement;
    array->capacity = capacity;
    array->size = new_size;
}

void ByteArray_Destroy(ByteArray *array) {
    if (array == NULL) {
        return;
    }
    free(array->data);
    array->data = NULL;
    array->size = 0u;
    array->capacity = 0u;
}

void ByteArray_AppendData(ByteArray *array, const uint8 *data,
                          size_t data_size) {
    size_t old_size;
    if (array == NULL || data_size == 0u) {
        return;
    }
    if (data == NULL || array->size > SIZE_MAX - data_size) {
        abort();
    }
    old_size = array->size;
    ByteArray_Resize(array, old_size + data_size);
    memcpy(array->data + old_size, data, data_size);
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
