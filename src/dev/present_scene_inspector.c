#include "dev/present_scene_inspector.h"

#include "dev/scene_inspector.h"
#include "host/host_video.h"
#include "present/present.h"
#include "render/present_hud.h"
#include "settings_overlay/settings_overlay_render.h"

static int InspectorScreenToOutputX(ArRenderRectI viewport, double screen_x,
                                    const FrameSlot *slot) {
  int visible_left = slot->visible_x0 - slot->ws_extra;
  return viewport.x + (int)((screen_x - visible_left) * viewport.w /
                            slot->visible_width + 0.5);
}

static int InspectorScreenToOutputY(ArRenderRectI viewport, double screen_y,
                                    const FrameSlot *slot) {
  return viewport.y + (int)(screen_y * viewport.h / slot->snes_height + 0.5);
}

static int HudSourceToOutputX(const HudPresentationChunk *chunk, double source_x) {
  return chunk->output_destination.x +
      (int)((source_x - chunk->screen_source.x) *
            chunk->output_destination.w / chunk->screen_source.w + 0.5);
}

static int HudSourceToOutputY(const HudPresentationChunk *chunk, double source_y) {
  return chunk->output_destination.y +
      (int)((source_y - chunk->screen_source.y) *
            chunk->output_destination.h / chunk->screen_source.h + 0.5);
}

static bool HudHighlightToOutput(const HudPresentationChunk *chunk,
                                 int x0, int y0, int x1, int y1,
                                 ArRenderRectI *output) {
  if (!chunk || !output) return false;
  x0 -= chunk->inspector_x_bias;
  x1 -= chunk->inspector_x_bias;
  const ArRenderRectI source = chunk->screen_source;
  if (x0 < source.x) x0 = source.x;
  if (y0 < source.y) y0 = source.y;
  if (x1 > source.x + source.w) x1 = source.x + source.w;
  if (y1 > source.y + source.h) y1 = source.y + source.h;
  if (x1 <= x0 || y1 <= y0) return false;
  int output_x0 = HudSourceToOutputX(chunk, x0);
  int output_y0 = HudSourceToOutputY(chunk, y0);
  int output_x1 = HudSourceToOutputX(chunk, x1);
  int output_y1 = HudSourceToOutputY(chunk, y1);
  *output = (ArRenderRectI){
    output_x0, output_y0, output_x1 - output_x0, output_y1 - output_y0,
  };
  return output->w > 0 && output->h > 0;
}

static bool FindSelectedHudChunk(const FrameSlot *slot,
                                 ArRenderRectI viewport,
                                 HudPresentationChunk *selected) {
  if (slot->inspector_selection.kind == kInspectorPresentation_Base)
    return false;
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = PresentHud_BuildChunks(slot, viewport, chunks);
  for (int i = count - 1; i >= 0; i--) {
    const ArRenderRectI source = chunks[i].screen_source;
    if (chunks[i].inspector_kind != slot->inspector_selection.kind ||
        slot->inspector_selection.source_x < source.x ||
        slot->inspector_selection.source_x >= source.x + source.w ||
        slot->inspector_selection.source_y < source.y ||
        slot->inspector_selection.source_y >= source.y + source.h)
      continue;
    if (selected) *selected = chunks[i];
    return true;
  }
  return false;
}

void PresentSceneInspector_Draw(const FrameSlot *slot,
                                  ArRenderRectI viewport) {
  if (!slot->scene_inspector_enabled || !SceneInspector_HasSelection())
    return;
  int x = 0, y = 0;
  if (!SceneInspector_GetPoint(&x, &y)) return;
  HudPresentationChunk hud_chunk;
  bool hud_selection = FindSelectedHudChunk(slot, viewport, &hud_chunk);
  int projected_px = hud_selection
      ? HudSourceToOutputX(&hud_chunk, slot->inspector_selection.source_x)
      : InspectorScreenToOutputX(viewport, slot->inspector_selection.source_x, slot);
  int projected_py = hud_selection
      ? HudSourceToOutputY(&hud_chunk, slot->inspector_selection.source_y)
      : InspectorScreenToOutputY(viewport, slot->inspector_selection.source_y, slot);
  int output_width = 0, output_height = 0;
  (void)ArRenderDevice_GetOutputSize(
      &g_render_device, &output_width, &output_height);
  bool same_output = output_width == slot->inspector_selection.output_width &&
                     output_height == slot->inspector_selection.output_height;
  int px = same_output ? slot->inspector_selection.output_x : projected_px;
  int py = same_output ? slot->inspector_selection.output_y : projected_py;
  int anchor_dx = px - projected_px;
  int anchor_dy = py - projected_py;

  const ArRenderColorF gold = {
    1.0f, 192.0f / 255.0f, 32.0f / 255.0f, 1.0f,
  };
  /* Crosshair arms scale with the output (7 SNES pixels' worth at the
   * current viewport scale, min the historical 7px) — a fixed 7 output
   * pixels is near-invisible at 4K/high-density output. */
  enum { kInspectorCrosshairMinimumArmPixels = 7 };
  int arm = viewport.h > 0
      ? (viewport.h * kInspectorCrosshairMinimumArmPixels +
         kFrameSlotAuthenticHeight / 2) /
          kFrameSlotAuthenticHeight
      : kInspectorCrosshairMinimumArmPixels;
  if (arm < kInspectorCrosshairMinimumArmPixels)
    arm = kInspectorCrosshairMinimumArmPixels;
  (void)ArRenderDevice_DrawLine(
      &g_render_device, (ArRenderPointF){(float)(px - arm), (float)py},
      (ArRenderPointF){(float)(px + arm), (float)py},
      1.0f, gold, kArRenderBlendMode_Alpha);
  (void)ArRenderDevice_DrawLine(
      &g_render_device, (ArRenderPointF){(float)px, (float)(py - arm)},
      (ArRenderPointF){(float)px, (float)(py + arm)},
      1.0f, gold, kArRenderBlendMode_Alpha);

  int x0, y0, x1, y1;
  if (SceneInspector_GetHighlight(&x0, &y0, &x1, &y1)) {
    ArRenderRectI rect;
    bool have_rect = hud_selection &&
        HudHighlightToOutput(&hud_chunk, x0, y0, x1, y1, &rect);
    if (!hud_selection) {
      rect = (ArRenderRectI){
        InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y0, slot),
        InspectorScreenToOutputX(viewport, x1, slot) -
            InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y1, slot) -
            InspectorScreenToOutputY(viewport, y0, slot),
      };
      have_rect = rect.w > 0 && rect.h > 0;
    }
    if (have_rect) {
      rect.x += anchor_dx;
      rect.y += anchor_dy;
      const float x0f = (float)rect.x;
      const float y0f = (float)rect.y;
      const float x1f = (float)(rect.x + rect.w);
      const float y1f = (float)(rect.y + rect.h);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0f, y0f},
          (ArRenderPointF){x1f, y0f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x1f, y0f},
          (ArRenderPointF){x1f, y1f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x1f, y1f},
          (ArRenderPointF){x0f, y1f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0f, y1f},
          (ArRenderPointF){x0f, y0f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
    }
  }
  SettingsOverlay_RenderDebugPanel(
      "SCENE INSPECTOR", SceneInspector_PanelText(),
      (ArRenderPointI){ px, py });
}
