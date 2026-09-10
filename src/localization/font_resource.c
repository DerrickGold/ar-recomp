#include "localization/font_resource.h"

#include <stdio.h>
#include <string.h>

bool ArFontResources_IsReady(const ArFontResources *resources) {
  return resources && resources->ops &&
      resources->ops->struct_size >= offsetof(ArFontResourceOps, release) +
                                      sizeof(resources->ops->release) &&
      resources->ops->abi_version == AR_FONT_RESOURCE_ABI_VERSION &&
      resources->ops->acquire && resources->ops->release;
}

void ArFontResource_Release(ArFontResourceLease *lease) {
  if (!lease) return;
  if (lease->owner.ops)
    lease->owner.ops->release(lease->owner.context, &lease->data);
  memset(lease, 0, sizeof(*lease));
}

bool ArFontResource_Acquire(ArFontResourceLease *lease,
                           const ArFontResources *resources,
                           ArFontResourceId id, char *error, size_t capacity) {
  if (error && capacity) error[0] = 0;
  if (!lease || !id || !ArFontResources_IsReady(resources)) {
    if (error && capacity) snprintf(error, capacity, "invalid font resource request");
    return false;
  }
  const ArFontResources owner = *resources;
  ArFontResourceData data = {0};
  const bool acquired = owner.ops->acquire(
      owner.context, id, &data, error, capacity);
  if (!acquired || !data.bytes || !data.size ||
      data.size > AR_FONT_RESOURCE_MAXIMUM_BYTES) {
    /* Providers may publish partial ownership even when acquisition fails. */
    if (acquired || data.bytes || data.token)
      owner.ops->release(owner.context, &data);
    if (error && capacity && !error[0])
      snprintf(error, capacity, "font resource unavailable or outside byte limit");
    return false;
  }
  ArFontResource_Release(lease);
  *lease = (ArFontResourceLease){.data = data, .owner = owner};
  return true;
}
