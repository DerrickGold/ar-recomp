#ifndef AR_LOCALIZATION_FONT_RESOURCE_H
#define AR_LOCALIZATION_FONT_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AR_FONT_RESOURCE_ABI_VERSION UINT32_C(1)
#define AR_FONT_RESOURCE_MAXIMUM_BYTES ((size_t)64 * 1024 * 1024)

/* Process-local identities, never pointers, paths or persistent save data.
 * Zero is invalid. Providers must not reuse an identity for different bytes. */
typedef uint64_t ArFontResourceId;

typedef struct ArFontResourceData {
  const void *bytes;
  size_t size;
  void *token;
} ArFontResourceData;

typedef struct ArFontResourceOps {
  size_t struct_size;
  uint32_t abi_version;
  bool (*acquire)(void *context, ArFontResourceId id,
                  ArFontResourceData *data, char *error, size_t capacity);
  void (*release)(void *context, ArFontResourceData *data);
} ArFontResourceOps;

typedef struct ArFontResources {
  const ArFontResourceOps *ops;
  void *context;
} ArFontResources;

/* Immutable, bounded bytes pinned until Release. A backend acquires once per
 * stack member, not once per raster size. Close all font/stream objects before
 * releasing their bytes. Providers and contexts outlive every lease. Calls
 * are on the host/presenter thread; a multithreaded port must synchronize.
 * Zero-initialize leases. Acquire is transactional; release is idempotent. */
typedef struct ArFontResourceLease {
  ArFontResourceData data;
  ArFontResources owner;
} ArFontResourceLease;

bool ArFontResources_IsReady(const ArFontResources *resources);
bool ArFontResource_Acquire(ArFontResourceLease *lease,
                           const ArFontResources *resources,
                           ArFontResourceId id, char *error, size_t capacity);
void ArFontResource_Release(ArFontResourceLease *lease);

#endif
