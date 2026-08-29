#ifndef DIORAMA_UPLOAD_H
#define DIORAMA_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "diorama_planes.h"
#include "diorama_coverage.h"
#include "render/render_device.h"

/* Synchronize the requested captured planes into persistent backend textures.
 * The producer can release pixels as soon as this returns. Arrays are indexed
 * by kDioramaPlane_*; invalid textures and NULL pixels are skipped.
 * `plane_mask` is the immutable request/content intersection. A failed plane
 * is omitted from synchronized_plane_mask so no stale texture can resurface;
 * changed_plane_mask identifies its subset that required a backend upload.
 * `snes_width` includes both resolve aprons, while `obj_apron` identifies the
 * per-side columns that non-OBJ planes can omit as known-zero padding. */
typedef struct DioramaUploadResult {
  uint32_t synchronized_plane_mask;
  uint32_t changed_plane_mask;
  DioramaCoverageMask coverage_masks[kDioramaPlane_Count];
} DioramaUploadResult;

DioramaUploadResult Diorama_Upload(
    ArRenderDevice *device,
    ArRenderTexture textures[kDioramaPlane_Count],
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    int snes_width, int snes_height, int obj_apron, uint32_t plane_mask);

/* Forget retained upload hashes after a render-device reset or shutdown. */
void DioramaUpload_Reset(void);

#endif /* DIORAMA_UPLOAD_H */
