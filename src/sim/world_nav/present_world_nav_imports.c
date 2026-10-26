#include "sim/world_nav/present_world_nav_imports.h"
#include "sim/world_nav/present_world_nav_internal.h"
#include "actraiser2/ar2_content.h"
#include "settings_overlay/settings_overlay_render.h"
#include <stdlib.h>
#include <stdio.h>

static bool s_uploaded;
static ArRenderTexture s_flat;

bool WorldNavigationImports_Normal(const WorldNavigationProjection *projection, float x, float y,
                           float normal[3]) {
  if (!SimWorldNavigationGlobe_SampleAtRadius(projection->chart_radius_tiles, x / 8, y / 8, normal,
                                              NULL))
    return false;
  normal[0] = -normal[0];
  normal[2] = -normal[2];
  SimWorldNavigationGlobe_TransformNormal(&projection->globe_frame, normal, normal);
  return true;
}

bool WorldNavigationImports_Draw(const FrameSlot *slot, ArRenderRectI viewport,
                                 const WorldNavigationProjection *projection) {
  const Ar2WorldAtlas *atlas = Ar2Content_Atlas();
  if (!atlas) return true;
  if (!s_uploaded) {
    uint32_t *pixels = malloc(sizeof(atlas->pixels));
    if (!pixels) return false;
    for (size_t i = 0; i < sizeof(atlas->pixels) / sizeof(*pixels); i++)
      pixels[i] = atlas->indices[i] == 0x80 ? 0 : atlas->pixels[i];
    const ArRenderRectI all = {0, 0, 1024, 1024};
    s_uploaded = Sim3DDepthPass_UploadAtlasRegions(&g_render_device, kSim3DDepthPass_ImportedGround,
                                                   pixels, 1024, 1024, 1024 * 4, &all, 1);
    free(pixels);
    if (!s_uploaded) return false;
  }
  /* A 64x64 tessellation follows the globe; texture texels retain their
   * native resolution. Shared ocean depth hides the far hemisphere. */
  const int dx[4] = {0, 16, 16, 0}, dy[4] = {0, 0, 16, 16};
  const float azimuth = slot->sim.light_azimuth_deg * (3.14159265f / 180);
  const float elevation = slot->sim.light_elevation_deg * (3.14159265f / 180);
  const float light[3] = {-cosf(azimuth) * cosf(elevation), -sinf(azimuth) * cosf(elevation),
                          sinf(elevation)};
  for (int y = 0; y < 1024; y += 16) {
    for (int x = 0; x < 1024; x += 16) {
      Sim3DDepthVertex vertices[4];
      Scene3DClipPoint clips[4];
      bool valid = true;
      for (int i = 0; i < 4; i++) {
        float normal[3], world[3], depth;
        Scene3DPoint screen;
        if (!WorldNavigationImports_Normal(projection, x + dx[i], y + dy[i], normal)) {
          valid = false;
          break;
        }
        WorldNavigationRadialPoint(
            projection, normal,
            .006f - projection->reference_height_units * projection->height_world_per_unit, world);
        if (!WorldNavigationProjectPoint(projection, viewport, world, &screen, &depth, &clips[i])) {
          valid = false;
          break;
        }
        const float diffuse = fmaxf(0,
            normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]);
        const float shade = slot->sim.world_navigation_lighting
            ? kWorldNavigationTerrainAmbient + (1 - kWorldNavigationTerrainAmbient) * diffuse : 1;
        vertices[i] = (Sim3DDepthVertex){screen.x,
                                         screen.y,
                                         depth,
                                         {shade, shade, shade, 1},
                                         {(x + dx[i]) / 1024.f, (y + dy[i]) / 1024.f}};
      }
      if (valid &&
          !WorldNavigationAppendProjectedQuad(kSim3DDepthPass_ImportedGround, vertices,
                                              projection->clip_frustum ? clips : NULL, viewport))
        return false;
    }
  }
  return true;
}

bool WorldNavigationImports_DrawFlat(const FrameSlot *slot, ArRenderRectI viewport) {
  const Ar2WorldAtlas *atlas = Ar2Content_Atlas();
  if (!atlas) return false;
  if (!ArRenderTexture_IsValid(s_flat)) {
    const ArRenderTextureDesc desc = {1024, 1024, kArRenderPixelFormat_Argb8888,
        kArRenderTextureUsage_Static, kArRenderFilter_Nearest, kArRenderBlendMode_Opaque};
    if (!ArRenderDevice_CreateTexture(&g_render_device, &desc, &s_flat) ||
        !ArRenderDevice_UpdateTexture(&g_render_device, s_flat, NULL, atlas->pixels, 1024 * 4))
      return false;
  }
  ArRenderVertex2D vertices[4];
  const float brightness = slot->sim.world_navigation_brightness / 15.f;
  const uint32_t ocean = atlas->pixels[0];
  const ArRenderRectF area = {0, 0, viewport.w, viewport.h};
  if (!ArRenderDevice_DrawSolidRect(&g_render_device, &area,
          (ArRenderColorF){((ocean >> 16) & 255) / 255.f * brightness,
                          ((ocean >> 8) & 255) / 255.f * brightness,
                          (ocean & 255) / 255.f * brightness, 1},
          kArRenderBlendMode_Opaque)) return false;
  const int x[] = {0, 1024, 1024, 0}, y[] = {0, 0, 1024, 1024};
  for (int i = 0; i < 4; i++) {
    ArRenderPointF point;
    if (!WorldNavigationFlatOutputPoint(slot, viewport, x[i], y[i], &point)) return false;
    vertices[i] = (ArRenderVertex2D){point, {brightness, brightness, brightness, 1},
        {x[i] / 1024.f, y[i] / 1024.f}};
  }
  const int indices[] = {0, 1, 2, 0, 2, 3};
  return ArRenderDevice_DrawGeometry(&g_render_device, s_flat, vertices, 4, indices, 6);
}

void WorldNavigationImports_DrawLabel(const FrameSlot *slot, ArRenderRectI viewport) {
  const Ar2WorldAtlas *atlas = Ar2Content_Atlas();
  const int index = slot->sim.world_travel.destination;
  const char *label = "ActRaiser 2";
  char unavailable[64];
  if (atlas && index >= 0 && (size_t)index < atlas->destination_count) {
    const Ar2WorldDestination *destination = &atlas->destinations[index];
    label = destination->label;
    if (!Ar2Content_Playable(destination)) {
      snprintf(unavailable, sizeof(unavailable), "%s (unavailable)", label);
      label = unavailable;
    }
  }
  const ArRenderRectF bounds = FrameSlot_ProjectNativeRect(
      slot, viewport, (ArRenderRectF){kSimWorldNavigationLabelX, kSimWorldNavigationLabelY,
          kSimWorldNavigationLabelWidth, kSimWorldNavigationLabelHeight});
  const ArRenderRectI rect = {bounds.x, bounds.y, bounds.w, bounds.h};
  const uint8_t alpha = slot->sim.world_navigation_brightness * 17;
  if (!SettingsOverlay_DrawEnhancedLabel(rect, alpha, label)) {
    const int scale = fmaxf(1, floorf(viewport.h / 224.f));
    const int width = SettingsOverlay_GameTextWidth(label, scale);
    SettingsOverlay_DrawGameText(rect.x + (rect.w - width) / 2, rect.y, scale, alpha, label);
  }
}

void WorldNavigationImports_Reset(void) {
  s_uploaded = false;
  ArRenderDevice_DestroyTexture(&g_render_device, s_flat);
  s_flat = ArRenderTexture_Invalid();
}
