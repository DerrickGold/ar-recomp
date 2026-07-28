#ifndef SIM_WORLD_NAVIGATION_SCENE_H
#define SIM_WORLD_NAVIGATION_SCENE_H

#include <stdbool.h>
#include <stdint.h>

/* Raw $09 camera state captured from WRAM on the game thread. The current
 * matrix is the transform uploaded for this frame; the staged matrix is kept
 * separately so presentation never guesses which side of the ROM's update it
 * observed. */
typedef struct SimWorldNavigationFrame {
  uint16_t focus_x, focus_y;
  int16_t matrix[4];       /* current A/B/C/D uploaded for this frame */
  int16_t next_matrix[4];  /* staged A/B/C/D for the next update */
  uint16_t rotation;
  uint16_t zoom_current;
  uint16_t zoom_target;
  uint16_t active_location;
} SimWorldNavigationFrame;

enum {
  kSimWorldNavigationZoomNear = 0x0206,
  kSimWorldNavigationZoomMiddle = 0x040A,
  kSimWorldNavigationZoomFar = 0x0562,
};

typedef struct SimWorldNavigationGroundVertex {
  /* Full-world tile coordinates. The four vertices cover [0,128] on each
   * axis; the half-open content extent is [0,128) x [0,128). */
  uint16_t tile_x, tile_y;
  float texture_u, texture_v;
} SimWorldNavigationGroundVertex;

typedef struct SimWorldNavigationCompositionLayer {
  bool visible;
  uint8_t oam_first, oam_count;
  int16_t screen_x, screen_y;
  uint16_t width, height;
} SimWorldNavigationCompositionLayer;

/* Navigation OAM is not a simulation-town record list. The steady screen has
 * a screen-space location label/frame followed by the fixed-centre 3x3 Palace
 * composition; the action-entry spin hides every OAM slot. */
typedef struct SimWorldNavigationComposition {
  bool valid;
  bool empty_animation;
  SimWorldNavigationCompositionLayer palace;
  SimWorldNavigationCompositionLayer ui;
} SimWorldNavigationComposition;

/* Immutable Step-3 scene contract.
 *
 * This is intentionally not a simulation-town profile. It owns one complete
 * developed-world texture and one four-corner ground plane. There is no town
 * canvas, captured BG stack, object atlas, cull window, or underlay margin.
 *
 * `source_to_screen` maps a point in the 1024x1024 world texture to authentic
 * 256x224 screen coordinates:
 *
 *   sx = m[0] * source_x + m[1] * source_y + m[2]
 *   sy = m[3] * source_x + m[4] * source_y + m[5]
 *
 * It is the exact inverse of the current Mode-7 A/B/C/D matrix around
 * focus_x/focus_y, so the host plane follows both steady navigation and the
 * action-entry zoom/spin without consulting live WRAM or PPU state. */
typedef struct SimWorldNavigationScene {
  bool valid;
  uint32_t texture_serial;
  uint16_t texture_width, texture_height;
  uint16_t tile_width, tile_height;
  float source_to_screen[6];
  SimWorldNavigationGroundVertex ground[4];
  /* 1-based ROM location and its $01:B73C region in source-texture pixels.
   * Zero/unknown locations mean the Palace is outside every town border, so
   * no clear region is cut out of the full-world haze. */
  uint16_t active_location;
  bool active_region_valid;
  uint16_t active_region_x, active_region_y;
  uint16_t active_region_width, active_region_height;
  SimWorldNavigationComposition composition;
} SimWorldNavigationScene;

/* Builds the complete immutable scene. A missing developed texture or a
 * singular/non-finite matrix fails closed, leaving `out` fully zeroed. */
bool SimWorldNavigationScene_Build(
    SimWorldNavigationScene *out,
    const SimWorldNavigationFrame *navigation,
    uint32_t developed_texture_serial);

/* Pure projection helper shared by tests and the Step-4 renderer. Coordinates
 * are world-texture pixels and authentic screen pixels respectively. */
bool SimWorldNavigationScene_ProjectSource(
    const SimWorldNavigationScene *scene,
    float source_x, float source_y,
    float *screen_x, float *screen_y);

/* Visibility of cloud bodies from the scripted top-down camera. At the
 * authentic near zoom the camera is below a normally elevated cloud deck; at
 * middle/far zoom it is above it. Cloud shadows are intentionally independent
 * so terrain can still show moving cover while the camera is below the deck. */
float SimWorldNavigationScene_CloudVisibility(
    uint16_t zoom_current, uint16_t cloud_altitude_px);

/* Black-overlay alpha that reproduces INIDISP's 0..15 master brightness over
 * a full-intensity host composition. 255 is black; zero is full brightness. */
uint8_t SimWorldNavigationScene_MasterFadeAlpha(uint8_t brightness);

/* World-space haze proximity, 0 clear .. 1 fully hazed. A scene with no
 * active region returns 1 everywhere: between town borders the whole world is
 * distant. */
float SimWorldNavigationScene_LocationHaze(
    const SimWorldNavigationScene *scene,
    float source_x, float source_y, float lead);

/* Pure OAM ownership classifier. `oam` is the PPU's 256-word low table
 * (position/attributes pairs). It recognizes either the fixed Palace
 * signature with the packed UI prefix, or the all-hidden action-entry state.
 * Anything else fails closed. Raster bounds are filled later by the PPU-backed
 * capture step. */
bool SimWorldNavigationScene_ClassifyOam(
    const uint16_t oam[256],
    SimWorldNavigationComposition *out);

#endif  /* SIM_WORLD_NAVIGATION_SCENE_H */
