#ifndef ZELDA3_UTIL_H_
#define ZELDA3_UTIL_H_

#include "types.h"

typedef struct SDL_Window SDL_Window;

struct RendererFuncs {
    bool (*Initialize)(SDL_Window *window);
    void (*Destroy)(void);
    void (*BeginDraw)(int width, int height, uint8 **pixels, int *pitch);
    void (*EndDraw)(void);
};

typedef struct ByteArray {
    uint8 *data;
    size_t size;
    size_t capacity;
} ByteArray;

void ByteArray_Resize(ByteArray *array, size_t new_size);
void ByteArray_Destroy(ByteArray *array);
void ByteArray_AppendData(ByteArray *array, const uint8 *data, size_t data_size);

uint8 *ReadWholeFile(const char *name, size_t *length);
char *NextDelim(char **text, int separator);
char *NextLineStripComments(char **text);
char *NextPossiblyQuotedString(char **text);
char *SplitKeyValue(char *text);
bool StringEqualsNoCase(const char *left, const char *right);
const char *StringStartsWithNoCase(const char *text, const char *prefix);
bool ParseBool(const char *value, bool *result);
const char *SkipPrefix(const char *text, const char *prefix);
void StrSet(char **destination, const char *source);
char *ReplaceFilenameWithNewPath(const char *old_path, const char *new_name);
uint8 *ApplyBps(const uint8 *source, size_t source_size,
                const uint8 *patch, size_t patch_size, size_t *output_size);

#endif
