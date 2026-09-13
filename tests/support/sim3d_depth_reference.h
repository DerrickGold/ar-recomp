#ifndef SIM3D_DEPTH_REFERENCE_H
#define SIM3D_DEPTH_REFERENCE_H

#if !defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
#error "Depth reference models belong only to focused tests and benchmarks"
#endif

#include "sim/sim3d_depth_pass.h"

/* Independent untextured model references, not shipping renderer contracts.
 * Both copy vertices and transforms and obey the depth pass's opaque ordering,
 * budgets and lifetime rules. The affine reference rejects AABBs crossing any
 * clip plane; the homogeneous reference lets hardware clip and preserves
 * screen-linear color. Neither infers scene-facing or pixel-snapping policy. */
typedef struct Sim3DDepthModelVertex {
  float position[3];
  ArRenderColorF color;
} Sim3DDepthModelVertex;
Sim3DDepthMesh *Sim3DDepthPass_CreateModelMesh(void);
Sim3DDepthMesh *Sim3DDepthPass_CreateHardwareClippedModelMesh(void);
bool Sim3DDepthPass_UpdateModelMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthModelVertex *vertices, size_t quad_count);
bool Sim3DDepthPass_AppendModelMesh(Sim3DDepthMesh *mesh, const float matrix[16]);

/* Fault injection affects only the next unattempted linear preparation.
 * Clearing it does not undo a latched failure: a device reset is required. */
void Sim3DDepthReference_RejectLinearPreparation(bool reject);

#endif
