#include "host/font_resources.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

typedef struct Provider {
  unsigned acquisitions, releases;
  int mode;
} Provider;

static bool Acquire(void *context, ArFontResourceId id,
                     ArFontResourceData *data, char *error, size_t capacity) {
  (void)error; (void)capacity;
  Provider *provider = context;
  ++provider->acquisitions;
  CHECK(id == 7);
  *data = (ArFontResourceData){.bytes = "font", .size = 4, .token = provider};
  if (provider->mode == 1) data->size = 0;
  if (provider->mode == 2) data->bytes = NULL;
  if (provider->mode == 3) data->size = AR_FONT_RESOURCE_MAXIMUM_BYTES + 1;
  return provider->mode != 4;
}
static void Release(void *context, ArFontResourceData *data) {
  Provider *provider = context;
  CHECK(data->token == provider);
  ++provider->releases;
  *data = (ArFontResourceData){0};
}
static const ArFontResourceOps kOps = {
    .struct_size = sizeof(kOps), .abi_version = AR_FONT_RESOURCE_ABI_VERSION,
    .acquire = Acquire, .release = Release,
};

static void TestProviderContract(void) {
  Provider provider = {0};
  ArFontResources resources = {&kOps, &provider};
  ArFontResourceLease lease = {0};
  char error[128];
  CHECK(ArFontResources_IsReady(&resources));
  CHECK(!ArFontResource_Acquire(&lease, &resources, 0, error, sizeof(error)));
  CHECK(!provider.acquisitions);
  ArFontResourceOps wrong = kOps;
  ArFontResources invalid = {&wrong, &provider};
  ++wrong.abi_version;
  CHECK(!ArFontResource_Acquire(&lease, &invalid, 7, error, sizeof(error)));
  wrong = kOps;
  wrong.struct_size = offsetof(ArFontResourceOps, release);
  CHECK(!ArFontResources_IsReady(&invalid));
  wrong = kOps;
  wrong.release = NULL;
  CHECK(!ArFontResources_IsReady(&invalid));
  CHECK(!provider.acquisitions);
  CHECK(ArFontResource_Acquire(&lease, &resources, 7, error, sizeof(error)));
  const ArFontResourceLease original = lease;
  for (int mode = 1; mode <= 4; ++mode) {
    provider.mode = mode;
    CHECK(!ArFontResource_Acquire(&lease, &resources, 7, error, sizeof(error)));
    CHECK(error[0] && provider.releases == (unsigned)mode);
    CHECK(!memcmp(&lease, &original, sizeof(lease)));
  }
  provider.mode = 0;
  CHECK(ArFontResource_Acquire(&lease, &lease.owner, 7, error, sizeof(error)));
  CHECK(provider.releases == 5); /* Old valid lease retired only on success. */
  ArFontResource_Release(&lease);
  ArFontResource_Release(&lease);
  CHECK(provider.releases == 6 && provider.acquisitions == 6);
}

static bool WriteFixture(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  const size_t size = strlen(text);
  const bool written = fwrite(text, 1, size, file) == size;
  return fclose(file) == 0 && written;
}

static void TestHostSnapshots(void) {
  /* The test owns this CMake-provided path exclusively; no ROM/font is edited. */
  const char *path = AR_FONT_RESOURCE_TEST_FILE;
  char error[128];
  ArHostFontResources store = {0};
  ArFontResources resources = ArHostFontResources_Provider(&store);
  CHECK(WriteFixture(path, "first"));
  ArFontResourceId first = ArHostFontResources_RegisterFile(&store, path, error, sizeof(error));
  CHECK(first);
  ArFontResourceLease a = {0}, b = {0};
  CHECK(ArFontResource_Acquire(&a, &resources, first, error, sizeof(error)));
  CHECK(!ArHostFontResources_Destroy(&store)); /* Live bytes are untouched. */
  CHECK(WriteFixture(path, "second"));
  CHECK(ArFontResource_Acquire(&b, &resources, first, error, sizeof(error)));
  CHECK(a.data.bytes == b.data.bytes && a.data.size == 5);
  CHECK(!memcmp(a.data.bytes, "first", 5));
  ArFontResourceId second = ArHostFontResources_RegisterFile(&store, path, error, sizeof(error));
  CHECK(second && second != first);
  ArHostFontResources_Retire(&store, first);
  ArFontResourceLease rejected = {0};
  CHECK(!ArFontResource_Acquire(&rejected, &resources, first, error, sizeof(error)));
  CHECK(!memcmp(b.data.bytes, "first", 5)); /* Retired does not mean freed. */
  ArFontResource_Release(&a);
  ArFontResource_Release(&b);
  CHECK(ArFontResource_Acquire(&a, &resources, second, error, sizeof(error)));
  CHECK(a.data.size == 6 && !memcmp(a.data.bytes, "second", 6));
  CHECK(remove(path) == 0);
  CHECK(ArFontResource_Acquire(&b, &resources, second, error, sizeof(error)));
  ArFontResource_Release(&a);
  ArFontResource_Release(&b);
  CHECK(ArHostFontResources_Destroy(&store));
  CHECK(WriteFixture(path, "third"));
  const ArFontResourceId third = ArHostFontResources_RegisterFile(&store, path, error, sizeof(error));
  CHECK(third > second);
  CHECK(!ArFontResource_Acquire(&a, &resources, second, error, sizeof(error)));
  CHECK(ArHostFontResources_Destroy(&store));

  /* Empty/missing/over-limit files fail before registration or large reads. */
  CHECK(WriteFixture(path, ""));
  CHECK(!ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)));
  FILE *large = fopen(path, "wb");
  CHECK(large);
  if (large) {
    CHECK(!fseek(large, (long)AR_FONT_RESOURCE_MAXIMUM_BYTES, SEEK_SET));
    CHECK(fputc(0, large) == 0);
    CHECK(!fclose(large));
    CHECK(!ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)));
  }
  CHECK(remove(path) == 0);
  CHECK(!ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)));

  CHECK(WriteFixture(path, "x"));
  ArFontResourceId ids[64];
  for (size_t i = 0; i < 64; ++i) {
    ids[i] = ArHostFontResources_RegisterFile(&store, path, error, sizeof(error));
    CHECK(ids[i]);
  }
  CHECK(!ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)));
  CHECK(ArFontResource_Acquire(&a, &resources, ids[0], error, sizeof(error)));
  ArHostFontResources_Retire(&store, ids[0]);
  CHECK(!ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)));
  ArFontResource_Release(&a);
  CHECK(ArHostFontResources_RegisterFile(&store, path, error, sizeof(error)) > ids[63]);
  CHECK(ArHostFontResources_Destroy(&store));
  CHECK(ArHostFontResources_Destroy(&store));
  CHECK(remove(path) == 0);
}

int main(void) {
  TestProviderContract();
  TestHostSnapshots();
  return failures ? 1 : 0;
}
