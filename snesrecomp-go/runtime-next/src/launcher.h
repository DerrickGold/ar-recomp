#ifndef SNESRECOMP_NEXT_LAUNCHER_H
#define SNESRECOMP_NEXT_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc);
int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256);
int snesrecomp_launcher_resolve_rom_sha256_multi(int argc, char **argv,
                                                 char *out_path, size_t max_len,
                                                 const uint8_t (*hashes)[32],
                                                 size_t hash_count);
int snesrecomp_anchor_to_exe_dir(void);
int snesrecomp_abspath(const char *path, char *out, size_t max_len);
int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif
