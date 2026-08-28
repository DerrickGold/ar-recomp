#ifndef SIM3D_DEPTH_PASS_H
#define SIM3D_DEPTH_PASS_H

#include <stdbool.h>

#include "render/render_device.h"

typedef enum Sim3DDepthPassLayer {
  /* Invisible terrain geometry is submitted first and writes only depth.  The
   * textured town ground remains in the SDL color pass, while this layer lets
   * the same hills and cliff skirts reject solid models hidden behind them. */
  kSim3DDepthPass_DepthOccluder,
  kSim3DDepthPass_Solid,
  kSim3DDepthPass_Mountain,
  /* Samples the accumulated screen-space shadow mask on the exact terrain
   * top mesh. It tests against opaque depth without writing it, which clips
   * shadows at ridges, cliff lips, buildings and bridge geometry. */
  kSim3DDepthPass_ShadowReceiver,
  /* Transparent world effects are submitted after all opaque geometry. They
   * still test against the shared depth target, but use a no-depth-write
   * pipeline so smoke/glow cannot punch transparent holes through mountains. */
  kSim3DDepthPass_Effect,
  kSim3DDepthPassLayerCount,
} Sim3DDepthPassLayer;

typedef struct Sim3DDepthVertex {
  float x, y;
  float depth;
  ArRenderColorF color;
  ArRenderPointF uv;
} Sim3DDepthVertex;

/* Creates the shaders/pipeline and verifies D32 support. Call during video
 * startup so an unsupported backend is a launch error, never a missing-scene
 * fallback discovered after entering SIM mode. */
bool Sim3DDepthPass_Require(ArRenderDevice *device);

/* A viewport-sized, transparent color target paired with a real D32 depth
 * attachment. Geometry is collected by material so texture changes cost a
 * handful of draws; draw order inside and between those groups is resolved by
 * the GPU depth test, not by backend painter ordering. */
bool Sim3DDepthPass_Begin(ArRenderDevice *device, int width, int height,
                          ArRenderFilter output_filter);
/* Ordinary backend textures are not necessarily valid sampling resources for
 * a backend's custom depth pipeline. Upload the immutable mountain cutout
 * atlas into pass-owned storage instead. */
bool Sim3DDepthPass_UploadMountainAtlas(ArRenderDevice *device,
                                        const uint32_t *argb_pixels,
                                        int width, int height, int pitch);
bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex vertices[4]);
/* Submits all collected layers. shadow_texture is required only when a
 * ShadowReceiver quad was appended; pass an invalid handle for the ordinary
 * solid pass. */
ArRenderTexture Sim3DDepthPass_Submit(
    ArRenderDevice *device, ArRenderTexture shadow_texture);
bool Sim3DDepthPass_IsCollecting(void);
const char *Sim3DDepthPass_LastError(void);
void Sim3DDepthPass_Reset(ArRenderDevice *device);

#endif  /* SIM3D_DEPTH_PASS_H */
