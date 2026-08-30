/**
 * @file ppu.h
 * @brief PPU snapshots, widescreen policy, captures, and scanout contracts.
 * @ingroup sr_runner_ppu
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_ppu
 *  @{
 */

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

/* Instantaneous live PPU controls at the call or callback phase that produced
 * the snapshot. These values are not a semantic world camera and do not
 * reconstruct the per-scanline register timeline of a completed frame. A
 * zero scroll value is valid when the game or HDMA has not written it yet. */
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
    /* Raw window edges and SETINI are primarily useful to diagnostics and
     * custom composition. Prefer the derived flags above for normal paths. */
    uint8_t window1_left;
    uint8_t window1_right;
    uint8_t window2_left;
    uint8_t window2_right;
    uint8_t setini_control;
    uint8_t layer_clamp_mask;
    uint8_t layer_mirror_mask;
    uint8_t layer_repeat_mask;
    uint8_t layer_normal_scroll_mask;
    uint8_t oam_address_low;
    uint8_t oam_address_high;
    uint8_t object_priority_rotation;
    uint8_t reserved8_3;
} SrPpuStateSnapshot;

#define SR_PPU_STATE_SNAPSHOT_V2_SIZE                                    \
    ((uint32_t)(offsetof(SrPpuStateSnapshot, reserved8_3) +                \
                sizeof(((SrPpuStateSnapshot *)0)->reserved8_3)))

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

#define SR_PPU_FRAME_HUD_POLICY_CONFIGURED UINT32_C(0x00000001)

/* Frame-derived runner policy and capture state. HUD fields echo the policy
 * published by the integration; the runner does not infer HUD geometry from
 * pixels, tilemaps, or hardware registers. */
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
    /* Repeat the native 256-pixel span into horizontal margins. On a
     * VRAM-backed 32-tile-wide BG, LIVE_WORLD and REPEAT can render identical
     * pixels because their source coordinates differ by one complete tilemap
     * period. Use resolve_ppu_background_coordinate to verify which policy was
     * selected instead of relying on visual inspection alone. */
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
    /* Make the configured budget immediately rasterizable on both sides. Use
     * this when ordinary PPU scanout should draw into horizontal margins. */
    SR_PPU_HORIZONTAL_MARGIN_AVAILABLE = 0u,
    /* Reserve a wide output allocation while keeping live rasterization at the
     * native 256-pixel width in its centre. A wide bound surface will therefore
     * still show a centred 256-pixel image until later policy makes side pixels
     * available; this is intentional, not missing background streaming. */
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

/* Declarative, frame-scoped presentation policy. Applying a policy replaces
 * horizontal/vertical margin geometry, layer fill modes, row-band overrides,
 * vertical clipping, and HUD split state as one validated transaction. It
 * normally starts a new background-policy frame: retained virtual tilemaps
 * and layer extents are cleared and may be republished afterward. FINALIZE is
 * the optional second phase for policy that depends on publication success;
 * it preserves those resources. The runner retains no request or band
 * pointer. Later bands win when ordered ranges overlap, matching normal
 * raster-policy composition. Per-frame geometry does not change host surface
 * bindings. A policy is rejected atomically if its complete reserved width or
 * rendered height exceeds a bound main/authentic surface's retained capacity.
 * Geometry changes expire borrowed surface snapshots. */
#define SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET UINT32_C(0x00000001)
/* Finalize a policy after frame-scoped virtual providers have been published.
 * The budget must match the active begin transaction. Finalize preserves
 * provider bindings and layer extents while replacing exact margins, fill
 * modes, row bands, vertical clipping, and HUD state. */
#define SR_PPU_FRAME_POLICY_FINALIZE UINT32_C(0x00000002)
#define SR_PPU_FRAME_POLICY_LAYER_MASK UINT32_C(0x0000000f)
#define SR_PPU_FRAME_POLICY_BAND_MAX 32u

typedef struct SrPpuFramePolicyBand {
    uint32_t layer;
    uint32_t y0;
    uint32_t y1;
    SrPpuBackgroundFill fill;
    SrPpuBackgroundMotion motion;
} SrPpuFramePolicyBand;

typedef struct SrPpuFramePolicy {
    uint32_t struct_size;
    uint32_t flags;
    SrPpuHorizontalMarginMode horizontal_mode;
    uint32_t margin_budget_pixels;
    uint32_t margin_left_pixels;
    uint32_t margin_right_pixels;
    /* Exact rasterized rows, not a reservation budget. Nonzero values cause
     * run_ppu_scanout to render signed rows above and below the native viewport
     * with the live PPU state at those points in scanout. Ordinary VRAM-backed
     * layers need no additional publication; forced blank, zero brightness,
     * layer masks, finite extents, and optional virtual providers still apply. */
    uint32_t margin_top_pixels;
    uint32_t margin_bottom_pixels;
    uint32_t layer_clamp_mask;
    uint32_t layer_mirror_mask;
    uint32_t layer_repeat_mask;
    uint32_t layer_normal_scroll_mask;
    /* Per-layer limits for the exact extra rows above and below the native
     * viewport. They do not crop native rows or horizontal side margins. */
    uint32_t vertical_clip_layer_mask;
    uint32_t vertical_clip_top_rows[4];
    uint32_t vertical_clip_bottom_rows[4];
    uint32_t hud_split_height;
    uint32_t hud_left_end_x;
    uint32_t hud_right_start_x;
    uint32_t hud_player_row_y;
    uint32_t hud_left_only_y;
    const SrPpuFramePolicyBand *bands;
    uint32_t band_count;
    uint32_t reserved;
} SrPpuFramePolicy;

#define SR_PPU_FRAME_POLICY_V2_SIZE                                     \
    ((uint32_t)(offsetof(SrPpuFramePolicy, reserved) +                   \
                sizeof(((SrPpuFramePolicy *)0)->reserved)))

typedef struct SrPpuFramePolicyRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    SrPpuFramePolicy policy;
} SrPpuFramePolicyRequest;

#define SR_PPU_FRAME_POLICY_REQUEST_V2_SIZE                             \
    ((uint32_t)(offsetof(SrPpuFramePolicyRequest, policy) +              \
                sizeof(((SrPpuFramePolicyRequest *)0)->policy)))

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
    SrBorrowedU16Span vram;
    SrBorrowedU16Span cgram;
    SrBorrowedU16Span oam;
    SrBorrowedSpan high_oam;
    SrPpuWritableSurfaceView main;
    SrPpuWritableSurfaceView authentic;
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

/* Clears runner-derived enhancement capture policy at the beginning of a
 * frame without disturbing persistent output bindings. This resets overlay
 * rectangles, OBJ relocation/range capture, and the active Mode-7 override.
 * Game-authored OBJ position metadata has an independent lifetime contract. */
typedef struct SrPpuFrameResetRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
} SrPpuFrameResetRequest;

#define SR_PPU_FRAME_RESET_REQUEST_V2_SIZE                              \
    ((uint32_t)(offsetof(SrPpuFrameResetRequest, lifetime_generation) +   \
                sizeof(((SrPpuFrameResetRequest *)0)->lifetime_generation)))

/* Optional OBJ capture facets. RANGE asks the live sprite evaluator to copy a
 * selected OAM range into a caller-owned surface during scanout. RELOCATED
 * excludes a selected subset from a full-add OBJ plane after it has been
 * moved to an independent presentation layer. A zero count clears that facet.
 * The range surface remains caller-owned and must stay alive through scanout. */
#define SR_PPU_OBJ_CAPTURE_RANGE UINT32_C(0x00000001)
#define SR_PPU_OBJ_CAPTURE_RELOCATED UINT32_C(0x00000002)

typedef struct SrPpuObjCaptureRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lifetime_generation;
    uint32_t range_first;
    uint32_t range_count;
    int32_t range_x;
    int32_t range_y;
    uint32_t range_width;
    uint32_t range_height;
    uint8_t *range_pixels;
    uint64_t range_pixel_byte_size;
    uint64_t range_pitch_bytes;
    uint32_t relocated_first;
    uint32_t relocated_count;
    uint32_t reserved[2];
} SrPpuObjCaptureRequest;

#define SR_PPU_OBJ_CAPTURE_REQUEST_V2_SIZE                              \
    ((uint32_t)(offsetof(SrPpuObjCaptureRequest, reserved) +             \
                sizeof(((SrPpuObjCaptureRequest *)0)->reserved)))

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

/* Scalar provider result. Existing boolean providers remain compatible:
 * zero is an intentional transparent gap and one supplies *entry. Authentic
 * fallback samples the resident VRAM tilemap for the same displayed point;
 * it is distinct from a transparent finite-world boundary. Other values are
 * reserved and currently fail closed as transparent. */
typedef uint32_t SrPpuVirtualTileLookupResult;
enum {
    SR_PPU_VIRTUAL_TILE_TRANSPARENT = 0u,
    SR_PPU_VIRTUAL_TILE_FOUND = 1u,
    SR_PPU_VIRTUAL_TILE_FALLBACK_AUTHENTIC = 2u
};

/* Finite-world tile providers are retained until the next replacement,
 * runner reset, or lifetime invalidation. Scalar lookup is the mandatory
 * correctness path. Span and priority-band lookup are optional fast paths.
 * Span output pointers remain valid only until the provider's next span call.
 * A nonzero span with a null entries pointer is an intentional transparent
 * gap. Return zero at a mixed-coverage boundary to request scalar lookup,
 * including when that coordinate needs authentic fallback. */
typedef SrPpuVirtualTileLookupResult (*SrPpuVirtualTileLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, uint16_t *entry);
typedef uint32_t (*SrPpuVirtualTileSpanLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, int32_t tile_step,
    uint32_t capacity, const uint16_t **entries, int64_t *word_stride);
typedef uint32_t (*SrPpuVirtualTileBandLookup)(
    void *user_data, int32_t tile_x, int32_t tile_y, uint16_t entry,
    uint8_t *band);

/* Consult the provider inside the native viewport as well as synthetic
 * margins. Without this flag the resident VRAM path retains authentic pixels. */
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
 * scheduling. HDMA starts from the runner-owned $420C state; a request may
 * suppress channels for enhancement policy, but it cannot arm one. The
 * zero-initialized request therefore preserves normal hardware behavior.
 * Scanout consumes the live PPU/DMA state present when it is called; it does
 * not decide which body or NMI phase prepared that state. irq_callback is
 * required and owns only the recompiled CPU's IRQ handler.
 * Optional line callbacks are for diagnostics and receive callback-lifetime,
 * zero-copy surface views; normal frames should leave them null. */
#define SR_PPU_SCANOUT_LINE_BEFORE UINT32_C(0x00000001)
#define SR_PPU_SCANOUT_LINE_AFTER_HDMA UINT32_C(0x00000002)
#define SR_PPU_SCANOUT_HDMA_ACTIVE UINT32_C(0x00000001)
#define SR_PPU_SCANOUT_HDMA_INDIRECT UINT32_C(0x00000002)
/** Capture the completed logical main canvas for a later determinism query. */
#define SR_PPU_SCANOUT_CAPTURE_PRESENTATION_DIGEST UINT32_C(0x00000001)

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
    /* Set SR_PPU_SCANOUT_CAPTURE_PRESENTATION_DIGEST only on frames that need
     * a canonical checkpoint; ordinary scanout pays no hashing cost. */
    uint32_t flags;
    uint64_t lifetime_generation;
    /* Bits set here suppress the corresponding channel after applying the
     * game's $420C enable state. Zero preserves every hardware-armed channel. */
    uint32_t hdma_suppress_mask;
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

/** @} */

#ifdef __cplusplus
}
#endif
