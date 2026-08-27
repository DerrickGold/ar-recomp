#include "action/action_obj_apron.h"
#include "hd_replacement_host.h"

#include "actraiser_game.h"   /* kActRaiserAuthenticHeight */

#include <stdio.h>
#include <stdlib.h>

#include "actraiser_rtl.h"
#include "snesrecomp/game_runtime.h"
#include "hd_replacements.h"
#include "host/host_display.h"
#include "snesrecomp/runner.h"
#include "settings.h"

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
    SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
    kHostDisplayFramebufferHeight];
extern uint8_t g_authentic_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
    kHostDisplayFramebufferHeight];
extern uint8_t g_hud_bg_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
    kHostDisplayFramebufferHeight];
extern uint8_t g_hud_obj_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
    kHostDisplayFramebufferHeight];
extern bool g_ws_active;
extern int g_ws_extra;

/* Authentic pixels captured for a replacement are never presented. These
 * bindings exist because RemoveFromGame only engages for a bound source;
 * BG3 and OBJ reuse the dedicated HUD surfaces. */
static uint8_t *s_overlay_pixels[SR_PPU_OVERLAY_SOURCE_COUNT];
static bool s_authentic_capture_enabled;
static bool s_authentic_surface_bound;
static uint64_t s_authentic_frame_serial;
static uint64_t s_authentic_next_frame_serial;

uint8_t *g_m7_overlay_pixels;
SDL_Texture *g_m7_texture;

typedef struct PpuOutputControl {
  const SnesRunnerApi *api;
  SrRunnerHandle *runner;
  uint64_t lifetime_generation;
} PpuOutputControl;

static bool PpuOutputControl_Begin(PpuOutputControl *control) {
  if (!control || !RtlGameRunner()) return false;
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (!api || api->struct_size < SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE ||
      (api->capabilities & SR_RUNNER_CAP_PPU_OUTPUT_CONTROL) == 0u)
    return false;
  SrGenerationSnapshot generation = {
      .struct_size = sizeof(generation),
  };
  SrRunnerHandle *runner = RtlGameRunner();
  if (api->query_generations(runner, &generation) != SR_RESULT_OK)
    return false;
  control->api = api;
  control->runner = runner;
  control->lifetime_generation = generation.lifetime_generation;
  return true;
}

static SrResult PpuOutputControl_Bind(
    const PpuOutputControl *control, SrPpuOutputKind kind,
    uint32_t source, uint32_t band, uint32_t scale, uint8_t *pixels,
    uint64_t pixel_byte_size, uint64_t pitch_bytes, uint32_t height_pixels,
    uint32_t flags) {
  if (!control) return SR_RESULT_UNAVAILABLE;
  const SrPpuOutputBindingRequest request = {
      .struct_size = sizeof(request),
      .flags = flags,
      .lifetime_generation = control->lifetime_generation,
      .kind = kind,
      .source = source,
      .band = band,
      .scale = scale,
      .pixels = pixels,
      .pixel_byte_size = pixel_byte_size,
      .pitch_bytes = pitch_bytes,
      .height_pixels = height_pixels,
  };
  return control->api->bind_ppu_output_surface(
      control->runner, &request);
}

static SrResult PpuOutputControl_SetHorizontalMargin(
    const PpuOutputControl *control, SrPpuHorizontalMarginMode mode,
    uint32_t budget_pixels) {
  if (!control) return SR_RESULT_UNAVAILABLE;
  const SrPpuHorizontalMarginRequest request = {
      .struct_size = sizeof(request),
      .lifetime_generation = control->lifetime_generation,
      .mode = mode,
      .budget_pixels = budget_pixels,
  };
  return control->api->configure_ppu_horizontal_margin(
      control->runner, &request);
}

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
  PpuOutputControl output;
  const bool output_available = PpuOutputControl_Begin(&output);
  for (int i = 0; i < g_hd_replacement_count; i++) {
    const HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Mode7 && entry->pixels &&
        !g_m7_overlay_pixels && g_renderer) {
      const size_t capacity_pitch =
          (size_t)SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale *
          kArgbBytesPerPixel;
      const size_t active_pitch =
          (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel;
      const size_t capacity_bytes =
          capacity_pitch * kActRaiserAuthenticHeight * kHdMode7Scale;
      g_m7_overlay_pixels = calloc(
          1, capacity_bytes);
      g_m7_texture = SDL_CreateTexture(
          g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale,
          g_snes_height * kHdMode7Scale);
      if (g_m7_overlay_pixels && g_m7_texture) {
        SDL_SetTextureBlendMode(g_m7_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_m7_texture, SDL_SCALEMODE_NEAREST);
        if (output_available)
          (void)PpuOutputControl_Bind(
              &output, SR_PPU_OUTPUT_MODE7, 0u, 0u, kHdMode7Scale,
              g_m7_overlay_pixels, capacity_bytes, active_pitch,
              kActRaiserAuthenticHeight * kHdMode7Scale, 0u);
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
    if (source < 0 || source >= SR_PPU_OVERLAY_SOURCE_COUNT) continue;
    if (source == SR_PPU_OVERLAY_BG3 ||
        source == SR_PPU_OVERLAY_OBJ ||
        s_overlay_pixels[source])
      continue;
    s_overlay_pixels[source] = calloc(
        1, (size_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
            kHostDisplayFramebufferHeight);
    if (s_overlay_pixels[source] && output_available)
      (void)PpuOutputControl_Bind(
          &output, SR_PPU_OUTPUT_OVERLAY, (uint32_t)source, 0u, 0u,
          s_overlay_pixels[source],
          (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
              kHostDisplayFramebufferHeight,
          (size_t)g_snes_width * kArgbBytesPerPixel,
          kHostDisplayFramebufferHeight, 0u);
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
      s_authentic_surface_bound == enabled)
    return;
  s_authentic_capture_enabled = enabled;
  s_authentic_surface_bound = false;
  s_authentic_frame_serial = 0;
  PpuOutputControl output;
  if (!PpuOutputControl_Begin(&output)) return;
  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  const SrResult result = PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
      enabled ? g_authentic_pixels : NULL,
      enabled ? sizeof(g_authentic_pixels) : 0u,
      enabled ? pitch : 0u,
      enabled ? kHostDisplayFramebufferHeight : 0u, 0u);
  if (result != SR_RESULT_OK) {
    s_authentic_capture_enabled = false;
    (void)PpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
        NULL, 0u, 0u, 0u, 0u);
    fprintf(stderr,
            "[compare] authentic surface rejected for width %d\n",
            g_snes_width);
  } else {
    s_authentic_surface_bound = enabled;
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
  if (!s_authentic_capture_enabled || !s_authentic_surface_bound)
    return;
  s_authentic_next_frame_serial++;
  if (!s_authentic_next_frame_serial) s_authentic_next_frame_serial++;
  s_authentic_frame_serial = s_authentic_next_frame_serial;
}

uint64_t ActRaiser_AuthenticFrameSerial(void) {
  return s_authentic_frame_serial;
}

void ActRaiser_RebindPpuOutputSurfaces(void) {
  PpuOutputControl output;
  if (!PpuOutputControl_Begin(&output)) return;

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
      ActionApron_SurfacePitch(g_snes_width, SR_PPU_OBJ_APRON);
  /* Keep the general renderer available as a deterministic A/B oracle for
   * optimized scanout.  This is intentionally a process-start diagnostic,
   * not a player setting: switching algorithms mid-frame would invalidate
   * comparison captures. */
  const uint32_t render_flags = getenv("AR_PPU_REFERENCE")
      ? SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER : 0u;
  (void)PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_MAIN, 0u, 0u, 0u, g_pixels,
      sizeof(g_pixels), frame_pitch, kHostDisplayFramebufferHeight,
      render_flags);
  /* Geometry may be contracting from a wider prior bind. Clear first so a
   * validation failure cannot leave the old stride attached to new pixels. */
  (void)PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
      NULL, 0u, 0u, 0u, 0u);
  s_authentic_surface_bound = false;
  (void)PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES, 0u, 0u, 0u,
      NULL, 0u, 0u, 0u, 0u);
  (void)PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u, 0u,
      g_hud_bg_texture ? g_hud_bg_pixels : NULL,
      g_hud_bg_texture ? sizeof(g_hud_bg_pixels) : 0u,
      g_hud_bg_texture ? pitch : 0u,
      g_hud_bg_texture ? kHostDisplayFramebufferHeight : 0u, 0u);
  (void)PpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_OBJ, 0u, 0u,
      g_hud_obj_texture ? g_hud_obj_pixels : NULL,
      g_hud_obj_texture ? sizeof(g_hud_obj_pixels) : 0u,
      g_hud_obj_texture ? pitch : 0u,
      g_hud_obj_texture ? kHostDisplayFramebufferHeight : 0u, 0u);
  for (int source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++) {
    if (source == SR_PPU_OVERLAY_BG3 ||
        source == SR_PPU_OVERLAY_OBJ ||
        !s_overlay_pixels[source])
      continue;
    (void)PpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_OVERLAY, (uint32_t)source, 0u, 0u,
        s_overlay_pixels[source],
        (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kArgbBytesPerPixel *
            kHostDisplayFramebufferHeight,
        pitch, kHostDisplayFramebufferHeight, 0u);
  }
  if (g_m7_overlay_pixels)
    (void)PpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_MODE7, 0u, 0u, kHdMode7Scale,
        g_m7_overlay_pixels,
        (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * kHdMode7Scale *
            kArgbBytesPerPixel * kActRaiserAuthenticHeight * kHdMode7Scale,
        (size_t)g_snes_width * kHdMode7Scale * kArgbBytesPerPixel,
        kActRaiserAuthenticHeight * kHdMode7Scale, 0u);
  if (g_ws_active)
    (void)PpuOutputControl_SetHorizontalMargin(
        &output, SR_PPU_HORIZONTAL_MARGIN_CENTERED, (uint32_t)g_ws_extra);
  else
    (void)PpuOutputControl_SetHorizontalMargin(
        &output, SR_PPU_HORIZONTAL_MARGIN_AVAILABLE, 0u);
  if (s_authentic_capture_enabled) {
    const SrResult result = PpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
        g_authentic_pixels, sizeof(g_authentic_pixels), pitch,
        kHostDisplayFramebufferHeight, 0u);
    if (result != SR_RESULT_OK) {
      s_authentic_capture_enabled = false;
      fprintf(stderr,
              "[compare] authentic surface rejected after rebind for width %d\n",
              g_snes_width);
    } else {
      s_authentic_surface_bound = true;
    }
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
  for (int source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++) {
    free(s_overlay_pixels[source]);
    s_overlay_pixels[source] = NULL;
  }
  s_authentic_capture_enabled = false;
  s_authentic_surface_bound = false;
  s_authentic_frame_serial = 0;
  s_authentic_next_frame_serial = 0;
}
