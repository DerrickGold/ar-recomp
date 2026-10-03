#include "present/presentation_textures.h"

#include "sim/sim3d/sim3d_textures.h"
#include "diorama/present_diorama.h"
#include "host/host_display.h"
#include "host/host_video.h"
#include "render/render_device.h"
#include "render/present_hud.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/runner.h"

ArRenderTexture g_texture;
ArRenderTexture g_authentic_texture;

void PresentationTextures_Create(void) {
  const ArRenderTextureDesc base_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = SR_PPU_SURFACE_MAX_HEIGHT,
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

  if (!PresentHud_CreateSources(&g_render_device, g_snes_height))
    Die(ArRenderDevice_LastError(&g_render_device));

  Sim3DTextures_Create(&g_render_device);

  PresentDiorama_CreatePlanes(&g_render_device);
}

void PresentationTextures_HandleDeviceReset(void) {
  PresentDiorama_DestroyPlanes(&g_render_device);
  PresentDiorama_CreatePlanes(&g_render_device);
}

void PresentationTextures_Destroy(void) {
  PresentDiorama_DestroyPlanes(&g_render_device);
  Sim3DTextures_Destroy(&g_render_device);
  PresentHud_DestroySources(&g_render_device);
  ArRenderDevice_DestroyTexture(&g_render_device, g_authentic_texture);
  g_authentic_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, g_texture);
  g_texture = ArRenderTexture_Invalid();
}
