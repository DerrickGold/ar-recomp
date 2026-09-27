#ifndef AR_LOCALIZED_TEXT_RESOURCES_INTERNAL_H
#define AR_LOCALIZED_TEXT_RESOURCES_INTERNAL_H

/* Private to the localized text presenter. Owns transactional font selection,
 * backend instances and their surface caches; page layout borrows the active
 * instance/cache and never manages their lifetime. */
#include "render/localized_text_presenter.h"

typedef struct ArLocalizedPendingFont {
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  char stack[kArLocalizationFrameFontStackCapacity];
  ArFontResourceId primary;
  ArFontResourceId fallbacks[kArTextPresentationMaximumFallbackFonts];
  size_t fallback_count;
  ArTextFontRole roles[kArTextFontMaximumRoles];
  size_t role_count;
  uint64_t revision, backend_revision;
} ArLocalizedPendingFont;

typedef struct ArLocalizedTextResources {
  ArTextBackend backend;
  ArFontResources resources;
  uint64_t backend_revision, active_backend_revision;
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  bool cache_initialized;
  char active_stack[kArLocalizationFrameFontStackCapacity];
  ArFontResourceId active_primary;
  ArFontResourceId active_fallbacks[kArTextPresentationMaximumFallbackFonts];
  size_t active_fallback_count;
  ArTextFontRole active_roles[kArTextFontMaximumRoles];
  size_t active_role_count;
  ArLocalizedPendingFont pending;
  uint64_t active_revision;
} ArLocalizedTextResources;

void ArLocalizedTextResources_SetBackend(ArLocalizedTextResources *resources,
                                         const ArTextBackend *backend);
void ArLocalizedTextResources_SetFontResources(ArLocalizedTextResources *resources,
                                               const ArFontResources *fonts);
bool ArLocalizedTextResources_PrepareFont(ArLocalizedTextResources *resources,
                                          ArRenderDevice *device,
                                          const ArTextPresentationFont *font, char *error,
                                          size_t error_capacity);
void ArLocalizedTextResources_DiscardPreparedFont(ArLocalizedTextResources *resources,
                                                  ArRenderDevice *device);
/* Activates only a fully prepared identity. changed asks the presenter to
 * invalidate layout/artwork derived from the previous font. Failure retains
 * the active font and borrowed surfaces. */
bool ArLocalizedTextResources_Activate(ArLocalizedTextResources *resources, ArRenderDevice *device,
                                       const ArLocalizationFrame *frame, bool *changed, char *error,
                                       size_t error_capacity);
void ArLocalizedTextResources_Reset(ArLocalizedTextResources *resources, ArRenderDevice *device);
#endif
