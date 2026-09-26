/* Sim3DDepthPass meshes (SDL): the retained GPU meshes behind the depth pass
 * (geometry, model, linear, radial, spherical and surface meshes): create,
 * update, select and append, plus plain quad appends.
 * Phase: present (render owner thread).
 * Tests: tests/sim3d_depth_pass_gpu_test.c */
#include "platform/sdl/sim3d_depth_pass_sdl_internal.h"

static bool IsGeometryMesh(Sim3DMeshKind kind) {
  return kind == kMeshGeometry || IsModelMesh(kind) || kind == kMeshRadial ||
      kind == kMeshSurface || kind == kMeshLinear;
}

static Sim3DDepthMesh *CreateMesh(Sim3DMeshKind kind) {
  if (!g_depth_pass.collecting || !MeshPipeline(kind)) return NULL;
  unsigned count = 0;
  for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next)
    count += IsGeometryMesh(mesh->kind) == IsGeometryMesh(kind);
  if (count >= (IsGeometryMesh(kind) ? kMaximumGeometryMeshes : kMaximumRetainedMeshes))
    return NULL;
  Sim3DDepthMesh *mesh = calloc(1, sizeof(*mesh));
  if (!mesh) return NULL;
  mesh->kind = kind;
  mesh->next = g_depth_pass_meshes;
  g_depth_pass_meshes = mesh;
  return mesh;
}

Sim3DDepthMesh *Sim3DDepthPass_CreateMesh(void) { return CreateMesh(kMeshScreen); }
Sim3DDepthMesh *Sim3DDepthPass_CreateSphericalMesh(void) { return CreateMesh(kMeshSpherical); }
Sim3DDepthMesh *Sim3DDepthPass_CreateGeometryMesh(void) { return CreateMesh(kMeshGeometry); }
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
Sim3DDepthMesh *Sim3DDepthPass_CreateModelMesh(void) {
  return g_depth_pass.collecting && CreateModelPipeline(kModelPipeline_ReferenceAffine)
      ? CreateMesh(kMeshModel)
      : NULL;
}
Sim3DDepthMesh *Sim3DDepthPass_CreateHardwareClippedModelMesh(void) {
  return g_depth_pass.collecting && CreateModelPipeline(kModelPipeline_ReferenceClipped)
      ? CreateMesh(kMeshModelClipped)
      : NULL;
}
#endif
Sim3DDepthMesh *Sim3DDepthPass_CreateLinearMesh(void) {
  return g_depth_pass.collecting && CreateModelPipeline(kModelPipeline_Linear)
      ? CreateMesh(kMeshLinear)
      : NULL;
}
Sim3DDepthMesh *Sim3DDepthPass_CreateRadialMesh(void) {
  return g_depth_pass.collecting && CreateModelPipeline(kModelPipeline_Radial)
      ? CreateMesh(kMeshRadial)
      : NULL;
}
Sim3DDepthMesh *Sim3DDepthPass_CreateSurfaceMesh(void) {
  return g_depth_pass.collecting && CreateSurfacePipelines() ? CreateMesh(kMeshSurface) : NULL;
}
Sim3DDepthMesh *Sim3DDepthPass_CreateSphericalBodyMesh(void) {
  return g_depth_pass.collecting && CreateBodyPipeline() ? CreateMesh(kMeshSphericalBody) : NULL;
}

Uint32 MeshVertexBytes(const Sim3DDepthMesh *mesh) {
  switch (mesh->kind) {
    case kMeshScreen: return 4 * (Uint32)sizeof(float);
    case kMeshSpherical: return (Uint32)sizeof(Sim3DSphericalGpuQuad) / 4;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
    case kMeshModel: case kMeshModelClipped: return sizeof(Sim3DDepthModelVertex);
#endif
    case kMeshRadial: return sizeof(Sim3DDepthRadialVertex);
    case kMeshLinear: return sizeof(Sim3DDepthLinearVertex);
    case kMeshSurface: return sizeof(Sim3DSurfaceGpuQuad) / 4;
    case kMeshGeometry: return sizeof(Sim3DGpuVertex);
    case kMeshSphericalBody: return sizeof(Sim3DDepthSphericalBodyVertex);
  }
  return 0;
}

void ReleaseMeshStorage(Sim3DDepthMesh *mesh) {
  if (mesh->positions) SDL_ReleaseGPUBuffer(mesh->device, mesh->positions);
  if (mesh->transfer) SDL_ReleaseGPUTransferBuffer(mesh->device, mesh->transfer);
  if (mesh->selection) SDL_ReleaseGPUBuffer(mesh->device, mesh->selection);
  if (mesh->selection_transfer)
    SDL_ReleaseGPUTransferBuffer(mesh->device, mesh->selection_transfer);
  mesh->selection = NULL;
  mesh->selection_transfer = NULL;
  mesh->selection_count = mesh->selection_capacity = 0;
  mesh->selection_dirty = false;
  mesh->surface_range_count = 0;
  mesh->surface_selected = false;
  mesh->positions = NULL;
  mesh->transfer = NULL;
  mesh->device = NULL;
  mesh->count = mesh->capacity = 0;
  mesh->width = mesh->height = 0;
  mesh->dirty = mesh->queued = false;
}

void Sim3DDepthPass_DestroyMesh(Sim3DDepthMesh *mesh) {
  if (!mesh) return;
  if (g_depth_pass.collecting && mesh->queued) g_depth_pass.geometry_failed = true;
  Sim3DDepthMesh **link = &g_depth_pass_meshes;
  while (*link && *link != mesh) link = &(*link)->next;
  if (*link != mesh) return;
  *link = mesh->next;
  ReleaseMeshStorage(mesh);
  free(mesh);
}

bool Sim3DDepthPass_MeshReady(const Sim3DDepthMesh *mesh) {
  return g_depth_pass.collecting && mesh &&
      MeshPipeline(mesh->kind) &&
      mesh->device == g_depth_pass.device && mesh->positions && mesh->count &&
      (IsModelMesh(mesh->kind) || mesh->kind == kMeshRadial || mesh->kind == kMeshSurface ||
       mesh->kind == kMeshSphericalBody || mesh->kind == kMeshLinear ||
       (mesh->width == g_depth_pass.width && mesh->height == g_depth_pass.height));
}

static bool ReserveMesh(Sim3DDepthMesh *mesh, Uint32 count) {
  if (count > mesh->capacity) {
    Uint32 capacity = mesh->capacity ? mesh->capacity : 4096;
    while (capacity < count) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {
      .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = capacity * MeshVertexBytes(mesh),
    };
    const SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = info.size,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g_depth_pass.device, &info);
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(g_depth_pass.device, &transfer_info);
    if (!buffer || !transfer) {
      if (buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
      if (transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      return false;
    }
    ReleaseMeshStorage(mesh);
    mesh->device = g_depth_pass.device;
    mesh->positions = buffer;
    mesh->transfer = transfer;
    mesh->capacity = capacity;
  }
  return true;
}

static bool CanUpdateMesh(const Sim3DDepthMesh *mesh, size_t quad_count, Sim3DMeshKind kind) {
  return g_depth_pass.collecting && mesh && mesh->kind == kind && MeshPipeline(kind) &&
      !mesh->queued && (!mesh->device || mesh->device == g_depth_pass.device) &&
      quad_count && quad_count <= (kind == kMeshRadial
          ? kSim3DDepthMaximumRadialSourceQuads : kSim3DDepthMaximumSourceQuads);
}

bool ValidPosition(Sim3DDepthPosition position) {
  return isfinite(position.x) && isfinite(position.y) && isfinite(position.depth);
}

static void MapPosition(Sim3DDepthPosition position, float out[4]) {
  out[0] = position.x * g_depth_pass.clip_x_scale - 1.0f;
  out[1] = 1.0f - position.y * g_depth_pass.clip_y_scale;
  out[2] = position.depth;
  out[3] = 1.0f;
}

static void PublishMesh(Sim3DDepthMesh *mesh, Uint32 count) {
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, mesh->transfer);
  mesh->count = count;
  mesh->width = g_depth_pass.width;
  mesh->height = g_depth_pass.height;
  mesh->dirty = true;
}

bool Sim3DDepthPass_UpdateMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthPosition *positions, size_t quad_count) {
  if (!CanUpdateMesh(mesh, quad_count, kMeshScreen) || !positions) return false;
  const Uint32 count = (Uint32)quad_count * 4;
  for (Uint32 i = 0; i < count; ++i) if (!ValidPosition(positions[i])) return false;
  if (!ReserveMesh(mesh, count)) return false;
  float (*mapped)[4] = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (Uint32 i = 0; i < count; ++i) MapPosition(positions[i], mapped[i]);
  PublishMesh(mesh, count);
  return true;
}

bool Sim3DDepthPass_UpdateSphericalMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalQuad *quads, size_t quad_count) {
  if (!CanUpdateMesh(mesh, quad_count, kMeshSpherical) || !quads) return false;
  for (size_t i = 0; i < quad_count; ++i) {
    if (quads[i].triangle > 2) return false;
    for (int p = 0; p < 4; ++p) {
      if (!ValidPosition(quads[i].positions[p])) return false;
      for (int j = 0; j < 3; ++j) if (!isfinite(quads[i].normals[p][j])) return false;
      for (int j = 0; j < 2; ++j) if (!isfinite(quads[i].weights[p][j])) return false;
    }
  }
  const Uint32 count = (Uint32)quad_count * 4;
  if (!ReserveMesh(mesh, count)) return false;
  Sim3DSphericalGpuQuad *mapped =
      SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (size_t i = 0; i < quad_count; ++i)
    for (int p = 0; p < 4; ++p) {
      MapPosition(quads[i].positions[p], mapped[i].positions[p]);
      memcpy(mapped[i].normals[p], quads[i].normals[p], 3 * sizeof(float));
      mapped[i].normals[p][3] = p ? 0 : (float)quads[i].triangle;
      memcpy(mapped[i].weights[p], quads[i].weights[p], 2 * sizeof(float));
    }
  PublishMesh(mesh, count);
  return true;
}

static bool MeshLayerSupported(Sim3DDepthPassLayer layer) {
  return layer == kSim3DDepthPass_CloudShadow || layer == kSim3DDepthPass_Cloud ||
      layer == kSim3DDepthPass_GroundBlur || layer == kSim3DDepthPass_GroundHaze ||
      layer == kSim3DDepthPass_Effect || layer == kSim3DDepthPass_VolumeCloud;
}

bool ValidSampleColor(ArRenderColorF color) {
  return isfinite(color.r) && color.r >= 0 && color.r <= 1 &&
      isfinite(color.g) && color.g >= 0 && color.g <= 1 &&
      isfinite(color.b) && color.b >= 0 && color.b <= 1 &&
      isfinite(color.a) && color.a >= 0 && color.a <= 1;
}

static Sim3DGpuVertex PackScreenVertex(Sim3DDepthVertex source) {
  return (Sim3DGpuVertex){
    .position = {source.x * g_depth_pass.clip_x_scale - 1.0f,
        1.0f - source.y * g_depth_pass.clip_y_scale, source.depth, 1.0f},
    .color = {source.color.r, source.color.g, source.color.b, source.color.a},
    .uv = {source.uv.x, source.uv.y},
  };
}

bool GeometryLayerSupported(Sim3DDepthPassLayer layer) {
  return layer == kSim3DDepthPass_Solid || layer == kSim3DDepthPass_Ground ||
      layer == kSim3DDepthPass_Mountain || layer == kSim3DDepthPass_WorldMountain ||
      layer == kSim3DDepthPass_DepthOccluder || layer == kSim3DDepthPass_ShadowReceiver;
}

bool OrderedLayerSupported(Sim3DDepthPassLayer layer) {
  return GeometryLayerSupported(layer) || layer == kSim3DDepthPass_GroundBlur ||
      layer == kSim3DDepthPass_GroundHaze;
}

bool Sim3DDepthPass_UpdateGeometryMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthVertex *vertices, size_t quad_count) {
  if (!CanUpdateMesh(mesh, quad_count, kMeshGeometry) || !vertices) return false;
  const Uint32 count = (Uint32)quad_count * 4;
  for (Uint32 i = 0; i < count; ++i) {
    const Sim3DDepthVertex *v = &vertices[i];
    if (!ValidPosition((Sim3DDepthPosition){v->x, v->y, v->depth}) ||
        !ValidSampleColor(v->color) || !isfinite(v->uv.x) || !isfinite(v->uv.y)) return false;
  }
  if (!ReserveMesh(mesh, count)) return false;
  Sim3DGpuVertex *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (Uint32 i = 0; i < count; ++i) mapped[i] = PackScreenVertex(vertices[i]);
  PublishMesh(mesh, count);
  return true;
}

bool Sim3DDepthPass_AppendGeometryMeshRange(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh,
    size_t first_quad, size_t quad_count) {
  if (!GeometryLayerSupported(layer) || !Sim3DDepthPass_MeshReady(mesh) ||
      mesh->kind != kMeshGeometry || !quad_count ||
      first_quad >= mesh->count / 4 || quad_count > mesh->count / 4 - first_quad ||
      g_depth_pass.geometry_sample_count == kMaximumGeometrySamples) return false;
  g_depth_pass.samples[g_depth_pass.sample_count++] = (Sim3DMeshSample){
    .mesh = mesh, .layer = layer,
    .first = (Uint32)first_quad * 4, .count = (Uint32)quad_count * 4,
    .ordinary_before = g_depth_pass.lists[layer].count,
  };
  ++g_depth_pass.geometry_sample_count;
  mesh->queued = true;
  return true;
}

bool Sim3DDepthPass_AppendGeometryMesh(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh) {
  return mesh && Sim3DDepthPass_AppendGeometryMeshRange(layer, mesh, 0, mesh->count / 4);
}

bool Sim3DDepthPass_CaptureGeometryMesh(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh) {
  Sim3DDepthGeometryRange range;
  return Sim3DDepthPass_CaptureGeometryLayers(mesh, &layer, 1, &range);
}

bool Sim3DDepthPass_CaptureGeometryLayers(Sim3DDepthMesh *mesh,
    const Sim3DDepthPassLayer *layers, size_t layer_count,
    Sim3DDepthGeometryRange *ranges) {
  if (!layers || !ranges || !layer_count || layer_count > kSim3DDepthPassLayerCount ||
      g_depth_pass.geometry_failed) return false;
  Uint32 count = 0, mask = 0;
  Sim3DDepthGeometryRange resolved[kSim3DDepthPassLayerCount];
  _Static_assert(kSim3DDepthPassLayerCount < 32, "layer membership fits in uint32");
  for (size_t i = 0; i < layer_count; ++i) {
    const Sim3DDepthPassLayer layer = layers[i];
    if (!GeometryLayerSupported(layer) || (mask & (1u << layer))) return false;
    mask |= 1u << layer;
    const Uint32 vertices = g_depth_pass.lists[layer].count;
    if (vertices > kMaximumRetainedVertices - count) return false;
    resolved[i] = (Sim3DDepthGeometryRange){layer, count / 4, vertices / 4};
    count += vertices;
  }
  if (!CanUpdateMesh(mesh, count / 4, kMeshGeometry)) return false;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    if (mask & (1u << g_depth_pass.samples[i].layer)) return false;
  if (!ReserveMesh(mesh, count)) return false;
  Sim3DGpuVertex *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (size_t i = 0; i < layer_count; ++i) {
    const Sim3DDepthList *list = &g_depth_pass.lists[layers[i]];
    if (list->count) memcpy(mapped + resolved[i].first_quad * 4,
        list->vertices, list->count * sizeof(*list->vertices));
  }
  PublishMesh(mesh, count);
  memcpy(ranges, resolved, layer_count * sizeof(*ranges));
  return true;
}

bool Sim3DDepthPass_AppendGeometryRanges(Sim3DDepthMesh *mesh,
    const Sim3DDepthGeometryRange *ranges, size_t range_count) {
  if (!ranges || !range_count || range_count > kMaximumGeometrySamples ||
      !Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshGeometry) return false;
  unsigned needed = 0;
  for (size_t i = 0; i < range_count; ++i) {
    const Sim3DDepthGeometryRange range = ranges[i];
    if (!GeometryLayerSupported(range.layer) || range.first_quad > mesh->count / 4 ||
        range.quad_count > mesh->count / 4 - range.first_quad) return false;
    needed += range.quad_count != 0;
  }
  if (needed > kMaximumGeometrySamples - g_depth_pass.geometry_sample_count) return false;
  /* The owner thread cannot change readiness/budgets between validation and
   * these appends, so a rejected set never leaves a partially queued pass. */
  for (size_t i = 0; i < range_count; ++i) if (ranges[i].quad_count)
    (void)Sim3DDepthPass_AppendGeometryMeshRange(ranges[i].layer, mesh,
        ranges[i].first_quad, ranges[i].quad_count);
  return true;
}

#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
bool Sim3DDepthPass_UpdateModelMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthModelVertex *vertices, size_t quad_count) {
  if (!g_depth_pass.collecting || !mesh || !IsModelMesh(mesh->kind) ||
      !CreateModelPipeline(mesh->kind == kMeshModelClipped
          ? kModelPipeline_ReferenceClipped : kModelPipeline_ReferenceAffine) ||
      !CanUpdateMesh(mesh, quad_count, mesh->kind) || !vertices)
    return false;
  float low[3], high[3];
  memcpy(low, vertices[0].position, sizeof(low));
  memcpy(high, low, sizeof(high));
  const Uint32 count = (Uint32)quad_count * 4;
  for (Uint32 i = 0; i < count; ++i) {
    if (!ValidSampleColor(vertices[i].color)) return false;
    for (int p = 0; p < 3; ++p) {
      const float v = vertices[i].position[p];
      if (!isfinite(v)) return false;
      low[p] = fminf(low[p], v);
      high[p] = fmaxf(high[p], v);
    }
  }
  if (!ReserveMesh(mesh, count)) return false;
  void *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  memcpy(mapped, vertices, count * sizeof(*vertices));
  memcpy(mesh->bounds_min, low, sizeof(low));
  memcpy(mesh->bounds_max, high, sizeof(high));
  PublishMesh(mesh, count);
  return true;
}

static bool ModelTransformSafe(const Sim3DDepthMesh *mesh, const float matrix[16]) {
  for (int i = 0; i < 16; ++i) if (!isfinite(matrix[i])) return false;
  for (int corner = 0; corner < 8; ++corner) {
    double clip[4], error[4];
    for (int row = 0; row < 4; ++row) {
      clip[row] = matrix[12 + row];
      double magnitude = fabs(clip[row]);
      for (int axis = 0; axis < 3; ++axis) {
        const float v = corner & (1 << axis) ? mesh->bounds_max[axis] : mesh->bounds_min[axis];
        const double term = (double)matrix[axis * 4 + row] * v;
        clip[row] += term;
        magnitude += fabs(term);
      }
      /* Finite inputs can still overflow before cancellation. Leave room for
       * intermediate float rounding, including the homogeneous depth remap. */
      if (magnitude > (double)FLT_MAX / (1.0 + 8.0 * FLT_EPSILON)) return false;
      /* Bounds must enclose the shader's FLOAT result, not just the ideal
       * double transform. Ill-conditioned cancellation can dwarf the clip
       * inset even without overflow. Conservatively bound dot-product roundoff
       * before accepting the entire box; uncertain geometry stays on CPU. */
      error[row] = magnitude * (8.0 * FLT_EPSILON) + 8.0 * FLT_MIN;
    }
    /* Hardware clips BEFORE division, including negative/zero W. Only reject
     * nonfinite or potentially overflowing transforms for this policy; a
     * partly visible mesh is not a reason to project it on the CPU. */
    if (mesh->kind == kMeshModelClipped) continue;
    /* A deliberately conservative interior, not a new clipping convention.
     * Crossing it chooses the existing CPU path before anything is queued. */
    const double minimum_w = clip[3] - error[3];
    if (!(minimum_w > 0.001)) return false;
    for (int axis = 0; axis < 3; ++axis)
      if (fabs(clip[axis]) + error[axis] >= minimum_w * (1.0 - 0.00001)) return false;
  }
  return true;
}

bool Sim3DDepthPass_AppendModelMesh(Sim3DDepthMesh *mesh, const float matrix[16]) {
  if (!Sim3DDepthPass_MeshReady(mesh) || !IsModelMesh(mesh->kind) || !matrix ||
      g_depth_pass.geometry_sample_count == kMaximumGeometrySamples ||
      !ModelTransformSafe(mesh, matrix))
    return false;
  Sim3DMeshSample *sample = &g_depth_pass.samples[g_depth_pass.sample_count++];
  ++g_depth_pass.geometry_sample_count;
  *sample = (Sim3DMeshSample){.mesh = mesh, .layer = kSim3DDepthPass_Solid,
    .count = mesh->count, .ordinary_before = g_depth_pass.lists[kSim3DDepthPass_Solid].count};
  memcpy(sample->model.matrix, matrix, sizeof(sample->model.matrix));
  sample->model.viewport[0] = (float)g_depth_pass.width;
  sample->model.viewport[1] = (float)g_depth_pass.height;
  sample->model.viewport[2] = g_depth_pass.clip_x_scale;
  sample->model.viewport[3] = g_depth_pass.clip_y_scale;
  mesh->queued = true;
  return true;
}
#endif

bool Sim3DDepthPass_UpdateLinearMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthLinearVertex *vertices, size_t quad_count) {
  if (!g_depth_pass.collecting || !mesh || mesh->kind != kMeshLinear || !vertices ||
      !CreateModelPipeline(kModelPipeline_Linear) || !CanUpdateMesh(mesh, quad_count, kMeshLinear))
    return false;
  const Uint32 count = (Uint32)quad_count * 4;
  float extent[5] = {0};
  for (Uint32 i = 0; i < count; ++i) {
    const Sim3DDepthLinearVertex *v = &vertices[i];
    if (!ValidSampleColor(v->color) || !isfinite(v->axis) || v->axis < 0 ||
        v->axis >= kSim3DDepthLinearAxisCount || v->axis != floorf(v->axis) ||
        v->axis != vertices[i & ~3u].axis) return false;
    const float values[5] = {v->position[0], v->position[1], v->position[2],
      v->displacement, v->depth_offset};
    for (unsigned p = 0; p < 5; ++p) {
      if (!isfinite(values[p])) return false;
      extent[p] = fmaxf(extent[p], fabsf(values[p]));
    }
  }
  if (!ReserveMesh(mesh, count)) return false;
  void *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  memcpy(mapped, vertices, count * sizeof(*vertices));
  memcpy(mesh->linear_extent, extent, sizeof(extent));
  PublishMesh(mesh, count);
  return true;
}

static bool LinearTransformSafe(const Sim3DDepthMesh *mesh,
    const Sim3DDepthLinearTransform *t) {
  if (!t) return false;
  double position[3];
  for (unsigned c = 0; c < 3; ++c) {
    if (!isfinite(t->offset[c])) return false;
    double axis = 0;
    for (unsigned i = 0; i < kSim3DDepthLinearAxisCount; ++i) {
      if (!isfinite(t->axes[i][c])) return false;
      axis = fmax(axis, fabs((double)t->axes[i][c]));
    }
    position[c] = mesh->linear_extent[c] + fabs((double)t->offset[c]) +
        mesh->linear_extent[3] * axis + (c == 2 ? mesh->linear_extent[4] : 0);
    if (position[c] > FLT_MAX / 16.0) return false;
  }
  /* Reserve headroom for snapping's divide and the alternate depth/W product,
   * as well as intermediate sums before cancellation. No clip-plane rejection. */
  const double limit = sqrt((double)FLT_MAX) / 1024;
  for (unsigned r = 0; r < 4; ++r) {
    if (!isfinite(t->matrix[12+r])) return false;
    double magnitude = fabs((double)t->matrix[12+r]);
    for (unsigned c = 0; c < 3; ++c) {
      if (!isfinite(t->matrix[c*4+r])) return false;
      magnitude += fabs((double)t->matrix[c*4+r]) * position[c];
    }
    if (magnitude > limit) return false;
  }
  return true;
}

bool Sim3DDepthPass_AppendLinearMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthLinearTransform *transform) {
  return Sim3DDepthPass_AppendLinearMeshes(&mesh, 1, transform);
}

bool Sim3DDepthPass_AppendLinearMeshes(Sim3DDepthMesh *const *meshes,
    size_t mesh_count, const Sim3DDepthLinearTransform *transform) {
  if (!meshes || !mesh_count ||
      mesh_count > kMaximumGeometrySamples - g_depth_pass.geometry_sample_count) return false;
  for (size_t i = 0; i < mesh_count; ++i)
    if (!Sim3DDepthPass_MeshReady(meshes[i]) || meshes[i]->kind != kMeshLinear ||
        !LinearTransformSafe(meshes[i], transform)) return false;
  for (size_t i = 0; i < mesh_count; ++i) {
    Sim3DDepthMesh *mesh = meshes[i];
    Sim3DMeshSample *sample = &g_depth_pass.samples[g_depth_pass.sample_count++];
    ++g_depth_pass.geometry_sample_count;
    *sample = (Sim3DMeshSample){.mesh = mesh, .layer = kSim3DDepthPass_Solid,
      .count = mesh->count, .ordinary_before = g_depth_pass.lists[kSim3DDepthPass_Solid].count};
    memcpy(sample->linear.matrix, transform->matrix, sizeof(transform->matrix));
    memcpy(sample->linear.offset, transform->offset, sizeof(transform->offset));
    for (unsigned axis = 0; axis < kSim3DDepthLinearAxisCount; ++axis)
      memcpy(sample->linear.axes[axis], transform->axes[axis], sizeof(transform->axes[axis]));
    sample->linear.raster[0] = (float)g_depth_pass.width;
    sample->linear.raster[1] = (float)g_depth_pass.height;
    sample->linear.raster[2] = transform->pixel_centers ? 1 : 0;
    mesh->queued = true;
  }
  return true;
}

bool Sim3DDepthPass_UpdateRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthRadialVertex *vertices, size_t quad_count) {
  if (!g_depth_pass.collecting || !mesh || mesh->kind != kMeshRadial || !vertices ||
      !CreateModelPipeline(kModelPipeline_Radial) || !CanUpdateMesh(mesh, quad_count, kMeshRadial))
    return false;
  const Uint32 count = (Uint32)quad_count * 4;
  float extent[2] = {0};
  for (Uint32 i = 0; i < count; ++i) {
    const Sim3DDepthRadialVertex *v = &vertices[i];
    double norm = 0;
    for (unsigned axis = 0; axis < 3; ++axis) norm += (double)v->normal[axis] * v->normal[axis];
    if (!isfinite(norm) || fabs(norm - 1.0) > .002001 || !ValidSampleColor(v->color) ||
        !isfinite(v->variant) || v->variant < 0 || v->variant > 65535 ||
        v->variant != floorf(v->variant) || v->variant != vertices[i & ~3u].variant) return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
      if (!isfinite(v->elevation[axis])) return false;
      extent[axis] = fmaxf(extent[axis], fabsf(v->elevation[axis]));
    }
  }
  if (!ReserveMesh(mesh, count)) return false;
  void *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  memcpy(mapped, vertices, count * sizeof(*vertices));
  memcpy(mesh->radial_extent, extent, sizeof(extent));
  mesh->selection_count = 0;
  mesh->selection_dirty = false;
  PublishMesh(mesh, count);
  return true;
}

bool Sim3DDepthPass_SelectRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthMeshRange *ranges, size_t range_count) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshRadial || mesh->queued ||
      !ranges || !range_count || range_count > kSim3DDepthMaximumRadialSourceQuads) return false;
  size_t quads = 0;
  for (size_t i = 0; i < range_count; ++i) {
    if (ranges[i].first_quad > mesh->count / 4 ||
        ranges[i].quad_count > mesh->count / 4 - ranges[i].first_quad ||
        ranges[i].quad_count > kSim3DDepthMaximumRadialSourceQuads - quads) return false;
    quads += ranges[i].quad_count;
  }
  if (!quads) return false;
  const Uint32 count = (Uint32)quads * 6;
  SDL_GPUBuffer *buffer = mesh->selection;
  SDL_GPUTransferBuffer *transfer = mesh->selection_transfer;
  Uint32 capacity = mesh->selection_capacity;
  if (count > capacity) {
    capacity = capacity ? capacity : 6144;
    while (capacity < count) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {
      .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = capacity * (Uint32)sizeof(Uint32),
    };
    const SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = info.size,
    };
    buffer = SDL_CreateGPUBuffer(g_depth_pass.device, &info);
    transfer = SDL_CreateGPUTransferBuffer(g_depth_pass.device, &transfer_info);
    if (!buffer || !transfer) {
      if (buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
      if (transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      return false;
    }
  }
  Uint32 *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, transfer, true);
  if (!mapped) {
    if (buffer != mesh->selection) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
    if (transfer != mesh->selection_transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    return false;
  }
  const Uint32 order[] = {0,1,2,0,2,3};
  Uint32 at = 0;
  for (size_t i = 0; i < range_count; ++i) for (size_t q = 0; q < ranges[i].quad_count; ++q) {
    const Uint32 base = (Uint32)(ranges[i].first_quad + q) * 4;
    for (unsigned p = 0; p < 6; ++p) mapped[at++] = base + order[p];
  }
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, transfer);
  if (buffer != mesh->selection) {
    if (mesh->selection) SDL_ReleaseGPUBuffer(g_depth_pass.device, mesh->selection);
    if (mesh->selection_transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, mesh->selection_transfer);
  }
  mesh->selection = buffer;
  mesh->selection_transfer = transfer;
  mesh->selection_capacity = capacity;
  mesh->selection_count = count;
  mesh->selection_dirty = true;
  return true;
}

static bool RadialTransformSafe(const Sim3DDepthMesh *mesh, const Sim3DDepthRadialTransform *t,
    float extra_scale, double attribute_scale) {
  if (!t || t->variant > 65535 || !isfinite(t->sphere_radius) || t->sphere_radius <= 0 ||
      !isfinite(t->reference_height) || !isfinite(t->height_scale) || t->height_scale < 0 ||
      !isfinite(extra_scale) || extra_scale < 0) return false;
  const double limit = (double)FLT_MAX / (1.0 + 32.0 * FLT_EPSILON);
  const double reference = fabs((double)t->reference_height);
  const double radius = t->sphere_radius + reference * t->height_scale;
  const double anchor_delta = mesh->radial_extent[0] + reference;
  const double rise = anchor_delta * t->height_scale + (double)mesh->radial_extent[1] * extra_scale;
  if (radius > limit || anchor_delta > limit || rise > limit) return false;
  double world[3];
  for (unsigned row = 0; row < 3; ++row) {
    double normal = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (!isfinite(t->basis[row][axis])) return false;
      normal += fabs((double)t->basis[row][axis]) * 1.001;
    }
    world[row] = radius * (normal + (row == 2)) + normal * rise;
    if (normal + (row == 2) > limit || world[row] > limit) return false;
  }
  for (unsigned row = 0; row < 4; ++row) {
    if (!isfinite(t->matrix[12 + row])) return false;
    double clip = fabs((double)t->matrix[12 + row]);
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (!isfinite(t->matrix[axis * 4 + row])) return false;
      clip += fabs((double)t->matrix[axis * 4 + row]) * world[axis];
    }
    /* Explicit affine interpolation emits attribute * W. Bound that product
     * too, not just the position (surface lighting can exceed unit RGB). */
    if (clip > limit || (row == 3 && clip * attribute_scale > limit)) return false;
  }
  return true;
}

bool Sim3DDepthPass_AppendRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthRadialTransform *transform) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshRadial ||
      g_depth_pass.geometry_sample_count == kMaximumGeometrySamples ||
      !RadialTransformSafe(mesh, transform, 1, 1)) return false;
  Sim3DMeshSample *sample = &g_depth_pass.samples[g_depth_pass.sample_count++];
  ++g_depth_pass.geometry_sample_count;
  *sample = (Sim3DMeshSample){.mesh = mesh, .layer = kSim3DDepthPass_Solid,
    .count = mesh->selection_count ? mesh->selection_count / 6 * 4 : mesh->count,
    .ordinary_before = g_depth_pass.lists[kSim3DDepthPass_Solid].count};
  memcpy(sample->radial.matrix, transform->matrix, sizeof(sample->radial.matrix));
  for (unsigned row = 0; row < 3; ++row)
    memcpy(sample->radial.basis[row], transform->basis[row], sizeof(transform->basis[row]));
  sample->radial.radial[0] = transform->sphere_radius;
  sample->radial.radial[1] = transform->reference_height;
  sample->radial.radial[2] = transform->height_scale;
  sample->radial.radial[3] = (float)transform->variant;
  mesh->queued = true;
  return true;
}

bool Sim3DDepthPass_AppendMeshSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const ArRenderPointF *uv, size_t quad_count,
    ArRenderColorF color) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshScreen || !MeshLayerSupported(layer) ||
      !uv || !ValidSampleColor(color) || quad_count != mesh->count / 4 ||
      g_depth_pass.lists[layer].count ||
      g_depth_pass.sample_count - g_depth_pass.geometry_sample_count == kMaximumEffectSamples ||
      mesh->count > kMaximumSampleVertices - g_depth_pass.sample_vertices)
    return false;
  const Uint32 count = g_depth_pass.sample_vertices + mesh->count;
  if (count > g_depth_pass.sample_capacity) {
    Uint32 capacity = g_depth_pass.sample_capacity ? g_depth_pass.sample_capacity : 4096;
    while (capacity < count) capacity *= 2;
    void *storage = realloc(g_depth_pass.sample_uv, (size_t)capacity * sizeof(*uv));
    if (!storage) return false;
    g_depth_pass.sample_uv = storage;
    g_depth_pass.sample_capacity = capacity;
  }
  memcpy(g_depth_pass.sample_uv + g_depth_pass.sample_vertices, uv, mesh->count * sizeof(*uv));
  g_depth_pass.samples[g_depth_pass.sample_count++] = (Sim3DMeshSample){
    .mesh = mesh, .layer = layer, .first = g_depth_pass.sample_vertices, .color = color,
  };
  g_depth_pass.sample_vertices = count;
  mesh->queued = true;
  return true;
}

static bool ValidSphericalSample(const Sim3DDepthSphericalSample *sample) {
  if (!sample || !ValidSampleColor(sample->color)) return false;
  for (int i = 0; i < 4; ++i) if (!isfinite(sample->rotation[i])) return false;
  if (!isfinite(sample->offset.x) || !isfinite(sample->offset.y) ||
      !isfinite(sample->texture_size.x) || !isfinite(sample->texture_size.y) ||
      sample->texture_size.x <= 0 || sample->texture_size.y <= 0 ||
      sample->atlas.x < 0 || sample->atlas.y < 0 || sample->atlas.w < 2 || sample->atlas.h < 2 ||
      (double)sample->atlas.x + 2.0 * sample->atlas.w > sample->texture_size.x ||
      (double)sample->atlas.y + sample->atlas.h > sample->texture_size.y) return false;
  return true;
}

static void StoreSphericalSample(Sim3DSphericalUniform *uniform,
    const Sim3DDepthSphericalSample *sample) {
  memcpy(uniform->rotation, sample->rotation, sizeof(uniform->rotation));
  uniform->offset_extent[0] = sample->offset.x;
  uniform->offset_extent[1] = sample->offset.y;
  uniform->offset_extent[2] = sample->texture_size.x;
  uniform->offset_extent[3] = sample->texture_size.y;
  uniform->atlas[0] = (float)sample->atlas.x;
  uniform->atlas[1] = (float)sample->atlas.y;
  uniform->atlas[2] = (float)sample->atlas.w;
  uniform->atlas[3] = (float)sample->atlas.h;
  memcpy(uniform->color, &sample->color, sizeof(uniform->color));
}

bool Sim3DDepthPass_AppendSphericalSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const Sim3DDepthSphericalSample *sample) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshSpherical ||
      !MeshLayerSupported(layer) || !ValidSphericalSample(sample) ||
      g_depth_pass.lists[layer].count ||
      g_depth_pass.sample_count - g_depth_pass.geometry_sample_count == kMaximumEffectSamples)
    return false;
  Sim3DMeshSample *command = &g_depth_pass.samples[g_depth_pass.sample_count++];
  *command = (Sim3DMeshSample){.mesh = mesh, .layer = layer, .color = sample->color};
  StoreSphericalSample(&command->spherical, sample);
  mesh->queued = true;
  return true;
}

static bool UnitVectorOrZero(const float v[3], bool allow_zero) {
  double norm = 0;
  for (unsigned i = 0; i < 3; ++i) norm += (double)v[i] * v[i];
  return isfinite(norm) && ((allow_zero && norm == 0) || fabs(norm - 1) <= .002001);
}

bool Sim3DDepthPass_UpdateSphericalBodyMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalBodyVertex *vertices, size_t quad_count) {
  if (!g_depth_pass.collecting || !mesh || mesh->kind != kMeshSphericalBody || !vertices ||
      !CreateBodyPipeline() || !CanUpdateMesh(mesh,quad_count,kMeshSphericalBody)) return false;
  const Uint32 count = (Uint32)quad_count*4;
  for (Uint32 i = 0; i < count; ++i)
    if (!UnitVectorOrZero(vertices[i].normal,false) || !isfinite(vertices[i].opacity) ||
        vertices[i].opacity < 0 || vertices[i].opacity > 1) return false;
  if (!ReserveMesh(mesh,count)) return false;
  void *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device,mesh->transfer,true);
  if (!mapped) return false;
  memcpy(mapped,vertices,count*sizeof(*vertices));
  PublishMesh(mesh,count);
  return true;
}

static bool OrthonormalBasis(const float basis[3][3]) {
  for (unsigned r = 0; r < 3; ++r) {
    if (!UnitVectorOrZero(basis[r],false)) return false;
    for (unsigned s = 0; s < r; ++s) {
      double dot = 0;
      for (unsigned c = 0; c < 3; ++c) dot += (double)basis[r][c]*basis[s][c];
      if (fabs(dot) > .002001) return false;
    }
  }
  return true;
}

bool Sim3DDepthPass_AppendSphericalBodies(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalBodyTransform *t,
    const Sim3DDepthSphericalSample *samples, size_t count) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshSphericalBody || !t || !samples ||
      !count || count > kMaximumEffectSamples -
          (g_depth_pass.sample_count-g_depth_pass.geometry_sample_count) ||
      g_depth_pass.lists[kSim3DDepthPass_Cloud].count ||
      !isfinite(t->radius) || t->radius <= 0 || !OrthonormalBasis(t->basis) ||
      !OrthonormalBasis(t->texture_basis)) return false;
  /* Bound every transformed source point/clip coordinate, including overflow
   * in otherwise finite inputs. No per-vertex camera walk or borrowed data. */
  double extent[4] = {0,0,0,1};
  for (unsigned r = 0; r < 3; ++r) {
    if (!isfinite(t->centre[r])) return false;
    extent[r] = fabs((double)t->centre[r]) + 1.01*(double)t->radius;
    if (extent[r] > FLT_MAX/4.0) return false;
  }
  for (unsigned r = 0; r < 4; ++r) {
    double bound = 0;
    for (unsigned c = 0; c < 4; ++c) bound += fabs((double)t->matrix[c*4+r])*extent[c];
    if (!isfinite(bound) || bound > FLT_MAX/4.0) return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!ValidSphericalSample(&samples[i])) return false;
    const float *r = samples[i].rotation;
    if (fabs((double)r[0]*r[0]+(double)r[1]*r[1]-1) > .002001 ||
        fabs((double)r[2]*r[2]+(double)r[3]*r[3]-1) > .002001) return false;
  }
  Sim3DBodyView view = {0};
  memcpy(view.matrix,t->matrix,sizeof(view.matrix));
  memcpy(view.centre_radius, t->centre, sizeof(t->centre));
  view.centre_radius[3] = t->radius;
  for (unsigned r = 0; r < 3; ++r) {
    memcpy(view.basis[r],t->basis[r],sizeof(t->basis[r]));
    memcpy(view.texture_basis[r],t->texture_basis[r],sizeof(t->texture_basis[r]));
  }
  for (size_t i = 0; i < count; ++i) {
    Sim3DMeshSample *out = &g_depth_pass.samples[g_depth_pass.sample_count++];
    *out = (Sim3DMeshSample){.mesh = mesh, .layer = kSim3DDepthPass_Cloud};
    out->body.view = view;
    memcpy(out->body.view.rotation,samples[i].rotation,sizeof(view.rotation));
    StoreSphericalSample(&out->body.sample,&samples[i]);
  }
  mesh->queued = true;
  return true;
}

bool Sim3DDepthPass_UpdateSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceVertex *vertices, size_t quad_count) {
  return Sim3DDepthPass_UpdateSurfaceMeshWithMask(mesh,vertices,NULL,quad_count);
}

bool Sim3DDepthPass_UpdateSurfaceMeshWithMask(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceVertex *vertices, const ArRenderPointF *mask_uv,
    size_t quad_count) {
  if (!g_depth_pass.collecting || !mesh || mesh->kind != kMeshSurface || !vertices ||
      !CreateSurfacePipelines() || !CanUpdateMesh(mesh, quad_count, kMeshSurface)) return false;
  const Uint32 count = (Uint32)quad_count * 4;
  float extent[2] = {0}, uv_extent = 0;
  for (Uint32 i = 0; i < count; ++i) {
    const Sim3DDepthSurfaceVertex *v = &vertices[i];
    if (!UnitVectorOrZero(v->normal, false) || !UnitVectorOrZero(v->shade_normal, true) ||
        !ValidSampleColor(v->color) || !isfinite(v->uv.x) || !isfinite(v->uv.y) ||
        fabsf(v->uv.x) > FLT_MAX / 4 || fabsf(v->uv.y) > FLT_MAX / 4) return false;
    uv_extent = fmaxf(uv_extent, fmaxf(fabsf(v->uv.x), fabsf(v->uv.y)));
    if (mask_uv && (!isfinite(mask_uv[i].x) || !isfinite(mask_uv[i].y) ||
        fabsf(mask_uv[i].x) > 16 || fabsf(mask_uv[i].y) > 16)) return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
      if (!isfinite(v->elevation[axis])) return false;
      extent[axis] = fmaxf(extent[axis], fabsf(v->elevation[axis]));
    }
  }
  if (!ReserveMesh(mesh, count)) return false;
  Sim3DSurfaceGpuQuad *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (Uint32 i = 0; i < count; ++i) {
    const Sim3DDepthSurfaceVertex *v = &vertices[i];
    Sim3DSurfaceGpuQuad *q = &mapped[i / 4];
    const unsigned p = i % 4;
    memcpy(q->points[p], v->normal, sizeof(v->normal));
    q->points[p][3] = v->elevation[0];
    memcpy(q->shades[p], v->shade_normal, sizeof(v->shade_normal));
    q->shades[p][3] = v->elevation[1];
    memcpy(q->colors[p], &v->color, sizeof(v->color));
    memcpy(q->uv[p], &v->uv, sizeof(v->uv));
    memcpy(q->mask_uv[p], mask_uv ? &mask_uv[i] : &v->uv, sizeof(v->uv));
  }
  memcpy(mesh->radial_extent, extent, sizeof(extent));
  mesh->surface_uv_extent = uv_extent;
  mesh->surface_selected = false;
  mesh->surface_range_count = 0;
  mesh->selection_count = 0;
  mesh->selection_dirty = false;
  PublishMesh(mesh, count);
  return true;
}

bool Sim3DDepthPass_SelectSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthMeshRange *ranges, size_t range_count) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshSurface || mesh->queued ||
      range_count > kMaximumSurfaceRanges || (range_count && !ranges)) return false;
  if (!range_count) {
    mesh->surface_selected = false;
    mesh->surface_range_count = 0;
    mesh->selection_count = 0;
    mesh->selection_dirty = false;
    return true;
  }
  size_t quads = 0;
  for (size_t i = 0; i < range_count; ++i) {
    if (ranges[i].first_quad > mesh->count/4 ||
        ranges[i].quad_count > mesh->count/4-ranges[i].first_quad ||
        ranges[i].quad_count > kMaximumRetainedVertices/4-quads) return false;
    quads += ranges[i].quad_count;
  }
  if (mesh->surface_selected && range_count == mesh->surface_range_count &&
      !memcmp(ranges,mesh->surface_ranges,range_count*sizeof(*ranges))) return true;
  const Uint32 count = (Uint32)quads*4;
  if (count > mesh->selection_capacity) {
    Uint32 capacity = mesh->selection_capacity ? mesh->selection_capacity : 4096;
    while (capacity < count) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {
      .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = capacity*MeshVertexBytes(mesh),
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g_depth_pass.device,&info);
    if (!buffer) return false;
    if (mesh->selection) SDL_ReleaseGPUBuffer(g_depth_pass.device,mesh->selection);
    mesh->selection = buffer;
    mesh->selection_capacity = capacity;
  }
  memcpy(mesh->surface_ranges,ranges,range_count*sizeof(*ranges));
  mesh->surface_range_count = (Uint32)range_count;
  mesh->surface_selected = true;
  mesh->selection_count = count;
  mesh->selection_dirty = count != 0;
  return true;
}

bool Sim3DDepthPass_AppendSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count) {
  return Sim3DDepthPass_AppendSurfaceLayers(mesh, transform, shadows, shadow_count, NULL, 0);
}

static bool SurfaceLayersSafe(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count,
    const Sim3DDepthSurfaceOverlay *overlays, size_t overlay_count) {
  /* Ambient/diffuse bounds plus unit-vector tolerance give RGB gain <2.003.
   * Leave rounding margin when bounding the affine attribute-times-W payload. */
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshSurface || !transform ||
      transform->radial.variant ||
      !RadialTransformSafe(mesh, &transform->radial, transform->extra_scale,
                           fmax(2.01, mesh->surface_uv_extent)) ||
      !UnitVectorOrZero(transform->light, true) || !isfinite(transform->ambient) ||
      transform->ambient < 0 || transform->ambient > 1 || !isfinite(transform->diffuse) ||
      transform->diffuse < 0 || transform->diffuse > 1 ||
      g_depth_pass.geometry_sample_count == kMaximumGeometrySamples ||
      shadow_count > kMaximumEffectSamples -
              (g_depth_pass.sample_count - g_depth_pass.geometry_sample_count) ||
      (shadow_count && (!shadows || g_depth_pass.lists[kSim3DDepthPass_CloudShadow].count)))
    return false;
  const Sim3DDepthSurfaceFocus *focus=&transform->focus;
  if (!ValidSampleColor(focus->haze) || !isfinite(focus->dim) || focus->dim<0 || focus->dim>1 ||
      !isfinite(focus->feather) || focus->feather<0 || focus->feather>16 ||
      (focus->feather && focus->feather<.000001f) ||
      !isfinite(focus->corner_radius) || focus->corner_radius<0 || focus->corner_radius>16 ||
      !isfinite(focus->inset) || focus->inset<0 || focus->inset>16 ||
      !isfinite(focus->clear_rect.x) || !isfinite(focus->clear_rect.y) ||
      !isfinite(focus->clear_rect.w) || !isfinite(focus->clear_rect.h) ||
      fabsf(focus->clear_rect.x)>16 || fabsf(focus->clear_rect.y)>16 ||
      focus->clear_rect.w<0 || focus->clear_rect.w>16 ||
      focus->clear_rect.h<0 || focus->clear_rect.h>16) return false;
  if (overlay_count > kSim3DDepthMaximumSurfaceOverlays || (overlay_count && !overlays) ||
      overlay_count > kMaximumEffectSamples -
              (g_depth_pass.sample_count - g_depth_pass.geometry_sample_count) - shadow_count)
    return false;
  unsigned overlay_mask = 0;
  for (size_t i = 0; i < overlay_count; ++i) {
    const Sim3DDepthSurfaceOverlay *o = &overlays[i];
    if ((o->layer != kSim3DDepthPass_GroundBlur && o->layer != kSim3DDepthPass_GroundHaze &&
         o->layer != kSim3DDepthPass_ShadowReceiver) ||
        (o->layer == kSim3DDepthPass_GroundHaze && ArRenderTexture_IsValid(o->texture)) ||
        !ValidSampleColor(o->color) || !isfinite(o->feather) || o->feather < 0 ||
        o->feather > 16 || (o->feather && o->feather < 0.000001f) ||
        !isfinite(o->clear_rect.x) || !isfinite(o->clear_rect.y) ||
        !isfinite(o->clear_rect.w) || !isfinite(o->clear_rect.h) ||
        fabsf(o->clear_rect.x) > 16 || fabsf(o->clear_rect.y) > 16 ||
        o->clear_rect.w < 0 || o->clear_rect.h < 0 ||
        o->clear_rect.w > 16 || o->clear_rect.h > 16 || mesh->surface_uv_extent > 16 ||
        (overlay_mask & (1u << o->layer))) return false;
    overlay_mask |= 1u << o->layer;
  }
  for (size_t i = 0; i < shadow_count; ++i) {
    if (!ValidSphericalSample(&shadows[i]) || fabsf(shadows[i].offset.x) > FLT_MAX / 4 ||
        fabsf(shadows[i].offset.y) > FLT_MAX / 4)
      return false;
    /* Bound the trigonometric input arithmetic as well as its final output.
     * Unlike arbitrary matrices, these pairs explicitly represent rotations. */
    for (unsigned p = 0; p < 4; p += 2) {
      const double norm = (double)shadows[i].rotation[p] * shadows[i].rotation[p] +
          (double)shadows[i].rotation[p + 1] * shadows[i].rotation[p + 1];
      if (fabs(norm - 1) > .002001) return false;
    }
  }
  bool identity = true;
  for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
    identity &= transform->shadow_basis[r][c] == 0;
  if (!identity) for (unsigned r = 0; r < 3; ++r) {
    if (!UnitVectorOrZero(transform->shadow_basis[r],false)) return false;
    for (unsigned s = 0; s < r; ++s) {
      double dot = 0;
      for (unsigned c = 0; c < 3; ++c)
        dot += (double)transform->shadow_basis[r][c]*transform->shadow_basis[s][c];
      if (fabs(dot) > .002001) return false;
    }
  }
  return true;
}

static void QueueSurfaceLayers(Sim3DDepthMesh *mesh, Sim3DDepthPassLayer opaque_layer,
    Sim3DDepthMeshRange range, SDL_GPUTexture *texture,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count,
    const Sim3DDepthSurfaceOverlay *overlays, size_t overlay_count,
    SDL_GPUTexture *const *overlay_textures) {
  /* All validation/budget checks precede any queue mutation. A malformed
   * final shadow cannot leave an opaque-only surface stranded in the pass. */
  Sim3DSurfaceUniform uniform = {0};
  memcpy(uniform.view.matrix, transform->radial.matrix, sizeof(uniform.view.matrix));
  for (unsigned row = 0; row < 3; ++row)
    memcpy(uniform.view.basis[row], transform->radial.basis[row],
           sizeof(transform->radial.basis[row]));
  uniform.view.radial[0] = transform->radial.sphere_radius;
  uniform.view.radial[1] = transform->radial.reference_height;
  uniform.view.radial[2] = transform->radial.height_scale;
  uniform.view.radial[3] = transform->extra_scale;
  memcpy(uniform.light, transform->light, sizeof(transform->light));
  uniform.light[3] = transform->ambient;
  uniform.material[0] = transform->diffuse;
  /* Reuse material registers otherwise reserved for overlays: focus is part
   * of the base draw, not another pass, vertex stream or shader variant. */
  const Sim3DDepthSurfaceFocus *focus=&transform->focus;
  if (focus->dim>0 || focus->haze.a>0) {
    uniform.material[1]=4;
    memcpy(uniform.spherical.color,&focus->haze,sizeof(focus->haze));
    uniform.mask_rect[0] = focus->clear_rect.x;
    uniform.mask_rect[1] = focus->clear_rect.y;
    uniform.mask_rect[2]=focus->clear_rect.x+focus->clear_rect.w;
    uniform.mask_rect[3]=focus->clear_rect.y+focus->clear_rect.h;
    uniform.mask[0] = focus->feather;
    uniform.mask[1] = focus->dim;
    uniform.mask[2] = focus->corner_radius;
    uniform.mask[3] = focus->inset;
  }
  for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) {
    uniform.shadow_basis[r][c] = transform->shadow_basis[r][c];
    if (transform->shadow_basis[r][c] != 0) uniform.material[2] = 1;
  }
  for (size_t i = 0; i <= shadow_count + overlay_count; ++i) {
    Sim3DDepthPassLayer layer = opaque_layer;
    if (i && i <= shadow_count) {
      uniform.material[1] = 1;
      StoreSphericalSample(&uniform.spherical, &shadows[i - 1]);
      layer = kSim3DDepthPass_CloudShadow;
    } else if (i) {
      const Sim3DDepthSurfaceOverlay *o = &overlays[i - shadow_count - 1];
      layer = o->layer;
      uniform.material[1] = layer == kSim3DDepthPass_GroundHaze ? 3 : 2;
      memcpy(uniform.spherical.color, &o->color, sizeof(o->color));
      uniform.mask_rect[0] = o->clear_rect.x;
      uniform.mask_rect[1] = o->clear_rect.y;
      uniform.mask_rect[2] = o->clear_rect.x + o->clear_rect.w;
      uniform.mask_rect[3] = o->clear_rect.y + o->clear_rect.h;
      uniform.mask[0] = o->feather;
    }
    g_depth_pass.samples[g_depth_pass.sample_count++] = (Sim3DMeshSample){
      .mesh = mesh, .layer = layer, .surface = uniform,
      .texture = !i ? texture : i > shadow_count ? overlay_textures[i-shadow_count-1] : NULL,
      .first = (Uint32)range.first_quad*4, .count = (Uint32)range.quad_count*4,
      .ordinary_before = g_depth_pass.lists[layer].count,
    };
  }
  ++g_depth_pass.geometry_sample_count;
  mesh->queued = true;
}

bool Sim3DDepthPass_AppendSurfaceBatches(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceBatch *batches, size_t batch_count) {
  if (!batches || !batch_count || batch_count > kMaximumGeometrySamples) return false;
  Sim3DDepthSurfaceMeshBatch source[kMaximumGeometrySamples];
  for (size_t i = 0; i < batch_count; ++i)
    source[i] = (Sim3DDepthSurfaceMeshBatch){mesh, batches[i]};
  return Sim3DDepthPass_AppendSurfaceMeshBatches(source, batch_count);
}

bool Sim3DDepthPass_AppendSurfaceMeshBatches(
    const Sim3DDepthSurfaceMeshBatch *batches, size_t batch_count) {
  if (!batches || !batch_count || batch_count > kMaximumGeometrySamples) return false;
  size_t geometry = 0, effects = 0;
  SDL_GPUTexture *textures[kMaximumGeometrySamples] = {0};
  SDL_GPUTexture *overlay_textures[kMaximumGeometrySamples][kSim3DDepthMaximumSurfaceOverlays] = {
    { 0 }
  };
  for (size_t i = 0; i < batch_count; ++i) {
    Sim3DDepthMesh *mesh = batches[i].mesh;
    if (!Sim3DDepthPass_MeshReady(mesh) || mesh->kind != kMeshSurface) return false;
    const size_t quads = (mesh->surface_selected ? mesh->selection_count : mesh->count)/4;
    const Sim3DDepthSurfaceBatch *b = &batches[i].batch;
    if (ArRenderTexture_IsValid(b->texture)) {
      textures[i] = GpuTexture(ArSdlRenderBackend_UnwrapTexture(b->texture));
      if (!textures[i]) return false;
    }
    if ((b->layer != kSim3DDepthPass_Ground && b->layer != kSim3DDepthPass_Mountain &&
         b->layer != kSim3DDepthPass_WorldMountain) ||
        (b->layer != kSim3DDepthPass_Ground && (b->shadow_count || b->overlay_count)) ||
        b->range.first_quad > quads || b->range.quad_count > quads - b->range.first_quad ||
        !SurfaceLayersSafe(mesh, &b->transform, b->shadows, b->shadow_count, b->overlays,
                           b->overlay_count))
      return false;
    for (size_t o = 0; o < b->overlay_count; ++o)
      if (ArRenderTexture_IsValid(b->overlays[o].texture)) {
        overlay_textures[i][o] =
            GpuTexture(ArSdlRenderBackend_UnwrapTexture(b->overlays[o].texture));
        if (!overlay_textures[i][o]) return false;
      }
    if (b->range.quad_count) { ++geometry; effects += b->shadow_count+b->overlay_count; }
  }
  if (geometry > kMaximumGeometrySamples - g_depth_pass.geometry_sample_count ||
      effects >
          kMaximumEffectSamples - (g_depth_pass.sample_count - g_depth_pass.geometry_sample_count))
    return false;
  for (size_t i = 0; i < batch_count; ++i) {
    Sim3DDepthMesh *mesh = batches[i].mesh;
    const Sim3DDepthSurfaceBatch *b = &batches[i].batch;
    if (b->range.quad_count)
      QueueSurfaceLayers(mesh, b->layer, b->range, textures[i], &b->transform, b->shadows,
                         b->shadow_count, b->overlays, b->overlay_count, overlay_textures[i]);
  }
  return true;
}

bool Sim3DDepthPass_AppendSurfaceLayers(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count,
    const Sim3DDepthSurfaceOverlay *overlays, size_t overlay_count) {
  if (!transform || !Sim3DDepthPass_MeshReady(mesh)) return false;
  const Sim3DDepthSurfaceBatch batch = {
    .layer = kSim3DDepthPass_Ground,
    .range = {0,(mesh->surface_selected ? mesh->selection_count : mesh->count)/4},
    .transform = *transform, .shadows = shadows, .shadow_count = shadow_count,
    .overlays = overlays, .overlay_count = overlay_count};
  return Sim3DDepthPass_AppendSurfaceBatches(mesh,&batch,1);
}

bool Sim3DDepthPass_AppendQuads(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex *vertices,
                               size_t quad_count) {
  if (!g_depth_pass.collecting || !vertices || layer < 0 ||
      layer >= kSim3DDepthPassLayerCount)
    return false;
  if (!quad_count) return true;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    if (g_depth_pass.samples[i].layer == layer && !OrderedLayerSupported(layer)) return false;
  if (quad_count > UINT32_MAX / kSim3DDepthVerticesPerQuad) return false;
  const Uint32 vertex_count =
      (Uint32)quad_count * kSim3DDepthVerticesPerQuad;
  Sim3DDepthList *list = &g_depth_pass.lists[layer];
  if (!ReserveList(list, vertex_count)) {
    g_depth_pass.geometry_failed = true;
    return false;
  }
  for (Uint32 i = 0; i < vertex_count; i++) {
    /* Read a complete value before writing backend storage. Besides keeping
     * the conversion independent of caller storage, this lets the compiler
     * group unchanged color/UV transfers without alias checks per field. */
    const Sim3DDepthVertex source = vertices[i];
    list->vertices[list->count++] = PackScreenVertex(source);
  }
  return true;
}

bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex vertices[4]) {
  return Sim3DDepthPass_AppendQuads(layer, vertices, 1);
}
