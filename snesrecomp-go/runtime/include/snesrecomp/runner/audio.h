#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous, atomic SPC program-counter control for narrow game-adapter
 * handshakes. The runner holds the APU lock while it compares the inclusive
 * PC range and up to eight consecutive ARAM bytes, then applies the new PC
 * only when every predicate matches. This is an emulation-thread service; it
 * must not be called from an audio callback or audio-trace observer. */
#define SR_SPC_PC_EXPECTED_ARAM_MAX 8u

typedef struct SrSpcPcControlRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint16_t expected_pc_low;
    uint16_t expected_pc_high;
    uint16_t replacement_pc;
    uint16_t expected_aram_address;
    uint8_t expected_aram_count;
    uint8_t reserved8[3];
    uint8_t expected_aram[SR_SPC_PC_EXPECTED_ARAM_MAX];
} SrSpcPcControlRequest;

#define SR_SPC_PC_CONTROL_REQUEST_V2_SIZE                                \
    ((uint32_t)(offsetof(SrSpcPcControlRequest, expected_aram) +          \
                sizeof(((SrSpcPcControlRequest *)0)->expected_aram)))

#define SR_SPC_PC_CONTROL_MATCHED UINT32_C(0x00000001)
#define SR_SPC_PC_CONTROL_WRITTEN UINT32_C(0x00000002)

typedef struct SrSpcPcControlResult {
    uint32_t struct_size;
    uint32_t flags;
    uint16_t observed_pc;
    uint16_t current_pc;
    uint32_t reserved;
} SrSpcPcControlResult;

#define SR_SPC_PC_CONTROL_RESULT_V2_SIZE                                 \
    ((uint32_t)(offsetof(SrSpcPcControlResult, reserved) +                \
                sizeof(((SrSpcPcControlResult *)0)->reserved)))

/* Synchronous host-mix policy. Percentages are inclusive 0..100 values and
 * affect the runner's native DSP output buses; replacement-stream volume is a
 * host concern and remains outside this request. */
typedef struct SrAudioMixControl {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t music_gain_percent;
    uint32_t sfx_gain_percent;
    uint32_t reserved[2];
} SrAudioMixControl;

#define SR_AUDIO_MIX_CONTROL_V2_SIZE                                     \
    ((uint32_t)(offsetof(SrAudioMixControl, reserved) +                   \
                sizeof(((SrAudioMixControl *)0)->reserved)))

#ifdef __cplusplus
}
#endif

