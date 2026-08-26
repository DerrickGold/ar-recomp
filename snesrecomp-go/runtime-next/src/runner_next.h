#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_RUNNER_ABI_VERSION 1u

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

typedef uint32_t SrResult;
enum {
    SR_RESULT_OK = 0u,
    SR_RESULT_INVALID_ARGUMENT = 1u,
    SR_RESULT_UNSUPPORTED = 2u,
    SR_RESULT_UNAVAILABLE = 3u,
    SR_RESULT_STALE_VIEW = 4u
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
#define SR_PPU_OBJ_X_WRAP 512u
#define SR_PPU_OBJ_Y_WRAP 256u
#define SR_PPU_OBJ_Y_NEGATIVE_FROM 224u
#define SR_PPU_TILE_ID_COUNT 256u
#define SR_PPU_OVERLAY_SOURCE_COUNT 5u
#define SR_PPU_SURFACE_BAND_COUNT 4u
enum {
    SR_PPU_OVERLAY_BG1 = 0u,
    SR_PPU_OVERLAY_BG2 = 1u,
    SR_PPU_OVERLAY_BG3 = 2u,
    SR_PPU_OVERLAY_BG4 = 3u,
    SR_PPU_OVERLAY_OBJ = 4u
};

#define SR_PPU_OVERLAY_REMOVE_FROM_GAME UINT32_C(0x00000001)
#define SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH UINT32_C(0x00000002)
#define SR_PPU_OVERLAY_MARK_BG_HALF_ADD UINT32_C(0x00000004)
#define SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT UINT32_C(0x00000008)
#define SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN UINT32_C(0x00000010)
#define SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER UINT32_C(0x00000020)
#define SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER UINT32_C(0x00000040)

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

#define SR_BORROWED_SPAN_V1_SIZE                                           \
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

#define SR_BORROWED_U16_SPAN_V1_SIZE                                      \
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

#define SR_GENERATION_SNAPSHOT_V1_SIZE                                    \
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

#define SR_CPU_STATE_SNAPSHOT_V1_SIZE                                    \
    ((uint32_t)(offsetof(SrCpuStateSnapshot, reserved) +                  \
                sizeof(((SrCpuStateSnapshot *)0)->reserved)))

#define SR_PPU_STATE_FORCED_BLANK UINT32_C(0x00000001)
#define SR_PPU_STATE_BG3_PRIORITY UINT32_C(0x00000002)
#define SR_PPU_STATE_INTERLACE UINT32_C(0x00000004)
#define SR_PPU_STATE_OBJ_INTERLACE UINT32_C(0x00000008)
#define SR_PPU_STATE_OVERSCAN UINT32_C(0x00000010)
#define SR_PPU_STATE_PSEUDO_HIRES UINT32_C(0x00000020)
#define SR_PPU_STATE_MODE7_EXT_BG UINT32_C(0x00000040)

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
} SrPpuStateSnapshot;

#define SR_PPU_STATE_SNAPSHOT_V1_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuStateSnapshot, backgrounds) +               \
                sizeof(((SrPpuStateSnapshot *)0)->backgrounds)))

typedef struct SrPpuOverlayState {
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t y1;
    uint32_t flags;
    uint32_t content_band_mask;
    uint32_t transparent_fill_argb;
    uint8_t transparent_fill_configured;
    uint8_t oam_first;
    uint8_t oam_count;
    uint8_t reserved8;
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

#define SR_PPU_FRAME_SNAPSHOT_V1_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuFrameSnapshot, overlays) +                  \
                sizeof(((SrPpuFrameSnapshot *)0)->overlays)))

/* Caller-owned raster output. Each pixel is a host-native uint32_t whose
 * numeric value is 0xAARRGGBB; transparent source pixels are zero. The
 * request generation must come from a coherent PPU snapshot or borrow. */
#define SR_PPU_PIXEL_FORMAT_ARGB8888_U32 1u
#define SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32 \
    SR_PPU_PIXEL_FORMAT_ARGB8888_U32

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

#define SR_PPU_OBJ_RASTER_REQUEST_V1_SIZE                                \
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

#define SR_PPU_OBJ_RASTER_RESULT_V1_SIZE                                 \
    ((uint32_t)(offsetof(SrPpuObjRasterResult, height) +                  \
                sizeof(((SrPpuObjRasterResult *)0)->height)))

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

#define SR_PPU_SURFACE_SNAPSHOT_V1_SIZE                                  \
    ((uint32_t)(offsetof(SrPpuSurfaceSnapshot, mode7) +                    \
                sizeof(((SrPpuSurfaceSnapshot *)0)->mode7)))

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
    SrResult (*query_ppu_surfaces)(SrRunnerHandle *runner,
                                   SrPpuSurfaceSnapshot *out_surfaces);
    uint32_t (*ppu_surface_snapshot_is_valid)(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces);
} SnesRunnerApi;

#define SNES_RUNNER_API_V1_SIZE                                           \
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

#define SNES_RUNNER_API_PPU_SURFACE_SIZE                                  \
    ((uint32_t)(offsetof(SnesRunnerApi, ppu_surface_snapshot_is_valid) +   \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->ppu_surface_snapshot_is_valid)))

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
