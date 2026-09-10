#ifndef ACTRAISER_LOCALIZATION_TEXT_BACKEND_H
#define ACTRAISER_LOCALIZATION_TEXT_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/text_rasterizer.h"

#define AR_TEXT_BACKEND_ABI_VERSION UINT32_C(1)
#define AR_TEXT_BACKEND_CONFIG_ABI_VERSION UINT32_C(1)

/* Portable input for constructing one ordered font-stack rasterizer. Paths
 * have already been resolved by the pack/catalogue owner. */
typedef struct ArTextBackendConfig {
  size_t struct_size;
  uint32_t abi_version;
  const char *font_stack_id;
  const char *primary_font_path;
  const char *const *fallback_font_paths;
  size_t fallback_font_count;
  uint64_t font_revision;
  size_t cached_size_capacity;
} ArTextBackendConfig;

/* Opaque backend instance plus the renderer-neutral rasterizer it publishes.
 * Consumers zero-initialize it, then construct/destroy it only through the
 * wrappers below. Create replaces an existing live instance transactionally. */
typedef struct ArTextBackendInstance {
  void *implementation;
  ArTextRasterizer rasterizer;
  const struct ArTextBackendOps *owner_ops;
  void *owner_context;
} ArTextBackendInstance;

typedef struct ArTextBackendOps {
  size_t struct_size;
  uint32_t abi_version;
  bool (*create)(void *context, ArTextBackendInstance *instance,
                 const ArTextBackendConfig *config,
                 char *error, size_t error_capacity);
  void (*destroy)(void *context, ArTextBackendInstance *instance);
} ArTextBackendOps;

typedef struct ArTextBackend {
  const ArTextBackendOps *ops;
  void *context;
} ArTextBackend;

bool ArTextBackend_IsReady(const ArTextBackend *backend);
bool ArTextBackendInstance_Create(
    ArTextBackendInstance *instance, const ArTextBackend *backend,
    const ArTextBackendConfig *config,
    char *error, size_t error_capacity);
void ArTextBackendInstance_Destroy(ArTextBackendInstance *instance);
const ArTextRasterizer *ArTextBackendInstance_Get(
    const ArTextBackendInstance *instance);

#endif
