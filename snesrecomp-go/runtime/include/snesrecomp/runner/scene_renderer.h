#pragma once
/* Isolated Mode-1 background rasterizer for authoring tools. Uses the runtime's
 * production PPU, without a runner, CPU, audio, ROM or gameplay state. Inputs
 * are borrowed only for Render. Output buffers stay owned by the renderer until
 * its next successful render or destruction. This is an in-process API, not a
 * serialized struct layout. No process-global renderer state is installed. */
#include "snesrecomp/runner/ppu.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SrSceneRenderer SrSceneRenderer;
enum { SR_SCENE_WIDTH = 640, SR_SCENE_HEIGHT = 352, SR_SCENE_LINES = 224 };
typedef struct SrSceneRowPolicy {
    uint8_t fill; /* SR_PPU_BACKGROUND_FILL_* */
    uint8_t motion;
    uint16_t left, right;
} SrSceneRowPolicy;
typedef struct SrSceneBackground {
    SrPpuVirtualTilemapBinding tiles;
    SrPpuCaptureTileBinding edits;
    const SrSceneRowPolicy *rows; /* 224 entries */
    const uint16_t *hscroll, *vscroll; /* 224 entries each */
    uint16_t top, bottom;
    uint16_t clip_top, clip_bottom;
    bool clip_vertical;
    uint8_t capture_flags; /* SR_PPU_OVERLAY_* */
    uint8_t fill_mode, fill_cgram;
    bool fill_configured;
} SrSceneBackground;
typedef struct SrSceneBackgroundView {
    uint16_t width, world_width, world_height; /* width 0 disables */
    int16_t screen_x0;
    uint8_t layer;
} SrSceneBackgroundView;
typedef struct SrSceneFrame {
    uint32_t struct_size;
    const uint16_t *vram; /* 32768 host-endian words */
    const uint16_t *cgram; /* 256 host-endian words */
    const uint8_t *mosaic; /* 224 entries */
    SrSceneBackground backgrounds[2];
    SrSceneBackgroundView background_view;
    uint16_t extra_x, top, bottom;
    uint8_t main_screen, sub_screen, cgwsel, cgadsub;
    uint16_t fixed_color;
    uint8_t native_page_mask; /* optional 256x256 canonical pages; no edits/classification */
    bool reference_renderer; /* correctness oracle; normal tools leave false */
    SrPpuBgPacket *background_packet; /* optional live capture export oracle */
} SrSceneFrame;
typedef struct SrSceneSurfaces {
    const uint32_t *backdrop;
    const uint32_t *bands[2][3]; /* ordinary, high, far */
    uint8_t content[2]; /* bit per band */
    int width, height, pitch_pixels;
    uint32_t transparent_fill[2];
    const uint32_t *background_view; /* tightly packed view_width x height */
    int view_width;
    const uint32_t *native_pages[2]; /* tightly packed; NULL if not requested/unsupported */
} SrSceneSurfaces;

SrSceneRenderer *sr_scene_renderer_create(void);
void sr_scene_renderer_destroy(SrSceneRenderer *renderer);
/* Invalid requests leave the previous surfaces untouched. */
bool sr_scene_renderer_render(SrSceneRenderer *renderer,
                               const SrSceneFrame *frame,
                               SrSceneSurfaces *out);

#ifdef __cplusplus
}
#endif
