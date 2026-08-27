#ifndef SNESRECOMP_SUPPORT_FILE_H
#define SNESRECOMP_SUPPORT_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads a complete file into malloc-owned storage. The caller must free the
 * returned buffer. Returns NULL on open, seek, size, or allocation failure. */
uint8_t *snesrecomp_read_whole_file(const char *path, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
