/**
 * @file launcher.h
 * @brief Executable-relative paths and verified ROM selection helpers.
 * @ingroup sr_host
 */
#ifndef SNESRECOMP_HOST_LAUNCHER_H
#define SNESRECOMP_HOST_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_host
 *  @{
 */

/** Resolve a command-line ROM and require the expected IEEE CRC32. */
int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc);
/** Resolve a command-line ROM and require one SHA-256 digest. */
int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256);
/** Resolve a command-line ROM matching any digest in a bounded set. */
int snesrecomp_launcher_resolve_rom_sha256_multi(int argc, char **argv,
                                                 char *out_path, size_t max_len,
                                                 const uint8_t (*hashes)[32],
                                                 size_t hash_count);
/** Change the working directory to the executable's directory. */
int snesrecomp_anchor_to_exe_dir(void);
/** Write a normalized absolute path into caller-owned storage. */
int snesrecomp_abspath(const char *path, char *out, size_t max_len);
/** Resolve a leaf path relative to the executable's directory. */
int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
