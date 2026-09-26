#ifndef MANIFEST_UTILS_H
#define MANIFEST_UTILS_H

#include <stddef.h>

char *Manifest_Trim(char *text);
void Manifest_ResolvePath(const char *manifest_path, const char *value,
                          char *out, size_t out_size);

#endif
