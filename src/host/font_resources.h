#ifndef AR_HOST_FONT_RESOURCES_H
#define AR_HOST_FONT_RESOURCES_H

#include "localization/font_resource.h"

/* Desktop file-backed provider. No renderer or game code opens font files.
 * Zero-initialize the store. At most 64 resources / 256 MiB, including retired
 * resources pinned by backends. Register reads an immutable snapshot, bounded
 * at 64 MiB. Retire ends registration ownership and disallows new acquisitions;
 * existing leases remain usable. Destroy refuses live leases, without changes.
 * Same-thread API. Destroy the presenter and interface backends before store. */
typedef struct ArHostFontResources { void *implementation; } ArHostFontResources;

ArFontResources ArHostFontResources_Provider(ArHostFontResources *store);
ArFontResourceId ArHostFontResources_RegisterFile(
    ArHostFontResources *store, const char *path, char *error, size_t capacity);
void ArHostFontResources_Retire(ArHostFontResources *store, ArFontResourceId id);
bool ArHostFontResources_Destroy(ArHostFontResources *store);

#endif
