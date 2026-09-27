#include "render/localized_text_resources_internal.h"

#include <stdio.h>
#include <string.h>

enum {
  kSurfaceCacheCapacity = 128,
  kFontSizeCacheCapacity = 32,
  /* Entry count alone cannot bound the memory of tall HiDPI pages. */
  kSurfaceCacheByteBudget = 96 << 20,
};
_Static_assert(kSurfaceCacheCapacity >= kArLocalizedPreparedTextCapacity,
               "a prepared frame must fit without evicting its own text surfaces");

void ArLocalizedTextResources_SetBackend(ArLocalizedTextResources *resources,
                                         const ArTextBackend *backend) {
  const ArTextBackend selected = ArTextBackend_IsReady(backend) ? *backend : (ArTextBackend){0};
  if (selected.ops != resources->backend.ops || selected.context != resources->backend.context)
    ++resources->backend_revision;
  resources->backend = selected;
}

void ArLocalizedTextResources_SetFontResources(ArLocalizedTextResources *resources,
                                               const ArFontResources *fonts) {
  const ArFontResources selected = ArFontResources_IsReady(fonts) ? *fonts : (ArFontResources){0};
  if (selected.ops != resources->resources.ops || selected.context != resources->resources.context)
    ++resources->backend_revision;
  resources->resources = selected;
}

static void DestroyResources(ArLocalizedTextResources *resources, ArRenderDevice *device) {
  if (resources->cache_initialized) ArTextSurfaceCache_Destroy(&resources->cache, device);
  ArTextBackendInstance_Destroy(&resources->instance);
  resources->cache_initialized = false;
  resources->active_stack[0] = 0;
  resources->active_primary = 0;
  resources->active_revision = 0;
  resources->active_fallback_count = 0;
  memset(resources->active_fallbacks, 0, sizeof(resources->active_fallbacks));
  resources->active_role_count = 0;
  memset(resources->active_roles, 0, sizeof(resources->active_roles));
}

static bool FontError(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
  return false;
}

static void DestroyPendingFont(ArLocalizedTextResources *resources, ArRenderDevice *device) {
  ArTextSurfaceCache_Destroy(&resources->pending.cache, device);
  ArTextBackendInstance_Destroy(&resources->pending.instance);
  memset(&resources->pending, 0, sizeof(resources->pending));
}

void ArLocalizedTextResources_DiscardPreparedFont(ArLocalizedTextResources *resources,
                                                  ArRenderDevice *device) {
  DestroyPendingFont(resources, device);
}

static bool ActiveFontMatches(ArLocalizedTextResources *resources, ArRenderDevice *device,
                              const ArTextPresentationFont *font) {
  if (!resources->cache_initialized || resources->cache.device != device ||
      resources->active_backend_revision != resources->backend_revision ||
      resources->active_revision != font->revision ||
      resources->active_fallback_count != font->fallback_count ||
      strcmp(resources->active_stack, font->stack_id) || resources->active_primary != font->primary)
    return false;
  for (size_t i = 0; i < font->fallback_count; ++i)
    if (font->fallbacks[i] != resources->active_fallbacks[i]) return false;
  return ArTextFontRoles_Equal(resources->active_roles, resources->active_role_count, font->roles,
                               font->role_count);
}

bool ArLocalizedTextResources_PrepareFont(ArLocalizedTextResources *resources,
                                          ArRenderDevice *device,
                                          const ArTextPresentationFont *font, char *error,
                                          size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  if (!ArRenderDevice_IsReady(device) || !ArTextBackend_IsReady(&resources->backend) ||
      !ArFontResources_IsReady(&resources->resources))
    return FontError(error, error_capacity, "enhanced text backend is unavailable");
  if (!font ||
      font->struct_size < offsetof(ArTextPresentationFont, role_count) + sizeof(font->role_count) ||
      font->abi_version != AR_TEXT_PRESENTATION_ABI_VERSION || !font->revision || !font->stack_id ||
      !font->stack_id[0] || !font->primary ||
      strlen(font->stack_id) >= sizeof(resources->active_stack) ||
      font->fallback_count > kArTextPresentationMaximumFallbackFonts ||
      (font->fallback_count && !font->fallbacks) ||
      !ArTextFontRoles_Valid(font->roles, font->role_count))
    return FontError(error, error_capacity, "invalid enhanced font selection");
  for (size_t i = 0; i < font->fallback_count; ++i) {
    if (!font->fallbacks[i])
      return FontError(error, error_capacity, "invalid fallback font resource");
  }
  if (ActiveFontMatches(resources, device, font)) return true;
  ArLocalizedPendingFont *pending = &resources->pending;
  bool same_pending =
      pending->cache.device == device && pending->backend_revision == resources->backend_revision &&
      pending->revision == font->revision && pending->fallback_count == font->fallback_count &&
      !strcmp(pending->stack, font->stack_id) && pending->primary == font->primary;
  for (size_t i = 0; same_pending && i < font->fallback_count; ++i)
    same_pending = pending->fallbacks[i] == font->fallbacks[i];
  same_pending = same_pending && ArTextFontRoles_Equal(pending->roles, pending->role_count,
                                                       font->roles, font->role_count);
  if (same_pending) return true;
  const ArTextBackendConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = font->stack_id,
      .resources = resources->resources,
      .primary_font = font->primary,
      .font_revision = font->revision,
      .cached_size_capacity = kFontSizeCacheCapacity,
      .fallback_fonts = font->fallbacks,
      .fallback_font_count = font->fallback_count,
      .roles = font->roles,
      .role_count = font->role_count,
  };
  ArTextBackendInstance instance = {0};
  if (!ArTextBackendInstance_Create(&instance, &resources->backend, &config, error, error_capacity))
    return false;
  ArTextSurfaceCache cache = {0};
  if (!ArTextSurfaceCache_Init(&cache, kSurfaceCacheCapacity)) {
    ArTextBackendInstance_Destroy(&instance);
    return FontError(error, error_capacity, "text cache initialization failed");
  }
  ArTextSurfaceCache_SetByteBudget(&cache, kSurfaceCacheByteBudget);
  /* Backend construction may be lazy, opening fonts only for requested sizes.
   * Exercise shaping, raster allocation, texture creation and upload before
   * advertising readiness to the game. Keep the warmed font/cache on success.
   */
  ArTextRasterRequest probe = {
      .struct_size = sizeof(probe),
      .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = "Ag",
      .utf8_bytes = 2,
      .font_stack_id = font->stack_id,
      .font_stack_id_bytes = strlen(font->stack_id),
      .source_revision = 1,
      .font_revision = font->revision,
      .style_id = kArTextStyle_RetailBlueWhiteBands,
      .flags = kArTextRasterFlag_IncludeRevealClusters,
      .font_pixels = 24,
      .minimum_font_pixels = 24,
      .maximum_width = 128,
      .maximum_height = 128,
      .filter = kArRenderFilter_Nearest,
  };
  ArTextSurface surface;
  if (!ArTextSurfaceCache_Acquire(&cache, device, ArTextBackendInstance_Get(&instance), &probe,
                                  &surface, error, error_capacity)) {
    ArTextSurfaceCache_Destroy(&cache, device);
    ArTextBackendInstance_Destroy(&instance);
    return false;
  }
  ArTextRunAppearance appearance = {
      .scale_basis = 10000, .band_rgb = 0xffffff, .body_rgb = 0xffffff};
  probe.appearance = &appearance;
  for (size_t i = 0; i < font->role_count; ++i) {
    snprintf(appearance.font_role, sizeof(appearance.font_role), "%s", font->roles[i].name);
    if (!ArTextSurfaceCache_Acquire(&cache, device, ArTextBackendInstance_Get(&instance), &probe,
                                    &surface, error, error_capacity)) {
      ArTextSurfaceCache_Destroy(&cache, device);
      ArTextBackendInstance_Destroy(&instance);
      return false;
    }
  }
  /* A source can still fail semantic validation after this readiness check.
   * Keep the active font and all borrowed surfaces intact until a frame with
   * the approved identity arrives. At most one candidate is retained. */
  DestroyPendingFont(resources, device);
  pending->instance = instance;
  pending->cache = cache;
  snprintf(pending->stack, sizeof(pending->stack), "%s", font->stack_id);
  pending->primary = font->primary;
  pending->revision = font->revision;
  pending->backend_revision = resources->backend_revision;
  pending->fallback_count = font->fallback_count;
  for (size_t i = 0; i < font->fallback_count; ++i)
    pending->fallbacks[i] = font->fallbacks[i];
  pending->role_count = font->role_count;
  if (font->role_count)
    memcpy(pending->roles, font->roles, font->role_count * sizeof(*font->roles));
  return true;
}

bool ArLocalizedTextResources_Activate(ArLocalizedTextResources *resources, ArRenderDevice *device,
                                       const ArLocalizationFrame *frame, bool *changed, char *error,
                                       size_t error_capacity) {
  *changed = false;
  if (frame->fallback_font_count > kArTextPresentationMaximumFallbackFonts)
    return FontError(error, error_capacity, "invalid fallback font count");
  const ArTextPresentationFont font = {
      .struct_size = sizeof(font),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .stack_id = frame->font_stack_id,
      .primary = frame->primary_font,
      .revision = frame->font_revision,
      .fallbacks = frame->fallback_fonts,
      .fallback_count = frame->fallback_font_count,
      .roles = frame->font_roles,
      .role_count = frame->font_role_count,
  };
  if (ArLocalizedTextResources_PrepareFont(resources, device, &font, error, error_capacity)) {
    if (!ActiveFontMatches(resources, device, &font)) {
      ArLocalizedPendingFont *pending = &resources->pending;
      DestroyResources(resources, device);
      *changed = true;
      resources->instance = pending->instance;
      resources->cache = pending->cache;
      resources->cache_initialized = true;
      memcpy(resources->active_stack, pending->stack, sizeof(pending->stack));
      resources->active_primary = pending->primary;
      memcpy(resources->active_fallbacks, pending->fallbacks, sizeof(pending->fallbacks));
      resources->active_fallback_count = pending->fallback_count;
      resources->active_role_count = pending->role_count;
      memcpy(resources->active_roles, pending->roles, sizeof(pending->roles));
      resources->active_revision = pending->revision;
      resources->active_backend_revision = pending->backend_revision;
      memset(pending, 0, sizeof(*pending)); /* Ownership moved, not duplicated. */
    }
    return true;
  }
  return false;
}

void ArLocalizedTextResources_Reset(ArLocalizedTextResources *resources, ArRenderDevice *device) {
  DestroyPendingFont(resources, device);
  if (resources->cache_initialized) {
    const ArTextSurfaceCacheStats *stats = ArTextSurfaceCache_GetStats(&resources->cache);
    if (stats && stats->lookups)
      fprintf(stderr,
              "[localized-text] cache lookups=%llu hits=%llu misses=%llu "
              "rasters=%llu uploads=%llu failures=%llu\n",
              (unsigned long long)stats->lookups, (unsigned long long)stats->hits,
              (unsigned long long)stats->misses, (unsigned long long)stats->rasterize_calls,
              (unsigned long long)stats->upload_calls, (unsigned long long)stats->failures);
  }
  DestroyResources(resources, device);
}
