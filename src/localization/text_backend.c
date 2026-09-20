#include "localization/text_backend.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define AR_MEMBER_END(type, member) \
  (offsetof(type, member) + sizeof(((type *)0)->member))

static void SetError(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
}

bool ArTextBackend_IsReady(const ArTextBackend *backend) {
  return backend && backend->ops &&
      backend->ops->struct_size >=
          AR_MEMBER_END(ArTextBackendOps, destroy) &&
      backend->ops->abi_version == AR_TEXT_BACKEND_ABI_VERSION &&
      backend->ops->create && backend->ops->destroy;
}

bool ArTextBackendConfig_IsValid(const ArTextBackendConfig *config) {
  if (!config ||
      config->struct_size < AR_MEMBER_END(ArTextBackendConfig, role_count) ||
      config->abi_version != AR_TEXT_BACKEND_CONFIG_ABI_VERSION ||
      !config->font_stack_id || !config->font_stack_id[0] ||
      !config->primary_font || !ArFontResources_IsReady(&config->resources) ||
      config->fallback_font_count > kArTextBackendMaximumFallbackFonts ||
      !config->font_revision || !config->cached_size_capacity ||
      config->role_count > kArTextBackendMaximumFontRoles ||
      (config->role_count && !config->roles))
    return false;
  if (config->fallback_font_count && !config->fallback_fonts)
    return false;
  for (size_t index = 0; index < config->fallback_font_count; ++index)
    if (!config->fallback_fonts[index])
      return false;
  return ArTextFontRoles_Valid(config->roles, config->role_count);
}

bool ArTextBackendInstance_Create(
    ArTextBackendInstance *instance, const ArTextBackend *backend,
    const ArTextBackendConfig *config,
    char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  if (!instance || !ArTextBackend_IsReady(backend) ||
      !ArTextBackendConfig_IsValid(config)) {
    SetError(error, error_capacity, "invalid text backend configuration");
    return false;
  }
  ArTextBackendInstance created = {0};
  if (!backend->ops->create(
          backend->context, &created, config, error, error_capacity)) {
    if (created.implementation || ArTextRasterizer_IsReady(&created.rasterizer))
      backend->ops->destroy(backend->context, &created);
    return false;
  }
  if (!ArTextRasterizer_IsReady(&created.rasterizer)) {
    backend->ops->destroy(backend->context, &created);
    SetError(error, error_capacity,
             "text backend did not publish a ready rasterizer");
    return false;
  }
  created.owner_ops = backend->ops;
  created.owner_context = backend->context;
  ArTextBackendInstance_Destroy(instance);
  *instance = created;
  return true;
}

void ArTextBackendInstance_Destroy(ArTextBackendInstance *instance) {
  if (!instance) return;
  if (instance->owner_ops && instance->owner_ops->destroy)
    instance->owner_ops->destroy(instance->owner_context, instance);
  memset(instance, 0, sizeof(*instance));
}

const ArTextRasterizer *ArTextBackendInstance_Get(
    const ArTextBackendInstance *instance) {
  return instance && ArTextRasterizer_IsReady(&instance->rasterizer)
      ? &instance->rasterizer : NULL;
}
