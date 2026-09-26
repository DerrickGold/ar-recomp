#ifndef AR_MANIFEST_UTILS_H
#define AR_MANIFEST_UTILS_H
/* Manifest utils: helpers shared by the HD and music replacement manifests:
 * trim a value and resolve a path relative to the manifest file.
 * Phase: pure.
 * Tests: tests/consolidated_utils_test.c */

#include <stddef.h>

char *Manifest_Trim(char *text);
void Manifest_ResolvePath(const char *manifest_path, const char *value,
                          char *out, size_t out_size);

#endif
