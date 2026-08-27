#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_RUNNER_ABI_VERSION 2u

/* ABI features are additive. A caller must test a bit before using the
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

/* Opaque to ABI consumers. The compatibility adapter currently maps these to
 * the independently implemented runner's internal components. */
typedef struct SrRunnerHandle SrRunnerHandle;
typedef struct SrComponentHandle SrComponentHandle;

/* A borrowed view is immutable through this API and thread-confined. It is
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

/* Host-native fixed-width values, not an encoded byte stream. This permits a
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

#define SR_PPU_STATE_FORCED_BLANK UINT32_C(0x00000001)
#define SR_PPU_STATE_BG3_PRIORITY UINT32_C(0x00000002)
#define SR_PPU_STATE_INTERLACE UINT32_C(0x00000004)
#define SR_PPU_STATE_OBJ_INTERLACE UINT32_C(0x00000008)
#define SR_PPU_STATE_OVERSCAN UINT32_C(0x00000010)
#define SR_PPU_STATE_PSEUDO_HIRES UINT32_C(0x00000020)
#define SR_PPU_STATE_MODE7_EXT_BG UINT32_C(0x00000040)

#define SR_PPU_RENDERER_AUTHENTIC_SURFACE_BOUND UINT32_C(0x00000001)

#define SR_PPU_MODE7_X_FLIP UINT8_C(0x01)
#define SR_PPU_MODE7_Y_FLIP UINT8_C(0x02)
#define SR_PPU_MODE7_LARGE_FIELD UINT8_C(0x40)
#define SR_PPU_MODE7_CHARACTER_FILL UINT8_C(0x80)

typedef struct SrPpuBackgroundState {
    uint16_t h_scroll;
    uint16_t v_scroll;
    uint16_t tilemap_base_word;
    uint16_t tile_base_word;
    uint8_t tilemap_width_tiles;
    uint8_t tilemap_height_tiles;
    uint8_t tile_size_pixels;
    uint8_t bits_per_pixel;
} SrPpuBackgroundState;

typedef struct SrPpuStateSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint8_t display_control;
    uint8_t object_select;
    uint8_t bg_mode_control;
    uint8_t mosaic_control;
    uint8_t bg_mode;
    uint8_t brightness;
    uint8_t main_screen;
    uint8_t sub_screen;
    uint8_t main_windowed;
    uint8_t sub_windowed;
    uint8_t object_size_select;
    uint8_t reserved8;
    uint16_t margin_left;
    uint16_t margin_right;
    uint16_t margin_top;
    uint16_t margin_bottom;
    uint32_t object_tile_base_1_word;
    uint32_t object_tile_base_2_word;
    SrPpuBackgroundState backgrounds[4];
    /* V2: raw controls retained for diagnostics and game-agnostic render
     * inspection. Derived fields above remain the preferred normal path. */
    uint8_t window_select;
    uint8_t window_logic;
    uint8_t color_math_control;
    uint8_t color_math_designation;
    uint8_t background_tilemap_control[4];
    uint16_t background_tile_base_control;
    uint8_t mode7_select;
    uint8_t reserved8_2;
    int16_t mode7_matrix[8];
    /* Raw BGR555 fixed colour used when CGWSEL selects fixed-colour math. */
    uint16_t fixed_color;
    uint16_t reserved16;
    /* Resolved dimensions for the two OAM size-bit values under OBSEL. */
    uint8_t object_small_size_pixels;
    uint8_t object_large_size_pixels;
    uint16_t reserved16_2;
    /* Host-renderer availability, separate from emulated PPU flags. */
    uint32_t renderer_flags;
} SrPpuStateSnapshot;

#define SR_PPU_STATE_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuStateSnapshot, renderer_flags) +             \
                sizeof(((SrPpuStateSnapshot *)0)->renderer_flags)))

#define SR_DMA_CHANNEL_DMA_ACTIVE UINT32_C(0x00000001)
#define SR_DMA_CHANNEL_HDMA_ACTIVE UINT32_C(0x00000002)
#define SR_DMA_CHANNEL_FIXED_A_BUS UINT32_C(0x00000004)
#define SR_DMA_CHANNEL_DECREMENT_A_BUS UINT32_C(0x00000008)
#define SR_DMA_CHANNEL_INDIRECT UINT32_C(0x00000010)
#define SR_DMA_CHANNEL_FROM_B_BUS UINT32_C(0x00000020)
#define SR_DMA_CHANNEL_TRANSFER_PENDING UINT32_C(0x00000040)
#define SR_DMA_CHANNEL_TERMINATED UINT32_C(0x00000080)

typedef struct SrDmaChannelState {
    uint32_t flags;
    uint16_t a_address;
    uint16_t transfer_size;
    uint16_t table_address;
    uint8_t a_bank;
    uint8_t b_address;
    uint8_t indirect_bank;
    uint8_t mode;
    uint8_t repeat_count;
    uint8_t reserved8[3];
} SrDmaChannelState;

typedef struct SrDmaStateSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t timer;
    uint32_t channel_count;
    SrDmaChannelState channels[SR_DMA_CHANNEL_COUNT];
} SrDmaStateSnapshot;

#define SR_DMA_STATE_BUSY UINT32_C(0x00000001)

#define SR_DMA_STATE_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrDmaStateSnapshot, channels) +                  \
                sizeof(((SrDmaStateSnapshot *)0)->channels)))

typedef uint32_t SrPpuTransparentFillMode;
enum {
    SR_PPU_TRANSPARENT_FILL_NONE = 0u,
    SR_PPU_TRANSPARENT_FILL_BLACK = 1u,
    SR_PPU_TRANSPARENT_FILL_CGRAM = 2u
};

typedef struct SrPpuOverlayState {
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t y1;
    uint32_t flags;
    uint32_t content_band_mask;
    uint32_t transparent_fill_argb;
    uint8_t transparent_fill_configured;
    uint8_t transparent_fill_mode;
    uint8_t transparent_fill_cgram;
    uint8_t oam_first;
    uint8_t oam_count;
    uint8_t reserved8[3];
} SrPpuOverlayState;

typedef struct SrPpuFrameSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint8_t display_control;
    uint8_t bg_mode;
    uint8_t hud_split_height;
    uint8_t hud_left_end;
    uint8_t hud_right_start;
    uint8_t hud_player_row_y;
    uint8_t hud_left_only_y;
    uint8_t margin_budget;
    uint8_t mode7_override_active;
    uint8_t overlay_count;
    uint8_t reserved8[6];
    SrPpuOverlayState overlays[SR_PPU_OVERLAY_SOURCE_COUNT];
} SrPpuFrameSnapshot;

#define SR_PPU_FRAME_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuFrameSnapshot, overlays) +                  \
                sizeof(((SrPpuFrameSnapshot *)0)->overlays)))

typedef uint32_t SrPpuBackgroundFill;
enum {
    SR_PPU_BACKGROUND_FILL_INHERIT = 0u,
    SR_PPU_BACKGROUND_FILL_TRANSPARENT = 1u,
    SR_PPU_BACKGROUND_FILL_LIVE_WORLD = 2u,
    SR_PPU_BACKGROUND_FILL_CLAMP = 3u,
    SR_PPU_BACKGROUND_FILL_MIRROR = 4u,
    SR_PPU_BACKGROUND_FILL_REPEAT = 5u,
    SR_PPU_BACKGROUND_FILL_RAW_WRAP = 6u
};

typedef uint32_t SrPpuBackgroundMotion;
enum {
    SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE = 0u,
    SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL = 1u
};

#define SR_PPU_BACKGROUND_COORDINATE_MAPPED UINT32_C(0x00000001)
#define SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE UINT32_C(0x00000002)
#define SR_PPU_BACKGROUND_COORDINATE_MOSAIC UINT32_C(0x00000004)

/* Resolves the source coordinates sampled for a displayed BG point after
 * per-layer mosaic, finite-world extents, vertical clipping, and widescreen
 * fill policy. screen_y is zero-based display space; sample_y follows the
 * PPU's one-based BG/Mode-7 fetch convention. An unmapped point is a normal
 * result with the MAPPED flag clear. */
typedef struct SrPpuBackgroundCoordinateRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t layer;
    int32_t screen_x;
    int32_t screen_y;
    uint32_t reserved;
} SrPpuBackgroundCoordinateRequest;

#define SR_PPU_BACKGROUND_COORDINATE_REQUEST_V2_SIZE                     \
    ((uint32_t)(offsetof(SrPpuBackgroundCoordinateRequest, reserved) +    \
                sizeof(((SrPpuBackgroundCoordinateRequest *)0)->reserved)))

typedef struct SrPpuBackgroundCoordinateResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    int32_t source_x;
    int32_t sample_y;
    SrPpuBackgroundFill fill;
    SrPpuBackgroundMotion motion;
} SrPpuBackgroundCoordinateResult;

#define SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE                      \
    ((uint32_t)(offsetof(SrPpuBackgroundCoordinateResult, motion) +       \
                sizeof(((SrPpuBackgroundCoordinateResult *)0)->motion)))

/* Caller-owned raster output. Each pixel is a host-native uint32_t whose
 * numeric value is 0xAARRGGBB; transparent source pixels are zero. The
 * request generation must come from a coherent PPU snapshot or borrow. */
#define SR_PPU_PIXEL_FORMAT_ARGB8888_U32 1u
#define SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32 \
    SR_PPU_PIXEL_FORMAT_ARGB8888_U32

/* A resolved SNES OBJ part in screen coordinates. This is a value type: it
 * contains no runner pointer and may be copied or retained by the caller. */
typedef struct SrPpuObjPart {
    int16_t x;
    int16_t y;
    uint16_t tile_attr;
    uint8_t size;
    uint8_t reserved;
} SrPpuObjPart;

typedef struct SrPpuObjRasterRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t first_sprite;
    uint32_t sprite_count;
    uint32_t priority;
    uint32_t pixel_format;
    uint32_t *pixels;
    uint64_t pixel_byte_size;
    uint64_t pitch_bytes;
} SrPpuObjRasterRequest;

#define SR_PPU_OBJ_RASTER_REQUEST_V2_SIZE                                \
    ((uint32_t)(offsetof(SrPpuObjRasterRequest, pitch_bytes) +            \
                sizeof(((SrPpuObjRasterRequest *)0)->pitch_bytes)))

typedef struct SrPpuObjRasterResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t width;
    uint32_t height;
} SrPpuObjRasterResult;

#define SR_PPU_OBJ_RASTER_RESULT_V2_SIZE                                 \
    ((uint32_t)(offsetof(SrPpuObjRasterResult, height) +                  \
                sizeof(((SrPpuObjRasterResult *)0)->height)))

/* Resolve an OAM range once into caller-owned fixed-width parts. The result
 * preserves the PPU's live rotation, exact-position, camera-relative, size,
 * and priority rules without exposing any of their concrete storage. The
 * returned parts are ordinary values and may be retained by the caller. */
typedef struct SrPpuObjResolveRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t first_sprite;
    uint32_t sprite_count;
    uint32_t priority;
    uint32_t part_capacity;
    SrPpuObjPart *parts;
} SrPpuObjResolveRequest;

#define SR_PPU_OBJ_RESOLVE_REQUEST_V2_SIZE                               \
    ((uint32_t)(offsetof(SrPpuObjResolveRequest, parts) +                 \
                sizeof(((SrPpuObjResolveRequest *)0)->parts)))

typedef struct SrPpuObjResolveResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t part_count;
    uint32_t reserved;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} SrPpuObjResolveResult;

#define SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE                                \
    ((uint32_t)(offsetof(SrPpuObjResolveResult, y1) +                     \
                sizeof(((SrPpuObjResolveResult *)0)->y1)))

/* Rasterize caller-owned resolved or synthetic parts into caller-owned
 * storage. The explicit bounds are also the crop rectangle, allowing a
 * consumer to pack or band a composition without an intermediate image copy. */
typedef struct SrPpuObjPartsRasterRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    const SrPpuObjPart *parts;
    uint64_t part_count;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t pixel_format;
    uint32_t reserved;
    uint32_t *pixels;
    uint64_t pixel_byte_size;
    uint64_t pitch_bytes;
} SrPpuObjPartsRasterRequest;

#define SR_PPU_OBJ_PARTS_RASTER_REQUEST_V2_SIZE                          \
    ((uint32_t)(offsetof(SrPpuObjPartsRasterRequest, pitch_bytes) +       \
                sizeof(((SrPpuObjPartsRasterRequest *)0)->pitch_bytes)))

/* Read-only views of the output surfaces currently bound to the PPU. The
 * storage remains host-owned. A snapshot is valid only until the next runner
 * lifetime invalidation or successful PPU surface rebind; callers can test
 * both conditions with ppu_surface_snapshot_is_valid. */
#define SR_PPU_SURFACE_BOUND UINT32_C(0x00000001)
#define SR_PPU_SURFACE_HAS_CONTENT UINT32_C(0x00000002)

typedef struct SrPpuSurfaceView {
    uint32_t flags;
    uint32_t pixel_format;
    const uint8_t *data;
    uint64_t byte_size;
    uint64_t pitch_bytes;
    uint32_t width_pixels;
    uint32_t height_pixels;
    int32_t origin_x;
    int32_t origin_y;
    uint32_t scale;
    uint32_t reserved;
} SrPpuSurfaceView;

typedef struct SrPpuSurfaceSnapshot {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint64_t binding_generation;
    uint32_t overlay_count;
    uint32_t band_count;
    SrPpuSurfaceView main;
    SrPpuSurfaceView authentic;
    SrPpuSurfaceView
        overlays[SR_PPU_OVERLAY_SOURCE_COUNT][SR_PPU_SURFACE_BAND_COUNT];
    SrPpuSurfaceView mode7;
} SrPpuSurfaceSnapshot;

#define SR_PPU_SURFACE_SNAPSHOT_V2_SIZE                                  \
    ((uint32_t)(offsetof(SrPpuSurfaceSnapshot, mode7) +                    \
                sizeof(((SrPpuSurfaceSnapshot *)0)->mode7)))

/* Synchronous host-owned output bindings. The caller retains ownership of
 * every supplied buffer and must keep it alive until it is unbound or the
 * runner is destroyed. byte_size and height_pixels describe capacity rather
 * than current PPU content; the runner validates them before retaining a
 * pointer. Binding a null pointer unbinds that surface and requires all
 * capacity fields to be zero. Priority bands inherit the base overlay pitch,
 * so their request pitch must match the already-bound base surface. */
typedef uint32_t SrPpuOutputKind;
enum {
    SR_PPU_OUTPUT_MAIN = 0u,
    SR_PPU_OUTPUT_AUTHENTIC = 1u,
    SR_PPU_OUTPUT_OVERLAY = 2u,
    SR_PPU_OUTPUT_OVERLAY_PRIORITY = 3u,
    SR_PPU_OUTPUT_MODE7 = 4u,
    /* Clears all ordinary BG/OBJ sources, priority bands, and captures.
     * The separately bound Mode-7 surface remains available. */
    SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES = 5u
};

#define SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER UINT32_C(0x00000001)

typedef struct SrPpuOutputBindingRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    SrPpuOutputKind kind;
    uint32_t source;
    uint32_t band;
    uint32_t scale;
    uint8_t *pixels;
    uint64_t pixel_byte_size;
    uint64_t pitch_bytes;
    uint32_t height_pixels;
    uint32_t reserved;
} SrPpuOutputBindingRequest;

#define SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE                            \
    ((uint32_t)(offsetof(SrPpuOutputBindingRequest, reserved) +           \
                sizeof(((SrPpuOutputBindingRequest *)0)->reserved)))

typedef uint32_t SrPpuHorizontalMarginMode;
enum {
    /* The configured budget is immediately available on both sides. */
    SR_PPU_HORIZONTAL_MARGIN_AVAILABLE = 0u,
    /* Keep the budget reserved while starting at the native-width centre. */
    SR_PPU_HORIZONTAL_MARGIN_CENTERED = 1u
};

typedef struct SrPpuHorizontalMarginRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    SrPpuHorizontalMarginMode mode;
    uint32_t budget_pixels;
    uint32_t reserved[2];
} SrPpuHorizontalMarginRequest;

#define SR_PPU_HORIZONTAL_MARGIN_REQUEST_V2_SIZE                         \
    ((uint32_t)(offsetof(SrPpuHorizontalMarginRequest, reserved) +        \
                sizeof(((SrPpuHorizontalMarginRequest *)0)->reserved)))

/* Atomic, synchronous claims for frame-scoped enhancement capture. Claims
 * return SR_RESULT_BUSY when an earlier game/host policy already owns the
 * source for this frame. They run on the emulation thread and retain no
 * overlay-request pointer. A successful Mode-7 claim borrows pixels until
 * captures are cleared; callers must keep that image alive for the frame. */
typedef struct SrPpuOverlayCaptureRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t source;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t reserved[2];
} SrPpuOverlayCaptureRequest;

#define SR_PPU_OVERLAY_CAPTURE_REQUEST_V2_SIZE                           \
    ((uint32_t)(offsetof(SrPpuOverlayCaptureRequest, reserved) +          \
                sizeof(((SrPpuOverlayCaptureRequest *)0)->reserved)))

/* Exact capture-policy value. Unlike SrPpuOverlayState this excludes derived
 * content and resolved-colour fields, so it can be compared and restored
 * without depending on pixels produced during scanout. */
typedef struct SrPpuOverlayCaptureState {
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t y1;
    uint32_t flags;
    uint8_t transparent_fill_configured;
    uint8_t transparent_fill_mode;
    uint8_t transparent_fill_cgram;
    uint8_t oam_first;
    uint8_t oam_count;
    uint8_t reserved8[3];
} SrPpuOverlayCaptureState;

/* Atomically replaces any selected capture policies only if every selected
 * source still exactly matches the caller's expected value. No source is
 * changed when a comparison or validation fails. This is intended for a
 * frame-scoped enhancement which temporarily supersedes a known producer and
 * restores it after scanout. */
typedef struct SrPpuOverlayCaptureExchangeRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t source_mask;
    uint32_t reserved;
    SrPpuOverlayCaptureState expected[SR_PPU_OVERLAY_SOURCE_COUNT];
    SrPpuOverlayCaptureState replacement[SR_PPU_OVERLAY_SOURCE_COUNT];
} SrPpuOverlayCaptureExchangeRequest;

#define SR_PPU_OVERLAY_CAPTURE_EXCHANGE_REQUEST_V2_SIZE                  \
    ((uint32_t)(offsetof(SrPpuOverlayCaptureExchangeRequest, replacement) + \
                sizeof(((SrPpuOverlayCaptureExchangeRequest *)0)          \
                           ->replacement)))

/* Mutable access to a host-owned output surface. The runner never transfers
 * ownership. Writes must stay within byte_size. The runner's binding/view
 * guarantee ends when the callback returns; the host buffer's owner may keep
 * using its own storage afterward and is responsible for its lifetime. */
typedef struct SrPpuWritableSurfaceView {
    uint32_t flags;
    uint32_t pixel_format;
    uint8_t *data;
    uint64_t byte_size;
    uint64_t pitch_bytes;
    uint32_t width_pixels;
    uint32_t height_pixels;
    int32_t origin_x;
    int32_t origin_y;
    uint32_t scale;
    uint32_t reserved;
} SrPpuWritableSurfaceView;

/* One coherent callback-lifetime view for frame-critical enhancement policy.
 * It deliberately exposes values and bounded borrows rather than a concrete
 * PPU layout. Nested synchronous PPU services may use lifetime_generation.
 * Borrowed emulated-memory pointers may not be retained after the callback.
 * A writable surface pointer may be retained only by the owner of that host
 * buffer; it carries no runner validity or ownership after return. */
typedef struct SrPpuFrameTransactionContext {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    SrPpuStateSnapshot state;
    SrPpuFrameSnapshot frame;
    SrBorrowedU16Span cgram;
    SrBorrowedU16Span oam;
    SrBorrowedSpan high_oam;
    SrPpuWritableSurfaceView
        overlays[SR_PPU_OVERLAY_SOURCE_COUNT];
} SrPpuFrameTransactionContext;

#define SR_PPU_FRAME_TRANSACTION_CONTEXT_V2_SIZE                         \
    ((uint32_t)(offsetof(SrPpuFrameTransactionContext, overlays) +        \
                sizeof(((SrPpuFrameTransactionContext *)0)->overlays)))

typedef SrResult (*SrPpuFrameTransactionCallback)(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context);

typedef struct SrPpuFrameTransactionRequest {
    uint32_t struct_size;
    uint32_t flags;
    SrPpuFrameTransactionCallback callback;
    void *user_data;
} SrPpuFrameTransactionRequest;

#define SR_PPU_FRAME_TRANSACTION_REQUEST_V2_SIZE                         \
    ((uint32_t)(offsetof(SrPpuFrameTransactionRequest, user_data) +       \
                sizeof(((SrPpuFrameTransactionRequest *)0)->user_data)))

/* One sparse, host-native VRAM word replacement. Transactions compare every
 * expected value before applying any replacement, so a game-side producer
 * cannot be partially overwritten. Word addresses use the SNES VRAM word
 * convention (0..$7fff), not byte addresses. */
typedef struct SrPpuVramWordPatch {
    uint16_t word_address;
    uint16_t expected;
    uint16_t replacement;
    uint16_t reserved;
} SrPpuVramWordPatch;

#define SR_PPU_VRAM_PATCH_MAX_WORDS 4096u
/* Promise that word_address values are strictly increasing. The runner still
 * validates the order, but can prove uniqueness without scratch initialization. */
#define SR_PPU_VRAM_PATCH_ADDRESSES_SORTED UINT32_C(0x00000001)

typedef struct SrPpuVramPatchRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    const SrPpuVramWordPatch *patches;
    uint32_t patch_count;
    uint32_t reserved;
} SrPpuVramPatchRequest;

#define SR_PPU_VRAM_PATCH_REQUEST_V2_SIZE                               \
    ((uint32_t)(offsetof(SrPpuVramPatchRequest, reserved) +               \
                sizeof(((SrPpuVramPatchRequest *)0)->reserved)))

/* Exact screen coordinates supplement the lossy 9-bit/8-bit OAM encoding for
 * renderers that expose pixels beyond the native viewport. Updates are
 * synchronous derived-renderer metadata: they do not mutate emulated memory
 * or advance the runner lifetime generation. Each request is validated in
 * full before any clear or update is applied. */
#define SR_PPU_OBJ_POSITION_CAMERA_RELATIVE UINT8_C(0x01)

typedef struct SrPpuObjPositionUpdate {
    int16_t x;
    int16_t y;
    uint8_t slot;
    uint8_t flags;
    uint16_t reserved;
} SrPpuObjPositionUpdate;

#define SR_PPU_OBJ_POSITION_UPDATE_MAX 128u
#define SR_PPU_OBJ_METADATA_CLEAR_POSITIONS UINT32_C(0x00000001)
#define SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE UINT32_C(0x00000002)

typedef struct SrPpuObjMetadataRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    const SrPpuObjPositionUpdate *updates;
    uint32_t update_count;
    uint32_t reserved;
} SrPpuObjMetadataRequest;

#define SR_PPU_OBJ_METADATA_REQUEST_V2_SIZE                              \
    ((uint32_t)(offsetof(SrPpuObjMetadataRequest, reserved) +             \
                sizeof(((SrPpuObjMetadataRequest *)0)->reserved)))

/* Frame-scoped background limits. DEFAULT replaces a layer's four default
 * extents and seeds every visible row's horizontal values. HORIZONTAL_BAND
 * replaces only [y0,y1) for that layer. The runner validates the complete
 * ordered update list before applying any entry. */
#define SR_PPU_LAYER_EXTENT_AVAILABLE UINT32_C(0x0000ffff)

typedef uint32_t SrPpuLayerExtentKind;
enum {
    SR_PPU_LAYER_EXTENT_DEFAULT = 1u,
    SR_PPU_LAYER_EXTENT_HORIZONTAL_BAND = 2u
};

typedef struct SrPpuLayerExtentUpdate {
    SrPpuLayerExtentKind kind;
    uint32_t layer;
    uint32_t y0;
    uint32_t y1;
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;
} SrPpuLayerExtentUpdate;

#define SR_PPU_LAYER_EXTENT_UPDATE_MAX 32u

typedef struct SrPpuLayerExtentRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    const SrPpuLayerExtentUpdate *updates;
    uint32_t update_count;
    uint32_t reserved;
} SrPpuLayerExtentRequest;

#define SR_PPU_LAYER_EXTENT_REQUEST_V2_SIZE                              \
    ((uint32_t)(offsetof(SrPpuLayerExtentRequest, reserved) +             \
                sizeof(((SrPpuLayerExtentRequest *)0)->reserved)))

/* Finite-world tile providers are retained until the next replacement,
 * runner reset, or lifetime invalidation. Callback output pointers are valid
 * only until the provider's next span callback. Scalar lookup is mandatory;
 * span and priority-band lookup are optional fast paths. */
typedef uint32_t (*SrPpuVirtualTileLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, uint16_t *entry);
typedef uint32_t (*SrPpuVirtualTileSpanLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, int32_t tile_step,
    uint32_t capacity, const uint16_t **entries, int64_t *word_stride);
typedef uint32_t (*SrPpuVirtualTileBandLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, uint16_t entry,
    uint8_t *band);

#define SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC UINT32_C(0x00000001)

typedef struct SrPpuVirtualTilemapBinding {
    SrPpuVirtualTileLookup lookup;
    SrPpuVirtualTileSpanLookup lookup_span;
    SrPpuVirtualTileBandLookup band_lookup;
    void *user_data;
    int32_t camera_x;
    int32_t camera_y;
    uint32_t hscroll_anchor;
    uint32_t vscroll_anchor;
    uint32_t flags;
    uint32_t reserved;
} SrPpuVirtualTilemapBinding;

typedef struct SrPpuVirtualTilemapRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t layer_mask;
    uint32_t reserved;
    SrPpuVirtualTilemapBinding bindings[2];
} SrPpuVirtualTilemapRequest;

#define SR_PPU_VIRTUAL_TILEMAP_REQUEST_V2_SIZE                           \
    ((uint32_t)(offsetof(SrPpuVirtualTilemapRequest, bindings) +          \
                sizeof(((SrPpuVirtualTilemapRequest *)0)->bindings)))

#define SR_PPU_AUTHENTIC_CAMERA_CLEAR UINT32_C(0x00000001)
#define SR_PPU_AUTHENTIC_CAMERA_BG1 UINT32_C(0x00000001)
#define SR_PPU_AUTHENTIC_CAMERA_BG2 UINT32_C(0x00000002)
#define SR_PPU_AUTHENTIC_CAMERA_ALL                                     \
    (SR_PPU_AUTHENTIC_CAMERA_BG1 | SR_PPU_AUTHENTIC_CAMERA_BG2)

/* Selected horizontal-scroll arrays are copied synchronously. A clear-only
 * request has layer_mask zero and null arrays. */
typedef struct SrPpuAuthenticCameraRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t layer_mask;
    uint32_t row_count;
    const uint16_t *bg1_hscroll;
    const uint16_t *bg2_hscroll;
    int32_t object_offset_x;
    uint32_t reserved;
} SrPpuAuthenticCameraRequest;

#define SR_PPU_AUTHENTIC_CAMERA_REQUEST_V2_SIZE                          \
    ((uint32_t)(offsetof(SrPpuAuthenticCameraRequest, reserved) +         \
                sizeof(((SrPpuAuthenticCameraRequest *)0)->reserved)))

/* Synchronous native scanout. The runner owns PPU line execution, HDMA
 * advancement, margin hold-first/hold-last rendering, and vertical IRQ
 * scheduling. irq_callback is required and owns only the recompiled CPU's IRQ
 * handler. Optional line callbacks are for diagnostics and receive
 * callback-lifetime, zero-copy surface views; normal frames should leave them
 * null. */
#define SR_PPU_SCANOUT_LINE_BEFORE UINT32_C(0x00000001)
#define SR_PPU_SCANOUT_LINE_AFTER_HDMA UINT32_C(0x00000002)
#define SR_PPU_SCANOUT_HDMA_ACTIVE UINT32_C(0x00000001)
#define SR_PPU_SCANOUT_HDMA_INDIRECT UINT32_C(0x00000002)

typedef struct SrPpuScanoutHdmaState {
    uint32_t flags;
    uint8_t repeat_count;
    uint8_t mode;
    uint8_t b_address;
    uint8_t indirect_bank;
} SrPpuScanoutHdmaState;

typedef struct SrPpuScanoutLineContext {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t line;
    uint32_t channel_count;
    SrPpuStateSnapshot state;
    SrPpuSurfaceView main_surface;
    SrPpuSurfaceView authentic_surface;
    SrPpuScanoutHdmaState channels[SR_DMA_CHANNEL_COUNT];
} SrPpuScanoutLineContext;

#define SR_PPU_SCANOUT_LINE_CONTEXT_V2_SIZE                              \
    ((uint32_t)(offsetof(SrPpuScanoutLineContext, channels) +             \
                sizeof(((SrPpuScanoutLineContext *)0)->channels)))

typedef void (*SrPpuScanoutLineCallback)(
    void *user_data, const SrPpuScanoutLineContext *context);
typedef void (*SrPpuScanoutIrqCallback)(void *user_data, uint32_t line);

typedef struct SrPpuScanoutRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t hdma_channel_mask;
    uint32_t reserved;
    SrPpuScanoutLineCallback line_callback;
    SrPpuScanoutIrqCallback irq_callback;
    void *user_data;
} SrPpuScanoutRequest;

#define SR_PPU_SCANOUT_REQUEST_V2_SIZE                                   \
    ((uint32_t)(offsetof(SrPpuScanoutRequest, user_data) +                \
                sizeof(((SrPpuScanoutRequest *)0)->user_data)))

#define SR_PPU_SCANOUT_AUTHENTIC_SURFACE_READY UINT32_C(0x00000001)
#define SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG1 UINT32_C(0x00000002)
#define SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG2 UINT32_C(0x00000004)

typedef struct SrPpuScanoutResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    SrPpuStateSnapshot final_state;
} SrPpuScanoutResult;

#define SR_PPU_SCANOUT_RESULT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuScanoutResult, final_state) +               \
                sizeof(((SrPpuScanoutResult *)0)->final_state)))

typedef struct SrPpuMode7OverrideRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    const uint32_t *pixels;
    uint64_t pixel_byte_size;
    uint32_t width_pixels;
    uint32_t height_pixels;
    int32_t canvas_x0;
    int32_t canvas_y0;
    int32_t canvas_x1;
    int32_t canvas_y1;
    uint32_t wrap;
    uint32_t reserved;
} SrPpuMode7OverrideRequest;

#define SR_PPU_MODE7_OVERRIDE_REQUEST_V2_SIZE                            \
    ((uint32_t)(offsetof(SrPpuMode7OverrideRequest, reserved) +           \
                sizeof(((SrPpuMode7OverrideRequest *)0)->reserved)))

/* Generated-code execution state. Names are immutable runner-owned strings;
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

/* Synchronous event observers. Event classes are checked before payload
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
#define SR_EVENT_INTERRUPT_ENTER UINT32_C(0x00000001)
#define SR_EVENT_INTERRUPT_EXIT UINT32_C(0x00000002)
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

/* Memory-event addresses are zero-based byte offsets within memory_region.
 * Register-event addresses are the complete CPU-visible register addresses;
 * previous_value and memory_region are not defined for register events.
 * DMA_BEGIN address and dma_a_address24 are the initial A-bus address. For
 * HDMA this is the table start, dma_table_address is its 16-bit offset, and
 * dma_transfer_bytes is zero because the table determines the total. General
 * DMA normalizes a programmed size of zero to 65536 bytes. Interrupt events
 * identify the vector and use SR_INTERRUPT_SCANLINE_UNKNOWN when no raster
 * position applies. Error events identify their stable error_code; pc24 is the
 * affected execution/ROM address and source_pc24 is its caller when known.
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

/* Low-level, synchronous APU/SPC observation for diagnostics. The callback
 * runs while the producing thread owns the APU lock, so the register values
 * and ARAM view form one coherent observation. apu_ram is immutable through
 * this interface and valid only for the callback; consumers must copy any
 * bytes they retain. Install and remove observers only while audio production
 * and runner execution are stopped. */
#define SR_APU_RAM_BYTE_COUNT UINT64_C(0x10000)
#define SR_APU_INPUT_PORT_COUNT 6u
#define SR_APU_OUTPUT_PORT_COUNT 4u

typedef uint32_t SrAudioTraceEventType;
enum {
    SR_AUDIO_TRACE_APU_PORT_APPLY = 1u,
    SR_AUDIO_TRACE_SPC_PORT_READ = 2u,
    SR_AUDIO_TRACE_SPC_OPCODE = 3u,
    SR_AUDIO_TRACE_DSP_WRITE = 4u
};

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
} SrAudioTraceEvent;

#define SR_AUDIO_TRACE_EVENT_V2_SIZE                                     \
    ((uint32_t)(offsetof(SrAudioTraceEvent, reserved8) +                  \
                sizeof(((SrAudioTraceEvent *)0)->reserved8)))

typedef void (*SrAudioTraceCallback)(void *user_data,
                                     SrRunnerHandle *runner,
                                     const SrAudioTraceEvent *event);

typedef struct SrAudioTraceSubscription {
    uint32_t struct_size;
    uint32_t flags;
    SrAudioTraceCallback callback;
    void *user_data;
} SrAudioTraceSubscription;

#define SR_AUDIO_TRACE_SUBSCRIPTION_V2_SIZE                              \
    ((uint32_t)(offsetof(SrAudioTraceSubscription, user_data) +           \
                sizeof(((SrAudioTraceSubscription *)0)->user_data)))

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

/* Small, copied commands applied at the beginning of a host frame before
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

/* WRITE_MEMORY addresses are byte offsets within WRAM, SRAM, VRAM, CGRAM,
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

/* SR_MUTATION_QUERY_CONSUME releases terminal APPLIED/FAILED records after
 * copying them to out_status. QUEUED/APPLYING records are never consumed. */

#define SR_MUTATION_STATUS_V2_SIZE                                       \
    ((uint32_t)(offsetof(SrMutationStatus, applied_frame_counter) +       \
                sizeof(((SrMutationStatus *)0)->applied_frame_counter)))

typedef struct SnesRunnerApi {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    SrResult (*get_component)(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component);
    SrResult (*query_generations)(SrRunnerHandle *runner,
                                  SrGenerationSnapshot *out_generations);
    SrResult (*borrow_memory)(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span);
    uint32_t (*borrow_is_valid)(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span);
    SrResult (*query_cpu_state)(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state);
    SrResult (*query_ppu_state)(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state);
    SrResult (*borrow_u16_memory)(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span);
    uint32_t (*borrow_u16_is_valid)(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span);
    SrResult (*query_ppu_frame_state)(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state);
    SrResult (*rasterize_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    SrResult (*resolve_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result);
    SrResult (*rasterize_ppu_obj_parts)(
        SrRunnerHandle *runner,
        const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    SrResult (*query_ppu_surfaces)(SrRunnerHandle *runner,
                                   SrPpuSurfaceSnapshot *out_surfaces);
    uint32_t (*ppu_surface_snapshot_is_valid)(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces);
    SrResult (*query_execution_state)(SrRunnerHandle *runner,
                                      SrExecutionSnapshot *out_state);
    SrResult (*subscribe_events)(SrRunnerHandle *runner,
                                 const SrEventSubscription *subscription,
                                 uint64_t *out_subscription_id);
    SrResult (*unsubscribe_events)(SrRunnerHandle *runner,
                                   uint64_t subscription_id);
    SrResult (*queue_mutation)(SrRunnerHandle *runner,
                               const SrMutationCommand *command,
                               uint64_t *out_command_id);
    SrResult (*query_mutation)(SrRunnerHandle *runner,
                               uint64_t command_id, uint32_t flags,
                               SrMutationStatus *out_status);
    SrResult (*resolve_ppu_background_coordinate)(
        SrRunnerHandle *runner,
        const SrPpuBackgroundCoordinateRequest *request,
        SrPpuBackgroundCoordinateResult *out_result);
    SrResult (*bind_ppu_output_surface)(
        SrRunnerHandle *runner, const SrPpuOutputBindingRequest *request);
    SrResult (*configure_ppu_horizontal_margin)(
        SrRunnerHandle *runner,
        const SrPpuHorizontalMarginRequest *request);
    SrResult (*claim_ppu_overlay_capture)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureRequest *request);
    SrResult (*claim_ppu_mode7_override)(
        SrRunnerHandle *runner,
        const SrPpuMode7OverrideRequest *request);
    SrResult (*query_cpu_math_state)(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state);
    SrResult (*restore_cpu_math_state)(SrRunnerHandle *runner,
                                       const SrCpuMathState *state);
    SrResult (*subscribe_audio_trace)(
        SrRunnerHandle *runner,
        const SrAudioTraceSubscription *subscription,
        uint64_t *out_subscription_id);
    SrResult (*unsubscribe_audio_trace)(SrRunnerHandle *runner,
                                        uint64_t subscription_id);
    SrResult (*compare_exchange_spc_pc)(
        SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
        SrSpcPcControlResult *out_result);
    SrResult (*configure_audio_mix)(SrRunnerHandle *runner,
                                    const SrAudioMixControl *control);
    SrResult (*visit_ppu_frame_transaction)(
        SrRunnerHandle *runner,
        const SrPpuFrameTransactionRequest *request);
    SrResult (*compare_exchange_ppu_overlay_captures)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureExchangeRequest *request);
    SrResult (*compare_exchange_ppu_vram_words)(
        SrRunnerHandle *runner,
        const SrPpuVramPatchRequest *request);
    SrResult (*update_ppu_obj_metadata)(
        SrRunnerHandle *runner,
        const SrPpuObjMetadataRequest *request);
    SrResult (*query_dma_state)(SrRunnerHandle *runner,
                                SrDmaStateSnapshot *out_state);
    SrResult (*update_ppu_layer_extents)(
        SrRunnerHandle *runner,
        const SrPpuLayerExtentRequest *request);
    SrResult (*replace_ppu_virtual_tilemaps)(
        SrRunnerHandle *runner,
        const SrPpuVirtualTilemapRequest *request);
    SrResult (*update_ppu_authentic_camera)(
        SrRunnerHandle *runner,
        const SrPpuAuthenticCameraRequest *request);
    SrResult (*run_ppu_scanout)(
        SrRunnerHandle *runner,
        const SrPpuScanoutRequest *request,
        SrPpuScanoutResult *out_result);
} SnesRunnerApi;

#define SNES_RUNNER_API_V2_BASE_SIZE                                           \
    ((uint32_t)(offsetof(SnesRunnerApi, borrow_is_valid) +                 \
                sizeof(((SnesRunnerApi *)0)->borrow_is_valid)))

#define SNES_RUNNER_API_CPU_STATE_SIZE                                    \
    ((uint32_t)(offsetof(SnesRunnerApi, query_cpu_state) +                 \
                sizeof(((SnesRunnerApi *)0)->query_cpu_state)))

#define SNES_RUNNER_API_PPU_STATE_SIZE                                    \
    ((uint32_t)(offsetof(SnesRunnerApi, borrow_u16_is_valid) +             \
                sizeof(((SnesRunnerApi *)0)->borrow_u16_is_valid)))

#define SNES_RUNNER_API_PPU_FRAME_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, query_ppu_frame_state) +           \
                sizeof(((SnesRunnerApi *)0)->query_ppu_frame_state)))

#define SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE                               \
    ((uint32_t)(offsetof(SnesRunnerApi, rasterize_ppu_obj_range) +         \
                sizeof(((SnesRunnerApi *)0)->rasterize_ppu_obj_range)))

#define SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE                                \
    ((uint32_t)(offsetof(SnesRunnerApi, rasterize_ppu_obj_parts) +         \
                sizeof(((SnesRunnerApi *)0)->rasterize_ppu_obj_parts)))

#define SNES_RUNNER_API_PPU_SURFACE_SIZE                                  \
    ((uint32_t)(offsetof(SnesRunnerApi, ppu_surface_snapshot_is_valid) +   \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->ppu_surface_snapshot_is_valid)))

#define SNES_RUNNER_API_EXECUTION_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, query_execution_state) +           \
                sizeof(((SnesRunnerApi *)0)->query_execution_state)))

#define SNES_RUNNER_API_EVENT_OBSERVER_SIZE                               \
    ((uint32_t)(offsetof(SnesRunnerApi, unsubscribe_events) +              \
                sizeof(((SnesRunnerApi *)0)->unsubscribe_events)))

#define SNES_RUNNER_API_SAFE_POINT_MUTATION_SIZE                          \
    ((uint32_t)(offsetof(SnesRunnerApi, query_mutation) +                  \
                sizeof(((SnesRunnerApi *)0)->query_mutation)))

#define SNES_RUNNER_API_PPU_BACKGROUND_COORDINATE_SIZE                    \
    ((uint32_t)(offsetof(SnesRunnerApi, resolve_ppu_background_coordinate) + \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->resolve_ppu_background_coordinate)))

#define SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE                           \
    ((uint32_t)(offsetof(SnesRunnerApi, configure_ppu_horizontal_margin) + \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->configure_ppu_horizontal_margin)))

#define SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE                          \
    ((uint32_t)(offsetof(SnesRunnerApi, claim_ppu_mode7_override) +        \
                sizeof(((SnesRunnerApi *)0)->claim_ppu_mode7_override)))

#define SNES_RUNNER_API_CPU_MATH_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, restore_cpu_math_state) +         \
                sizeof(((SnesRunnerApi *)0)->restore_cpu_math_state)))

#define SNES_RUNNER_API_AUDIO_TRACE_OBSERVER_SIZE                        \
    ((uint32_t)(offsetof(SnesRunnerApi, unsubscribe_audio_trace) +        \
                sizeof(((SnesRunnerApi *)0)->unsubscribe_audio_trace)))

#define SNES_RUNNER_API_SPC_CONTROL_SIZE                                 \
    ((uint32_t)(offsetof(SnesRunnerApi, compare_exchange_spc_pc) +        \
                sizeof(((SnesRunnerApi *)0)->compare_exchange_spc_pc)))

#define SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE                           \
    ((uint32_t)(offsetof(SnesRunnerApi, configure_audio_mix) +            \
                sizeof(((SnesRunnerApi *)0)->configure_audio_mix)))

#define SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE                       \
    ((uint32_t)(offsetof(SnesRunnerApi,                                  \
                         compare_exchange_ppu_overlay_captures) +         \
                sizeof(((SnesRunnerApi *)0)                              \
                           ->compare_exchange_ppu_overlay_captures)))

#define SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, compare_exchange_ppu_vram_words) + \
                sizeof(((SnesRunnerApi *)0)                              \
                           ->compare_exchange_ppu_vram_words)))

#define SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE                            \
    ((uint32_t)(offsetof(SnesRunnerApi, update_ppu_obj_metadata) +         \
                sizeof(((SnesRunnerApi *)0)->update_ppu_obj_metadata)))

#define SNES_RUNNER_API_DMA_STATE_SIZE                                   \
    ((uint32_t)(offsetof(SnesRunnerApi, query_dma_state) +                \
                sizeof(((SnesRunnerApi *)0)->query_dma_state)))

#define SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE                       \
    ((uint32_t)(offsetof(SnesRunnerApi, update_ppu_authentic_camera) +    \
                sizeof(((SnesRunnerApi *)0)->update_ppu_authentic_camera)))

#define SNES_RUNNER_API_PPU_SCANOUT_SIZE                                 \
    ((uint32_t)(offsetof(SnesRunnerApi, run_ppu_scanout) +                \
                sizeof(((SnesRunnerApi *)0)->run_ppu_scanout)))

typedef struct SrRunnerDescriptor {
    uint32_t abi_version;
    const char *variant;
    /* Must remain zero for the independently licensed runner. */
    uint32_t legacy_source_count;
    uint32_t struct_size;
    uint64_t capabilities;
} SrRunnerDescriptor;

/* Describes the selected replacement-runner boundary. */
const SrRunnerDescriptor *sr_runner_descriptor(void);

/* Returns NULL for an unsupported version. The table and all function
 * pointers remain runner-owned for the process lifetime. */
const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif
