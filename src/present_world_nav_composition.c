#include "present_world_nav_composition.h"
#include "present_sim3d_internal.h"
#include "presentation_upload_mirror.h"
#include "sim/sim_world_navigation_capture.h"
#include <stdio.h>

extern ArRenderDevice g_render_device;
static WorldNavigationCompositionTextures s_textures;
static PresentationUploadMirror s_palace_mirror, s_label_mirror, s_plaque_mirror;

const WorldNavigationCompositionTextures *WorldNavigationComposition_Get(void) {
  return &s_textures;
}

static ArRenderTexture EnsureWorldNavigationCompositionTexture(ArRenderTexture *texture) {
  if (ArRenderTexture_IsValid(*texture)) return *texture;
  const ArRenderTextureDesc desc = {
      .width = kSimWorldNavigationCompositionWidth,
      .height = kSimWorldNavigationCompositionHeight,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(&g_render_device, &desc, texture)) {
    fprintf(stderr, "[world-navigation] composition texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return ArRenderTexture_Invalid();
  }
  return *texture;
}

static bool UploadWorldNavigationLayer(PresentationUploadMirror *mirror, ArRenderTexture texture,
                                       const SimWorldNavigationCompositionLayer *layer,
                                       const uint32_t *pixels) {
  return !layer->visible ||
         PresentationUploadMirror_UploadArgb8888(
             mirror, &g_render_device, texture, (const uint8_t *)pixels, layer->width,
             layer->height, kSimWorldNavigationCompositionPitch, 0, 0, NULL);
}

void UploadWorldNavigationComposition(const FrameSlot *slot) {
  s_textures.uploaded = false;
  if (!slot || slot->sim.view != kSimView_WorldNavigation) return;
  const SimWorldNavigationComposition *composition = &slot->sim.world_navigation_scene.composition;
  if (!composition->valid) return;
  if (composition->empty_animation) {
    s_textures.uploaded = true;
    return;
  }

  ArRenderTexture palace = EnsureWorldNavigationCompositionTexture(&s_textures.palace);
  ArRenderTexture plaque = EnsureWorldNavigationCompositionTexture(&s_textures.plaque);
  ArRenderTexture label = composition->label.visible
                              ? EnsureWorldNavigationCompositionTexture(&s_textures.label)
                              : ArRenderTexture_Invalid();
  if (!ArRenderTexture_IsValid(palace) || !ArRenderTexture_IsValid(plaque) ||
      (composition->label.visible && !ArRenderTexture_IsValid(label)))
    return;
  if (!UploadWorldNavigationLayer(&s_palace_mirror, palace, &composition->palace,
                                  g_sim_world_navigation_palace_pixels) ||
      !UploadWorldNavigationLayer(&s_plaque_mirror, plaque, &composition->plaque,
                                  g_sim_world_navigation_plaque_pixels) ||
      !UploadWorldNavigationLayer(&s_label_mirror, label, &composition->label,
                                  g_sim_world_navigation_label_pixels))
    return;
  s_textures.uploaded = true;
}

ArRenderPointF WorldNavigationComposition_ProjectPoint(const FrameSlot *slot,
                                                       ArRenderRectI viewport, float authentic_x,
                                                       float authentic_y) {
  const float authentic_x0 =
      ((float)slot->snes_width - (float)kSimWorldNavigationCompositionWidth) * 0.5f;
  const float captured_x = authentic_x0 + authentic_x;
  return (ArRenderPointF){
      (float)viewport.x +
          (captured_x - (float)slot->visible_x0) * (float)viewport.w / (float)slot->visible_width,
      (float)viewport.y + authentic_y * (float)viewport.h / (float)slot->snes_height,
  };
}

bool WorldNavigationComposition_DrawLayer(const FrameSlot *slot, ArRenderRectI viewport,
                                          const SimWorldNavigationCompositionLayer *layer,
                                          ArRenderTexture texture, ArRenderPointF offset,
                                          float scale) {
  if (!layer || !layer->visible) return true;
  if (!ArRenderTexture_IsValid(texture) || !layer->width || !layer->height) return false;
  const ArRenderPointF top_left =
      WorldNavigationComposition_ProjectPoint(slot, viewport, layer->screen_x, layer->screen_y);
  const ArRenderPointF bottom_right = WorldNavigationComposition_ProjectPoint(
      slot, viewport, layer->screen_x + layer->width, layer->screen_y + layer->height);
  ArRenderRectF source = {0.0f, 0.0f, layer->width, layer->height};
  ArRenderRectF destination = {
      top_left.x + offset.x,
      top_left.y + offset.y,
      bottom_right.x - top_left.x,
      bottom_right.y - top_left.y,
  };
  if (scale != 1.0f) {
    /* Scale about the native travel focus, including the authored offset of
     * the cloud/platform, not about a potentially asymmetric raster crop. */
    destination.x = viewport.w * .5f + (top_left.x - viewport.w * .5f) * scale + offset.x;
    destination.y = viewport.h * .5f + (top_left.y - viewport.h * .5f) * scale + offset.y;
    destination.w *= scale;
    destination.h *= scale;
  }
  return ArRenderDevice_DrawTexture(&g_render_device, texture, &source, &destination);
}

void WorldNavigationComposition_Reset(void) {
  ArRenderDevice_DestroyTexture(&g_render_device, s_textures.palace);
  s_textures.palace = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, s_textures.label);
  s_textures.label = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, s_textures.plaque);
  s_textures.plaque = ArRenderTexture_Invalid();
  s_textures.uploaded = false;
  PresentationUploadMirror_Reset(&s_palace_mirror);
  PresentationUploadMirror_Reset(&s_plaque_mirror);
  PresentationUploadMirror_Reset(&s_label_mirror);
}
