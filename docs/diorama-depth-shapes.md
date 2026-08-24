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

Ownership is split intentionally. `diorama_layer_order.h` owns manifest
grammar, defaults, caps, and resolved authoring state.
`diorama_depth_shapes.h` owns pure, renderer-independent geometry contracts.
`diorama.c` only assembles, projects, and draws the meshes.
