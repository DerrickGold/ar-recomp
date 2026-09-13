#include "sim3d_mesh_set.h"

#include <string.h>

static size_t ChunkQuads(const Sim3DMeshSet *set, size_t i) {
  const size_t left = set->quads - i * kSim3DDepthMaximumSourceQuads;
  return left < kSim3DDepthMaximumSourceQuads ? left : kSim3DDepthMaximumSourceQuads;
}

void Sim3DMeshSet_Destroy(Sim3DMeshSet *set) {
  for (size_t i = 0; i < kSim3DMeshSetMaximumChunks; ++i)
    Sim3DDepthPass_DestroyMesh(set->meshes[i]);
  memset(set, 0, sizeof(*set));
}

bool Sim3DMeshSet_Ready(const Sim3DMeshSet *set) {
  if (!set->valid) return false;
  for (size_t i = 0; i < set->count; ++i)
    if (!Sim3DDepthPass_MeshReady(set->meshes[i])) return false;
  return true;
}

static bool Update(Sim3DMeshSet *set, const void *vertices,
    const ArRenderPointF *mask, size_t quads, bool linear) {
  set->valid = false;
  if (quads > kSim3DMeshSetMaximumQuads || (quads && !vertices)) return false;
  if (set->count && set->linear != linear) return false;
  const size_t count = (quads + kSim3DDepthMaximumSourceQuads - 1) / kSim3DDepthMaximumSourceQuads;
  for (size_t i = 0; i < count; ++i) {
    const size_t first = i * kSim3DDepthMaximumSourceQuads;
    const size_t n = quads - first < kSim3DDepthMaximumSourceQuads
        ? quads - first : kSim3DDepthMaximumSourceQuads;
    if (!set->meshes[i]) set->meshes[i] = linear
        ? Sim3DDepthPass_CreateLinearMesh() : Sim3DDepthPass_CreateSurfaceMesh();
    const bool ok = set->meshes[i] && (linear
        ? Sim3DDepthPass_UpdateLinearMesh(set->meshes[i],
            (const Sim3DDepthLinearVertex *)vertices + first * 4, n)
        : Sim3DDepthPass_UpdateSurfaceMeshWithMask(set->meshes[i],
            (const Sim3DDepthSurfaceVertex *)vertices + first * 4,
            mask ? mask + first * 4 : NULL, n));
    if (!ok) { Sim3DMeshSet_Destroy(set); return false; }
    set->selected[i] = n;
  }
  for (size_t i = count; i < kSim3DMeshSetMaximumChunks; ++i) {
    Sim3DDepthPass_DestroyMesh(set->meshes[i]);
    set->meshes[i] = NULL;
    set->selected[i] = 0;
  }
  set->count = count; set->quads = quads; set->linear = linear; set->valid = true;
  return true;
}

bool Sim3DMeshSet_UpdateLinear(Sim3DMeshSet *set,
    const Sim3DDepthLinearVertex *vertices, size_t quads) {
  return Update(set, vertices, NULL, quads, true);
}

bool Sim3DMeshSet_UpdateSurface(Sim3DMeshSet *set,
    const Sim3DDepthSurfaceVertex *vertices, const ArRenderPointF *mask, size_t quads) {
  return Update(set, vertices, mask, quads, false);
}

bool Sim3DMeshSet_SelectSurface(Sim3DMeshSet *set,
    const Sim3DDepthMeshRange *ranges, size_t count) {
  if (!Sim3DMeshSet_Ready(set) || set->linear || count > 64 || (count && !ranges)) return false;
  size_t end = 0;
  for (size_t r = 0; r < count; ++r) {
    if (ranges[r].first_quad < end || ranges[r].first_quad > set->quads ||
        ranges[r].quad_count > set->quads - ranges[r].first_quad) return false;
    end = ranges[r].first_quad + ranges[r].quad_count;
  }
  for (size_t i = 0; i < set->count; ++i) {
    const size_t first = i * kSim3DDepthMaximumSourceQuads, n = ChunkQuads(set, i);
    Sim3DDepthMeshRange local[64] = {{0}};
    size_t used = 0, selected = 0;
    for (size_t r = 0; r < count; ++r) {
      const size_t lo = ranges[r].first_quad > first ? ranges[r].first_quad : first;
      size_t hi = ranges[r].first_quad + ranges[r].quad_count;
      if (hi > first + n) hi = first + n;
      if (hi > lo) { local[used++] = (Sim3DDepthMeshRange){lo - first, hi - lo}; selected += hi - lo; }
    }
    if (!Sim3DDepthPass_SelectSurfaceMesh(set->meshes[i], count ? local : NULL,
            count ? (used ? used : 1) : 0)) {
      bool restored = true;
      for (size_t j = 0; j < set->count; ++j) {
        restored &= Sim3DDepthPass_SelectSurfaceMesh(set->meshes[j], NULL, 0);
        set->selected[j] = ChunkQuads(set, j);
      }
      if (!restored) set->valid = false;
      return false;
    }
    set->selected[i] = count ? selected : n;
  }
  return true;
}

bool Sim3DMeshSet_AppendLinear(const Sim3DMeshSet *set,
    const Sim3DDepthLinearTransform *transform) {
  return Sim3DMeshSet_Ready(set) && set->linear && (!set->count ||
      Sim3DDepthPass_AppendLinearMeshes(set->meshes, set->count, transform));
}

bool Sim3DMeshSet_AppendSurface(const Sim3DMeshSet *set,
    const Sim3DDepthSurfaceBatch *batches, size_t count) {
  if (!Sim3DMeshSet_Ready(set) || set->linear || !batches || !count || count > 3) return false;
  Sim3DDepthSurfaceMeshBatch split[3 * kSim3DMeshSetMaximumChunks];
  size_t used = 0, total = 0;
  for (size_t i = 0; i < set->count; ++i) total += set->selected[i];
  for (size_t b = 0; b < count; ++b) {
    const Sim3DDepthMeshRange range = batches[b].range;
    if (range.first_quad > total || range.quad_count > total - range.first_quad) return false;
    size_t first = 0;
    for (size_t i = 0; i < set->count; first += set->selected[i++]) {
      const size_t lo = range.first_quad > first ? range.first_quad : first;
      size_t hi = range.first_quad + range.quad_count;
      if (hi > first + set->selected[i]) hi = first + set->selected[i];
      if (hi <= lo) continue;
      split[used] = (Sim3DDepthSurfaceMeshBatch){set->meshes[i], batches[b]};
      split[used++].batch.range = (Sim3DDepthMeshRange){lo - first, hi - lo};
    }
  }
  return !used || Sim3DDepthPass_AppendSurfaceMeshBatches(split, used);
}
