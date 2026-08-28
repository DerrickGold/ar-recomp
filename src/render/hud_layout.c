#include "hud_layout.h"

enum { kHudPercentScale = 100 };

static int ScaledPixels(int pixels, double scale) {
  const int result = (int)(pixels * scale + 0.5);
  return result > 0 ? result : 1;
}

static void AddChunk(HudPresentationChunk *chunks, int *count,
                     ArRenderTexture texture,
                     ArRenderRectI texture_source,
                     ArRenderRectI screen_source,
                     ArRenderRectI output_destination,
                     InspectorPresentationKind kind,
                     int inspector_x_bias) {
  if (!chunks || !count || *count >= kHudPresentationChunkCapacity ||
      !ArRenderTexture_IsValid(texture) ||
      texture_source.w <= 0 || texture_source.h <= 0 ||
      screen_source.w <= 0 || screen_source.h <= 0 ||
      output_destination.w <= 0 || output_destination.h <= 0)
    return;
  chunks[(*count)++] = (HudPresentationChunk){
    texture, texture_source, screen_source, output_destination,
    kind, inspector_x_bias,
  };
}

int ArHudLayout_BuildPresentationChunks(
    ArRenderRectI viewport, const HudProjectionInputs *in,
    HudPresentationChunk *chunks) {
  if (!in || !ArRenderTexture_IsValid(in->hud_bg_texture) ||
      !in->hud_split_height || in->snes_width <= 0 ||
      in->snes_height <= 0 || in->visible_width <= 0 ||
      in->authentic_width <= 0)
    return 0;

  int count = 0;
  double scale_y, scale_x;
  if (in->hud_scale_percent == 0) {
    scale_y = (double)viewport.h / in->snes_height;
    scale_x = (double)viewport.w / in->visible_width;
  } else {
    scale_y = in->hud_scale_percent / (double)kHudPercentScale;
    scale_x = scale_y * (in->crt_pixel_aspect ? 7.0 / 6.0 : 1.0);
  }

  const int texture_extra = (in->snes_width - in->authentic_width) / 2;
  const int height = in->hud_split_height;
  int player_y = in->hud_player_row_y;
  int enemy_y = in->hud_left_only_y;
  if (player_y > height) player_y = height;
  if (enemy_y > height) enemy_y = height;
  if (player_y > enemy_y) player_y = enemy_y;

  const int upper_height = player_y;
  const int upper_destination_height = ScaledPixels(upper_height, scale_y);
  ArRenderRectI source = {
    texture_extra, 0, in->hud_left_end, upper_height,
  };
  ArRenderRectI destination = {
    viewport.x, viewport.y,
    ScaledPixels(source.w, scale_x), upper_destination_height,
  };
  AddChunk(
      chunks, &count, in->hud_bg_texture, source,
      (ArRenderRectI){0, 0, source.w, source.h}, destination,
      kInspectorPresentation_HudBg, -in->extra_left_right);

  if (in->hud_left_end < in->hud_right_start) {
    source.x = texture_extra + in->hud_left_end;
    source.w = in->hud_right_start - in->hud_left_end;
    destination.w = ScaledPixels(source.w, scale_x);
    destination.x = viewport.x + (viewport.w - destination.w) / 2;
    AddChunk(
        chunks, &count, in->hud_bg_texture, source,
        (ArRenderRectI){in->hud_left_end, 0, source.w, source.h},
        destination, kInspectorPresentation_HudBg, 0);
  }

  const int right_source_width =
      in->authentic_width - in->hud_right_start;
  const int right_destination_width =
      ScaledPixels(right_source_width, scale_x);
  source.x = texture_extra + in->hud_right_start;
  source.w = right_source_width;
  destination.x = viewport.x + viewport.w - right_destination_width;
  destination.w = right_destination_width;
  AddChunk(
      chunks, &count, in->hud_bg_texture, source,
      (ArRenderRectI){in->hud_right_start, 0, source.w, source.h},
      destination, kInspectorPresentation_HudBg, in->extra_left_right);

  if (player_y < enemy_y) {
    const int middle_height = enemy_y - player_y;
    const int middle_destination_height =
        ScaledPixels(middle_height, scale_y);
    const int middle_destination_y =
        viewport.y + ScaledPixels(player_y, scale_y);

    source = (ArRenderRectI){
      texture_extra, player_y, in->hud_right_start, middle_height,
    };
    destination = (ArRenderRectI){
      viewport.x, middle_destination_y,
      ScaledPixels(source.w, scale_x), middle_destination_height,
    };
    AddChunk(
        chunks, &count, in->hud_bg_texture, source,
        (ArRenderRectI){0, player_y, source.w, source.h}, destination,
        kInspectorPresentation_HudBg, -in->extra_left_right);

    source.x = texture_extra + in->hud_right_start;
    source.w = in->authentic_width - in->hud_right_start;
    destination.x = viewport.x + viewport.w -
        ScaledPixels(source.w, scale_x);
    destination.w = ScaledPixels(source.w, scale_x);
    AddChunk(
        chunks, &count, in->hud_bg_texture, source,
        (ArRenderRectI){in->hud_right_start, player_y,
                        source.w, source.h},
        destination, kInspectorPresentation_HudBg, in->extra_left_right);
  }

  if (enemy_y < height) {
    const int lower_height = height - enemy_y;
    source = (ArRenderRectI){
      texture_extra, enemy_y, in->authentic_width, lower_height,
    };
    destination = (ArRenderRectI){
      viewport.x,
      viewport.y + ScaledPixels(enemy_y, scale_y),
      ScaledPixels(source.w, scale_x),
      ScaledPixels(lower_height, scale_y),
    };
    AddChunk(
        chunks, &count, in->hud_bg_texture, source,
        (ArRenderRectI){0, enemy_y, source.w, source.h}, destination,
        kInspectorPresentation_HudBg, -in->extra_left_right);
  }

  if (in->hud_body_y1 > height) {
    const int body_height = in->hud_body_y1 - height;
    const int body_destination_width =
        ScaledPixels(in->authentic_width, scale_x);
    const ArRenderRectI body_source = {
      texture_extra, height, in->authentic_width, body_height,
    };
    const ArRenderRectI body_destination = {
      viewport.x + (viewport.w - body_destination_width) / 2,
      viewport.y + ScaledPixels(height, scale_y),
      body_destination_width, ScaledPixels(body_height, scale_y),
    };
    AddChunk(
        chunks, &count, in->hud_bg_texture, body_source,
        (ArRenderRectI){0, height, in->authentic_width, body_height},
        body_destination, kInspectorPresentation_HudBg, 0);
  }

  if (ArRenderTexture_IsValid(in->hud_obj_texture) && in->obj_icon_valid &&
      in->obj_icon_x < in->authentic_width) {
    enum { kIconSize = 16, kIconRightInset = 20 };
    const int x = in->obj_icon_x;
    const int y = in->obj_icon_y;
    const ArRenderRectI icon_source = {
      texture_extra + x, y, kIconSize, kIconSize,
    };
    const ArRenderRectI icon_destination = {
      viewport.x + viewport.w - right_destination_width -
          ScaledPixels(kIconRightInset, scale_x),
      viewport.y + ScaledPixels(y, scale_y),
      ScaledPixels(kIconSize, scale_x),
      ScaledPixels(kIconSize, scale_y),
    };
    AddChunk(
        chunks, &count, in->hud_obj_texture, icon_source,
        (ArRenderRectI){x, y, kIconSize, kIconSize}, icon_destination,
        kInspectorPresentation_HudObj, 0);
  }
  return count;
}
