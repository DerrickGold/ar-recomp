#ifndef AR_DIORAMA_DEPTH_PASS_H
#define AR_DIORAMA_DEPTH_PASS_H

#include "render/render_types.h"

/* Shared visibility for action terrain and native sprite props. This is a
 * raster contract, not a room or tile policy. The caller supplies meshes in
 * native priority order: equal depth lets the later fragment win; a physically
 * nearer fragment wins regardless of that order. Transparent texels neither
 * color nor occlude. Actors, walking tops and translucent effects still need
 * an explicit caller policy; do not infer it from a texture or tile number.
 *
 * Clip Z is in [0,W], with smaller values nearer. Preserve W for perspective
 * materials, or use W=1 with projected XY/Z to retain the existing compositor's
 * affine texture interpolation. Never substitute a face's average distance.
 * All arrays are borrowed for Encode only. No platform handles live here. */
enum {
  kDioramaDepthMaximumDraws = 256,
  kDioramaDepthMaximumVertices = 131072,
  kDioramaDepthMaximumIndices = 393216,
  /* A hybrid scope ends at a render-order boundary. Its storage may grow
   * beyond the legacy upload budget without changing how alpha is resolved. */
  kDioramaDepthHybridMaximumVertices = 4 * kDioramaDepthMaximumVertices,
  kDioramaDepthHybridMaximumIndices = 4 * kDioramaDepthMaximumIndices,
  kDioramaDepthHybridMaximumDraws = kDioramaDepthHybridMaximumIndices / 3,
};

typedef struct DioramaDepthVertex {
  float clip[4];
  ArRenderColorF color;
  ArRenderPointF uv;
  ArRenderPointF heat_world_xy;
} DioramaDepthVertex;

typedef enum DioramaDepthMode {
  /* Opaque/cutout world surfaces. Fractional alpha is edge coverage, not a
   * transmissive material: a nonempty texel writes depth. */
  kDioramaDepth_Write,
  /* Translucent effects test the world but cannot hide subsequent surfaces.
   * The caller orders translucent draws back to front, after opaque geometry. */
  kDioramaDepth_Read,
  /* HUD or an explicitly ordered overlay; never modifies world depth. */
  kDioramaDepth_Disabled,
  /* Native actors keep their authored draw slot, including walking-top and
   * foreground priority. Covered actor pixels replace earlier color/depth;
   * depth becomes far, allowing later native foreground surfaces to cover
   * the actor. Holes retain the world's existing depth. Modeled props never
   * use this mode: their visibility remains geometric across draw slots. */
  kDioramaDepth_NativeActor,
  kDioramaDepth_Count,
} DioramaDepthMode;

typedef struct DioramaDepthDraw {
  const DioramaDepthVertex *vertices;
  const int32_t *indices;
  unsigned vertex_count, index_count;
  DioramaDepthMode depth_mode;
  /* Opaque, Alpha or Add. World Add is read-only; NativeActor Add retains the
   * explicit native-priority depth reset, independent of color blending. */
  ArRenderBlendMode blend;
  bool textured, nearest;
  bool cutout; /* Nearest binary source, unit vertex alpha and no alpha filtering. */
  bool clip_enabled;
  ArRenderRectI clip; /* Target pixels, independent of camera projection. */
} DioramaDepthDraw;

struct ArRenderDevice;
struct DioramaSceneryLightEffectParams;
typedef struct DioramaDepthTextureBank {
  ArRenderTexture additional[2];
  const uint8_t *slots; /* 0 selects the draw texture, 1–2 select additional[]. */
} DioramaDepthTextureBank;
/* Bridge from the existing material/exposure compositor. Positions are local
 * viewport pixels; depths use the same projection's [0,1] GPU mapping. UVs,
 * color and depth have already undergone the same triangle clipping. The
 * receiver owns material binding/submission, borrowing everything for this
 * call only. The current render target/viewport and native draw order remain
 * authoritative. Native actor planes are marked separately from 3D props in
 * an OBJ slot so a backend can preserve actor/walking-top priority policy. */
typedef struct DioramaDepthSubmission {
  ArRenderTexture texture;
  const ArRenderVertex2D *vertices;
  const float *depths;
  const int32_t *indices;
  int vertex_count, index_count, plane;
  bool native_actor;
  bool read_only_depth; /* Translucent 3D effects test against the world without blocking it. */
  bool binary_texture_alpha; /* The caller verified every sampled texel. */
  const DioramaDepthTextureBank *texture_bank; /* Optional nearest world sources. */
  ArRenderBlendMode blend;
  const struct DioramaSceneryLightEffectParams *lighting;
  /* Optional homogeneous W for projected generated-world faces. XY and
   * depth remain viewport pixels/[0,1]; the sink restores clip XYZ*W so UVs
   * stay attached to the surface under rotation. NULL preserves the native
   * compositor's affine mapping. Native actors always use NULL. Any clipping
   * before this boundary must also preserve the corresponding vertex W. */
  const float *clip_w;
  /* Optional transformed native-world receiver XY, independent of sample UV.
   * Owned artwork supplies this only with lighting.heat_owned enabled. NULL
   * retains the existing capture/guard UV mapping. Borrowed for submit only. */
  const ArRenderPointF *heat_world_xy;
} DioramaDepthSubmission;

typedef struct DioramaDepthSink {
  bool (*submit)(void *userdata, struct ArRenderDevice *, const DioramaDepthSubmission *);
  void *userdata;
  bool texture_banks;
  /* A native color-math target was cleared and begins a new visibility group.
   * The backend invalidates depth for the currently bound target only. */
  bool (*clear)(void *userdata, struct ArRenderDevice *);
  /* Called once when the composite exits, including early failure. Complete
   * queued draws on success, discard them on failure, and release any scoped
   * backend registration. A failed finish makes the composite unusable. */
  bool (*finish)(void *userdata, struct ArRenderDevice *, bool success);
  /* Publish ordered draws when a logical texture update reused identical
   * pixels. Keeps the existing depth of the active target. */
  bool (*flush)(void *userdata, struct ArRenderDevice *);
} DioramaDepthSink;

#endif
