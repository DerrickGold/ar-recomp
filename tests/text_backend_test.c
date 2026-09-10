#include "localization/text_backend.h"

#include <stdio.h>
#include <string.h>

static int s_failures;
#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    ++s_failures; \
  } \
} while (0)

typedef struct FakeBackend {
  unsigned creates;
  unsigned destroys;
  bool fail_create;
} FakeBackend;

static bool Rasterize(void *context, const ArTextRasterRequest *request,
                      ArTextBitmap *bitmap,
                      char *error, size_t error_capacity) {
  (void)context;
  (void)request;
  (void)bitmap;
  (void)error;
  (void)error_capacity;
  return false;
}

static void Release(void *context, ArTextBitmap *bitmap) {
  (void)context;
  (void)bitmap;
}

static const ArTextRasterizerOps kRasterizerOps = {
  .struct_size = sizeof(kRasterizerOps),
  .abi_version = AR_TEXT_RASTERIZER_ABI_VERSION,
  .rasterize = Rasterize,
  .release_bitmap = Release,
};

static bool Create(void *context, ArTextBackendInstance *instance,
                   const ArTextBackendConfig *config,
                   char *error, size_t error_capacity) {
  FakeBackend *fake = (FakeBackend *)context;
  ++fake->creates;
  instance->implementation = fake;
  CHECK(ArTextRasterizer_Init(
      &instance->rasterizer, &kRasterizerOps, fake,
      config->font_revision));
  if (!fake->fail_create) return true;
  if (error && error_capacity)
    snprintf(error, error_capacity, "%s", "injected create failure");
  return false;
}

static void Destroy(void *context, ArTextBackendInstance *instance) {
  FakeBackend *fake = (FakeBackend *)context;
  ++fake->destroys;
  instance->implementation = NULL;
  ArTextRasterizer_Reset(&instance->rasterizer);
}

static const ArTextBackendOps kBackendOps = {
  .struct_size = sizeof(kBackendOps),
  .abi_version = AR_TEXT_BACKEND_ABI_VERSION,
  .create = Create,
  .destroy = Destroy,
};

int main(void) {
  FakeBackend fake = {0};
  ArTextBackend backend = {
    .ops = &kBackendOps,
    .context = &fake,
  };
  CHECK(ArTextBackend_IsReady(&backend));
  ArTextBackend invalid = backend;
  ArTextBackendOps invalid_ops = kBackendOps;
  invalid_ops.abi_version += 1;
  invalid.ops = &invalid_ops;
  CHECK(!ArTextBackend_IsReady(&invalid));

  const char *fallbacks[] = {"fallback.ttf"};
  ArTextBackendConfig config = {
    .struct_size = sizeof(config),
    .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
    .font_stack_id = "test-stack",
    .primary_font_path = "primary.ttf",
    .fallback_font_paths = fallbacks,
    .fallback_font_count = 1,
    .font_revision = 7,
    .cached_size_capacity = 4,
  };
  ArTextBackendInstance instance = {0};
  char error[kArTextRasterErrorCapacity] = {0};
  CHECK(ArTextBackendInstance_Create(
      &instance, &backend, &config, error, sizeof(error)));
  CHECK(fake.creates == 1);
  CHECK(fake.destroys == 0);
  CHECK(ArTextBackendInstance_Get(&instance) != NULL);

  /* Replacement is transactional: a failed new instance is cleaned up while
   * the already-live rasterizer remains owned and usable. */
  fake.fail_create = true;
  CHECK(!ArTextBackendInstance_Create(
      &instance, &backend, &config, error, sizeof(error)));
  CHECK(strstr(error, "injected") != NULL);
  CHECK(fake.creates == 2);
  CHECK(fake.destroys == 1);
  CHECK(ArTextBackendInstance_Get(&instance) != NULL);

  fake.fail_create = false;
  config.font_revision = 8;
  CHECK(ArTextBackendInstance_Create(
      &instance, &backend, &config, error, sizeof(error)));
  CHECK(fake.creates == 3);
  CHECK(fake.destroys == 2); /* old live instance */
  CHECK(ArTextBackendInstance_Get(&instance)->implementation_revision == 8);

  ArTextBackendConfig bad = config;
  bad.fallback_font_paths = NULL;
  CHECK(!ArTextBackendInstance_Create(
      &instance, &backend, &bad, error, sizeof(error)));
  CHECK(fake.creates == 3);
  CHECK(ArTextBackendInstance_Get(&instance) != NULL);

  ArTextBackendInstance_Destroy(&instance);
  CHECK(fake.destroys == 3);
  CHECK(ArTextBackendInstance_Get(&instance) == NULL);
  ArTextBackendInstance_Destroy(&instance);
  CHECK(fake.destroys == 3);

  if (s_failures) return 1;
  puts("text backend checks passed");
  return 0;
}
