#ifndef SCENE3D_MATH_H
#define SCENE3D_MATH_H

#include <stdbool.h>

typedef struct Scene3DCamera {
  float tilt_x;
  float tilt_y;
  float distance;
  float fov_y;
} Scene3DCamera;

typedef struct Scene3DPoint {
  float x, y;
} Scene3DPoint;

void Scene3D_BuildViewProjection(const Scene3DCamera *camera,
                                 int output_width, int output_height,
                                 float out_matrix[16]);
Scene3DPoint Scene3D_ProjectWorldPoint(const float matrix[16],
                                      float x, float y, float z,
                                      int output_width, int output_height);
/* Perspective scale relative to a point at `reference_depth`. A screen-facing
 * billboard multiplies its unprojected pixel size by this value while keeping
 * its anchor at Scene3D_ProjectWorldPoint(...). */
float Scene3D_ProjectBillboardScale(const float matrix[16],
                                    float x, float y, float z,
                                    float reference_depth);
/* Ground-plane shadow of a caster sample at height `z`. The directional light
 * travels downward, so the sample lands on z=0 at (x + z*light_x,
 * y + z*light_y); a zero-height sample therefore projects onto itself and a
 * shadow can never leave the ground plane. */
Scene3DPoint Scene3D_ProjectShadowPoint(const float matrix[16],
                                        float x, float y, float z,
                                        float light_x, float light_y,
                                        int output_width, int output_height);
/* Ground-footprint scale for a caster at height `z`. A directional light casts
 * a constant-size shadow, which reads as no height at all; shrinking the
 * footprint with height is the cue that actually sells altitude. Returns 1 at
 * z=0 and falls off monotonically, never reaching zero or going negative. */
float Scene3D_ShadowFootprintScale(float z, float shrink);
float Scene3D_AutoFitDistance(float fov_y);

/* Homogeneous depth of a world point under `matrix`. Positive is in front of
 * the camera; it crosses zero at the camera plane, where a perspective divide
 * flips the projection inside out. Geometry that extends toward the horizon —
 * anything larger than the unit ground quad — has to bound itself against
 * this rather than assume every vertex is projectable. */
float Scene3D_ClipDepth(const float matrix[16], float x, float y, float z);

/* Ground y at which clip depth equals `minimum_depth`, at world x. Clip depth
 * is affine in y on the ground plane, so this is solved, not searched.
 * `*increasing` reports whether depth grows with y — with the ground tilted
 * away from the camera it does, which makes the boundary a floor on y and the
 * near edge (toward the viewer) the one that folds. Returns false when depth
 * does not vary with y at all, leaving no bound to apply. */
bool Scene3D_GroundDepthBoundaryY(const float matrix[16], float x,
                                  float minimum_depth, float *boundary_y,
                                  bool *increasing);

/* Screen y of the ground plane's vanishing line — the horizon.
 *
 * Solved as the limit of the projection as ground y runs to infinity, not
 * searched and not approximated by projecting some "far enough" point: the
 * ground extension already reaches thousands of captured pixels out, so any
 * finite stand-in for infinity is a number that has to be re-tuned whenever
 * the extent changes.
 *
 * The x terms are constant in that limit and drop out of the ratio, so the
 * horizon is a horizontal line on screen for every camera this projection can
 * build, yaw included. Returns false when depth does not vary with ground y,
 * which is the degenerate camera that has no horizon to report. */
bool Scene3D_GroundHorizonScreenY(const float matrix[16], int output_height,
                                  float *screen_y);

#endif  /* SCENE3D_MATH_H */
