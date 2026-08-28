#include "diorama_upload.h"

#include <limits.h>

#include "diorama_performance.h"
#include "presentation_upload_mirror.h"

static PresentationUploadMirror
    s_upload_mirrors[kDioramaPlane_Count];

DioramaUploadResult Diorama_Upload(
    ArRenderDevice *device,
    ArRenderTexture textures[kDioramaPlane_Count],
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    int snes_width, int snes_height, int obj_apron, uint32_t plane_mask) {
  DioramaUploadResult upload = {0};
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Upload);
  /* `snes_width` is the full surface width (display + both aprons); the pitch
   * always spans that width. Planes that cannot hold apron content upload
   * only their display columns because persistent textures are zero-filled at
   * creation. Skipping those known-zero columns saves about 47 MB/s at 60 Hz. */
  if (!ArRenderDevice_IsReady(device) || !textures || !pixels ||
      !pitch_bytes || snes_width <= 0 || snes_height <= 0 || obj_apron < 0 ||
      obj_apron > snes_width / 2) {
    DioramaPerformance_End(performance);
    return upload;
  }
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    /* Mode 1 never draws BG4; unlike the other primary/split planes it has no
     * persistent texture and is not part of the compositor layer table. */
    if (plane == SR_PPU_OVERLAY_BG4) continue;
    if (!(plane_mask & (1u << plane)) ||
        !ArRenderTexture_IsValid(textures[plane]) || !pixels[plane] ||
        pitch_bytes[plane] > INT_MAX)
      continue;
    DioramaPlaneCaptureRegion region;
    if (!DioramaPlaneCaptureRegion_Resolve(
            plane, snes_width, snes_height, obj_apron, &region))
      continue;
    const uint8_t *source = pixels[plane] +
        (size_t)region.x * sizeof(uint32_t);
    PresentationUploadResult plane_upload = {0};
    const bool synchronized = PresentationUploadMirror_UploadArgb8888(
        &s_upload_mirrors[plane], device, textures[plane], source,
        region.width, region.height, (int)pitch_bytes[plane],
        region.x, 0, &plane_upload);
    DioramaPerformance_AddPlaneSync(
        synchronized, synchronized && plane_upload.changed,
        plane_upload.uploaded_bytes);
    if (!synchronized) continue;
    upload.synchronized_plane_mask |= 1u << plane;
    if (plane_upload.changed) upload.changed_plane_mask |= 1u << plane;
  }
  DioramaPerformance_End(performance);
  return upload;
}

void DioramaUpload_Reset(void) {
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    PresentationUploadMirror_Reset(&s_upload_mirrors[plane]);
}
