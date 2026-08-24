#ifndef DIORAMA_DEPTH_SHAPES_H
#define DIORAMA_DEPTH_SHAPES_H

#include <stdbool.h>

/* Pure, renderer-independent geometry for authored diorama depth shapes.
 * Defaults and grammar belong to diorama_layer_order.h; design tradeoffs are
 * documented in docs/diorama-depth-shapes.md. */

/* One skirt vertex. t runs from the bottom-edge fold (0) to the near lip (1);
 * z_bottom already includes any rake. */
void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade);

/* Shade multiplier at the near lip of a skirt (t == 1). Exposed so the value is
 * named once rather than duplicated between the geometry and its test. */
float DioramaSkirtNearShade(void);

/* Depth of a captured-plane row. t is its normalized top-to-bottom position;
 * rake is linear and bow is eased quadratically. */
float DioramaTiltedRowDepth(float z_world, float rake, float bow, float t);

/* Presentation-only continuation based on the stable authentic source band.
 * Returned values are capture/texture rows, not normalized UVs. */
typedef struct DioramaVerticalRepeatPlan {
  int source_y0, source_y1;
  int fold_y;
  int repeat_height;
} DioramaVerticalRepeatPlan;

bool DioramaVerticalRepeatPlan_Build(
    int authentic_y0, int authentic_height,
    int capture_height, int texture_height,
    DioramaVerticalRepeatPlan *out);

/* Folded continuation shared by mesh and effect projection. t spans the
 * authentic fold to the near lip; overlap_t preserves a coplanar handoff. */
void DioramaOverflowFoldPoint(
    float t, float y_top, float z_top, float z_handoff,
    float overflow_height, float overlap_t,
    float front_z, float front_drop,
    float *out_y, float *out_z);

/* Mesh-row parameterization for a folded overflow. Row 0 is the authentic
 * fold and row 1 is the exact end of the coplanar overlap; the remaining rows
 * subdivide only the curved section. A uniform grid cannot generally contain
 * that boundary and will start pulling the shared edge forward before the host
 * plane ends, producing a camera-dependent crack. */
float DioramaOverflowFoldRowT(int row, int subdivisions, float overlap_t);

/* Depth, shade, and opacity for one parallel copy. solid selects voxel-style
 * uniform falloff; ordinary stacks fade with distance from the source plane. */
void DioramaStackCopyShaped(int index, int copies, float z_base, float depth,
                            int direction, bool solid, float *out_z,
                            float *out_shade, float *out_alpha);

/* True when this copy coincides with the plane's OWN depth, so the caller must
 * skip it -- the plane's existing draw already covers it, and drawing both would
 * double-darken the front face at that depth.
 *
 * Not simply "index == 0": that only holds for a one-sided fill. A centred (Both)
 * stack puts index 0 at the FAR edge, and its redundant copy is the middle one --
 * which exists only for an ODD count. Keeping this rule here rather than at the
 * call site is what stops the two from disagreeing. */
bool DioramaStackCopyIsRedundant(int index, int copies, int direction);
#endif /* DIORAMA_DEPTH_SHAPES_H */
