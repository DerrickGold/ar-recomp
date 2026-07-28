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
 *   stack  repeat it at parallel depths, faded. DioramaStackCopy*
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
 * in diorama.c (precedent: this file's own DioramaInterpUvWindow, and
 * diorama_skybox_uv.c).
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
 * Clamps t to [0,1] and treats a non-positive thickness as zero, so a caller
 * cannot produce geometry above the fold or behind the plane. */
void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade);

/* Shade multiplier at the near lip of a skirt (t == 1). Exposed so the value is
 * named once rather than duplicated between the geometry and its test. */
float DioramaSkirtNearShade(void);

/* STACK: depth, shade and opacity of one copy in a stacked layer.
 *
 * A stack fills a depth gap by repeating the layer at intermediate depths instead
 * of tilting it. Tilting (rake) puts a single plane's own rows at different
 * depths, which gives that layer two different parallax rates within itself and
 * shears it as the camera moves. Every copy here stays exactly parallel, so each
 * has ONE depth and one parallax rate, and the layer keeps the flat poster-like
 * motion the diorama is built on.
 *
 * `index` is 0 for the copy nearest the plane's own depth and `copies - 1` for the
 * farthest into the gap; `copies` is clamped to >= 1. `depth` is the total fill,
 * laid from `z_base` toward `z_base + depth`.
 *
 * Copy 0 sits AT z_base -- deliberately, so it coincides with the plane the caller
 * already draws and the stack has no seam at its own front face. The caller
 * therefore skips index 0 and draws 1..copies-1 as extra passes.
 *
 * `out_alpha` and `out_shade` both fall off with distance into the gap so the
 * stack reads as receding volume rather than a smear of identical sprites. Alpha
 * never reaches 0 for a valid index, since an invisible copy is a wasted draw. */
void DioramaStackCopy(int index, int copies, float z_base, float depth,
                      float *out_z, float *out_shade, float *out_alpha);

/* Same, with a direction (a DioramaStackDirection, passed as int so this header
 * keeps no dependency on the override module).
 *
 * Forward lays copies from z_base toward z_base + depth, i.e. TOWARD the camera,
 * since higher z is nearer in this projection. That is the default and what the
 * reported Fillmore act 2 case needs -- its water sits behind the rock path, so
 * the gap to fill is on the camera side. Backward is the mirror, for a foreground
 * layer receding into the scene. Both centres the fill on the plane, for something
 * the plane sits in the middle of.
 *
 * Shade and alpha fall off with DISTANCE from the plane, not with signed depth, so
 * a Backward or Both stack recedes visually the same way a Forward one does rather
 * than brightening as it goes. */
void DioramaStackCopyDirected(int index, int copies, float z_base, float depth,
                              int direction, float *out_z, float *out_shade,
                              float *out_alpha);

/* Same again, with `solid` selecting a VOXEL fill: identical depth arithmetic, but
 * no shade or alpha falloff.
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

/* True when a copy at `index` of `copies` is worth drawing at all: in range and
 * not fully transparent. The caller's loop guard, so the "is this a wasted draw"
 * rule lives with the arithmetic instead of being restated at the call site. */
bool DioramaStackCopyIsVisible(int index, int copies);

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
