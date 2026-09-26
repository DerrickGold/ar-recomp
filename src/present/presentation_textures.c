#include "present/presentation_textures.h"

#include <stdio.h>
#include <stdlib.h>

#include "app/settings.h"
#include "app/session_fatal.h"
#include "host/host_display.h"
#include "host/host_video.h"
#include "render/render_device.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_render_metadata.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/runner.h"

ArRenderTexture g_texture;
ArRenderTexture g_authentic_texture;
ArRenderTexture g_hud_bg_texture;
ArRenderTexture g_hud_obj_texture;
ArRenderTexture g_diorama_textures[kDioramaPlane_Count];
ArRenderTexture g_sim_obj_atlas_texture;
ArRenderTexture g_sim3d_layer_textures[kSim3DPlane_Count];
ArRenderTexture g_sim3d_flat_texture;
bool g_sim3d_textures_ready;
bool g_sim3d_billboard_renderer_ready;

static void DestroyDioramaTextures(void) {
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(&g_render_device, g_diorama_textures[i]);
    g_diorama_textures[i] = ArRenderTexture_Invalid();
  }
}

static void CreateDioramaTextures(void) {
  /* Allocated at the PPU's full render-target size on BOTH axes, for the same
   * reason: the ABI surface limits already cover every horizontal and vertical
   * margin without a realloc. Only the leading snes_width x
   * (snes_height + ws_extra_top + ws_extra_bottom) region is uploaded
   * each frame; Diorama_Composite's UV window is expressed against these
   * allocated dimensions. */
  uint8_t *zero_fill =
      calloc(1, (size_t)SR_PPU_SURFACE_MAX_WIDTH *
                    SR_PPU_SURFACE_MAX_HEIGHT * 4);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    if (i == SR_PPU_OVERLAY_BG4)
      continue;
    const ArRenderTextureDesc desc = {
      .width = SR_PPU_SURFACE_MAX_WIDTH,
      .height = SR_PPU_SURFACE_MAX_HEIGHT,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = i == kDioramaPlane_Backdrop
          ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            &g_render_device, &desc, &g_diorama_textures[i]))
      continue;
    if (zero_fill)
      ArRenderDevice_UpdateTexture(
          &g_render_device, g_diorama_textures[i], NULL, zero_fill,
          SR_PPU_SURFACE_MAX_WIDTH * 4);
  }
  free(zero_fill);
}

void PresentationTextures_Create(void) {
  const ArRenderTextureDesc base_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = g_snes_height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &base_texture, &g_texture))
    Die(ArRenderDevice_LastError(&g_render_device));
  /* The base framebuffer is opaque: the PPU writes RGB with the alpha byte
   * left 0 (see ppu_old.c). SDL2 defaulted new textures to BLENDMODE_NONE so
   * that alpha was ignored, but SDL3 defaults them to BLENDMODE_BLEND — which
   * would blend those alpha-0 pixels to fully transparent and present a BLACK
   * screen. The descriptor's opaque blend mode preserves that behavior. (The
   * HUD/overlay textures below deliberately use alpha; they carry real alpha.) */
  /* SDL3 textures default to linear filtering; the SDL2 build set the global
   * SDL_HINT_RENDER_SCALE_QUALITY=0 (nearest). The descriptor pins nearest
   * filtering so the pixel-art framebuffer upscales crisply. */

  const ArRenderTextureDesc authentic_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = SR_PPU_SURFACE_MAX_HEIGHT,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &authentic_texture, &g_authentic_texture))
    Die(ArRenderDevice_LastError(&g_render_device));

  const ArRenderTextureDesc hud_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = g_snes_height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &hud_texture, &g_hud_bg_texture) ||
      !ArRenderDevice_CreateTexture(
          &g_render_device, &hud_texture, &g_hud_obj_texture))
    Die(ArRenderDevice_LastError(&g_render_device));

  /* D1b semantic OBJ atlas. It is uploaded every supported SIM frame but is
   * not selected by the compositor until the later separated-composite
   * capability lands, keeping this checkpoint visually authentic. */
  const ArRenderTextureDesc sim_atlas_texture = {
    .width = kSimObjAtlasWidth,
    .height = kSimObjAtlasHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (ArRenderDevice_CreateTexture(
          &g_render_device, &sim_atlas_texture,
          &g_sim_obj_atlas_texture)) {
    /* Static storage is zero-initialized before the game thread starts. */
    ArRenderDevice_UpdateTexture(
        &g_render_device, g_sim_obj_atlas_texture, NULL,
        g_sim_obj_atlas_pixels, kSimObjAtlasPitch);
  } else {
    fprintf(stderr,
            "[sim3d-d1] semantic atlas texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
  }
  g_sim3d_billboard_renderer_ready =
      ArRenderTexture_IsValid(g_sim_obj_atlas_texture);

  /* D2's observational Mode-1 capture family. Layer textures are retained
   * for inspector/future geometry use; the pitch-zero reference and its
   * absolute-difference image have dedicated opaque streaming textures. */
  g_sim3d_textures_ready = true;
  const ArRenderTextureDesc sim_layer_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!ArRenderDevice_CreateTexture(
            &g_render_device, &sim_layer_texture,
            &g_sim3d_layer_textures[plane])) {
      g_sim3d_textures_ready = false;
      break;
    }
  }
  const ArRenderTextureDesc sim_flat_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &sim_flat_texture, &g_sim3d_flat_texture))
    g_sim3d_textures_ready = false;
  if (!g_sim3d_textures_ready) {
    fprintf(stderr,
            "[sim3d-d2] capture textures unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      ArRenderDevice_DestroyTexture(
          &g_render_device, g_sim3d_layer_textures[plane]);
      g_sim3d_layer_textures[plane] = ArRenderTexture_Invalid();
    }
    ArRenderDevice_DestroyTexture(&g_render_device, g_sim3d_flat_texture);
    g_sim3d_flat_texture = ArRenderTexture_Invalid();
  }
  if (g_settings.sim3d_mode && !g_sim3d_textures_ready) {
    Die("Simulation town 3D is enabled, but its core capture textures could "
        "not be created. Restart after checking graphics memory and driver "
        "stability, or disable Simulation town 3D in settings.ini.");
  }
  if (g_settings.sim3d_mode &&
      (Settings_Sim3DRequestedFeatures() & kSimFeature_ObjectBillboards) &&
      !g_sim3d_billboard_renderer_ready) {
    Die("Simulation object billboards are enabled, but their renderer atlas "
        "could not be created. Restart after checking graphics memory and "
        "driver stability, or disable object billboards in settings.ini.");
  }

  /* One streaming texture per diorama plane (priority bands included).
   * Only the backdrop is opaque — every other plane alpha-blends. */
  /* Live report (2026-07-21): a persistent pink/garbage-colored line at
   * the diorama's right edge, root-caused across two failed attempts (the
   * B1b-crisp supersample copy, then suspected in the DOF/edge-AA shader)
   * before landing on the actual source: every consumer that ever samples
   * near the true edge of what Diorama_Upload writes (u=uv_u1 =
   * snes_width/SR_PPU_SURFACE_MAX_WIDTH, always < 1.0 — the buffer is
   * allocated at the PPU's max width but a layer's real content is narrower,
   * capped by kActRaiserWidescreenExtraMax's tilemap-ring streaming limit)
   * can reach into
   * columns snes_width..SR_PPU_SURFACE_MAX_WIDTH-1, which Diorama_Upload's
   * SDL_UpdateTexture never touches. SDL_TEXTUREACCESS_STREAMING content
   * is undefined until written (no zero guarantee, confirmed non-zero in
   * practice on this backend), so that tail is genuine garbage, not just
   * theoretically risky — and every fix so far (B1b's UV-window clamp,
   * B1b-crisp's valid-subrect blit, the skybox blur's UV inset) was
   * patching ONE consumer at a time as each was discovered, while the DOF/
   * edge-AA shader's own unclamped blur sampling proved there would always
   * be another. Fix it once at the SOURCE instead: zero-fill each
   * texture's FULL extent immediately after creation, before any real
   * frame ever writes into it. Diorama_Upload only ever touches the valid
   * {0,0,snes_width,snes_height} sub-rect afterward, so the margin stays
   * deterministically transparent black (not garbage) for the texture's
   * entire lifetime — every current and future consumer is safe without
   * needing its own clamp/inset workaround. */
  CreateDioramaTextures();
}

void PresentationTextures_HandleDeviceReset(void) {
  DestroyDioramaTextures();
  CreateDioramaTextures();
}

void PresentationTextures_Destroy(void) {
  DestroyDioramaTextures();
  ArRenderDevice_DestroyTexture(&g_render_device, g_sim_obj_atlas_texture);
  g_sim_obj_atlas_texture = ArRenderTexture_Invalid();
  g_sim3d_billboard_renderer_ready = false;
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    ArRenderDevice_DestroyTexture(
        &g_render_device, g_sim3d_layer_textures[plane]);
    g_sim3d_layer_textures[plane] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(&g_render_device, g_sim3d_flat_texture);
  g_sim3d_flat_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, g_hud_obj_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, g_hud_bg_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, g_authentic_texture);
  g_hud_obj_texture = ArRenderTexture_Invalid();
  g_hud_bg_texture = ArRenderTexture_Invalid();
  g_authentic_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, g_texture);
  g_texture = ArRenderTexture_Invalid();
}

bool PresentationTextures_ValidateSetting(const SettingDesc *desc) {
  if (g_settings.sim3d_mode &&
      (desc->field == &g_settings.sim3d_mode ||
       desc->field == &g_settings.sim3d_object_billboards)) {
    if (!g_sim3d_textures_ready) {
      SessionFatal_Request(
          "Simulation town 3D was selected, but its core renderer textures "
          "are unavailable. Restart after checking graphics memory and driver "
          "stability, or leave Simulation town 3D disabled.");
      return false;
    } else if (g_settings.sim3d_object_billboards &&
               !g_sim3d_billboard_renderer_ready) {
      SessionFatal_Request(
          "Simulation object billboards were selected, but their renderer "
          "atlas is unavailable. Restart after checking graphics memory and "
          "driver stability, or leave object billboards disabled.");
      return false;
    }
  }
  return true;
}
