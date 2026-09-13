#ifndef SIM3D_MESH_SET_H
#define SIM3D_MESH_SET_H

#include "sim3d_depth_pass.h"

/* Presentation-owned current-scene storage, not an LRU. Each GPU allocation
 * obeys the ordinary source contract. Only populated chunks are allocated;
 * publishing a smaller scene releases unused chunks. Zero-initialize and
 * destroy before the render device. All calls belong to the render thread. */
enum {
  kSim3DMeshSetMaximumChunks = 16,
  kSim3DMeshSetMaximumQuads = kSim3DMeshSetMaximumChunks * kSim3DDepthMaximumSourceQuads,
};
typedef struct Sim3DMeshSet {
  Sim3DDepthMesh *meshes[kSim3DMeshSetMaximumChunks];
  size_t selected[kSim3DMeshSetMaximumChunks];
  size_t count, quads;
  bool valid, linear;
} Sim3DMeshSet;

void Sim3DMeshSet_Destroy(Sim3DMeshSet *set);
bool Sim3DMeshSet_Ready(const Sim3DMeshSet *set);
/* Failed publication invalidates the set. Update once, before any append in
 * the pass. Sources are copied; no pointers into a frame or model are kept. */
bool Sim3DMeshSet_UpdateLinear(Sim3DMeshSet *set,
    const Sim3DDepthLinearVertex *vertices, size_t quads);
bool Sim3DMeshSet_UpdateSurface(Sim3DMeshSet *set,
    const Sim3DDepthSurfaceVertex *vertices, const ArRenderPointF *mask_uv, size_t quads);
/* Sorted, nonoverlapping source ranges. NULL/zero restores the full source.
 * Invalid requests leave selection unchanged. Backend rejection restores
 * full selection (or invalidates the set if restoration itself fails). */
bool Sim3DMeshSet_SelectSurface(Sim3DMeshSet *set,
    const Sim3DDepthMeshRange *ranges, size_t count);
bool Sim3DMeshSet_AppendLinear(const Sim3DMeshSet *set,
    const Sim3DDepthLinearTransform *transform);
/* Ranges address the concatenated selected stream, just as for one mesh.
 * At most three material ranges; queuing the split submission is atomic. */
bool Sim3DMeshSet_AppendSurface(const Sim3DMeshSet *set,
    const Sim3DDepthSurfaceBatch *batches, size_t count);

#endif
