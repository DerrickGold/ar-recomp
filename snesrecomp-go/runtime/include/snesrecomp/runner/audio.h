/**
 * @file audio.h
 * @brief Bounded SPC control and native SNES audio-mix policy.
 * @ingroup sr_runner_audio
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_audio
 *  @{
 */

/** Synchronous, atomic SPC program-counter control for narrow game-adapter
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

/** Synchronous host-mix policy. Percentages are inclusive 0..100 values and
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

/** Caller-owned destinations for one coherent APU snapshot. Both buffers
 * are required and must have at least the documented byte capacities. The
 * runner retains neither pointer after this synchronous operation returns. */
typedef struct SrApuStateQuery {
    uint32_t struct_size;
    uint32_t flags;
    uint8_t *apu_ram;
    uint64_t apu_ram_capacity;
    uint8_t *dsp_registers;
    uint64_t dsp_register_capacity;
    uint32_t reserved[2];
} SrApuStateQuery;

#define SR_APU_STATE_QUERY_V2_SIZE                                      \
    ((uint32_t)(offsetof(SrApuStateQuery, reserved) +                    \
                sizeof(((SrApuStateQuery *)0)->reserved)))

#define SR_APU_STATE_SPC_STOPPED UINT32_C(0x00000001)
#define SR_APU_STATE_DSP_MUTED UINT32_C(0x00000002)
#define SR_APU_STATE_DSP_RESET UINT32_C(0x00000004)
#define SR_APU_STATE_BOOT_ROM_VISIBLE UINT32_C(0x00000008)
#define SR_APU_STATE_EXTENDED_VOICES UINT32_C(0x00000010)

/** Scalar portion of a coherent APU snapshot. dsp_registers contains the
 * SNES-visible 128-byte register image: dynamic readback registers reflect
 * their most recently committed DSP slot, not pending internal pipeline
 * values. dsp_slot names the next slot in the 0..31 native DSP schedule.
 * PCM cursors describe runner transport, not emulated hardware registers. */
typedef struct SrApuStateSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t load_generation;
    uint64_t apu_cycles;
    uint64_t dsp_frames_completed;
    uint64_t pcm_write_cursor;
    uint64_t pcm_read_cursor;
    uint64_t apu_ram_bytes_written;
    uint64_t dsp_register_bytes_written;
    uint32_t pcm_ring_fill_frames;
    uint32_t scheduled_port_write_count;
    uint16_t spc_pc;
    uint16_t spc_instruction_pc;
    uint8_t spc_a;
    uint8_t spc_x;
    uint8_t spc_y;
    uint8_t spc_sp;
    uint8_t spc_psw;
    uint8_t spc_instruction_cycle;
    uint8_t dsp_slot;
    uint8_t current_dsp_address;
    uint8_t cpu_to_apu_ports[SR_APU_CPU_PORT_COUNT];
    uint8_t apu_aux_ports[SR_APU_AUX_PORT_COUNT];
    uint8_t apu_to_cpu_ports[SR_APU_OUTPUT_PORT_COUNT];
    uint8_t timer_targets[3];
    uint8_t timer_outputs[3];
    uint8_t timer_enabled_mask;
    uint8_t hardware_voice_count;
    uint8_t extended_voice_count;
    uint8_t reserved8[4];
    uint32_t reserved32[2];
} SrApuStateSnapshot;

#define SR_APU_STATE_SNAPSHOT_V2_SIZE                                   \
    ((uint32_t)(offsetof(SrApuStateSnapshot, reserved32) +               \
                sizeof(((SrApuStateSnapshot *)0)->reserved32)))

/** @} */

#ifdef __cplusplus
}
#endif
