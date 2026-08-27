#ifndef SNESRECOMP_UTIL_H
#define SNESRECOMP_UTIL_H

#include "snesrecomp/game/types.h"

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
