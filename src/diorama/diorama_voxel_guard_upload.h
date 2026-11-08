#ifndef AR_DIORAMA_VOXEL_GUARD_UPLOAD_H
#define AR_DIORAMA_VOXEL_GUARD_UPLOAD_H

#include "diorama_voxel_guard.h"
#include "present/presentation_upload_mirror.h"

struct ActionVoxelGeometry;
struct ActionRoomSceneFrameState;

enum {
  kDioramaVoxelGuardPrefixRows = 16,
  kDioramaVoxelGuardTextureHeight = 16 + kDioramaVoxelGuardRows,
  kDioramaVoxelGuardTexturePixels = kDioramaVoxelGuardWidth * kDioramaVoxelGuardTextureHeight
};
/* The final 16 already-resolved primary rows supply sampling overlap. No
 * extra scanout or source decode is implied. NULL bands are known empty. */
typedef struct DioramaVoxelGuardPrefix {
  const uint32_t *pixels[3];
  size_t pitch_pixels[3];
  int width;
  uint64_t capture_generation;
} DioramaVoxelGuardPrefix;

typedef struct DioramaVoxelGuardView {
  DioramaVoxelGuardLayout layout;
  /* Same bindings as the primary: split(Low).rear, native High,
   * split(Low).foreground. Native Far remains an independent input and is
   * intentionally unused by this existing generic Voxel binding policy. */
  ArRenderTexture textures[3];
  uint64_t capture_generation, source_revision;
} DioramaVoxelGuardView;

typedef struct DioramaVoxelGuardUpload {
  DioramaVoxelGuardView view;
  uint32_t *pixels[3];
  PresentationUploadMirror mirrors[3];
  const uint32_t *source_pixels[3];
  uint16_t fixed_color;
  uint8_t brightness, cgwsel, cgadsub;
  bool unavailable;
} DioramaVoxelGuardUpload;

/* Missing/unsupported source clears publication without touching the primary.
 * Textures belong to the present thread; no borrowed source is read at draw. */
bool DioramaVoxelGuardUpload_Update(DioramaVoxelGuardUpload *, ArRenderDevice *,
    const struct ActionVoxelGeometry *, const DioramaVoxelGuardSource *,
    const struct ActionRoomSceneFrameState *, const DioramaVoxelGuardPrefix *);
void DioramaVoxelGuardUpload_Reset(DioramaVoxelGuardUpload *, ArRenderDevice *);
#endif
