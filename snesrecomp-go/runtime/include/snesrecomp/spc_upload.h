/**
 * @file spc_upload.h
 * @brief Portable IPL and sample-stream upload helpers for game adapters.
 * @ingroup sr_game_audio
 */
#ifndef SNESRECOMP_SPC_UPLOAD_H
#define SNESRECOMP_SPC_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_game_audio
 *  @{
 */

typedef struct SrSpcUploadResult {
    uint16_t entry_point;
    uint16_t block_count;
    uint64_t script_offset;
} SrSpcUploadResult;

/** Callback-lifetime transaction used by a game adapter while the runner owns
 * the APU lock. ROM is immutable; ARAM is the live mutable 64 KiB image. The
 * callback requests narrow SPC control by setting control_flags and the
 * associated fields. The runner validates and applies those requests before
 * releasing the lock. */
#define SR_SPC_UPLOAD_STATE_INITIAL UINT32_C(0x00000001)
#define SR_SPC_UPLOAD_STATE_SPC_STOPPED UINT32_C(0x00000002)
#define SR_SPC_UPLOAD_CONTROL_SET_PC UINT32_C(0x00000001)
#define SR_SPC_UPLOAD_CONTROL_RUN_UNTIL_PC UINT32_C(0x00000002)
#define SR_SPC_UPLOAD_STOP_PC_MAX 2u
#define SR_SPC_UPLOAD_MAX_CONTROL_CYCLES UINT32_C(0x01000000)

typedef struct SrSpcUploadContext {
    uint32_t struct_size;
    uint32_t state_flags;
    uint32_t control_flags;
    uint32_t max_cycles;
    const uint8_t *rom_data;
    uint8_t *apu_ram;
    uint64_t rom_byte_size;
    uint64_t apu_ram_byte_size;
    uint64_t script_offset;
    uint16_t entry_point;
    uint16_t block_count;
    uint16_t spc_pc;
    uint16_t requested_pc;
    uint16_t stop_pc[SR_SPC_UPLOAD_STOP_PC_MAX];
    uint8_t stop_pc_count;
    uint8_t reserved8[3];
} SrSpcUploadContext;

#define SR_SPC_UPLOAD_CONTEXT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrSpcUploadContext, reserved8) +                 \
                sizeof(((SrSpcUploadContext *)0)->reserved8)))

/**
 * @brief Parses an IPL block stream into ARAM.
 * @param[in] rom Immutable ROM bytes; reads mirror at `rom_size`.
 * @param[in] rom_size Number of ROM bytes; must be nonzero.
 * @param[in] source_offset First `[length16, destination16, bytes...]` block.
 * @param[out] aram Mutable 64 KiB ARAM image; writes wrap at 64 KiB.
 * @param[out] result Parsed entry point and block metadata.
 * @return `true` when a terminating `[0, entry16]` record was decoded.
 */
bool sr_spc_upload_image(const uint8_t *rom, size_t rom_size,
                         size_t source_offset, uint8_t aram[0x10000],
                         SrSpcUploadResult *result);

/**
 * @brief Applies a second-stage length-prefixed sample stream.
 *
 * Script bytes select chunks from `pool_offset`; decoded chunks are packed
 * consecutively in ARAM.
 * @return `true` when every selected chunk fits and decodes successfully.
 */
bool sr_spc_upload_samples(const uint8_t *rom, size_t rom_size,
                           size_t script_offset, uint8_t segment_count,
                           size_t pool_offset, uint16_t first_destination,
                           uint8_t aram[0x10000], uint16_t *last_destination,
                           uint16_t *last_length);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
