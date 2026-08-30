/**
 * @file events.h
 * @brief Synchronous runner, execution, and audio observation contracts.
 * @ingroup sr_runner_events
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_events
 *  @{
 */

/** Generated-code execution state. Names are immutable runner-owned strings;
 * the snapshot itself expires with the runner lifetime generation. The stack
 * is ordered outermost first, matching actual host/recompiled nesting. */
#define SR_EXECUTION_STACK_CAPACITY 64u
#define SR_EXECUTION_HISTORY_CAPACITY 256u
#define SR_EXECUTION_CURRENT_BLOCK_VALID UINT32_C(0x00000001)
#define SR_EXECUTION_CURRENT_FUNCTION_VALID UINT32_C(0x00000002)
#define SR_EXECUTION_STACK_TRUNCATED UINT32_C(0x00000004)

typedef struct SrExecutionFrame {
    const char *function_name;
    uint16_t entry_stack;
    uint8_t host_return_valid;
    uint8_t reserved;
} SrExecutionFrame;

typedef struct SrExecutionBlock {
    uint32_t pc24;
    uint32_t cpu_flags;
    uint16_t register_x;
    uint16_t stack_pointer;
} SrExecutionBlock;

typedef struct SrExecutionSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t block_serial;
    uint32_t current_block_pc24;
    uint32_t stack_depth;
    uint32_t history_count;
    uint32_t reserved;
    const char *current_function;
    SrExecutionFrame stack[SR_EXECUTION_STACK_CAPACITY];
    SrExecutionBlock history[SR_EXECUTION_HISTORY_CAPACITY];
} SrExecutionSnapshot;

#define SR_EXECUTION_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrExecutionSnapshot, history) +                  \
                sizeof(((SrExecutionSnapshot *)0)->history)))

/** Synchronous event observers. Event classes are checked before payload
 * construction, and field filters are applied before callback dispatch.
 * Callbacks run on the producing runner or audio thread and are serialized
 * across producers. Install and remove subscriptions only while those
 * producers are stopped. Callbacks must not retain event-owned pointers or
 * re-enter runner execution, audio production, mutation, ticking, or
 * subscription APIs. Label strings remain runner-owned until unbound. */
typedef uint64_t SrEventMask;
#define SR_EVENT_MASK_EXECUTION_BLOCK UINT64_C(0x0000000000000001)
#define SR_EVENT_MASK_DYNAMIC_DISPATCH UINT64_C(0x0000000000000002)
#define SR_EVENT_MASK_RECOMP_FUNCTION UINT64_C(0x0000000000000004)
#define SR_EVENT_MASK_MEMORY_WRITE UINT64_C(0x0000000000000008)
#define SR_EVENT_MASK_REGISTER_ACCESS UINT64_C(0x0000000000000010)
#define SR_EVENT_MASK_DMA UINT64_C(0x0000000000000020)
#define SR_EVENT_MASK_AUDIO UINT64_C(0x0000000000000040)
#define SR_EVENT_MASK_FRAME UINT64_C(0x0000000000000080)
#define SR_EVENT_MASK_INTERRUPT UINT64_C(0x0000000000000100)
#define SR_EVENT_MASK_ERROR UINT64_C(0x0000000000000200)
#define SR_EVENT_MASK_V2_SUPPORTED                                      \
    (SR_EVENT_MASK_EXECUTION_BLOCK | SR_EVENT_MASK_DYNAMIC_DISPATCH |    \
     SR_EVENT_MASK_MEMORY_WRITE | SR_EVENT_MASK_REGISTER_ACCESS |        \
     SR_EVENT_MASK_DMA | SR_EVENT_MASK_AUDIO | SR_EVENT_MASK_FRAME |      \
     SR_EVENT_MASK_INTERRUPT | SR_EVENT_MASK_ERROR)

typedef uint32_t SrEventType;
enum {
    SR_EVENT_EXECUTION_BLOCK = 1u,
    SR_EVENT_DYNAMIC_DISPATCH = 2u,
    SR_EVENT_RECOMP_FUNCTION_ENTER = 3u,
    SR_EVENT_RECOMP_FUNCTION_EXIT = 4u,
    SR_EVENT_MEMORY_WRITE = 5u,
    SR_EVENT_REGISTER_READ = 6u,
    SR_EVENT_REGISTER_WRITE = 7u,
    SR_EVENT_DMA_BEGIN = 8u,
    SR_EVENT_AUDIO_PRODUCED = 9u,
    SR_EVENT_FRAME_BOUNDARY = 10u,
    SR_EVENT_INTERRUPT = 11u,
    SR_EVENT_ERROR = 12u
};

#define SR_EVENT_DISPATCH_FOUND UINT32_C(0x00000001)
#define SR_EVENT_DISPATCH_MIRRORED UINT32_C(0x00000002)
/* The source instruction is RTS/RTL, so a registry miss names an internal
 * continuation or ordinary hardware return rather than a missing handler. */
#define SR_EVENT_DISPATCH_CONTINUATION UINT32_C(0x00000004)
/* The generated edge recorded its architectural target immediately before
 * executing the existing unresolved-indirect hard diagnostic. */
#define SR_EVENT_DISPATCH_TRAPPED UINT32_C(0x00000008)
#define SR_EVENT_DMA_HDMA UINT32_C(0x00000001)
#define SR_EVENT_DMA_FROM_B_BUS UINT32_C(0x00000002)
#define SR_EVENT_DMA_FIXED_A_BUS UINT32_C(0x00000004)
#define SR_EVENT_DMA_DECREMENT_A_BUS UINT32_C(0x00000008)
#define SR_EVENT_DMA_INDIRECT UINT32_C(0x00000010)
#define SR_EVENT_AUDIO_FINAL_MIX UINT32_C(0x00000001)
#define SR_EVENT_AUDIO_TRANSIENT_SAMPLES UINT32_C(0x00000002)
#define SR_EVENT_FRAME_BEGIN UINT32_C(0x00000001)
#define SR_EVENT_FRAME_END UINT32_C(0x00000002)
#define SR_EVENT_FRAME_VBLANK UINT32_C(0x00000004)
#define SR_EVENT_FRAME_HOST_TICK UINT32_C(0x00000008)
#define SR_EVENT_FRAME_GAME_SLICE UINT32_C(0x00000010)
#define SR_EVENT_FRAME_SCANOUT UINT32_C(0x00000020)
#define SR_EVENT_INTERRUPT_ENTER UINT32_C(0x00000001)
#define SR_EVENT_INTERRUPT_EXIT UINT32_C(0x00000002)
#define SR_EVENT_INTERRUPT_TRANSITION UINT32_C(0x00000004)
#define SR_EVENT_INTERRUPT_CALLBACK UINT32_C(0x00000008)
#define SR_EVENT_ERROR_RECOVERABLE UINT32_C(0x00000001)

typedef uint16_t SrAudioSampleFormat;
enum {
    SR_AUDIO_SAMPLE_FORMAT_S16_NATIVE = 1u
};

typedef uint32_t SrInterruptKind;
enum {
    SR_INTERRUPT_NMI = 1u,
    SR_INTERRUPT_IRQ = 2u,
    SR_INTERRUPT_BRK = 3u,
    SR_INTERRUPT_COP = 4u,
    SR_INTERRUPT_ABORT = 5u,
    SR_INTERRUPT_RESET = 6u
};

#define SR_INTERRUPT_SCANLINE_UNKNOWN INT32_MIN

typedef uint32_t SrRunnerErrorCode;
enum {
    SR_RUNNER_ERROR_UNREACHABLE = 1u,
    SR_RUNNER_ERROR_UNMAPPED_ROM = 2u,
    SR_RUNNER_ERROR_DISPATCH_MISS = 3u,
    SR_RUNNER_ERROR_DISPATCH_RECURSION_LIMIT = 4u
};

typedef struct SrRunnerEvent {
    uint32_t struct_size;
    SrEventType type;
    uint64_t serial;
    uint64_t frame_counter;
    uint32_t flags;
    uint32_t cpu_flags;
    uint32_t pc24;
    uint32_t source_pc24;
    SrMemoryRegion memory_region;
    uint32_t address;
    uint32_t previous_value;
    uint32_t value;
    uint32_t width_bytes;
    uint16_t register_x;
    uint16_t stack_pointer;
    const char *label;
    uint32_t dma_a_address24;
    uint32_t dma_transfer_bytes;
    uint16_t dma_table_address;
    uint8_t dma_channel;
    uint8_t dma_mode;
    uint8_t dma_b_address;
    uint8_t dma_indirect_bank;
    uint8_t reserved8[2];
    SrInterruptKind interrupt_kind;
    int32_t interrupt_scanline;
    SrRunnerErrorCode error_code;
    uint16_t interrupt_vector;
    uint16_t reserved16;
    uint32_t reserved32;
    uint64_t audio_frame_offset;
    const int16_t *audio_samples;
    uint32_t audio_frame_count;
    uint32_t audio_sample_rate;
    uint16_t audio_channel_count;
    SrAudioSampleFormat audio_sample_format;
    uint32_t reserved_audio;
} SrRunnerEvent;

/** Memory-event addresses are zero-based byte offsets within memory_region.
 * Register-event addresses are the complete CPU-visible register addresses;
 * previous_value and memory_region are not defined for register events.
 * DMA_BEGIN address and dma_a_address24 are the initial A-bus address. For
 * HDMA this is the table start, dma_table_address is its 16-bit offset, and
 * dma_transfer_bytes is zero because the table determines the total. General
 * DMA normalizes a programmed size of zero to 65536 bytes. Frame BEGIN/END
 * events describe the boundary named by HOST_TICK, GAME_SLICE, or SCANOUT; a
 * host tick has no implied hardware phase. VBLANK means that boundary
 * positioned the modeled beam at VBlank. Interrupt events identify the vector
 * and use SR_INTERRUPT_SCANLINE_UNKNOWN when no raster position applies.
 * TRANSITION reports a runner-owned interrupt-latch transition, not handler
 * execution;
 * CALLBACK brackets runner delivery of a game-owned callback and leaves pc24
 * and interrupt_vector unspecified. Unqualified interrupt events are emitted
 * by game glue around actual handler execution. Error events identify their
 * stable error_code; pc24 is the affected execution/ROM address and
 * source_pc24 is its caller when known.
 * AUDIO_PRODUCED reports the final interleaved host mix. audio_frame_offset is
 * the first sample frame's monotonic output clock since reset. audio_samples is
 * valid only during the callback and must not be retained; frame_counter is
 * zero for audio-thread events. */

#define SR_RUNNER_EVENT_V2_SIZE                                          \
    ((uint32_t)(offsetof(SrRunnerEvent, reserved_audio) +                 \
                sizeof(((SrRunnerEvent *)0)->reserved_audio)))

typedef void (*SrRunnerEventCallback)(void *user_data,
                                      SrRunnerHandle *runner,
                                      const SrRunnerEvent *event);

#define SR_EVENT_FILTER_PC_RANGE UINT32_C(0x00000001)
#define SR_EVENT_FILTER_ADDRESS_RANGE UINT32_C(0x00000002)
#define SR_EVENT_FILTER_MEMORY_REGION UINT32_C(0x00000004)

typedef struct SrEventSubscription {
    uint32_t struct_size;
    uint32_t flags;
    SrEventMask event_mask;
    uint32_t pc_first;
    uint32_t pc_last;
    SrMemoryRegion memory_region;
    uint32_t address_first;
    uint32_t address_last;
    SrRunnerEventCallback callback;
    void *user_data;
} SrEventSubscription;

#define SR_EVENT_SUBSCRIPTION_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrEventSubscription, user_data) +                \
                sizeof(((SrEventSubscription *)0)->user_data)))

/** Low-level, synchronous APU/SPC observation for diagnostics. The callback
 * runs while the producing thread owns the APU lock, so the register values
 * and ARAM view form one coherent observation. apu_ram is immutable through
 * this interface and valid only for the callback; consumers must copy any
 * bytes they retain. Install and remove observers only while audio production
 * and runner execution are stopped. */
typedef uint32_t SrAudioTraceEventType;
enum {
    SR_AUDIO_TRACE_APU_PORT_APPLY = 1u,
    SR_AUDIO_TRACE_SPC_PORT_READ = 2u,
    SR_AUDIO_TRACE_SPC_OPCODE = 3u,
    SR_AUDIO_TRACE_DSP_WRITE = 4u,
    SR_AUDIO_TRACE_CPU_PORT_WRITE = 5u,
    SR_AUDIO_TRACE_SPC_UPLOAD = 6u
};

typedef uint64_t SrAudioTraceMask;
#define SR_AUDIO_TRACE_MASK_APU_PORT_APPLY UINT64_C(0x0000000000000001)
#define SR_AUDIO_TRACE_MASK_SPC_PORT_READ UINT64_C(0x0000000000000002)
#define SR_AUDIO_TRACE_MASK_SPC_OPCODE UINT64_C(0x0000000000000004)
#define SR_AUDIO_TRACE_MASK_DSP_WRITE UINT64_C(0x0000000000000008)
#define SR_AUDIO_TRACE_MASK_CPU_PORT_WRITE UINT64_C(0x0000000000000010)
#define SR_AUDIO_TRACE_MASK_SPC_UPLOAD UINT64_C(0x0000000000000020)
#define SR_AUDIO_TRACE_MASK_ALL                                         \
    (SR_AUDIO_TRACE_MASK_APU_PORT_APPLY |                               \
     SR_AUDIO_TRACE_MASK_SPC_PORT_READ |                                \
     SR_AUDIO_TRACE_MASK_SPC_OPCODE | SR_AUDIO_TRACE_MASK_DSP_WRITE |   \
     SR_AUDIO_TRACE_MASK_CPU_PORT_WRITE | SR_AUDIO_TRACE_MASK_SPC_UPLOAD)

typedef struct SrAudioTraceEvent {
    uint32_t struct_size;
    SrAudioTraceEventType type;
    uint64_t cycle_count;
    const uint8_t *apu_ram;
    uint64_t apu_ram_byte_size;
    uint16_t spc_pc;
    uint8_t spc_a;
    uint8_t spc_x;
    uint8_t spc_y;
    uint8_t spc_sp;
    uint8_t apu_input_ports[SR_APU_INPUT_PORT_COUNT];
    uint8_t apu_output_ports[SR_APU_OUTPUT_PORT_COUNT];
    uint8_t port;
    uint8_t value;
    uint8_t dsp_address;
    uint8_t reserved8;
    uint32_t source_address;
    uint32_t frame_counter;
    const char *function_name;
    uint16_t spc_instruction_pc;
    uint8_t spc_instruction_cycle;
    uint8_t dsp_slot;
    uint32_t reserved32;
} SrAudioTraceEvent;

#define SR_AUDIO_TRACE_EVENT_V2_SIZE                                     \
    ((uint32_t)(offsetof(SrAudioTraceEvent, function_name) +              \
                sizeof(((SrAudioTraceEvent *)0)->function_name)))

#define SR_AUDIO_TRACE_EVENT_V3_SIZE                                    \
    ((uint32_t)(offsetof(SrAudioTraceEvent, reserved32) +                \
                sizeof(((SrAudioTraceEvent *)0)->reserved32)))

typedef void (*SrAudioTraceCallback)(void *user_data,
                                     SrRunnerHandle *runner,
                                     const SrAudioTraceEvent *event);

typedef struct SrAudioTraceSubscription {
    uint32_t struct_size;
    uint32_t flags;
    SrAudioTraceCallback callback;
    void *user_data;
    SrAudioTraceMask event_mask;
    uint64_t reserved;
} SrAudioTraceSubscription;

#define SR_AUDIO_TRACE_SUBSCRIPTION_V2_SIZE                              \
    ((uint32_t)(offsetof(SrAudioTraceSubscription, user_data) +           \
                sizeof(((SrAudioTraceSubscription *)0)->user_data)))

#define SR_AUDIO_TRACE_SUBSCRIPTION_V3_SIZE                             \
    ((uint32_t)(offsetof(SrAudioTraceSubscription, reserved) +           \
                sizeof(((SrAudioTraceSubscription *)0)->reserved)))

/** @} */

#ifdef __cplusplus
}
#endif
