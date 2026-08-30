#ifndef AR_HUD_LAYOUT_H
#define AR_HUD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "render_types.h"

typedef enum InspectorPresentationKind {
  kInspectorPresentation_Base,
  kInspectorPresentation_HudBg,
  kInspectorPresentation_HudObj,
} InspectorPresentationKind;

typedef struct HudProjectionInputs {
  ArRenderTexture hud_bg_texture;
  ArRenderTexture hud_obj_texture;
  int hud_scale_percent;
  bool crt_pixel_aspect;
  int snes_width;
  int snes_height;
  int visible_width;
  int authentic_width;
  uint8_t hud_split_height, hud_left_end, hud_right_start;
  uint8_t hud_player_row_y, hud_left_only_y, extra_left_right;
  /* Bottom row (exclusive) of the BG3 capture when it extends below the
   * status bar. Zero, or a value at/before the split, means status bar only. */
  uint8_t hud_body_y1;
  bool obj_icon_valid;
  int obj_icon_x, obj_icon_y;
} HudProjectionInputs;

typedef struct HudPresentationChunk {
  ArRenderTexture texture;
  ArRenderRectI texture_source;
  ArRenderRectI screen_source;
  ArRenderRectI output_destination;
  InspectorPresentationKind inspector_kind;
  int inspector_x_bias;
} HudPresentationChunk;

/* Three top-band chunks, two player-row chunks, one enemy row, one optional
 * lower BG3 body, and one optional OBJ icon. A native unsplit capture uses at
 * most one BG3 chunk and one OBJ icon chunk. */
enum { kHudPresentationChunkCapacity = 8 };

/* Pure projection shared by rendering and inspector hit-testing. */
int ArHudLayout_BuildPresentationChunks(
    ArRenderRectI viewport, const HudProjectionInputs *inputs,
    HudPresentationChunk *chunks);

#endif /* AR_HUD_LAYOUT_H */
