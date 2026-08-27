#ifndef SNESRECOMP_NEXT_AUDIO_ADAPTER_H
#define SNESRECOMP_NEXT_AUDIO_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width, callback-lifetime views used by a game adapter while the SPC
 * owns the live APU/DSP state. ARAM and the current bus labels are read-only;
 * the runner validates and applies the bounded routing result after the
 * callback returns. */
#define RTL_AUDIO_VOICE_BUS_UNCLASSIFIED 0u
#define RTL_AUDIO_VOICE_BUS_MUSIC 1u
#define RTL_AUDIO_VOICE_BUS_SFX 2u
#define RTL_AUDIO_ADAPTER_HARDWARE_VOICE_COUNT 8u
#define RTL_AUDIO_ADAPTER_EXTENDED_VOICE_COUNT 32u
#define RTL_AUDIO_ADAPTER_APU_RAM_BYTE_COUNT 65536u
#define RTL_AUDIO_ADAPTER_VOICE_MAX                                      \
    (RTL_AUDIO_ADAPTER_HARDWARE_VOICE_COUNT +                            \
     RTL_AUDIO_ADAPTER_EXTENDED_VOICE_COUNT)
#define RTL_AUDIO_DSP_WRITE_UPDATE_MAX 2u

typedef struct RtlAudioDspWriteContext {
    uint32_t struct_size;
    uint32_t flags;
    const uint8_t *apu_ram;
    const uint8_t *voice_bus;
    uint64_t apu_ram_byte_size;
    uint32_t voice_bus_count;
    uint8_t spc_x;
    uint8_t dsp_address;
    uint8_t dsp_value;
    uint8_t extended_voices_enabled;
} RtlAudioDspWriteContext;

#define RTL_AUDIO_DSP_WRITE_CONTEXT_V2_SIZE                              \
    ((uint32_t)(offsetof(RtlAudioDspWriteContext,                        \
                         extended_voices_enabled) +                       \
                sizeof(((RtlAudioDspWriteContext *)0)->                  \
                           extended_voices_enabled)))

typedef struct RtlAudioVoiceBusUpdate {
    uint8_t voice;
    uint8_t bus;
    uint8_t reserved8[2];
} RtlAudioVoiceBusUpdate;

typedef struct RtlAudioDspWriteRouting {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t update_count;
    uint32_t reserved;
    RtlAudioVoiceBusUpdate update[RTL_AUDIO_DSP_WRITE_UPDATE_MAX];
} RtlAudioDspWriteRouting;

#define RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE                              \
    ((uint32_t)(offsetof(RtlAudioDspWriteRouting, update) +               \
                sizeof(((RtlAudioDspWriteRouting *)0)->update)))

typedef struct RtlAudioStateLoadedContext {
    uint32_t struct_size;
    uint32_t flags;
    const uint8_t *apu_ram;
    uint64_t apu_ram_byte_size;
    uint32_t voice_bus_count;
    uint8_t extended_voices_enabled;
    uint8_t reserved8[3];
} RtlAudioStateLoadedContext;

#define RTL_AUDIO_STATE_LOADED_CONTEXT_V2_SIZE                           \
    ((uint32_t)(offsetof(RtlAudioStateLoadedContext, reserved8) +         \
                sizeof(((RtlAudioStateLoadedContext *)0)->reserved8)))

typedef struct RtlAudioStateLoadedRouting {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t voice_bus_count;
    uint32_t reserved;
    uint8_t voice_bus[RTL_AUDIO_ADAPTER_VOICE_MAX];
} RtlAudioStateLoadedRouting;

#define RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE                           \
    ((uint32_t)(offsetof(RtlAudioStateLoadedRouting, voice_bus) +         \
                sizeof(((RtlAudioStateLoadedRouting *)0)->voice_bus)))

/* Synchronous game-extension access while the runner owns the live APU/DSP
 * state. ARAM is the SNES-visible memory region, not a concrete component
 * layout. It is mutable only for the duration of the callback. DSP mutations
 * go through the validated operation function, and the runner copies the
 * fixed-width SPC register values back after an opcode callback returns. */
typedef enum RtlAudioDspOperation {
    RTL_AUDIO_DSP_SET_VOICE_BUS = 1,
    RTL_AUDIO_DSP_WRITE_VIRTUAL_REGISTER = 2,
    RTL_AUDIO_DSP_WRITE_VIRTUAL_CONTROL = 3,
    RTL_AUDIO_DSP_WRITE_HARDWARE_MASK = 4,
} RtlAudioDspOperation;

typedef bool RtlAudioDspOperationFunc(
    void *service_context, uint32_t operation, uint32_t voice,
    uint8_t address, uint8_t value, uint8_t update_mask);

typedef struct RtlAudioExtensionContext {
    uint32_t struct_size;
    uint32_t flags;
    uint8_t *apu_ram;
    uint64_t apu_ram_byte_size;
    uint32_t hardware_voice_count;
    uint32_t extended_voice_count;
    uint16_t spc_pc;
    uint8_t spc_x;
    uint8_t spc_z;
    void *service_context;
    RtlAudioDspOperationFunc *dsp_operation;
} RtlAudioExtensionContext;

#define RTL_AUDIO_EXTENSION_CONTEXT_V2_SIZE                              \
    ((uint32_t)(offsetof(RtlAudioExtensionContext, dsp_operation) +       \
                sizeof(((RtlAudioExtensionContext *)0)->dsp_operation)))

typedef enum RtlAudioSaveValueKind {
    RTL_AUDIO_SAVE_BYTES = 1,
    RTL_AUDIO_SAVE_U8 = 2,
    RTL_AUDIO_SAVE_U16 = 3,
    RTL_AUDIO_SAVE_U32 = 4,
    RTL_AUDIO_SAVE_U64 = 5,
} RtlAudioSaveValueKind;

typedef bool RtlAudioSaveTransferFunc(
    void *service_context, uint32_t kind, void *values, uint64_t count);

typedef struct RtlAudioSaveContext {
    uint32_t struct_size;
    uint32_t flags;
    uint8_t saving;
    uint8_t portable;
    uint8_t reserved8[2];
    void *service_context;
    RtlAudioSaveTransferFunc *transfer;
} RtlAudioSaveContext;

#define RTL_AUDIO_SAVE_CONTEXT_V2_SIZE                                   \
    ((uint32_t)(offsetof(RtlAudioSaveContext, transfer) +                 \
                sizeof(((RtlAudioSaveContext *)0)->transfer)))

#ifdef __cplusplus
}
#endif

#endif
