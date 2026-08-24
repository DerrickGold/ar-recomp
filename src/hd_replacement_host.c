#include "action/action_obj_apron.h"
#include "hd_replacement_host.h"

#include "actraiser_game.h"   /* kActRaiserAuthenticHeight */

#include <stdio.h>
#include <stdlib.h>

#include "actraiser_rtl.h"
#include "hd_replacements.h"
#include "host/host_display.h"
#include "settings.h"
#include "snes/ppu.h"

/* HD art substitution is PNG-only and decoded once when textures are loaded.
 *
 * This TU owns STB_IMAGE_IMPLEMENTATION for the WHOLE binary, so the format
 * allowlist below is the binary's, not this file's. JPEG is here for the in-game
 * manual (src/manual_reader.c), whose scanned pages are baseline JPEG -- the
 * STBI_ONLY_* macros are a positive allowlist, so that is one line rather than a
 * second implementation and a duplicate-symbol link error. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

enum {
  kRgbaChannelCount = 4,
  kArgbBytesPerPixel = 4,
};

extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern int g_snes_width;
extern int g_snes_height;
extern uint8_t g_pixels[
    kPpuSurfaceWidth * kArgbBytesPerPixel * kHostDisplayFramebufferHeight];
extern uint8_t g_authentic_pixels[
    kPpuSurfaceWidth * kArgbBytesPerPixel * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_bg_pixels[
    kPpuSurfaceWidth * kArgbBytesPerPixel * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_obj_pixels[
    kPpuSurfaceWidth * kArgbBytesPerPixel * kHostDisplayFramebufferHeight];
extern bool g_ws_active;
extern int g_ws_extra;
extern Ppu *g_ppu;

/* Authentic pixels captured for a replacement are never presented. These
 * bindings exist because RemoveFromGame only engages for a bound source;
 * BG3 and OBJ reuse the dedicated HUD surfaces. */
static uint8_t *s_overlay_pixels[kPpuOverlaySource_Count];
static bool s_authentic_capture_enabled;
static uint64_t s_authentic_frame_serial;
static uint64_t s_authentic_next_frame_serial;

uint8_t *g_m7_overlay_pixels;
SDL_Texture *g_m7_texture;

void HdReplacementHost_LoadTextures(void) {
  Settings_SetHdReplacementsAvailable(false);
  const char *manifest_path = getenv("AR_HD_MANIFEST");
  if (!manifest_path || !manifest_path[0])
    manifest_path = "game-assets/manifest.ini";
  if (!HdReplacements_Load(manifest_path)) return;

  int loaded_art_count = 0;
  for (int i = 0; i < g_hd_replacement_count; i++) {
    HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Tiles) continue;

    /* Missing images are the normal "hook available, art not provided"
     * state. A present file that cannot decode is a real error. */
    FILE *probe = fopen(entry->image, "rb");
    if (!probe) continue;
    fclose(probe);

    int width = 0;
    int height = 0;
    int source_channel_count = 0;
    stbi_uc *rgba = stbi_load(
        entry->image, &width, &height, &source_channel_count,
        kRgbaChannelCount);
    if (!rgba) {
      fprintf(stderr, "[hd-manifest] [replace:%s] cannot decode %s (%s)\n",
              entry->name, entry->image, stbi_failure_reason());
      continue;
    }

    if (entry->plane == kHdPlane_Mode7) {
      /* The engine sampler consumes raw ARGB words, not an SDL texture. */
      uint32_t *argb = malloc(
          (size_t)width * (size_t)height * sizeof(*argb));
      if (argb) {
        for (size_t pixel = 0;
             pixel < (size_t)width * (size_t)height;
             pixel++) {
          const stbi_uc *source = rgba + pixel * kRgbaChannelCount;
          argb[pixel] = (uint32_t)source[3] << 24 |
                        (uint32_t)source[0] << 16 |
                        (uint32_t)source[1] << 8 |
                        source[2];
        }
        entry->pixels = argb;
        entry->pixels_width = width;
        entry->pixels_height = height;
        loaded_art_count++;
        fprintf(stderr, "[hd-manifest] [replace:%s] %s (%dx%d, mode7)\n",
                entry->name, entry->image, width, height);
      }
      stbi_image_free(rgba);
      continue;
    }

    /* ABGR8888 matches stb's little-endian R,G,B,A byte order directly. */
    SDL_Texture *texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
        width, height);
    if (texture && SDL_UpdateTexture(
            texture, NULL, rgba, width * kArgbBytesPerPixel)) {
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
      entry->texture = texture;
      loaded_art_count++;
      fprintf(stderr, "[hd-manifest] [replace:%s] %s (%dx%d)\n",
              entry->name, entry->image, width, height);
    } else {
      if (texture) SDL_DestroyTexture(texture);
      fprintf(stderr, "[hd-manifest] [replace:%s] texture upload failed: %s\n",
              entry->name, SDL_GetError());
    }
    stbi_image_free(rgba);
  }
  fprintf(stderr, "[hd-manifest] %d entries, %d with art\n",
          g_hd_replacement_count, loaded_art_count);
  Settings_SetHdReplacementsAvailable(loaded_art_count > 0);
}

void HdReplacementHost_BindSurfaces(void) {
  for (int i = 0; i < g_hd_replacement_count; i++) {
    const HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Mode7 && entry->pixels &&
        !g_m7_overlay_pixels && g_renderer) {
      const size_t capacity_pitch =
          (size_t)kPpuSurfaceWidth * kHdMode7Scale * kArgbBytesPerPixel;
      const size_t active_pitch =
          (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel;
      g_m7_overlay_pixels = calloc(
          1, capacity_pitch * kActRaiserAuthenticHeight * kHdMode7Scale);
      g_m7_texture = SDL_CreateTexture(
          g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          kPpuSurfaceWidth * kHdMode7Scale,
          g_snes_height * kHdMode7Scale);
      if (g_m7_overlay_pixels && g_m7_texture) {
        SDL_SetTextureBlendMode(g_m7_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_m7_texture, SDL_SCALEMODE_NEAREST);
        PpuBindMode7OverlaySurface(
            g_ppu, g_m7_overlay_pixels, active_pitch, kHdMode7Scale);
      } else {
        SDL_DestroyTexture(g_m7_texture);
        g_m7_texture = NULL;
        free(g_m7_overlay_pixels);
        g_m7_overlay_pixels = NULL;
        fprintf(stderr,
                "[hd-manifest] mode7 host-surface allocation failed: %s\n",
                SDL_GetError());
      }
      continue;
    }
    if (entry->plane != kHdPlane_Screen || !entry->texture) continue;

    const int source = entry->source;
    if (source < 0 || source >= kPpuOverlaySource_Count) continue;
    if (source == kPpuOverlaySource_Bg3 ||
        source == kPpuOverlaySource_Obj ||
        s_overlay_pixels[source])
      continue;
    s_overlay_pixels[source] = calloc(
        1, (size_t)kPpuSurfaceWidth * kArgbBytesPerPixel *
            kHostDisplayFramebufferHeight);
    if (s_overlay_pixels[source])
      PpuBindOverlaySurface(
          g_ppu, (PpuOverlaySource)source, s_overlay_pixels[source],
          (size_t)g_snes_width * kArgbBytesPerPixel);
  }
}

void HdReplacementHost_ReloadTextures(void) {
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (g_hd_replacements[i].texture) {
      SDL_DestroyTexture((SDL_Texture *)g_hd_replacements[i].texture);
      g_hd_replacements[i].texture = NULL;
    }
    free(g_hd_replacements[i].pixels);
    g_hd_replacements[i].pixels = NULL;
  }
  HdReplacementHost_LoadTextures();
  HdReplacementHost_BindSurfaces();
}

void ActRaiser_SetAuthenticCaptureEnabled(bool enabled) {
  if (s_authentic_capture_enabled == enabled &&
      (!g_ppu || PpuAuthenticSurfaceBound(g_ppu) == enabled))
    return;
  s_authentic_capture_enabled = enabled;
  s_authentic_frame_serial = 0;
  if (!g_ppu) return;
  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  if (!PpuBindAuthenticSurface(
          g_ppu, enabled ? g_authentic_pixels : NULL,
          enabled ? pitch : 0)) {
    s_authentic_capture_enabled = false;
    PpuBindAuthenticSurface(g_ppu, NULL, 0);
    fprintf(stderr,
            "[compare] authentic surface rejected for width %d\n",
            g_snes_width);
  }
}

bool ActRaiser_AuthenticCaptureEnabled(void) {
  return s_authentic_capture_enabled;
}

void ActRaiser_AuthenticCaptureFrameCompleted(bool frame_valid) {
  if (!frame_valid) {
    s_authentic_frame_serial = 0;
    return;
  }
  if (!s_authentic_capture_enabled || !g_ppu ||
      !PpuAuthenticSurfaceBound(g_ppu))
    return;
  s_authentic_next_frame_serial++;
  if (!s_authentic_next_frame_serial) s_authentic_next_frame_serial++;
  s_authentic_frame_serial = s_authentic_next_frame_serial;
}

uint64_t ActRaiser_AuthenticFrameSerial(void) {
  return s_authentic_frame_serial;
}

void ActRaiser_RebindPpuOutputSurfaces(void) {
  if (!g_ppu) return;

  /* The old pixels describe the old surface geometry until a complete pass
   * reaches the new binding. */
  s_authentic_frame_serial = 0;

  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  /* The main framebuffer is bound APRON-WIDE: it doubles as the diorama's
   * backdrop plane, and every other diorama plane is apron-wide, so a narrow
   * backdrop would composite offset from the layers by the apron. The
   * compositor centres the scanline span in it (PpuSurfaceApron), so screen
   * x = 0 lands at column apron + ws_extra. Readers of g_pixels therefore
   * offset by kPpuObjApron columns -- see present.c's flat upload. */
  const size_t frame_pitch =
      ActionApron_SurfacePitch(g_snes_width, kPpuObjApron);
  PpuBeginDrawing(g_ppu, g_pixels, frame_pitch, 0);
  /* Geometry may be contracting from a wider prior bind. Clear first so a
   * validation failure cannot leave the old stride attached to new pixels. */
  PpuBindAuthenticSurface(g_ppu, NULL, 0);
  PpuClearOverlayBindings(g_ppu);
  PpuBindOverlaySurface(
      g_ppu, kPpuOverlaySource_Bg3,
      g_hud_bg_texture ? g_hud_bg_pixels : NULL, pitch);
  PpuBindOverlaySurface(
      g_ppu, kPpuOverlaySource_Obj,
      g_hud_obj_texture ? g_hud_obj_pixels : NULL, pitch);
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    if (source == kPpuOverlaySource_Bg3 ||
        source == kPpuOverlaySource_Obj ||
        !s_overlay_pixels[source])
      continue;
    PpuBindOverlaySurface(
        g_ppu, (PpuOverlaySource)source, s_overlay_pixels[source], pitch);
  }
  if (g_m7_overlay_pixels)
    PpuBindMode7OverlaySurface(
        g_ppu, g_m7_overlay_pixels,
        (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel,
        kHdMode7Scale);
  if (g_ws_active)
    PpuSetExtraSpaceCentered(g_ppu, (uint8_t)g_ws_extra);
  else
    PpuSetExtraSpace(g_ppu, 0);
  if (s_authentic_capture_enabled && !PpuBindAuthenticSurface(
          g_ppu, g_authentic_pixels, pitch)) {
    s_authentic_capture_enabled = false;
    fprintf(stderr,
            "[compare] authentic surface rejected after rebind for width %d\n",
            g_snes_width);
  }
}

void HdReplacementHost_Shutdown(void) {
  Settings_SetHdReplacementsAvailable(false);
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (g_hd_replacements[i].texture)
      SDL_DestroyTexture((SDL_Texture *)g_hd_replacements[i].texture);
    g_hd_replacements[i].texture = NULL;
    free(g_hd_replacements[i].pixels);
    g_hd_replacements[i].pixels = NULL;
  }
  SDL_DestroyTexture(g_m7_texture);
  g_m7_texture = NULL;
  free(g_m7_overlay_pixels);
  g_m7_overlay_pixels = NULL;
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    free(s_overlay_pixels[source]);
    s_overlay_pixels[source] = NULL;
  }
  s_authentic_capture_enabled = false;
  s_authentic_frame_serial = 0;
  s_authentic_next_frame_serial = 0;
}
