/**
 * @file file.h
 * @brief Portable whole-file loading helper.
 * @ingroup sr_support
 */
#ifndef SNESRECOMP_SUPPORT_FILE_H
#define SNESRECOMP_SUPPORT_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_support
 *  @{
 */

/**
 * @brief Reads a complete file into newly allocated storage.
 * @param[in] path File to read.
 * @param[out] out_size Receives the byte count when non-`NULL`.
 * @return `malloc`-owned bytes for the caller to free, or `NULL` on failure.
 */
uint8_t *snesrecomp_read_whole_file(const char *path, size_t *out_size);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
