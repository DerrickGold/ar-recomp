#ifndef AR_DIORAMA_SNAPSHOT_H
#define AR_DIORAMA_SNAPSHOT_H
/* Portable compositor fixture, deliberately separate from gameplay snapshots.
 * It captures resolved base layers, not actor simulation, HUD or effect callbacks.
 * Decoded pixels borrow the immutable packet; no handles/pointers cross the wire. */
#include "diorama.h"

enum {
  kDioramaSnapshotVersion = 1,
  kDioramaSnapshotHeaderBytes = 4096,
  kDioramaSnapshotWidth = 640,
  kDioramaSnapshotHeight = 352,
  kDioramaSnapshotImageCount = kDioramaPlane_Count + 1,
  kDioramaSnapshotCapacity = kDioramaSnapshotHeaderBytes +
      kDioramaSnapshotImageCount * 640 * 352 * 4,
};

typedef struct DioramaSnapshot {
  DioramaCapture capture;
  DioramaView view;
  DioramaScene scene;
  DioramaRenderOptions options;
  DioramaResolvedLayer layers[kDioramaPlane_Count];
  int layer_count;
  uint32_t plane_mask;
  int skybox_width, skybox_height;
  DioramaSkyboxView skybox;
  DioramaBgValidSpanPlan spans;
  bool has_spans, has_coverage;
  bool fill_configured[2];
  uint32_t fill_argb[2];
  ArRenderPointF offsets[kDioramaPlane_Count];
  DioramaCoverageMask coverage[kDioramaPlane_Count];
  /* Canonical RGBA bytes, always top row first. Last image is skybox. */
  const uint8_t *rgba[kDioramaSnapshotImageCount];
  ArRenderTexture textures[kDioramaPlane_Count];
} DioramaSnapshot;

typedef struct DioramaSnapshotImage {
  const uint8_t *argb; /* Host-endian ARGB8888 words, copied during Encode. */
  size_t pitch;
} DioramaSnapshotImage;

/* Populate metadata while retaining no borrowed live structures. Named ROM
 * replacements and dynamic BG2/skybox inputs are rejected. The native capture
 * hook rejects frame generation for every plane. No artwork is substituted. */
bool DioramaSnapshot_Describe(DioramaSnapshot *out,
    const DioramaCapture *capture, const DioramaView *view,
    const DioramaScene *scene, uint32_t plane_mask,
    int skybox_width, int skybox_height);
/* The output is untouched on failure; size is zero on failure. All images
 * describe fixed 640x352 allocations, except the explicit skybox extent. */
bool DioramaSnapshot_Encode(const DioramaSnapshot *snapshot,
    const DioramaSnapshotImage images[kDioramaSnapshotImageCount],
    uint8_t *packet, size_t capacity, size_t *size);
/* Invalid packets leave out untouched. Packet must outlive the decoded view. */
bool DioramaSnapshot_Decode(const uint8_t *packet, size_t size,
                            DioramaSnapshot *out);
/* Rebind internal pointers after a struct move, or texture creation. */
void DioramaSnapshot_Bind(DioramaSnapshot *snapshot);
bool DioramaSnapshot_Upload(DioramaSnapshot *snapshot, ArRenderDevice *device);
void DioramaSnapshot_ReleaseTextures(DioramaSnapshot *snapshot,
                                     ArRenderDevice *device);
#endif
