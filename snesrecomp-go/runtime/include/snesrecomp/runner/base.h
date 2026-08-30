/**
 * @file base.h
 * @brief Fundamental runner ABI values, snapshots, and opaque handles.
 * @ingroup sr_runner_core
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_core
 *  @{
 */

#define SR_RUNNER_ABI_VERSION 2u

/** ABI features are additive. A caller must test a bit before using the
 * corresponding table entry or data contract. */
#define SR_RUNNER_CAP_COMPONENT_HANDLES UINT64_C(0x0000000000000001)
#define SR_RUNNER_CAP_GENERATION_COUNTERS UINT64_C(0x0000000000000002)
#define SR_RUNNER_CAP_BORROWED_BYTE_SPANS UINT64_C(0x0000000000000004)
#define SR_RUNNER_CAP_CPU_STATE UINT64_C(0x0000000000000008)
#define SR_RUNNER_CAP_PPU_STATE UINT64_C(0x0000000000000010)
#define SR_RUNNER_CAP_BORROWED_U16_SPANS UINT64_C(0x0000000000000020)
#define SR_RUNNER_CAP_PPU_FRAME_STATE UINT64_C(0x0000000000000040)
#define SR_RUNNER_CAP_PPU_OBJ_RASTER UINT64_C(0x0000000000000080)
#define SR_RUNNER_CAP_PPU_SURFACE_VIEWS UINT64_C(0x0000000000000100)
#define SR_RUNNER_CAP_EXECUTION_STATE UINT64_C(0x0000000000000200)
#define SR_RUNNER_CAP_EVENT_OBSERVERS UINT64_C(0x0000000000000400)
#define SR_RUNNER_CAP_SAFE_POINT_MUTATIONS UINT64_C(0x0000000000000800)
#define SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE UINT64_C(0x0000000000001000)
#define SR_RUNNER_CAP_PPU_OUTPUT_CONTROL UINT64_C(0x0000000000002000)
#define SR_RUNNER_CAP_PPU_CAPTURE_CONTROL UINT64_C(0x0000000000004000)
#define SR_RUNNER_CAP_CPU_MATH_STATE UINT64_C(0x0000000000008000)
#define SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS UINT64_C(0x0000000000010000)
#define SR_RUNNER_CAP_SPC_CONTROL UINT64_C(0x0000000000020000)
#define SR_RUNNER_CAP_AUDIO_MIX_CONTROL UINT64_C(0x0000000000040000)
#define SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS UINT64_C(0x0000000000080000)
#define SR_RUNNER_CAP_PPU_VRAM_PATCH UINT64_C(0x0000000000100000)
#define SR_RUNNER_CAP_PPU_OBJ_METADATA UINT64_C(0x0000000000200000)
#define SR_RUNNER_CAP_DMA_STATE UINT64_C(0x0000000000400000)
#define SR_RUNNER_CAP_PPU_BACKGROUND_POLICY UINT64_C(0x0000000000800000)
#define SR_RUNNER_CAP_PPU_SCANOUT UINT64_C(0x0000000001000000)
#define SR_RUNNER_CAP_GAME_TIMING_CONTROL UINT64_C(0x0000000002000000)
#define SR_RUNNER_CAP_INPUT_STATE UINT64_C(0x0000000004000000)
#define SR_RUNNER_CAP_PPU_FRAME_POLICY UINT64_C(0x0000000008000000)
#define SR_RUNNER_CAP_PPU_FRAME_RESET UINT64_C(0x0000000010000000)
#define SR_RUNNER_CAP_PPU_OBJ_CAPTURE UINT64_C(0x0000000020000000)
#define SR_RUNNER_CAP_APU_STATE_SNAPSHOT UINT64_C(0x0000000040000000)
#define SR_RUNNER_CAP_SEMANTIC_DIGEST UINT64_C(0x0000000080000000)

typedef uint32_t SrResult;
enum {
    SR_RESULT_OK = 0u,
    SR_RESULT_INVALID_ARGUMENT = 1u,
    SR_RESULT_UNSUPPORTED = 2u,
    SR_RESULT_UNAVAILABLE = 3u,
    SR_RESULT_STALE_VIEW = 4u,
    SR_RESULT_PENDING = 5u,
    SR_RESULT_BUSY = 6u
};

/** Stable lower-case name for a result code, for diagnostics. Never NULL;
 * unknown values render as "unknown". Hosts printing a bare integer is a
 * recurring source of unnecessary debugging. */
const char *sr_result_string(SrResult result);

typedef uint32_t SrComponentKind;
enum {
    SR_COMPONENT_RUNNER = 0u,
    SR_COMPONENT_CPU = 1u,
    SR_COMPONENT_PPU = 2u,
    SR_COMPONENT_APU = 3u,
    SR_COMPONENT_DSP = 4u,
    SR_COMPONENT_SPC = 5u,
    SR_COMPONENT_DMA = 6u,
    SR_COMPONENT_CARTRIDGE = 7u
};

typedef uint32_t SrMemoryRegion;
enum {
    SR_MEMORY_WRAM = 0u,
    SR_MEMORY_SRAM = 1u,
    SR_MEMORY_ROM = 2u,
    SR_MEMORY_APU_RAM = 3u,
    SR_MEMORY_DSP_REGISTERS = 4u,
    SR_MEMORY_VRAM = 5u,
    SR_MEMORY_CGRAM = 6u,
    SR_MEMORY_OAM = 7u,
    SR_MEMORY_HIGH_OAM = 8u
};

#define SR_PPU_VRAM_WORD_COUNT UINT64_C(0x8000)
#define SR_PPU_CGRAM_WORD_COUNT UINT64_C(0x0100)
#define SR_PPU_OAM_WORD_COUNT UINT64_C(0x0100)
#define SR_PPU_HIGH_OAM_BYTE_COUNT UINT64_C(0x0020)
#define SR_PPU_NATIVE_WIDTH 256u
#define SR_PPU_NATIVE_HEIGHT 224u
#define SR_PPU_HORIZONTAL_MARGIN_MAX 128u
#define SR_PPU_VERTICAL_MARGIN_MAX 64u
#define SR_PPU_SURFACE_MAX_WIDTH 640u
#define SR_PPU_SURFACE_MAX_HEIGHT 352u
#define SR_PPU_OBJ_APRON 64u
#define SR_PPU_OBJ_X_WRAP 512u
#define SR_PPU_OBJ_Y_WRAP 256u
#define SR_PPU_OBJ_Y_NEGATIVE_FROM 224u
#define SR_PPU_MODE7_CANVAS_EXTENT 1024u
#define SR_PPU_TILE_ID_COUNT 256u
#define SR_PPU_SURFACE_BAND_COUNT 4u
#define SR_DMA_CHANNEL_COUNT 8u
#define SR_APU_RAM_BYTE_COUNT UINT64_C(0x10000)
#define SR_DSP_REGISTER_BYTE_COUNT UINT64_C(0x0080)
#define SR_APU_CPU_PORT_COUNT 4u
#define SR_APU_AUX_PORT_COUNT 2u
#define SR_APU_INPUT_PORT_COUNT                                           \
    (SR_APU_CPU_PORT_COUNT + SR_APU_AUX_PORT_COUNT)
#define SR_APU_OUTPUT_PORT_COUNT SR_APU_CPU_PORT_COUNT
#define SR_DSP_HARDWARE_VOICE_COUNT 8u
#define SR_DSP_EXTENDED_VOICE_COUNT 32u
enum {
    SR_PPU_OVERLAY_BG1 = 0u,
    SR_PPU_OVERLAY_BG2 = 1u,
    SR_PPU_OVERLAY_BG3 = 2u,
    SR_PPU_OVERLAY_BG4 = 3u,
    SR_PPU_OVERLAY_OBJ = 4u,
    SR_PPU_OVERLAY_SOURCE_COUNT = 5u
};

#define SR_PPU_OVERLAY_REMOVE_FROM_GAME UINT32_C(0x00000001)
#define SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH UINT32_C(0x00000002)
#define SR_PPU_OVERLAY_MARK_BG_HALF_ADD UINT32_C(0x00000004)
#define SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT UINT32_C(0x00000008)
#define SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN UINT32_C(0x00000010)
#define SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER UINT32_C(0x00000020)
#define SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER UINT32_C(0x00000040)
#define SR_PPU_OVERLAY_FLAGS_SUPPORTED                                  \
    (SR_PPU_OVERLAY_REMOVE_FROM_GAME |                                   \
     SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH |                                \
     SR_PPU_OVERLAY_MARK_BG_HALF_ADD |                                   \
     SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT |                      \
     SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN |                            \
     SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER |                            \
     SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER)

/** Opaque identities for the runner and its independently implemented
 * components. ABI consumers must never cast or dereference them. */
typedef struct SrRunnerHandle SrRunnerHandle;
typedef struct SrComponentHandle SrComponentHandle;

typedef uint32_t SrGameTimingOperation;
enum {
    SR_GAME_TIMING_BEGIN_FRAME_SLICE = 1u,
    SR_GAME_TIMING_COMPLETE_FRAME_SLICE = 2u
};

#define SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED UINT32_C(0x00000001)
#define SR_GAME_TIMING_REQUEST_FLAGS_SUPPORTED                            \
    SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED

#define SR_GAME_TIMING_STATE_FORCE_NMI UINT32_C(0x00000001)
#define SR_GAME_TIMING_STATE_NMI_AVAILABLE UINT32_C(0x00000002)
#define SR_GAME_TIMING_STATE_NMI_ENABLED UINT32_C(0x00000004)
#define SR_GAME_TIMING_STATE_IN_NMI UINT32_C(0x00000008)
#define SR_GAME_TIMING_TRANSITION_NMI_ENTERED UINT32_C(0x00000001)

/** Synchronous control for a recompiled game's host-resumable frame slice.
 * BEGIN positions the beam at VBlank, publishes a fresh RDNMI token, and
 * enables forced pacing. COMPLETE always disables forced pacing, then
 * optionally enters NMI when the emulated hardware gate is enabled. These
 * operations define latch transitions only: they do not prescribe whether a
 * game's body runs before or after the reported NMI transition, nor where the
 * adapter places scanout. The game adapter owns that recovered schedule.
 * No runner generation changes: these are live emulated timing latches, not
 * borrowed-view ownership. */
typedef struct SrGameTimingRequest {
    uint32_t struct_size;
    SrGameTimingOperation operation;
    uint32_t flags;
    uint32_t reserved;
} SrGameTimingRequest;

#define SR_GAME_TIMING_REQUEST_V2_SIZE                                   \
    ((uint32_t)(offsetof(SrGameTimingRequest, reserved) +                 \
                sizeof(((SrGameTimingRequest *)0)->reserved)))

typedef struct SrGameTimingResult {
    uint32_t struct_size;
    uint32_t state_flags;
    uint32_t transition_flags;
} SrGameTimingResult;

#define SR_GAME_TIMING_RESULT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrGameTimingResult, transition_flags) +          \
                sizeof(((SrGameTimingResult *)0)->transition_flags)))

#define SR_INPUT_CONTROLLER_COUNT 2u

/** Coherent copied controller state at the current runner safe point.
 * packed_buttons uses the 12-bit-per-controller order accepted by RtlRunFrame
 * and SR_MUTATION_SET_INPUT. auto_joypad contains the SNES $4218-$421B bit
 * layout observed by the emulated game. */
typedef struct SrInputStateSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t frame_counter;
    uint16_t packed_buttons[SR_INPUT_CONTROLLER_COUNT];
    uint16_t auto_joypad[SR_INPUT_CONTROLLER_COUNT];
    uint32_t reserved;
} SrInputStateSnapshot;

#define SR_INPUT_STATE_SNAPSHOT_V2_SIZE                                 \
    ((uint32_t)(offsetof(SrInputStateSnapshot, reserved) +               \
                sizeof(((SrInputStateSnapshot *)0)->reserved)))

/** A borrowed view is immutable through this API and thread-confined. It is
 * valid only while borrow_is_valid reports true: the next runner tick, reset,
 * successful state load, or controlled mutation invalidates it. Callers that
 * retain data beyond that point must copy only the bytes they need. */
typedef struct SrBorrowedSpan {
    uint32_t struct_size;
    SrMemoryRegion region;
    const uint8_t *data;
    uint64_t byte_size;
    uint64_t lifetime_generation;
} SrBorrowedSpan;

#define SR_BORROWED_SPAN_V2_SIZE                                           \
    ((uint32_t)(offsetof(SrBorrowedSpan, lifetime_generation) +            \
                sizeof(((SrBorrowedSpan *)0)->lifetime_generation)))

/** Host-native fixed-width values, not an encoded byte stream. This permits a
 * zero-copy view on every target while keeping element width explicit. */
typedef struct SrBorrowedU16Span {
    uint32_t struct_size;
    SrMemoryRegion region;
    const uint16_t *data;
    uint64_t element_count;
    uint64_t lifetime_generation;
} SrBorrowedU16Span;

#define SR_BORROWED_U16_SPAN_V2_SIZE                                      \
    ((uint32_t)(offsetof(SrBorrowedU16Span, lifetime_generation) +         \
                sizeof(((SrBorrowedU16Span *)0)->lifetime_generation)))

typedef struct SrGenerationSnapshot {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t lifetime_generation;
    uint64_t tick_generation;
    uint64_t reset_generation;
    uint64_t load_generation;
    uint64_t mutation_generation;
} SrGenerationSnapshot;

#define SR_GENERATION_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrGenerationSnapshot, mutation_generation) +      \
                sizeof(((SrGenerationSnapshot *)0)->mutation_generation)))

#define SR_CPU_STATE_M_FLAG UINT32_C(0x00000001)
#define SR_CPU_STATE_X_FLAG UINT32_C(0x00000002)
#define SR_CPU_STATE_EMULATION UINT32_C(0x00000004)
#define SR_CPU_STATE_HOST_RETURN_VALID UINT32_C(0x00000008)
#define SR_CPU_STATE_EXECUTION_PC_VALID UINT32_C(0x00000010)

typedef struct SrCpuStateSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t frame_counter;
    uint32_t execution_pc24;
    uint16_t a;
    uint16_t x;
    uint16_t y;
    uint16_t s;
    uint16_t d;
    uint8_t db;
    uint8_t pb;
    uint8_t p;
    uint8_t reserved;
} SrCpuStateSnapshot;

#define SR_CPU_STATE_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrCpuStateSnapshot, reserved) +                  \
                sizeof(((SrCpuStateSnapshot *)0)->reserved)))

/* SNES CPU arithmetic-unit latches. The multiplication result register also
 * holds the remainder after division, matching $4216-$4217. Restore is a
 * synchronous operation intended for transactional diagnostics and must run
 * on the emulation thread at a safe point. */
typedef struct SrCpuMathState {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint8_t multiply_operand;
    uint8_t reserved8;
    uint16_t multiply_or_remainder_result;
    uint16_t divide_dividend;
    uint16_t divide_quotient;
    uint32_t reserved32;
} SrCpuMathState;

#define SR_CPU_MATH_STATE_V2_SIZE                                       \
    ((uint32_t)(offsetof(SrCpuMathState, reserved32) +                    \
                sizeof(((SrCpuMathState *)0)->reserved32)))

/** @} */

#ifdef __cplusplus
}
#endif
