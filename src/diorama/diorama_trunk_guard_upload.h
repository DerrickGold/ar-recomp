#ifndef AR_DIORAMA_TRUNK_GUARD_UPLOAD_H
#define AR_DIORAMA_TRUNK_GUARD_UPLOAD_H
#include "diorama_trunk_guard.h"
#include "present/presentation_upload_mirror.h"

/* Borrowed, present-thread-owned texture view. No live/queued pixel pointers
 * survive upload. All three bands belong to this exact native generation. */
typedef struct DioramaTrunkGuardView {
  DioramaTrunkGuardLayout layout;
  ArRenderTexture textures[3]; /* native Low, High, Far */
  uint64_t capture_generation, source_revision;
} DioramaTrunkGuardView;

typedef struct DioramaTrunkGuardUpload {
  DioramaTrunkGuardView view;
  uint32_t *pixels[3];
  PresentationUploadMirror mirrors[3];
  const uint32_t *source_pixels;
  unsigned brightness;
} DioramaTrunkGuardUpload;

/* An absent source clears publication but retains bounded reusable resources.
 * Failure clears publication as well; callers must never draw stale guards.
 * Reset when the source owner/device is destroyed or recreated. */
bool DioramaTrunkGuardUpload_Update(DioramaTrunkGuardUpload *, ArRenderDevice *,
    const struct ActionTrunkAssets *, const DioramaTrunkGuardSource *, unsigned brightness);
void DioramaTrunkGuardUpload_Reset(DioramaTrunkGuardUpload *, ArRenderDevice *);
#endif
