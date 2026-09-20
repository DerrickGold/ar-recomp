#ifndef AR_LOCALIZATION_FONT_RESOURCE_H
#define AR_LOCALIZATION_FONT_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AR_FONT_RESOURCE_ABI_VERSION UINT32_C(1)
#define AR_FONT_RESOURCE_MAXIMUM_BYTES ((size_t)64 * 1024 * 1024)

/* Process-local identities, never pointers, paths or persistent save data.
 * Zero is invalid. Providers must not reuse an identity for different bytes. */
typedef uint64_t ArFontResourceId;

enum {
  kArTextFontMaximumFallbacks = 16,
  kArTextFontMaximumRoles = 8,
  kArTextFontRoleCapacity = 97,
};

/* Pointer-free named stack for retained frames as well as backend creation.
 * The implicit body stack is supplied separately. Roles never inherit from
 * one another; each primary/fallback chain is complete. */
typedef struct ArTextFontRole {
  char name[kArTextFontRoleCapacity];
  ArFontResourceId primary;
  ArFontResourceId fallbacks[kArTextFontMaximumFallbacks];
  size_t fallback_count;
} ArTextFontRole;

static inline bool ArTextFontRoles_Valid(const ArTextFontRole *roles,
                                         size_t count) {
  if (count > kArTextFontMaximumRoles || (count && !roles))
    return false;
  for (size_t i = 0; i < count; ++i) {
    const ArTextFontRole *role = &roles[i];
    if (!role->name[0] || !memchr(role->name, 0, sizeof(role->name)) ||
        !strcmp(role->name, "body") || !role->primary ||
        role->fallback_count > kArTextFontMaximumFallbacks)
      return false;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(role->name, roles[j].name))
        return false;
    for (size_t j = 0; j < role->fallback_count; ++j)
      if (!role->fallbacks[j])
        return false;
  }
  return true;
}

static inline bool ArTextFontRoles_Equal(const ArTextFontRole *a,
                                         size_t a_count,
                                         const ArTextFontRole *b,
                                         size_t b_count) {
  if (a_count != b_count || !ArTextFontRoles_Valid(a, a_count) ||
      !ArTextFontRoles_Valid(b, b_count))
    return false;
  for (size_t i = 0; i < a_count; ++i) {
    if (strcmp(a[i].name, b[i].name) || a[i].primary != b[i].primary ||
        a[i].fallback_count != b[i].fallback_count)
      return false;
    for (size_t j = 0; j < a[i].fallback_count; ++j)
      if (a[i].fallbacks[j] != b[i].fallbacks[j])
        return false;
  }
  return true;
}

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
