#include "diorama_snapshot_capture.h"
#include "diorama/diorama_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  bool initialized, done;
  const char *path;
  unsigned after, frames;
  uint64_t timestamp;
  uint8_t *owned;
  uint32_t mask;
  int skybox_width, skybox_height;
  DioramaSnapshotImage images[kDioramaSnapshotImageCount];
} s_capture;

void DioramaSnapshotCapture_Reset(void) {
  free(s_capture.owned);
  s_capture.owned = NULL;
  memset(s_capture.images, 0, sizeof(s_capture.images));
}

void DioramaSnapshotCapture_Retain(const FrameSlot *slot,
    const uint8_t *const *pixels, const size_t *pitches, uint32_t mask,
    const SrPpuSurfaceView *skybox) {
  if (!s_capture.initialized) {
    s_capture.initialized = true;
    s_capture.path = getenv("AR_DIORAMA_SNAPSHOT");
    const char *after = getenv("AR_DIORAMA_SNAPSHOT_AFTER");
    s_capture.after = after ? (unsigned)strtoul(after, NULL, 10) : 120;
  }
  if (!s_capture.path || !s_capture.path[0] || s_capture.done ||
      ++s_capture.frames < s_capture.after) return;
  s_capture.done = true;
  /* Capturing raw endpoints with generated view offsets would lie about the
   * displayed image. A later format can carry both endpoints/motion data. */
  if (slot->interp_setting_enabled) {
    fprintf(stderr, "[diorama-snapshot] disable frame generation for capture\n");
    return;
  }
  s_capture.owned = calloc(kDioramaSnapshotImageCount, 640u * 352u * 4u);
  if (!s_capture.owned) return;
  s_capture.timestamp = slot->timestamp_ns;
  s_capture.mask = mask;
  for (int i = 0; i < kDioramaSnapshotImageCount; i++) {
    uint8_t *dst = s_capture.owned + (size_t)i * 640 * 352 * 4;
    s_capture.images[i] = (DioramaSnapshotImage){dst, 640 * 4};
    if (i == kDioramaPlane_Count) {
      s_capture.skybox_width = skybox ? (slot->diorama_skybox_periodic ? 256 : 640) : 0;
      s_capture.skybox_height = skybox ? (slot->diorama_skybox_periodic ? 256 : 352) : 0;
      if (!skybox) continue;
      s_capture.images[i].pitch = (size_t)s_capture.skybox_width * 4;
      if (skybox->width_pixels > (unsigned)s_capture.skybox_width ||
          skybox->height_pixels > (unsigned)s_capture.skybox_height) {
        DioramaSnapshotCapture_Reset();
        return;
      }
      for (unsigned y = 0; y < skybox->height_pixels; y++)
        memcpy(dst + y * s_capture.images[i].pitch,
            skybox->data + (size_t)y * skybox->pitch_bytes, skybox->width_pixels * 4);
    } else if ((mask & (1u << i)) && pixels[i]) {
      DioramaPlaneCaptureRegion region;
      if (!DioramaPlaneCaptureRegion_Resolve(i,
          slot->snes_width + slot->obj_apron * 2,
          slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
          slot->obj_apron, slot->diorama_bg_apron_mask, &region)) continue;
      for (int y = 0; y < region.height; y++)
        memcpy(dst + ((size_t)y * 640 + region.x) * 4,
            pixels[i] + (size_t)y * pitches[i] + region.x * 4,
            (size_t)region.width * 4);
    }
  }
}

void DioramaSnapshotCapture_Write(const FrameSlot *slot,
    const DioramaCapture *capture, const DioramaView *view,
    const DioramaScene *scene) {
  if (!s_capture.owned) return;
  DioramaSnapshot snapshot;
  bool ok = slot->timestamp_ns == s_capture.timestamp &&
      DioramaSnapshot_Describe(&snapshot, capture, view, scene, s_capture.mask,
          s_capture.skybox_width, s_capture.skybox_height);
  uint8_t *packet = ok ? malloc(kDioramaSnapshotCapacity) : NULL;
  size_t size = 0;
  ok = packet && DioramaSnapshot_Encode(&snapshot, s_capture.images, packet,
      kDioramaSnapshotCapacity, &size);
  if (ok) {
    FILE *f = fopen(s_capture.path, "wb");
    ok = f != NULL;
    if (f) {
      ok = fwrite(packet, 1, size, f) == size;
      if (fclose(f)) ok = false;
    }
  }
  fprintf(stderr, "[diorama-snapshot] %s %s (%zu bytes; base composition only)\n",
      ok ? "wrote" : "could not capture", s_capture.path, size);
  free(packet);
  DioramaSnapshotCapture_Reset();
}
