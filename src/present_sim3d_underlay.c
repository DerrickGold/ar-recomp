#include "present_sim3d_underlay.h"

#include <stdio.h>
#include <string.h>
#include "sim/sim_world_map.h"
#include "sim/sim3d_performance.h"

enum {
  /* Same four-by-four box filter as the original distant-map defocus. */
  kBlurDivisor = 4,
  kBlurPixels = kSimWorldMapPixels / kBlurDivisor,
};

static struct {
  ArRenderTexture sharp, blurred;
  uint32_t sharp_serial, blur_serial;
  bool sharp_unavailable, blur_unavailable;
  uint32_t pixels[kBlurPixels * kBlurPixels];
} s_underlay;

static bool CreateTexture(ArRenderDevice *device, bool blur, ArRenderTexture *texture) {
  const ArRenderTextureDesc desc = {
    .width = blur ? kBlurPixels : kSimWorldMapPixels,
    .height = blur ? kBlurPixels : kSimWorldMapPixels,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = blur ? kArRenderFilter_Linear : kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  return ArRenderDevice_CreateTexture(device, &desc, texture);
}

SimUnderlayTextures PresentSim3DUnderlay_Prepare(
    ArRenderDevice *device, uint32_t serial, bool want_blur) {
  SimUnderlayTextures result = {0};
  if (!serial || s_underlay.sharp_unavailable) return result;
  if (!ArRenderTexture_IsValid(s_underlay.sharp) &&
      !CreateTexture(device, false, &s_underlay.sharp)) {
    s_underlay.sharp_unavailable = true;
    fprintf(stderr, "[sim3d-underlay] world map texture unavailable: %s\n",
            ArRenderDevice_LastError(device));
    return result;
  }
  if (s_underlay.sharp_serial != serial) {
    const uint32_t *pixels = SimWorldMap_BakedPixels();
    if (!pixels || !ArRenderDevice_UpdateTexture(device, s_underlay.sharp, NULL,
            pixels, kSimWorldMapPixels * (int)sizeof(*pixels))) {
      /* A failed upload may have partially modified the resource. Never
       * expose it as either the old or new generation until a full retry. */
      s_underlay.sharp_serial = 0;
      return result;
    }
    s_underlay.sharp_serial = serial;
    Sim3DPerformance_AddUpload((uint64_t)kSimWorldMapPixels * kSimWorldMapPixels * sizeof(*pixels));
  }
  result.sharp = s_underlay.sharp;
  if (!want_blur || s_underlay.blur_unavailable) return result;
  if (!ArRenderTexture_IsValid(s_underlay.blurred) &&
      !CreateTexture(device, true, &s_underlay.blurred)) {
    s_underlay.blur_unavailable = true;
    return result;
  }
  if (s_underlay.blur_serial != serial) {
    /* Reads the owned CPU image, never a write-only streaming-texture map. */
    if (!SimWorldMap_Downsample(s_underlay.pixels, kBlurPixels, kBlurDivisor) ||
        !ArRenderDevice_UpdateTexture(device, s_underlay.blurred, NULL,
            s_underlay.pixels, kBlurPixels * (int)sizeof(uint32_t))) {
      ArRenderDevice_DestroyTexture(device, s_underlay.blurred);
      s_underlay.blurred = ArRenderTexture_Invalid();
      s_underlay.blur_serial = 0;
      s_underlay.blur_unavailable = true;
      return result;
    }
    s_underlay.blur_serial = serial;
    Sim3DPerformance_AddUpload((uint64_t)kBlurPixels * kBlurPixels * sizeof(uint32_t));
  }
  result.blurred = s_underlay.blurred;
  return result;
}

void PresentSim3DUnderlay_ResetResources(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_underlay.sharp);
  ArRenderDevice_DestroyTexture(device, s_underlay.blurred);
  memset(&s_underlay, 0, sizeof(s_underlay));
}
