#include "render/ui_text_renderer.h"
#include "localization/interface_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kUiTextCacheEntries = 256,
  kUiTextCacheBytes = 16 << 20,
  kUiTextMaximumWidth = 8192,
  kUiTextMaximumHeight = 256,
};

typedef struct UiTextState {
  ArRenderDevice *device;
  ArTextBackendInstance font;
  ArTextSurfaceCache cache;
  uint64_t font_revision;
  char stack_id[128];
} UiTextState;

static void DestroyState(UiTextState *state) {
  if (!state) return;
  ArTextSurfaceCache_Destroy(&state->cache, state->device);
  ArTextBackendInstance_Destroy(&state->font);
  free(state);
}

bool ArUiTextRenderer_Init(ArUiTextRenderer *renderer, ArRenderDevice *device,
                           const ArTextBackend *backend,
                           const ArTextBackendConfig *fonts,
                           char *error, size_t error_capacity) {
  if (!renderer || !ArRenderDevice_IsReady(device) || !fonts ||
      fonts->struct_size < sizeof(*fonts) ||
      fonts->abi_version != AR_TEXT_BACKEND_CONFIG_ABI_VERSION ||
      !fonts->font_stack_id || !fonts->font_stack_id[0] ||
      strlen(fonts->font_stack_id) >= sizeof(((UiTextState *)0)->stack_id)) {
    if (error && error_capacity)
      snprintf(error, error_capacity, "invalid interface font configuration");
    return false;
  }
  UiTextState *state = calloc(1, sizeof(*state));
  if (!state) return false;
  state->device = device;
  state->font_revision = fonts->font_revision;
  memcpy(state->stack_id, fonts->font_stack_id, strlen(fonts->font_stack_id) + 1);
  if (!ArTextBackendInstance_Create(&state->font, backend, fonts, error,
                                     error_capacity) ||
      !ArTextSurfaceCache_Init(&state->cache, kUiTextCacheEntries)) {
    DestroyState(state);
    return false;
  }
  ArTextSurfaceCache_SetByteBudget(&state->cache, kUiTextCacheBytes);
  /* Opening a backend can be lazy. Prove it can produce a real bitmap before
   * replacing a working instance. This requires no particular glyph-coverage
   * API and also exercises upload readiness through the portable cache. */
  const ArTextRasterRequest preflight = {
    .struct_size = sizeof(preflight),
    .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
    .utf8 = "A", .utf8_bytes = 1,
    .font_stack_id = state->stack_id,
    .font_stack_id_bytes = strlen(state->stack_id),
    .font_revision = state->font_revision,
    .font_pixels = 24, .minimum_font_pixels = 24,
    .maximum_width = 128, .maximum_height = 128,
    .filter = kArRenderFilter_Linear,
  };
  ArTextSurface surface;
  if (!ArTextSurfaceCache_Acquire(&state->cache, device,
        ArTextBackendInstance_Get(&state->font), &preflight, &surface,
        error, error_capacity)) {
    DestroyState(state);
    return false;
  }
  ArUiTextRenderer_Destroy(renderer);
  renderer->implementation = state;
  return true;
}

void ArUiTextRenderer_Destroy(ArUiTextRenderer *renderer) {
  if (!renderer) return;
  DestroyState(renderer->implementation);
  renderer->implementation = NULL;
}

bool ArUiTextRenderer_IsReady(const ArUiTextRenderer *renderer) {
  const UiTextState *state = renderer ? renderer->implementation : NULL;
  return state && ArRenderDevice_IsReady(state->device) &&
      ArTextRasterizer_IsReady(ArTextBackendInstance_Get(&state->font));
}

void ArUiTextRenderer_ClearTextures(ArUiTextRenderer *renderer) {
  UiTextState *state = renderer ? renderer->implementation : NULL;
  if (!state) return;
  ArTextSurfaceCache_Destroy(&state->cache, state->device);
  /* Allocation failure leaves an empty cache. Draw retries its initialization,
   * so a transient reset failure cannot permanently disable Unicode text. */
}

static bool AcquireRun(ArUiTextRenderer *renderer, const ArUiTextRun *run,
                         ArTextSurface *surface) {
  *surface = (ArTextSurface){0};
  if (!ArUiTextRenderer_IsReady(renderer) || !run ||
      run->struct_size < sizeof(*run) || run->abi_version != AR_UI_TEXT_RUN_ABI_VERSION ||
      !run->utf8 || run->utf8_bytes > kArInterfaceTextMaximumBytes ||
      run->bounds.w <= 0 || run->bounds.w > kUiTextMaximumWidth ||
      run->bounds.h <= 0 || run->bounds.h > kUiTextMaximumHeight ||
      run->alignment < kArUiTextAlignment_Left ||
      run->alignment > kArUiTextAlignment_Right)
    return false;
  if (!run->utf8_bytes) return true;
  UiTextState *state = renderer->implementation;
  if (!state->cache.entries) {
    if (!ArTextSurfaceCache_Init(&state->cache, kUiTextCacheEntries)) return false;
    ArTextSurfaceCache_SetByteBudget(&state->cache, kUiTextCacheBytes);
  }
  const int height = run->bounds.h;
  const ArTextRasterRequest request = {
    .struct_size = sizeof(request), .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
    .utf8 = run->utf8, .utf8_bytes = run->utf8_bytes,
    .font_stack_id = state->stack_id, .font_stack_id_bytes = strlen(state->stack_id),
    .font_revision = state->font_revision,
    .style_id = run->style_id, .band_rgb = run->band_rgb,
    .body_rgb = run->body_rgb, .shadow_rgb = run->shadow_rgb,
    .shadow_enabled = run->shadow_enabled,
    .flags = kArTextRasterFlag_CropHorizontalWhitespace | kArTextRasterFlag_CropVerticalWhitespace,
    .direction = kArTextDirection_Auto,
    .font_pixels = height + height / 2,
    .minimum_font_pixels = height > 1 ? height / 2 : 1,
    .maximum_width = run->bounds.w, .maximum_height = height,
    .filter = kArRenderFilter_Linear,
    .language_bcp47 = run->language_bcp47,
    .language_bcp47_bytes = run->language_bcp47 ? strlen(run->language_bcp47) : 0,
  };
  return ArTextSurfaceCache_Acquire(&state->cache, state->device,
        ArTextBackendInstance_Get(&state->font), &request, surface, NULL, 0);
}

bool ArUiTextRenderer_Measure(ArUiTextRenderer *renderer, const ArUiTextRun *run,
                              int *width, int *height) {
  if (width) *width = 0;
  if (height) *height = 0;
  ArTextSurface surface;
  if (!AcquireRun(renderer, run, &surface)) return false;
  if (width) *width = surface.width;
  if (height) *height = surface.height;
  return true;
}

bool ArUiTextRenderer_Draw(ArUiTextRenderer *renderer, const ArUiTextRun *run) {
  ArTextSurface surface;
  if (!AcquireRun(renderer, run, &surface)) return false;
  if (!run->utf8_bytes) return true;
  UiTextState *state = renderer->implementation;
  int offset = 0;
  if (run->alignment == kArUiTextAlignment_Center)
    offset = (run->bounds.w - surface.width) / 2;
  else if (run->alignment == kArUiTextAlignment_Right)
    offset = run->bounds.w - surface.width;
  const ArRenderRectF destination = {
    (float)run->bounds.x + offset,
    (float)run->bounds.y + (run->bounds.h - surface.height) / 2,
    (float)surface.width, (float)surface.height,
  };
  return ArRenderDevice_DrawTextureTinted(state->device, surface.texture, NULL,
                                          &destination, run->tint);
}

const ArTextSurfaceCacheStats *ArUiTextRenderer_GetStats(const ArUiTextRenderer *renderer) {
  const UiTextState *state = renderer ? renderer->implementation : NULL;
  return state ? ArTextSurfaceCache_GetStats(&state->cache) : NULL;
}
