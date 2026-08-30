#ifndef AR_DIORAMA_EDGE_AA_H
#define AR_DIORAMA_EDGE_AA_H

#include <stdbool.h>
#include <stdint.h>

#include "render/render_types.h"

typedef uint32_t DioramaEdgeAaMask;
enum {
  kDioramaEdgeAa_Top = UINT32_C(1) << 0,
  kDioramaEdgeAa_Right = UINT32_C(1) << 1,
  kDioramaEdgeAa_Bottom = UINT32_C(1) << 2,
  kDioramaEdgeAa_Left = UINT32_C(1) << 3,
  kDioramaEdgeAa_All =
      kDioramaEdgeAa_Top | kDioramaEdgeAa_Right |
      kDioramaEdgeAa_Bottom | kDioramaEdgeAa_Left,
};

/* A regular U/V grid has 2*(subdiv_x+subdiv_y) unique perimeter points. The
 * coverage fringe duplicates each point once at the true edge and once at its
 * transparent outward offset, then emits two triangles per enabled segment. */
int DioramaEdgeAa_FringeVertexCapacity(int subdiv_x, int subdiv_y);
int DioramaEdgeAa_FringeIndexCapacity(int subdiv_x, int subdiv_y);

/* Builds a screen-space coverage fringe around a projected regular grid.
 *
 * The source grid remains untouched and fully opaque through its true edge.
 * Inner fringe vertices coincide with that edge; outer vertices are displaced
 * by width_pixels and carry zero vertex alpha. UVs on both are pinned to valid
 * edge texel centres, so filtering cannot pull transparent allocation padding
 * into the fringe. Disabled sides emit no triangles; adjacent enabled sides
 * retain their own outward coverage through the shared corner.
 *
 * Returns false for invalid geometry/capacity. On failure both counts are 0. */
bool DioramaEdgeAa_BuildFringe(
    const ArRenderVertex2D *grid, int subdiv_x, int subdiv_y,
    float width_pixels, DioramaEdgeAaMask edge_mask,
    float u_min, float u_max, float v_min, float v_max,
    float texel_width, float texel_height,
    ArRenderVertex2D *out_vertices, int vertex_capacity,
    int32_t *out_indices, int index_capacity,
    int *out_vertex_count, int *out_index_count);

#endif /* AR_DIORAMA_EDGE_AA_H */
