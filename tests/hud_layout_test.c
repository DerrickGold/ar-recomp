#include "render/hud_layout.h"

#include <assert.h>
#include <stdio.h>

static void CheckRect(ArRenderRectI actual,
                      int x, int y, int width, int height) {
  assert(actual.x == x);
  assert(actual.y == y);
  assert(actual.w == width);
  assert(actual.h == height);
}

int main(void) {
  const HudProjectionInputs inputs = {
    .hud_bg_texture = {1},
    .hud_obj_texture = {2},
    .snes_width = 512,
    .snes_height = 224,
    .visible_width = 256,
    .authentic_width = 256,
    .hud_split_height = 32,
    .hud_left_end = 64,
    .hud_right_start = 192,
    .hud_player_row_y = 8,
    .hud_left_only_y = 16,
    .extra_left_right = 128,
    .hud_body_y1 = 64,
    .obj_icon_valid = true,
    .obj_icon_x = 224,
    .obj_icon_y = 8,
  };
  HudPresentationChunk chunks[kHudPresentationChunkCapacity] = {0};
  const int count = ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){100, 50, 960, 840}, &inputs, chunks);
  assert(count == kHudPresentationChunkCapacity);

  CheckRect(chunks[0].texture_source, 128, 0, 64, 8);
  CheckRect(chunks[0].output_destination, 100, 50, 240, 30);
  assert(chunks[0].inspector_x_bias == -128);
  CheckRect(chunks[1].output_destination, 340, 50, 480, 30);
  CheckRect(chunks[2].output_destination, 820, 50, 240, 30);
  CheckRect(chunks[3].output_destination, 100, 80, 720, 30);
  CheckRect(chunks[4].output_destination, 820, 80, 240, 30);
  CheckRect(chunks[5].output_destination, 100, 110, 960, 60);
  CheckRect(chunks[6].output_destination, 100, 170, 960, 120);
  assert(chunks[6].inspector_x_bias == 0);
  CheckRect(chunks[7].texture_source, 352, 8, 16, 16);
  CheckRect(chunks[7].output_destination, 745, 80, 60, 60);
  assert(chunks[7].inspector_kind == kInspectorPresentation_HudObj);

  /* Wide Raw Diorama has no HUD split. Its full BG3 fallback capture follows
   * the native viewport and the OBJ icon retains its screen coordinate. */
  HudProjectionInputs native = inputs;
  native.snes_width = 256;
  native.visible_width = 256;
  native.hud_scale_percent = 275;
  native.hud_split_height = 0;
  native.hud_body_y1 = 224;
  native.obj_icon_x = 220;
  native.obj_icon_y = 8;
  const int native_count = ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){100, 50, 1024, 896}, &native, chunks);
  assert(native_count == 2);
  CheckRect(chunks[0].texture_source, 0, 0, 256, 224);
  CheckRect(chunks[0].output_destination, 100, 50, 1024, 896);
  assert(chunks[0].inspector_kind == kInspectorPresentation_HudBg);
  CheckRect(chunks[1].texture_source, 220, 8, 16, 16);
  CheckRect(chunks[1].output_destination, 980, 82, 64, 64);
  assert(chunks[1].inspector_kind == kInspectorPresentation_HudObj);

  native.obj_icon_valid = false;
  native.hud_body_y1 = 64;
  assert(ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){100, 50, 1024, 896}, &native, chunks) == 1);
  CheckRect(chunks[0].output_destination, 100, 50, 1024, 256);

  native.snes_width = 512;
  native.visible_width = 496;
  native.hud_body_y1 = 224;
  const int raw_count = ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){0, 0, 1984, 896}, &native, chunks);
  assert(raw_count == 1);
  CheckRect(chunks[0].texture_source, 128, 0, 256, 224);
  CheckRect(chunks[0].output_destination, 480, 0, 1024, 896);

  HudProjectionInputs scaled_43 = inputs;
  scaled_43.snes_width = 256;
  scaled_43.visible_width = 256;
  scaled_43.extra_left_right = 0;
  scaled_43.hud_scale_percent = 200;
  const int scaled_43_count = ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){100, 50, 1024, 896}, &scaled_43, chunks);
  assert(scaled_43_count == kHudPresentationChunkCapacity);
  CheckRect(chunks[0].texture_source, 0, 0, 64, 8);
  CheckRect(chunks[0].output_destination, 100, 50, 128, 16);
  CheckRect(chunks[1].output_destination, 484, 50, 256, 16);
  CheckRect(chunks[2].output_destination, 996, 50, 128, 16);
  CheckRect(chunks[6].output_destination, 356, 114, 512, 64);
  CheckRect(chunks[7].texture_source, 224, 8, 16, 16);
  CheckRect(chunks[7].output_destination, 956, 66, 32, 32);

  HudProjectionInputs invalid = inputs;
  invalid.authentic_width = 0;
  assert(ArHudLayout_BuildPresentationChunks(
      (ArRenderRectI){100, 50, 960, 840}, &invalid, chunks) == 0);

  puts("hud_layout_test: PASS");
  return 0;
}
