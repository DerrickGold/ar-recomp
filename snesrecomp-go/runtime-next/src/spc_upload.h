#ifndef SNESRECOMP_NEXT_SPC_UPLOAD_H
#define SNESRECOMP_NEXT_SPC_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SrSpcUploadResult {
    uint16_t entry_point;
    uint16_t block_count;
    size_t script_offset;
} SrSpcUploadResult;

/* Parses the IPL block stream [length16, destination16, bytes...] followed by
 * [0, entry16]. ROM reads mirror at rom_size; ARAM writes wrap at 64 KiB. */
bool sr_spc_upload_image(const uint8_t *rom, size_t rom_size,
                         size_t source_offset, uint8_t aram[0x10000],
                         SrSpcUploadResult *result);

/* Applies a second-stage length-prefixed sample stream. Script bytes select
 * chunks from pool_offset. Chunks are packed consecutively in ARAM. */
bool sr_spc_upload_samples(const uint8_t *rom, size_t rom_size,
                           size_t script_offset, uint8_t segment_count,
                           size_t pool_offset, uint16_t first_destination,
                           uint8_t aram[0x10000], uint16_t *last_destination,
                           uint16_t *last_length);

#ifdef __cplusplus
}
#endif

#endif
