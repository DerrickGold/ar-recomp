/**
 * @file mutation.h
 * @brief Host-thread commands applied at deterministic runner safe points.
 * @ingroup sr_runner_mutation
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_mutation
 *  @{
 */

/** Small, copied commands applied at the beginning of a host frame before
 * emulation observes that frame's input or component state. Queueing is safe
 * from another host thread. The queue retains no caller pointer. */
#define SR_MUTATION_INLINE_BYTE_CAPACITY 16u

typedef uint32_t SrMutationType;
enum {
    SR_MUTATION_WRITE_MEMORY = 1u,
    SR_MUTATION_SET_INPUT = 2u
};

typedef struct SrMutationCommand {
    uint32_t struct_size;
    SrMutationType type;
    uint32_t flags;
    SrMemoryRegion memory_region;
    uint64_t address;
    uint32_t byte_count;
    uint32_t input_value;
    uint32_t input_mask;
    uint32_t reserved;
    uint8_t bytes[SR_MUTATION_INLINE_BYTE_CAPACITY];
} SrMutationCommand;

/** WRITE_MEMORY addresses are byte offsets within WRAM, SRAM, VRAM, CGRAM,
 * OAM, or high OAM. ROM, APU RAM, and DSP registers are rejected rather than
 * crossing an unsafe ownership boundary. SET_INPUT applies input_value under
 * input_mask to the packed controller word used by RtlRunFrame: controller 1
 * occupies bits 0-11 and controller 2 bits 12-23. Opposing directions are
 * normalized after the override. */

#define SR_MUTATION_COMMAND_V2_SIZE                                      \
    ((uint32_t)(offsetof(SrMutationCommand, bytes) +                      \
                sizeof(((SrMutationCommand *)0)->bytes)))

typedef uint32_t SrMutationState;
enum {
    SR_MUTATION_STATE_QUEUED = 1u,
    SR_MUTATION_STATE_APPLYING = 2u,
    SR_MUTATION_STATE_APPLIED = 3u,
    SR_MUTATION_STATE_FAILED = 4u
};

#define SR_MUTATION_QUERY_CONSUME UINT32_C(0x00000001)

typedef struct SrMutationStatus {
    uint32_t struct_size;
    SrMutationState state;
    SrResult result;
    uint32_t reserved;
    uint64_t command_id;
    uint64_t applied_frame_counter;
} SrMutationStatus;

/** SR_MUTATION_QUERY_CONSUME releases terminal APPLIED/FAILED records after
 * copying them to out_status. QUEUED/APPLYING records are never consumed. */

#define SR_MUTATION_STATUS_V2_SIZE                                       \
    ((uint32_t)(offsetof(SrMutationStatus, applied_frame_counter) +       \
                sizeof(((SrMutationStatus *)0)->applied_frame_counter)))

/** @} */

#ifdef __cplusplus
}
#endif
