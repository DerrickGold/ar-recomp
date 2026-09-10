#include "host/font_resources.h"

#include <stdio.h>
#include <stdlib.h>

enum { kMaximumResources = 64, kMaximumStoreBytes = 256 * 1024 * 1024 };

typedef struct FontEntry {
  ArFontResourceId id;
  void *bytes;
  size_t size, leases;
  bool registered;
} FontEntry;

typedef struct FontStore {
  FontEntry entries[kMaximumResources];
  size_t bytes;
} FontStore;

/* Do not reset on store destruction: an old frame must never alias a new font,
 * including across game resets or replacement of the host store. */
static ArFontResourceId s_last_id;

static bool Fail(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
  return false;
}

static void FreeEntry(FontStore *store, FontEntry *entry) {
  store->bytes -= entry->size;
  free(entry->bytes);
  *entry = (FontEntry){0};
}

static bool Acquire(void *context, ArFontResourceId id,
                     ArFontResourceData *data, char *error, size_t capacity) {
  ArHostFontResources *host = context;
  FontStore *store = host ? host->implementation : NULL;
  if (store) {
    for (size_t i = 0; i < kMaximumResources; ++i) {
      FontEntry *entry = &store->entries[i];
      if (entry->id != id || !entry->registered) continue;
      if (entry->leases == SIZE_MAX)
        return Fail(error, capacity, "font resource lease limit reached");
      ++entry->leases;
      *data = (ArFontResourceData){entry->bytes, entry->size, entry};
      return true;
    }
  }
  return Fail(error, capacity, "font resource has expired or is unavailable");
}

static void Release(void *context, ArFontResourceData *data) {
  ArHostFontResources *host = context;
  FontStore *store = host->implementation;
  FontEntry *entry = data->token;
  if (store && entry && entry->leases && --entry->leases == 0 &&
      !entry->registered)
    FreeEntry(store, entry);
  *data = (ArFontResourceData){0};
}

static const ArFontResourceOps kOps = {
    .struct_size = sizeof(kOps), .abi_version = AR_FONT_RESOURCE_ABI_VERSION,
    .acquire = Acquire, .release = Release,
};

ArFontResources ArHostFontResources_Provider(ArHostFontResources *store) {
  return (ArFontResources){.ops = store ? &kOps : NULL, .context = store};
}

ArFontResourceId ArHostFontResources_RegisterFile(
    ArHostFontResources *host, const char *path, char *error, size_t capacity) {
  if (error && capacity) error[0] = 0;
  if (!host || !path || !path[0] || s_last_id == UINT64_MAX) {
    Fail(error, capacity, "invalid font file request or exhausted identities");
    return 0;
  }
  if (!host->implementation)
    host->implementation = calloc(1, sizeof(FontStore));
  FontStore *store = host->implementation;
  FontEntry *entry = NULL;
  if (store)
    for (size_t i = 0; i < kMaximumResources; ++i)
      if (!store->entries[i].id) { entry = &store->entries[i]; break; }
  if (!entry) {
    Fail(error, capacity, "font resource capacity exhausted");
    return 0;
  }
  FILE *file = fopen(path, "rb");
  if (!file) { Fail(error, capacity, "cannot open font file"); return 0; }
  long length = -1;
  if (!fseek(file, 0, SEEK_END)) length = ftell(file);
  if (length <= 0 || (unsigned long)length > AR_FONT_RESOURCE_MAXIMUM_BYTES ||
      (size_t)length > kMaximumStoreBytes - store->bytes ||
      fseek(file, 0, SEEK_SET)) {
    fclose(file);
    Fail(error, capacity, "font file outside resource byte budget or not seekable");
    return 0;
  }
  const size_t size = (size_t)length;
  void *bytes = malloc(size);
  const bool read = bytes && fread(bytes, 1, size, file) == size &&
      fgetc(file) == EOF && !ferror(file);
  const bool closed = fclose(file) == 0;
  if (!read || !closed) {
    free(bytes);
    Fail(error, capacity, "cannot read immutable font snapshot");
    return 0;
  }
  *entry = (FontEntry){.id = ++s_last_id, .bytes = bytes, .size = size,
                       .registered = true};
  store->bytes += size;
  return entry->id;
}

void ArHostFontResources_Retire(ArHostFontResources *host, ArFontResourceId id) {
  FontStore *store = host ? host->implementation : NULL;
  if (!store || !id) return;
  for (size_t i = 0; i < kMaximumResources; ++i) {
    FontEntry *entry = &store->entries[i];
    if (entry->id != id) continue;
    entry->registered = false;
    if (!entry->leases) FreeEntry(store, entry);
    return;
  }
}

bool ArHostFontResources_Destroy(ArHostFontResources *host) {
  FontStore *store = host ? host->implementation : NULL;
  if (!store) return true;
  for (size_t i = 0; i < kMaximumResources; ++i)
    if (store->entries[i].leases) return false;
  for (size_t i = 0; i < kMaximumResources; ++i)
    FreeEntry(store, &store->entries[i]);
  free(store);
  host->implementation = NULL;
  return true;
}
