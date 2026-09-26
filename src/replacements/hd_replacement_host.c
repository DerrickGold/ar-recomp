#include "snesrecomp/support/utf8_fs.h"

#include "action/action_obj_apron.h"
#include "replacements/hd_replacement_host.h"

#include "actraiser_game.h"   /* kActRaiserAuthenticHeight */

#include <stdio.h>
#include <stdlib.h>

#include "replacements/hd_replacements.h"
#include "host/host_display.h"
#include "render/render_device.h"
#include "snesrecomp/runner.h"
#include "app/settings.h"

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
#include "host/host_video.h"
#include "host/host_ppu_output.h"

enum {
  kRgbaChannelCount = 4,
  kArgbBytesPerPixel = 4,
};

/* Authentic pixels captured for a replacement are never presented. These
 * bindings exist because RemoveFromGame only engages for a bound source;
 * BG3 and OBJ reuse the dedicated HUD surfaces. */
static uint8_t *s_overlay_pixels[SR_PPU_OVERLAY_SOURCE_COUNT];
static uint8_t *s_m7_overlay_pixels;
static ArRenderTexture s_m7_texture;

ArRenderTexture HdReplacementHost_Mode7Texture(void) { return s_m7_texture; }

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
    FILE *probe = sr_fopen(entry->image, "rb");
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

    uint8_t *padded_rgba = NULL;
    if (!HdReplacements_PrepareTitleCoverage(
            entry, rgba, width, height, &padded_rgba, &width)) {
      fprintf(stderr, "[hd-manifest] [replace:%s] title coverage allocation failed\n",
              entry->name);
      stbi_image_free(rgba);
      continue;
    }
    const stbi_uc *image = padded_rgba ? padded_rgba : rgba;

    if (entry->plane == kHdPlane_Mode7) {
      /* The engine sampler consumes raw ARGB words, not an SDL texture. */
      uint32_t *argb = malloc(
          (size_t)width * (size_t)height * sizeof(*argb));
      if (argb) {
        for (size_t pixel = 0;
             pixel < (size_t)width * (size_t)height;
             pixel++) {
          const stbi_uc *source = image + pixel * kRgbaChannelCount;
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
      free(padded_rgba);
      stbi_image_free(rgba);
      continue;
    }

    /* ABGR8888 matches stb's little-endian R,G,B,A byte order directly. */
    const ArRenderTextureDesc desc = {
      .width = width,
      .height = height,
      .format = kArRenderPixelFormat_Abgr8888,
      .usage = kArRenderTextureUsage_Static,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha,
    };
    ArRenderTexture texture = ArRenderTexture_Invalid();
    if (ArRenderDevice_CreateTexture(&g_render_device, &desc, &texture) &&
        ArRenderDevice_UpdateTexture(
            &g_render_device, texture, NULL, image,
            width * kArgbBytesPerPixel)) {
      entry->texture = texture;
      loaded_art_count++;
      fprintf(stderr, "[hd-manifest] [replace:%s] %s (%dx%d)\n",
              entry->name, entry->image, width, height);
    } else {
      ArRenderDevice_DestroyTexture(&g_render_device, texture);
      fprintf(stderr, "[hd-manifest] [replace:%s] texture upload failed: %s\n",
              entry->name, ArRenderDevice_LastError(&g_render_device));
    }
    free(padded_rgba);
    stbi_image_free(rgba);
  }
  fprintf(stderr, "[hd-manifest] %d entries, %d with art\n",
          g_hd_replacement_count, loaded_art_count);
  Settings_SetHdReplacementsAvailable(loaded_art_count > 0);
}

void HdReplacementHost_BindSurfaces(void) {
  HostPpuOutputControl output;
  const bool output_available = HostPpuOutputControl_Begin(&output);
  for (int i = 0; i < g_hd_replacement_count; i++) {
    const HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Mode7 && entry->pixels &&
        !s_m7_overlay_pixels && ArRenderDevice_IsReady(&g_render_device)) {
      const size_t capacity_pitch =
          (size_t)SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale *
          kArgbBytesPerPixel;
      const size_t active_pitch =
          (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel;
      const size_t capacity_bytes =
          capacity_pitch * kActRaiserAuthenticHeight * kHdMode7Scale;
      s_m7_overlay_pixels = calloc(
          1, capacity_bytes);
      const ArRenderTextureDesc texture_desc = {
        .width = SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale,
        .height = g_snes_height * kHdMode7Scale,
        .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Nearest,
        .blend = kArRenderBlendMode_Alpha,
      };
      (void)ArRenderDevice_CreateTexture(
          &g_render_device, &texture_desc, &s_m7_texture);
      if (s_m7_overlay_pixels && ArRenderTexture_IsValid(s_m7_texture)) {
        if (output_available)
          (void)HostPpuOutputControl_Bind(
              &output, SR_PPU_OUTPUT_MODE7, 0u, 0u, kHdMode7Scale,
              s_m7_overlay_pixels, capacity_bytes, active_pitch,
              kActRaiserAuthenticHeight * kHdMode7Scale, 0u);
      } else {
        ArRenderDevice_DestroyTexture(&g_render_device, s_m7_texture);
        s_m7_texture = ArRenderTexture_Invalid();
        free(s_m7_overlay_pixels);
        s_m7_overlay_pixels = NULL;
        fprintf(stderr,
                "[hd-manifest] mode7 host-surface allocation failed: %s\n",
                ArRenderDevice_LastError(&g_render_device));
      }
      continue;
    }
    if (entry->plane != kHdPlane_Screen ||
        !ArRenderTexture_IsValid(entry->texture))
      continue;

    const int source = entry->source;
    if (source < 0 || source >= SR_PPU_OVERLAY_SOURCE_COUNT) continue;
    if (source == SR_PPU_OVERLAY_BG3 ||
        source == SR_PPU_OVERLAY_OBJ ||
        s_overlay_pixels[source])
      continue;
    s_overlay_pixels[source] = calloc(
        1, (size_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
            kHostDisplayFramebufferHeight);
    if (s_overlay_pixels[source] && output_available)
      (void)HostPpuOutputControl_Bind(
          &output, SR_PPU_OUTPUT_OVERLAY, (uint32_t)source, 0u, 0u,
          s_overlay_pixels[source],
          (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
              kHostDisplayFramebufferHeight,
          (size_t)g_snes_width * kArgbBytesPerPixel,
          kHostDisplayFramebufferHeight, 0u);
  }
}

void HdReplacementHost_RebindSurfaces(const HostPpuOutputControl *output) {
  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  for (int source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++) {
    if (source == SR_PPU_OVERLAY_BG3 ||
        source == SR_PPU_OVERLAY_OBJ ||
        !s_overlay_pixels[source])
      continue;
    (void)HostPpuOutputControl_Bind(
        output, SR_PPU_OUTPUT_OVERLAY, (uint32_t)source, 0u, 0u,
        s_overlay_pixels[source],
        (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
            kHostDisplayFramebufferHeight,
        pitch, kHostDisplayFramebufferHeight, 0u);
  }
  if (s_m7_overlay_pixels)
    (void)HostPpuOutputControl_Bind(
        output, SR_PPU_OUTPUT_MODE7, 0u, 0u, kHdMode7Scale,
        s_m7_overlay_pixels,
        (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale *
            kArgbBytesPerPixel * kActRaiserAuthenticHeight * kHdMode7Scale,
        (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel,
        kActRaiserAuthenticHeight * kHdMode7Scale, 0u);
}

void HdReplacementHost_ReloadTextures(void) {
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (ArRenderTexture_IsValid(g_hd_replacements[i].texture)) {
      ArRenderDevice_DestroyTexture(
          &g_render_device, g_hd_replacements[i].texture);
      g_hd_replacements[i].texture = ArRenderTexture_Invalid();
    }
    free(g_hd_replacements[i].pixels);
    g_hd_replacements[i].pixels = NULL;
  }
  HdReplacementHost_LoadTextures();
  HdReplacementHost_BindSurfaces();
}

void HdReplacementHost_Shutdown(void) {
  Settings_SetHdReplacementsAvailable(false);
  for (int i = 0; i < g_hd_replacement_count; i++) {
    ArRenderDevice_DestroyTexture(
        &g_render_device, g_hd_replacements[i].texture);
    g_hd_replacements[i].texture = ArRenderTexture_Invalid();
    free(g_hd_replacements[i].pixels);
    g_hd_replacements[i].pixels = NULL;
  }
  ArRenderDevice_DestroyTexture(&g_render_device, s_m7_texture);
  s_m7_texture = ArRenderTexture_Invalid();
  free(s_m7_overlay_pixels);
  s_m7_overlay_pixels = NULL;
  for (int source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++) {
    free(s_overlay_pixels[source]);
    s_overlay_pixels[source] = NULL;
  }
}
