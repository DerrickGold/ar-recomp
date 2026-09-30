# Diorama depth shapes

Action rooms are captured as parallel 2D planes. A tilted diorama camera can
reveal gaps between planes, so room authoring may give a plane depth without
changing authentic game state. Paint order and geometric depth remain separate:
SDL does not depth-sort these meshes, and depth also controls focus.

The available strategies are ordered from least to most expensive:

- **Rake** linearly moves the bottom edge from `z` to `z + rake`. It suits a
  continuous surface receding into the scene, but introduces different
  parallax rates across the plane.
- **Bow** reaches the same bottom depth quadratically. Its slope is zero at the
  top, concentrating distortion near the fold.
- **Thickness** leaves the captured plane flat and extrudes a shaded skirt from
  its bottom source row. It composes with rake by starting at the raked bottom
  depth.
- **Stack** repeats parallel copies through a depth interval. It avoids shear
  and works for layered material such as clouds or foliage, at one draw per
  copy. Copies fade with distance from the source plane.
- **Voxel** uses the stack geometry densely without fading, allowing each
  transparent art island to preserve and extrude its own silhouette. It is the
  most expensive strategy and is capped separately.

Stack direction is relative to increasing `z`, which is nearer the camera:
forward fills toward the viewer, backward fills away, and both centers the
interval on the source plane. Explicit copy counts override density; density
otherwise keeps slice spacing consistent as authored depth changes. Both paths
are clamped to their draw-budget caps.

Connected BG1 art can cross the low/high tile-priority boundary, as the rocks
and sand do in Kassandora Act 1 (`03/02`) and the upper/lower water tiles do in
Bloodpool Act 1 (`02/01`) and Marahna Act 1 (`05/01`). Keep both front faces at
`z:0.5` with matching rake and bow. Tile priority still controls sprite occlusion;
it does not require separating the front faces in depth.

For opaque coplanar BG1 faces at that focal depth, the GPU compositor shares
filter coverage between the two captures while preserving their separate draw
slots. It samples premultiplied colors and conditions low-band coverage on the
remaining high-band coverage. This prevents complementary half-covered edges
from leaving a quarter-covered hole, and avoids filtering against transparent
black. Real transparent gaps and intervening sprites retain their coverage.
The low plane's depth copies keep their authored shape. Attached effects remain
enabled, using their original textures and projections after the surface shader
is unbound. Bloodpool's mist still draws before high-priority water, and its
reflections draw afterward. Neither effect requests nor room IDs exclude a
joined surface. Disjoint full-add subscreen captures also share this filter:
Marahna's BG1 low/high bands sum their premultiplied color contributions without
the source-over alpha correction, preserving the main-screen water tint and
the captured BG/OBJ winner masks. Both bands must use the same blend mode.
Translucent layer settings, mixed blend modes, separated faces, and high-band
depth copies use the existing independent path. Renderers without the shader
also retain that path. `AR_DIORAMA_PRIORITY_SURFACE=0` selects it for A/B captures.

In disjoint full-add scenes, a non-additive BG2 supplies the main-screen color
base. Marahna uses it for the water tint and animated ripples. Skybox-only mode
retains that color plane and its authored projection while replacing the
distant backdrop. Otherwise BG1/OBJ add their colors to the ROM skybox instead,
washing out the scenery and losing the water colors. Ordinary BG2 sky planes
keep the existing skybox-only behavior; explicit layer visibility still applies.
