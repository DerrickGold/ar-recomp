#ifndef AR_REGIONAL_MEDIA_FILES_H
#define AR_REGIONAL_MEDIA_FILES_H
/* ArHostRegionalMediaFiles: loads the regional media donor files at startup
 * and serves them to the regional media core as immutable borrowed views; the
 * core never sees a path.
 * Phase: host (startup).
 * Tests: tests/regional_media_test.c */

#include "regional/media/regional_media.h"

typedef struct ArHostRegionalMediaFiles {
  void *implementation;
} ArHostRegionalMediaFiles;

/* Startup-owned immutable assets. Duplicate donor registration is rejected,
 * not replaced under existing borrowed views. Destroy only after consumers
 * have detached, normally at process shutdown. No global paths in the core. */
bool ArHostRegionalMediaFiles_Load(ArHostRegionalMediaFiles *host, const char *path,
                                   ArRegionalMediaRelease expected, char *error, size_t capacity);
const ArRegionalMediaView *ArHostRegionalMediaFiles_View(
    const ArHostRegionalMediaFiles *host,ArRegionalMediaRelease release);
void ArHostRegionalMediaFiles_Destroy(ArHostRegionalMediaFiles *host);

#endif
