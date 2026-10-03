#ifndef AR_DIORAMA_SCENE_PASS_H
#define AR_DIORAMA_SCENE_PASS_H

/* Source-space action composition. Unlike ArRenderVertex2D, source is in
 * display-capture pixels, before plane projection and generated-frame motion.
 * This contract contains no native handles; adapters bind resident resources
 * separately. All borrowed arrays are consumed during Encode, never retained.
 * The existing compositor remains the production/reference implementation
 * while its materials and effect builders migrate to this contract. */
#include "diorama.h"

enum {
  kDioramaSceneMaximumDraws = 128,
  kDioramaSceneMaximumVertices = 131072,
  kDioramaSceneMaximumIndices = 393216,
};

typedef struct DioramaSceneVertex {
  ArRenderPointF source;
  ArRenderColorF color;
  /* Normalized coordinates within the bound source rectangle, not the atlas. */
  ArRenderPointF uv;
} DioramaSceneVertex;

typedef struct DioramaSceneDraw {
  const DioramaSceneVertex *vertices;
  const int32_t *indices;
  unsigned vertex_count, index_count;
  const DioramaProjection *view;
  /* Exactly one mapping is selected. A skybox band is already resolved from
   * the room's row policy; static-anchor effects keep their own source band. */
  DioramaPlaneProjection plane;
  DioramaSkyboxBandProjection skybox;
  bool use_skybox;
  bool textured;
  /* Exact texel handoffs use point sampling; scene materials default linear. */
  bool nearest;
  ArRenderBlendMode blend;
  /* -1 means no GPU motion. Otherwise this is a global-motion result slot
   * (0..7). Rejected motion uses zero displacement without any CPU readback.
   * Fixed plane meshes use -1; their attached source-space effects use the
   * matching slot. Already-warped textures do not move their plane mesh.
   * With a slot selected, plane.capture_offset must be zero: copying an
   * already generated CPU projection would apply the same motion twice. */
  int motion_slot;
  bool clip_enabled;
  ArRenderRectF clip; /* Finite capture-space rectangle, after motion. */
} DioramaSceneDraw;

#endif
