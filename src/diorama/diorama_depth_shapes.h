#ifndef DIORAMA_DEPTH_SHAPES_H
#define DIORAMA_DEPTH_SHAPES_H

#include <stdbool.h>

/* Pure per-vertex and per-copy arithmetic for the diorama's DEPTH SHAPES -- the
 * ways a captured 2D layer can be given extent in Z instead of being an
 * infinitely thin sheet parallel to the screen.
 *
 * There are five, and they exist because two parallel planes at different depths
 * leave a visible void once the diorama camera tilts: you see past the near
 * plane's bottom edge into the gap behind it. Fillmore act 2 is the reported case
 * (water at Bg2Hi z=0.21 floating behind a hole, rock path at Bg1 z=0.50).
 *
 *   rake   tilt the plane linearly.  DioramaTiltedRowDepth
 *   bow    tilt it on an eased curve. DioramaTiltedRowDepth
 *   thick  extrude a near face from its bottom edge. DioramaSkirtVertex
 *   stack  repeat it at parallel depths, faded. DioramaStackCopyShaped
 *   voxel  repeat it densely and unfaded, so it reads solid. DioramaStackCopyShaped
 *
 * WHY THESE LIVE IN A PURE MODULE. diorama.c's mesh builders are `static` and
 * unreachable from any test, so geometry written inside them would ship with no
 * coverage at all. Keeping the arithmetic here means every shape's contract is
 * asserted without a ROM, a renderer, or a window -- and diorama.c calls these
 * rather than holding a second copy of each formula, so the two cannot drift.
 * Same precedent as diorama_skybox_uv.c and actraiser_ws_gap.c.
 *
 * The DEFAULTS, CAPS and manifest grammar for these shapes live in
 * diorama_layer_order.h, which owns the per-room override table; this file owns
 * only the arithmetic. Split deliberately: the table is about authoring and
 * precedence, this is about geometry.
 */

/* THICKNESS: one vertex of the extruded near face ("skirt") below a layer.
 *
 * A thickness extrudes the plane's BOTTOM edge forward from `z` to
 * `z + thickness`, so the layer reads as a block with a near face instead of an
 * infinitely thin sheet. This is the per-vertex arithmetic, extracted pure so the
 * geometry is testable without a renderer -- the mesh assembly and projection stay
 * in diorama.c (precedent: diorama_skybox_uv.c).
 *
 * `t` is 0 at the fold (the plane's bottom edge) and 1 at the near lip.
 * `z_bottom` is the plane's bottom-edge depth -- z + rake, NOT z, so a room that
 * authors both a rake and a thickness does not tear at the fold.
 * `y_bottom` is the plane's bottom-edge world Y.
 *
 * The face drops in Y as it comes forward in Z, at half the thickness, so a thick
 * layer reads as a block rather than a tall wall. `out_shade` is a multiplier that
 * darkens with depth: a real near face turned away from the light is not the same
 * brightness as the top surface, and without the gradient the skirt reads as a
 * smear of the plane's bottom row rather than a separate surface.
 *
 * The renderer calls this only for an authored positive thickness, normalized
 * t in [0,1], and non-null outputs. */
void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade);

/* Shade multiplier at the near lip of a skirt (t == 1). Exposed so the value is
 * named once rather than duplicated between the geometry and its test. */
float DioramaSkirtNearShade(void);

/* Depth of a captured-plane row. t is its normalized top-to-bottom position;
 * rake is linear and bow is eased quadratically. */
float DioramaTiltedRowDepth(float z_world, float rake, float bow, float t);

/* A finite captured plane can grow a presentation-only continuation by
 * repeating the authentic frame, which is the one vertical source interval
 * guaranteed to remain populated under every BG Extents policy. `fold_y` is
 * the first row after that authentic interval; drawing the repeat there lets
 * any authored bottom margin overlap and hide the handoff. The returned rows
 * are capture/texture pixel coordinates, not normalized UVs.
 *
 * Keeping this plan independent of a layer's mutable valid-span table is
 * deliberate: changing how much synthetic margin BG Extents exposes must not
 * move or disable separately-authored overflow geometry. */
typedef struct DioramaVerticalRepeatPlan {
  int source_y0, source_y1;
  int fold_y;
  int repeat_height;
} DioramaVerticalRepeatPlan;

bool DioramaVerticalRepeatPlan_Build(
    int authentic_y0, int authentic_height,
    int capture_height, int texture_height,
    DioramaVerticalRepeatPlan *out);

/* FOLDED OVERFLOW: continue a captured plane through an overlap, then turn it
 * toward the front of the diorama. This is used by the Aitos waterfall, but
 * the arithmetic is presentation-generic so geometry and effect projection
 * can share one contract.
 *
 * `t` runs from 0 at the authentic-band fold to 1 at the continuation's near
 * lip. Through `overlap_t`, Y continues down the host plane and Z interpolates
 * to the host's drawable-bottom depth. That coplanar interval lets the host
 * cover the repeated texture handoff without a crack. After the overlap, a
 * smooth bend trades the would-be vertical continuation for forward Z travel;
 * only `front_drop` additional Y is retained, making the surface read as water
 * curling over the box edge instead of a second parallel billboard.
 *
 * The renderer supplies normalized/clamped authoring inputs and non-null
 * outputs. The function still clamps t so a projected particle one sample past
 * the mesh cannot escape the physical continuation. */
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

/* STACK: depth, shade and opacity of one parallel copy. Forward lays copies
 * from z_base toward z_base + depth, Backward mirrors that, and Both centres
 * the fill on the plane. Shade and alpha fall off with distance from the plane,
 * not signed depth. `solid` selects a VOXEL fill with the same depth arithmetic
 * but uniform shade and alpha.
 *
 * The draw loop supplies copies > 1, an in-range index, positive depth, a valid
 * DioramaStackDirection and non-null outputs.
 *
 * A stack fades so the eye reads separate things receding into the distance. A
 * voxel must NOT fade, because it is one object with volume -- a falloff would
 * make its own back half look like fog. The copies still carry the layer's own
 * alpha, so a captured layer's transparent regions stay transparent and each art
 * island extrudes itself; that silhouette behaviour is the reason a voxel exists
 * rather than a thickness, whose skirt hangs from the quad's bottom edge.
 *
 * A slight uniform darkening is still applied so a voxel is distinguishable from
 * the flat layer it extrudes; it does not vary with depth. */
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
