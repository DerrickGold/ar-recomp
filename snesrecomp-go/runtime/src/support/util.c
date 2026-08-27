#include "util.h"

#include "snesrecomp/support/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

uint8_t *snesrecomp_read_whole_file(const char *name, size_t *length) {
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

