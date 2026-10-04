/* Sim3DDepthPass (SDL): the SDL GPU implementation of the depth-pass contract
 * (sim3d_depth_pass.h). This file owns the pass state, the atlas cache and its
 * texture uploads, billboards, begin, submission and reset. Pipeline setup and
 * mesh management live in sim3d_depth_pass_{pipelines,meshes}_sdl.c.
 * Phase: present (render owner thread).
 * Tests: tests/sim3d_depth_pass_gpu_test.c */
#include "platform/sdl/sim3d_depth_pass_sdl_internal.h"
bool IsModelMesh(Sim3DMeshKind kind) {
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
  return kind == kMeshModel || kind == kMeshModelClipped;
#else
  (void)kind;
  return false;
#endif
}
typedef struct Sim3DSurfaceShadowBatchUniform {
  Sim3DSurfaceUniform surface;
  float taps[kSurfaceShadowBatchTaps][4];
} Sim3DSurfaceShadowBatchUniform;
_Static_assert(sizeof(Sim3DSurfaceShadowBatchUniform) == 352 &&
    offsetof(Sim3DSurfaceShadowBatchUniform, taps) == 304, "std140 three-tap surface shadows");
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
_Static_assert(sizeof(Sim3DModelUniform) == 80, "std140 model transform");
_Static_assert(sizeof(Sim3DDepthModelVertex) == 7 * sizeof(float) &&
    offsetof(Sim3DDepthModelVertex, color) == 3 * sizeof(float), "packed model vertex");
#endif

struct Sim3DDepthAtlasCache {
  SDL_GPUDevice *device;
  SDL_GPUTexture *versions[kSim3DDepthAtlasVersionLimit];
  int widths[kSim3DDepthAtlasVersionLimit], heights[kSim3DDepthAtlasVersionLimit];
  SDL_GPUTransferBuffer *transfer;
  Uint32 transfer_size;
};
static Sim3DDepthAtlasCache *s_atlas_cache;

/* Handles outlive a renderer reset; only their platform payload is reset.
 * This registry is never published outside the owner thread. */
Sim3DDepthMesh *g_depth_pass_meshes;

#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
bool g_depth_pass_reject_linear_preparation;
void Sim3DDepthReference_RejectLinearPreparation(bool reject) {
  g_depth_pass_reject_linear_preparation = reject;
}
#endif

Sim3DDepthPassState g_depth_pass;

bool Sim3DDepthPass_LinearMeshesAvailable(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  return renderer && renderer == g_depth_pass.renderer && !g_depth_pass.failed &&
      g_depth_pass.model_pipeline[kModelPipeline_Linear] != NULL;
}

static bool GroundIsQueued(void) {
  if (!g_depth_pass.collecting) return false;
  if (g_depth_pass.lists[kSim3DDepthPass_Ground].count) return true;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    if (g_depth_pass.samples[i].layer == kSim3DDepthPass_Ground) return true;
  return false;
}

static void ReleaseAtlasCacheStorage(Sim3DDepthAtlasCache *cache) {
  if (!cache) return;
  for (unsigned i = 0; i < kSim3DDepthAtlasVersionLimit; ++i)
    if (cache->versions[i]) SDL_ReleaseGPUTexture(cache->device, cache->versions[i]);
  if (cache->transfer) SDL_ReleaseGPUTransferBuffer(cache->device, cache->transfer);
  memset(cache, 0, sizeof(*cache));
  g_depth_pass.selected_ground = NULL;
}

Sim3DDepthAtlasCache *Sim3DDepthPass_CreateAtlasCache(void) {
  if (s_atlas_cache || !g_depth_pass.device) return NULL;
  s_atlas_cache = calloc(1, sizeof(*s_atlas_cache));
  return s_atlas_cache;
}

bool Sim3DDepthPass_HasAtlasVersion(const Sim3DDepthAtlasCache *cache, unsigned version) {
  return cache && cache == s_atlas_cache && version < kSim3DDepthAtlasVersionLimit &&
      cache->device == g_depth_pass.device && cache->versions[version];
}

bool Sim3DDepthPass_SelectAtlasVersion(Sim3DDepthAtlasCache *cache, unsigned version) {
  if (!g_depth_pass.collecting || GroundIsQueued()) return false;
  if (!cache && version == 0) {
    g_depth_pass.selected_ground = NULL;
    return true;
  }
  if (!Sim3DDepthPass_HasAtlasVersion(cache, version)) return false;
  g_depth_pass.selected_ground = cache->versions[version];
  return true;
}

bool Sim3DDepthPass_CaptureAtlasVersion(Sim3DDepthAtlasCache *cache, unsigned version) {
  const Sim3DDepthAtlas *atlas = &g_depth_pass.atlases[kSim3DDepthPass_Ground];
  if (!cache || cache != s_atlas_cache || version >= kSim3DDepthAtlasVersionLimit ||
      GroundIsQueued() || !g_depth_pass.device || !atlas->texture ||
      atlas->width <= 0 || atlas->height <= 0 ||
      atlas->width > 2048 || atlas->height > 2048 ||
      (cache->device && cache->device != g_depth_pass.device)) return false;
  SDL_GPUTextureCreateInfo info = {
    .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = (Uint32)atlas->width,
    .height = (Uint32)atlas->height, .layer_count_or_depth = 1, .num_levels = 1,
    .sample_count = SDL_GPU_SAMPLECOUNT_1,
  };
  /* Never overwrite/cycle a valid version before successful submission. */
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(g_depth_pass.device, &info);
  if (!texture) return false;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  const SDL_GPUTextureLocation source = {.texture = atlas->texture};
  const SDL_GPUTextureLocation destination = {.texture = texture};
  SDL_CopyGPUTextureToTexture(copy, &source, &destination,
      (Uint32)atlas->width, (Uint32)atlas->height, 1, false);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_GPUTexture *old = cache->versions[version];
  if (old && g_depth_pass.selected_ground == old) g_depth_pass.selected_ground = texture;
  if (old) SDL_ReleaseGPUTexture(g_depth_pass.device, old);
  cache->device = g_depth_pass.device;
  cache->versions[version] = texture;
  cache->widths[version] = atlas->width;
  cache->heights[version] = atlas->height;
  Sim3DPerformance_AddAtlasCopy((uint64_t)atlas->width * atlas->height * 4);
  return true;
}

void Sim3DDepthPass_DestroyAtlasCache(Sim3DDepthAtlasCache *cache) {
  if (!cache || cache != s_atlas_cache) return;
  if (g_depth_pass.selected_ground && GroundIsQueued()) g_depth_pass.geometry_failed = true;
  ReleaseAtlasCacheStorage(cache);
  free(cache);
  s_atlas_cache = NULL;
}

static bool UploadAtlasRegions(
    ArRenderDevice *device, Sim3DDepthAtlas *atlas,
    const uint32_t *argb_pixels,
    int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || !argb_pixels || width <= 0 || height <= 0 || pitch <= 0 ||
      !regions || region_count <= 0)
    return false;
  if ((size_t)width > SIZE_MAX / sizeof(uint32_t)) return false;
  const size_t row_bytes = (size_t)width * sizeof(uint32_t);
  if ((size_t)pitch < row_bytes ||
      row_bytes > UINT32_MAX ||
      (size_t)height > UINT32_MAX / row_bytes ||
      (size_t)height > SIZE_MAX / (size_t)pitch ||
      !EnsureInitialized(renderer))
    return false;
  const bool align_for_d3d12 = TextureUploadsNeedAlignment();
  Uint32 upload_size = 0;
  ArSdlTextureUploadLayout layout;
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    if (dirty->x < 0 || dirty->y < 0 || dirty->w <= 0 || dirty->h <= 0 ||
        dirty->x > width - dirty->w || dirty->y > height - dirty->h ||
        !ArSdlTextureUploadLayout_Append(dirty->w, dirty->h, align_for_d3d12,
            &upload_size, &layout))
      return false;
  }

  const bool replace_texture = !atlas->texture ||
      atlas->width != width || atlas->height != height;
  const bool replace_transfer = !atlas->transfer || atlas->transfer_size < upload_size;
  SDL_GPUTexture *texture = atlas->texture;
  SDL_GPUTransferBuffer *transfer = atlas->transfer;
  if (replace_texture) {
    const SDL_GPUTextureCreateInfo info = {
      .type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
      .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
      .width = (Uint32)width, .height = (Uint32)height,
      .layer_count_or_depth = 1, .num_levels = 1,
      .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    texture = SDL_CreateGPUTexture(g_depth_pass.device, &info);
    if (!texture) return false;
  }
  if (replace_transfer) {
    const SDL_GPUTransferBufferCreateInfo info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = upload_size,
    };
    transfer = SDL_CreateGPUTransferBuffer(g_depth_pass.device, &info);
    if (!transfer) {
      if (replace_texture) SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
      return false;
    }
  }
  /* Fragmented updates can need more padded staging than a full image. Grow
   * that buffer independently: replacing the texture would lose clean texels. */
  if (replace_texture) {
    if (atlas->texture) SDL_ReleaseGPUTexture(g_depth_pass.device, atlas->texture);
    atlas->texture = texture;
    atlas->width = width;
    atlas->height = height;
  }
  if (replace_transfer) {
    if (atlas->transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, atlas->transfer);
    atlas->transfer = transfer;
    atlas->transfer_size = upload_size;
  }

  uint8_t *mapped = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, atlas->transfer, true);
  if (!mapped) {
    fprintf(stderr, "[sim3d-depth] material atlas upload allocation failed: %s\n",
            SDL_GetError());
    return false;
  }
  /* Repack every requested region into the current transfer generation.
   * Cycled storage cannot preserve untouched rows from a previous upload.
   * The preflight above validated every layout; replay it without allocation. */
  Uint32 packed_at = 0;
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    (void)ArSdlTextureUploadLayout_Append(dirty->w, dirty->h, align_for_d3d12,
        &packed_at, &layout);
    const uint8_t *source = (const uint8_t *)argb_pixels +
        (size_t)dirty->y * (size_t)pitch +
        (size_t)dirty->x * sizeof(uint32_t);
    /* ARGB8888 describes native-endian integer values; RGBA32 describes the
     * GPU's byte order on every host. SDL owns optimized conversion, while
     * the portable caller retains its existing ARGB/pitch/region contract.
     * Validation above bounds row_bytes by the positive int source pitch. */
    if (!SDL_ConvertPixels(dirty->w, dirty->h, SDL_PIXELFORMAT_ARGB8888,
            source, pitch, SDL_PIXELFORMAT_RGBA32,
            mapped + layout.offset, (int)layout.row_pitch)) {
      SDL_UnmapGPUTransferBuffer(g_depth_pass.device, atlas->transfer);
      return false;
    }
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, atlas->transfer);

  if (!SDL_FlushRenderer(renderer)) return false;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    return false;
  }
  packed_at = 0;
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    (void)ArSdlTextureUploadLayout_Append(dirty->w, dirty->h, align_for_d3d12,
        &packed_at, &layout);
    SDL_GPUTextureTransferInfo source_info = {
      .transfer_buffer = atlas->transfer,
      .offset = layout.offset,
      .pixels_per_row = layout.row_pitch / kSim3DDepthRgbaBytesPerPixel,
      .rows_per_layer = (Uint32)dirty->h,
    };
    SDL_GPUTextureRegion destination = {
      .texture = atlas->texture,
      .x = (Uint32)dirty->x,
      .y = (Uint32)dirty->y,
      .w = (Uint32)dirty->w,
      .h = (Uint32)dirty->h,
      .d = 1,
    };
    /* Mapping with `cycle=true` selected writable storage for this frame.
     * Every region in this transaction must reference that same generation. */
    SDL_UploadToGPUTexture(copy, &source_info, &destination, false);
    PerformanceMetrics_AddTextureUpload(1,
        (uint64_t)dirty->w * dirty->h * kSim3DDepthRgbaBytesPerPixel);
  }
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] material atlas upload submission failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

bool Sim3DDepthPass_UploadAtlasRegions(
    ArRenderDevice *device, Sim3DDepthPassLayer layer, const uint32_t *argb_pixels,
    int width, int height, int pitch, const ArRenderRectI *regions, int region_count) {
  if (layer != kSim3DDepthPass_Mountain && layer != kSim3DDepthPass_Ground &&
      layer != kSim3DDepthPass_GroundBlur && layer != kSim3DDepthPass_Cloud &&
      layer != kSim3DDepthPass_WorldMountain && layer != kSim3DDepthPass_VolumeCloud)
    return false;
  return UploadAtlasRegions(device, &g_depth_pass.atlases[layer], argb_pixels,
      width, height, pitch, regions, region_count);
}

bool Sim3DDepthPass_UpdateAtlasVersionRegions(
    ArRenderDevice *device, Sim3DDepthAtlasCache *cache, unsigned version,
    const uint32_t *argb_pixels, int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  if (!Sim3DDepthPass_HasAtlasVersion(cache, version) || GroundIsQueued() ||
      ArSdlRenderBackend_Renderer(device) != g_depth_pass.renderer ||
      width != cache->widths[version] || height != cache->heights[version]) return false;
  /* One bounded staging buffer serves all versions. Same-size partial writes
   * preserve clean texels and never replace/cycle the destination texture.
   * Commands are ordered after previous draws; failure before submission
   * leaves the published version intact, including its selected binding. */
  Sim3DDepthAtlas target = {.texture = cache->versions[version],
      .width = width, .height = height, .transfer = cache->transfer,
      .transfer_size = cache->transfer_size};
  const bool uploaded = UploadAtlasRegions(device, &target, argb_pixels,
      width, height, pitch, regions, region_count);
  cache->transfer = target.transfer;
  cache->transfer_size = target.transfer_size;
  return uploaded;
}

bool Sim3DDepthPass_UploadMountainAtlasRegions(
    ArRenderDevice *device, const uint32_t *argb_pixels,
    int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  return Sim3DDepthPass_UploadAtlasRegions(
      device, kSim3DDepthPass_Mountain, argb_pixels,
      width, height, pitch, regions, region_count);
}

bool ReserveList(Sim3DDepthList *list, Uint32 additional) {
  if (additional > UINT32_MAX - list->count) return false;
  Uint32 required = list->count + additional;
  if (required <= list->capacity) return true;
  Uint32 capacity = list->capacity ? list->capacity
      : kSim3DDepthInitialCpuVertexCapacity;
  while (capacity < required) {
    if (capacity > UINT32_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }
  if ((size_t)capacity > SIZE_MAX / sizeof(*list->vertices)) return false;
  Sim3DGpuVertex *vertices = realloc(
      list->vertices, (size_t)capacity * sizeof(*vertices));
  if (!vertices) return false;
  list->vertices = vertices;
  list->capacity = capacity;
  return true;
}

bool Sim3DDepthPass_Begin(ArRenderDevice *device, int width, int height,
                          ArRenderFilter output_filter) {
  g_depth_pass.selected_ground = NULL;
  g_depth_pass.billboard_texture = NULL;
  g_depth_pass.billboard_run_count = 0;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const SDL_ScaleMode output_scale_mode =
      output_filter == kArRenderFilter_Linear
          ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
  if (!renderer || width <= 0 || height <= 0 ||
      !EnsureInitialized(renderer) ||
      !CreateTargets(renderer, width, height, output_scale_mode))
    return false;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++)
    g_depth_pass.lists[i].count = 0;
  g_depth_pass.sample_count = g_depth_pass.geometry_sample_count = g_depth_pass.sample_vertices = 0;
  for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next) mesh->queued = false;
  g_depth_pass.geometry_failed = false;
  g_depth_pass.collecting = true;
  g_depth_pass.clip_x_scale = 2.0f / (float)width;
  g_depth_pass.clip_y_scale = 2.0f / (float)height;
  return true;
}

static bool AppendBillboards(ArRenderTexture atlas,
    const Sim3DDepthVertex *vertices, size_t quad_count,
    const Sim3DDepthBillboardRim *rim) {
  if (!g_depth_pass.collecting || !vertices || !ArRenderTexture_IsValid(atlas) ||
      g_depth_pass.lists[kSim3DDepthPass_Billboard].count/4 > kSim3DDepthMaximumBillboardQuads ||
      quad_count > kSim3DDepthMaximumBillboardQuads -
          g_depth_pass.lists[kSim3DDepthPass_Billboard].count/4) return false;
  SDL_GPUTexture *texture = GpuTexture(ArSdlRenderBackend_UnwrapTexture(atlas));
  if (!texture || (g_depth_pass.billboard_texture &&
                  g_depth_pass.billboard_texture != texture)) return false;
  const size_t count = quad_count * kSim3DDepthVerticesPerQuad;
  for (size_t i = 0; i < count; ++i) {
    const Sim3DDepthVertex *v = &vertices[i];
    if (!ValidPosition((Sim3DDepthPosition){v->x,v->y,v->depth}) ||
        !ValidSampleColor(v->color) || !isfinite(v->uv.x) || !isfinite(v->uv.y))
      return false;
  }
  const Sim3DBillboardRun style = {
    .first=g_depth_pass.lists[kSim3DDepthPass_Billboard].count, .count=(Uint32)count,
    .rim={.offset={rim && rim->color.a > 0 ? rim->sample_offset.x : 0,
                  rim && rim->color.a > 0 ? rim->sample_offset.y : 0},
          .color=rim && rim->color.a > 0 ? rim->color : (ArRenderColorF){0}}};
  Sim3DBillboardRun *last = g_depth_pass.billboard_run_count
      ? &g_depth_pass.billboard_runs[g_depth_pass.billboard_run_count-1] : NULL;
  const bool merge = last && !memcmp(&last->rim,&style.rim,sizeof(style.rim));
  if (quad_count && !merge &&
      g_depth_pass.billboard_run_count == kSim3DDepthMaximumBillboardQuads) return false;
  if (!Sim3DDepthPass_AppendQuads(kSim3DDepthPass_Billboard,vertices,quad_count))
    return false;
  if (quad_count) {
    g_depth_pass.billboard_texture = texture;
    if (merge) last->count += (Uint32)count;
    else g_depth_pass.billboard_runs[g_depth_pass.billboard_run_count++] = style;
  }
  return true;
}

bool Sim3DDepthPass_AppendBillboards(ArRenderTexture atlas,
    const Sim3DDepthVertex *vertices, size_t quad_count) {
  return AppendBillboards(atlas,vertices,quad_count,NULL);
}

bool Sim3DDepthPass_AppendRimBillboards(ArRenderTexture atlas,
    const Sim3DDepthVertex *vertices, size_t quad_count,
    const Sim3DDepthBillboardRim *rim) {
  if (!rim || !isfinite(rim->sample_offset.x) || !isfinite(rim->sample_offset.y) ||
      fabsf(rim->sample_offset.x)>1 || fabsf(rim->sample_offset.y)>1 ||
      (!rim->sample_offset.x && !rim->sample_offset.y) || !ValidSampleColor(rim->color))
    return false;
  return AppendBillboards(atlas,vertices,quad_count,rim);
}

static bool EnsureGpuBuffers(Uint32 vertex_count) {
  if (vertex_count <= g_depth_pass.gpu_vertex_capacity) return true;
  const Uint32 maximum_vertices =
      UINT32_MAX / (Uint32)sizeof(Sim3DGpuVertex);
  if (vertex_count > maximum_vertices) {
    fprintf(stderr, "[sim3d-depth] geometry buffer exceeds GPU size limit\n");
    return false;
  }
  Uint32 capacity = g_depth_pass.gpu_vertex_capacity
      ? g_depth_pass.gpu_vertex_capacity
      : kSim3DDepthInitialGpuVertexCapacity;
  while (capacity < vertex_count) {
    if (capacity > maximum_vertices / 2) {
      capacity = vertex_count;
      break;
    }
    capacity *= 2;
  }
  SDL_GPUBufferCreateInfo buffer_info = {
    .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
    .size = capacity * (Uint32)sizeof(Sim3DGpuVertex),
  };
  SDL_GPUTransferBufferCreateInfo transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = buffer_info.size,
  };
  SDL_GPUBuffer *vertex_buffer = SDL_CreateGPUBuffer(
      g_depth_pass.device, &buffer_info);
  SDL_GPUTransferBuffer *transfer_buffer = SDL_CreateGPUTransferBuffer(
      g_depth_pass.device, &transfer_info);
  const Uint32 index_capacity =
      capacity / kSim3DDepthVerticesPerQuad * kSim3DDepthIndicesPerQuad;
  SDL_GPUBufferCreateInfo index_buffer_info = {
    .usage = SDL_GPU_BUFFERUSAGE_INDEX,
    .size = index_capacity * (Uint32)sizeof(Uint32),
  };
  SDL_GPUTransferBufferCreateInfo index_transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = index_buffer_info.size,
  };
  SDL_GPUBuffer *index_buffer = SDL_CreateGPUBuffer(
      g_depth_pass.device, &index_buffer_info);
  SDL_GPUTransferBuffer *index_transfer_buffer =
      SDL_CreateGPUTransferBuffer(
          g_depth_pass.device, &index_transfer_info);
  if (!vertex_buffer || !transfer_buffer || !index_buffer ||
      !index_transfer_buffer) {
    fprintf(stderr, "[sim3d-depth] geometry buffer creation failed: %s\n",
            SDL_GetError());
    if (vertex_buffer)
      SDL_ReleaseGPUBuffer(g_depth_pass.device, vertex_buffer);
    if (transfer_buffer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer_buffer);
    if (index_buffer)
      SDL_ReleaseGPUBuffer(g_depth_pass.device, index_buffer);
    if (index_transfer_buffer)
      SDL_ReleaseGPUTransferBuffer(
          g_depth_pass.device, index_transfer_buffer);
    return false;
  }
  Uint32 *indices = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, index_transfer_buffer, false);
  if (!indices) {
    fprintf(stderr, "[sim3d-depth] index staging map failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUBuffer(g_depth_pass.device, vertex_buffer);
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer_buffer);
    SDL_ReleaseGPUBuffer(g_depth_pass.device, index_buffer);
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, index_transfer_buffer);
    return false;
  }
  static const Uint32 order[kSim3DDepthIndicesPerQuad] = {
    0, 1, 2, 0, 2, 3,
  };
  for (Uint32 quad = 0;
       quad < capacity / kSim3DDepthVerticesPerQuad; quad++) {
    const Uint32 base = quad * kSim3DDepthVerticesPerQuad;
    for (int i = 0; i < kSim3DDepthIndicesPerQuad; i++)
      indices[quad * kSim3DDepthIndicesPerQuad + (Uint32)i] =
          base + order[i];
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, index_transfer_buffer);
  if (g_depth_pass.vertex_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.vertex_buffer);
  if (g_depth_pass.transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.transfer_buffer);
  if (g_depth_pass.index_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.index_buffer);
  if (g_depth_pass.index_transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.index_transfer_buffer);
  g_depth_pass.vertex_buffer = vertex_buffer;
  g_depth_pass.transfer_buffer = transfer_buffer;
  g_depth_pass.index_buffer = index_buffer;
  g_depth_pass.index_transfer_buffer = index_transfer_buffer;
  g_depth_pass.gpu_vertex_capacity = capacity;
  g_depth_pass.gpu_index_capacity = index_capacity;
  g_depth_pass.index_upload_required = true;
  return true;
}

static SDL_GPUTexture *TextureForLayer(
    Sim3DDepthPassLayer layer, SDL_Texture *shadow_texture) {
  switch (layer) {
    case kSim3DDepthPass_Mountain:
    case kSim3DDepthPass_WorldMountain:
    case kSim3DDepthPass_GroundBlur:
    case kSim3DDepthPass_Cloud:
    case kSim3DDepthPass_VolumeCloud:
      return g_depth_pass.atlases[layer].texture;
    case kSim3DDepthPass_Ground:
      return g_depth_pass.selected_ground ? g_depth_pass.selected_ground
          : g_depth_pass.atlases[layer].texture;
    case kSim3DDepthPass_CloudShadow:
      return g_depth_pass.atlases[kSim3DDepthPass_Cloud].texture;
    case kSim3DDepthPass_ShadowReceiver:
      return GpuTexture(shadow_texture);
    case kSim3DDepthPass_Billboard:
      return g_depth_pass.billboard_texture;
    case kSim3DDepthPass_GroundHaze:
    case kSim3DDepthPass_Effect:
    case kSim3DDepthPass_DepthOccluder:
    case kSim3DDepthPass_Solid:
      return g_depth_pass.white_texture;
    case kSim3DDepthPassLayerCount:
      break;
  }
  return NULL;
}

/* Opaque geometry writes depth; transparent effects only test against it.
 * Selecting per layer rather than switching once part-way through the loop
 * keeps that a property of the layer instead of a property of where the layer
 * happens to sit in the enum. */
static SDL_GPUGraphicsPipeline *PipelineForLayer(Sim3DDepthPassLayer layer) {
  switch (layer) {
    case kSim3DDepthPass_DepthOccluder:
      return g_depth_pass.depth_occluder_pipeline;
    case kSim3DDepthPass_GroundBlur:
    case kSim3DDepthPass_GroundHaze:
    case kSim3DDepthPass_CloudShadow:
    case kSim3DDepthPass_Cloud:
    case kSim3DDepthPass_VolumeCloud:
    case kSim3DDepthPass_Effect:
    case kSim3DDepthPass_ShadowReceiver:
    case kSim3DDepthPass_Billboard:
      return g_depth_pass.effect_pipeline;
    case kSim3DDepthPass_Ground:
    case kSim3DDepthPass_Solid:
    case kSim3DDepthPass_Mountain:
    case kSim3DDepthPass_WorldMountain:
      return g_depth_pass.pipeline;
    case kSim3DDepthPassLayerCount:
      break;
  }
  return NULL;
}

static SDL_GPUSampler *SamplerForLayer(Sim3DDepthPassLayer layer) {
  /* Pixel-art mountain cutouts remain nearest-neighbour. The shadow receiver
   * is a filtered screen-space mask and may intentionally use a smaller
   * working target, so linear sampling is part of that layer's contract. */
  return layer == kSim3DDepthPass_ShadowReceiver ||
      layer == kSim3DDepthPass_Cloud || layer == kSim3DDepthPass_CloudShadow ||
      layer == kSim3DDepthPass_VolumeCloud ||
      layer == kSim3DDepthPass_Ground || layer == kSim3DDepthPass_GroundBlur
      ? g_depth_pass.linear_sampler : g_depth_pass.nearest_sampler;
}

static bool PrepareMeshSamples(void) {
  g_depth_pass.sample_upload_bytes = 0;
  if (!g_depth_pass.sample_count) return true;
  bool needs_stream = false;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    needs_stream |= g_depth_pass.samples[i].mesh->kind == kMeshScreen;
  if (!needs_stream) return true;
  const Uint32 uv_bytes = g_depth_pass.sample_vertices * (Uint32)sizeof(ArRenderPointF);
  const Uint32 bytes = uv_bytes + g_depth_pass.sample_count * (Uint32)sizeof(ArRenderColorF);
  if (bytes > g_depth_pass.sample_gpu_bytes) {
    Uint32 capacity = g_depth_pass.sample_gpu_bytes ? g_depth_pass.sample_gpu_bytes : 32768;
    while (capacity < bytes) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = capacity};
    const SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = capacity,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g_depth_pass.device, &info);
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(g_depth_pass.device, &transfer_info);
    if (!buffer || !transfer) {
      if (buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
      if (transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      return false;
    }
    if (g_depth_pass.sample_buffer)
      SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.sample_buffer);
    if (g_depth_pass.sample_transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
    g_depth_pass.sample_buffer = buffer;
    g_depth_pass.sample_transfer = transfer;
    g_depth_pass.sample_gpu_bytes = capacity;
  }
  uint8_t *mapped =
      SDL_MapGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer, true);
  if (!mapped) return false;
  if (uv_bytes) memcpy(mapped, g_depth_pass.sample_uv, uv_bytes);
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    memcpy(mapped + uv_bytes + i * sizeof(ArRenderColorF), &g_depth_pass.samples[i].color,
        sizeof(ArRenderColorF));
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
  g_depth_pass.sample_upload_bytes = bytes;
  return true;
}

static bool SurfaceShadowBatchingEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_SIM3D_SHADOW_BATCH");
    enabled = !value || strcmp(value, "0") != 0; /* Diagnostic opt-out; enabled by default. */
  }
  return enabled != 0;
}

static bool IsBatchableSurfaceShadow(const Sim3DMeshSample *sample) {
  return sample->layer == kSim3DDepthPass_CloudShadow && sample->mesh->kind == kMeshSurface &&
      sample->surface.material[1] == 1 && sample->surface.spherical.color[0] == 0 &&
      sample->surface.spherical.color[1] == 0 && sample->surface.spherical.color[2] == 0;
}

/* Compare named placement/sampling fields, not byte slices spanning unrelated
 * uniform members. Offsets and opacity are the ONLY per-tap differences; RGB
 * eligibility is checked separately. Exact bits conservatively preserve the
 * existing batch choices, including signed zero in geometric inputs. */
static bool SameSurfaceShadowPlacement(const Sim3DSurfaceUniform *a,
    const Sim3DSurfaceUniform *b) {
  return !memcmp(a->view.matrix, b->view.matrix, sizeof(a->view.matrix)) &&
      !memcmp(a->view.basis, b->view.basis, sizeof(a->view.basis)) &&
      !memcmp(a->view.radial, b->view.radial, sizeof(a->view.radial)) &&
      !memcmp(a->light, b->light, sizeof(a->light)) &&
      !memcmp(a->material, b->material, sizeof(a->material)) &&
      !memcmp(a->mask_rect, b->mask_rect, sizeof(a->mask_rect)) &&
      !memcmp(a->mask, b->mask, sizeof(a->mask)) &&
      !memcmp(a->shadow_basis, b->shadow_basis, sizeof(a->shadow_basis)) &&
      !memcmp(a->spherical.rotation, b->spherical.rotation, sizeof(a->spherical.rotation)) &&
      !memcmp(a->spherical.atlas, b->spherical.atlas, sizeof(a->spherical.atlas)) &&
      a->spherical.offset_extent[2] == b->spherical.offset_extent[2] &&
      a->spherical.offset_extent[3] == b->spherical.offset_extent[3];
}

/* Submission-only coalescing: the logical sample queue, copied input ownership,
 * rejection rules and effect budgets are unchanged. Only consecutive black
 * samples sharing exact placement/chart/rotation are eligible. Their real-valued
 * transmittance commutes across overlapping receiver faces; colored samples do
 * not. Keep unrelated receivers/overlays and ordinary GPU paths untouched. */
static Uint32 SurfaceShadowBatchSize(Uint32 first) {
  if (!SurfaceShadowBatchingEnabled()) return 1;
  const Sim3DMeshSample *a = &g_depth_pass.samples[first];
  if (!IsBatchableSurfaceShadow(a)) return 1;
  Uint32 count = 1;
  for (; count < kSurfaceShadowBatchTaps && first + count < g_depth_pass.sample_count; ++count) {
    const Sim3DMeshSample *b = &g_depth_pass.samples[first + count];
    if (!IsBatchableSurfaceShadow(b) || a->mesh != b->mesh ||
        a->first != b->first || a->count != b->count ||
        !SameSurfaceShadowPlacement(&a->surface, &b->surface)) break;
  }
  return count;
}

ArRenderTexture Sim3DDepthPass_Submit(
    ArRenderDevice *device, ArRenderTexture shadow_texture) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *native_shadow =
      ArSdlRenderBackend_UnwrapTexture(shadow_texture);
  if (!g_depth_pass.collecting || renderer != g_depth_pass.renderer)
    return ArRenderTexture_Invalid();
  g_depth_pass.collecting = false;
  if (g_depth_pass.geometry_failed) {
    fprintf(stderr, "[sim3d-depth] geometry collection failed or a queued mesh was invalidated\n");
    return ArRenderTexture_Invalid();
  }
  Uint32 total = 0;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (g_depth_pass.lists[i].count > UINT32_MAX - total) {
      fprintf(stderr, "[sim3d-depth] geometry vertex count overflow\n");
      return ArRenderTexture_Invalid();
    }
    total += g_depth_pass.lists[i].count;
  }
  Uint32 needed = total;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i) {
    const Sim3DMeshSample *sample = &g_depth_pass.samples[i];
    if (!sample->texture && !TextureForLayer(sample->layer, native_shadow))
      return ArRenderTexture_Invalid();
    if (sample->mesh->count > needed) needed = sample->mesh->count;
  }
  if (!needed || total % kSim3DDepthVerticesPerQuad != 0 ||
      !EnsureGpuBuffers(needed) || !PrepareMeshSamples())
    return ArRenderTexture_Invalid();
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    if (!TextureForLayer((Sim3DDepthPassLayer)i, native_shadow)) {
      fprintf(stderr, "[sim3d-depth] material layer %d has no GPU texture\n",
              i);
      return ArRenderTexture_Invalid();
    }
  }

  Sim3DGpuVertex *mapped = total ? SDL_MapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.transfer_buffer, true) : NULL;
  if (total && !mapped) {
    fprintf(stderr, "[sim3d-depth] geometry upload map failed: %s\n",
            SDL_GetError());
    return ArRenderTexture_Invalid();
  }
  Uint32 first[kSim3DDepthPassLayerCount];
  Uint32 at = 0;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    first[i] = at;
    size_t bytes = (size_t)g_depth_pass.lists[i].count * sizeof(*mapped);
    if (bytes) memcpy(mapped + at, g_depth_pass.lists[i].vertices, bytes);
    at += g_depth_pass.lists[i].count;
  }
  if (mapped) SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.transfer_buffer);

  /* The ordered adapter submits preceding 2D producers/consumers without a
   * window present. SDL_FlushRenderer alone only records GPU commands; it
   * does not establish queue order for a current-frame shadow-mask read. */
  if (!ArSdlRenderBackend_SubmitPending(device)) return ArRenderTexture_Invalid();
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  if (!commands) return ArRenderTexture_Invalid();
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
  if (!copy) {
    SDL_CancelGPUCommandBuffer(commands);
    return ArRenderTexture_Invalid();
  }
  SDL_GPUTransferBufferLocation source = {
    .transfer_buffer = g_depth_pass.transfer_buffer,
    .offset = 0,
  };
  SDL_GPUBufferRegion destination = {
    .buffer = g_depth_pass.vertex_buffer,
    .offset = 0,
    .size = total * (Uint32)sizeof(Sim3DGpuVertex),
  };
  uint64_t vertex_upload_bytes = destination.size;
  if (total) SDL_UploadToGPUBuffer(copy, &source, &destination, true);
  if (g_depth_pass.sample_count) {
    const SDL_GPUTransferBufferLocation sample_source = {
      .transfer_buffer = g_depth_pass.sample_transfer,
    };
    const SDL_GPUBufferRegion sample_destination = {
      .buffer = g_depth_pass.sample_buffer,
      .size = g_depth_pass.sample_upload_bytes,
    };
    if (sample_destination.size)
      SDL_UploadToGPUBuffer(copy, &sample_source, &sample_destination, true);
    vertex_upload_bytes += sample_destination.size;
    for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next) {
      if (!mesh->queued) continue;
      if (mesh->dirty) {
        const SDL_GPUTransferBufferLocation mesh_source = {.transfer_buffer = mesh->transfer};
        const SDL_GPUBufferRegion mesh_destination = {
          .buffer = mesh->positions, .size = mesh->count * MeshVertexBytes(mesh),
        };
        SDL_UploadToGPUBuffer(copy, &mesh_source, &mesh_destination, true);
        vertex_upload_bytes += mesh_destination.size;
      }
      if (mesh->selection_dirty && mesh->kind != kMeshSurface) {
        const SDL_GPUTransferBufferLocation selection_source = { .transfer_buffer =
                                                                     mesh->selection_transfer };
        const SDL_GPUBufferRegion selection_destination = {
          .buffer = mesh->selection, .size = mesh->selection_count * (Uint32)sizeof(Uint32),
        };
        SDL_UploadToGPUBuffer(copy, &selection_source, &selection_destination, true);
        vertex_upload_bytes += selection_destination.size;
      }
    }
  }
  if (g_depth_pass.index_upload_required) {
    SDL_GPUTransferBufferLocation index_source = {
      .transfer_buffer = g_depth_pass.index_transfer_buffer,
      .offset = 0,
    };
    SDL_GPUBufferRegion index_destination = {
      .buffer = g_depth_pass.index_buffer,
      .offset = 0,
      .size = g_depth_pass.gpu_index_capacity * (Uint32)sizeof(Uint32),
    };
    SDL_UploadToGPUBuffer(
        copy, &index_source, &index_destination, false);
  }
  SDL_EndGPUCopyPass(copy);

  /* Source uploads precede compaction. Cycle the destination only on the first
   * range; subsequent copies populate the same new buffer. Selection ranges
   * remain owned/immutable through submission, with no readback or CPU fence. */
  uint64_t vertex_copy_bytes = 0, vertex_copy_calls = 0;
  bool compact = false;
  for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next)
    compact |= mesh->queued && mesh->kind == kMeshSurface && mesh->selection_dirty;
  if (compact) {
    copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); return ArRenderTexture_Invalid(); }
    for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next) {
      if (!mesh->queued || mesh->kind != kMeshSurface || !mesh->selection_dirty) continue;
      Uint32 offset = 0;
      for (Uint32 i = 0; i < mesh->surface_range_count; ++i) {
        const Sim3DDepthMeshRange range = mesh->surface_ranges[i];
        if (!range.quad_count) continue;
        const Uint32 bytes = (Uint32)range.quad_count*sizeof(Sim3DSurfaceGpuQuad);
        const SDL_GPUBufferLocation src = {
          mesh->positions, (Uint32)range.first_quad * sizeof(Sim3DSurfaceGpuQuad)
        };
        const SDL_GPUBufferLocation dst = {mesh->selection,offset};
        SDL_CopyGPUBufferToBuffer(copy,&src,&dst,bytes,offset == 0);
        offset += bytes;
        vertex_copy_bytes += bytes;
        ++vertex_copy_calls;
      }
    }
    SDL_EndGPUCopyPass(copy);
  }

  SDL_GPUColorTargetInfo color;
  SDL_zero(color);
  color.texture = g_depth_pass.color_target;
  color.clear_color = (SDL_FColor){0, 0, 0, 0};
  color.load_op = SDL_GPU_LOADOP_CLEAR;
  color.store_op = SDL_GPU_STOREOP_STORE;
  color.cycle = true;
  SDL_GPUDepthStencilTargetInfo depth;
  SDL_zero(depth);
  depth.texture = g_depth_pass.depth_target;
  depth.clear_depth = 1.0f;
  depth.load_op = SDL_GPU_LOADOP_CLEAR;
  depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
  depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
  depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  depth.cycle = true;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
      commands, &color, 1, &depth);
  if (!pass) {
    SDL_CancelGPUCommandBuffer(commands);
    return ArRenderTexture_Invalid();
  }
  SDL_GPUBufferBinding vertex_binding = {
    .buffer = g_depth_pass.vertex_buffer,
    .offset = 0,
  };
  SDL_GPUBufferBinding index_binding = {
    .buffer = g_depth_pass.index_buffer,
    .offset = 0,
  };
  SDL_BindGPUIndexBuffer(
      pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_GPUGraphicsPipeline *bound = NULL;
  static const Sim3DDepthPassLayer order[] = {
    kSim3DDepthPass_DepthOccluder, kSim3DDepthPass_Ground,
    kSim3DDepthPass_GroundBlur, kSim3DDepthPass_GroundHaze,
    kSim3DDepthPass_CloudShadow,
    kSim3DDepthPass_Solid, kSim3DDepthPass_Mountain, kSim3DDepthPass_WorldMountain,
    kSim3DDepthPass_ShadowReceiver, kSim3DDepthPass_Billboard,
    kSim3DDepthPass_Effect, kSim3DDepthPass_Cloud,
    kSim3DDepthPass_VolumeCloud,
  };
  _Static_assert(sizeof(order) / sizeof(order[0]) == kSim3DDepthPassLayerCount,
                 "Every material layer needs a depth draw order");
  for (int draw = 0; draw < kSim3DDepthPassLayerCount; draw++) {
    const Sim3DDepthPassLayer i = order[draw];
    bool samples = false;
    for (Uint32 j = 0; j < g_depth_pass.sample_count; ++j)
      samples |= g_depth_pass.samples[j].layer == i;
    if (!g_depth_pass.lists[i].count && !samples) continue;
    SDL_GPUGraphicsPipeline *pipeline = PipelineForLayer((Sim3DDepthPassLayer)i);
    if (pipeline != bound) {
      SDL_BindGPUGraphicsPipeline(pass, pipeline);
      bound = pipeline;
    }
    SDL_GPUTexture *texture = TextureForLayer(
        (Sim3DDepthPassLayer)i, native_shadow);
    SDL_GPUTextureSamplerBinding texture_binding = {
      .texture = texture,
      .sampler = SamplerForLayer((Sim3DDepthPassLayer)i),
    };
    if (texture) SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
    SDL_GPUTexture *bound_texture = texture;
    if (i == kSim3DDepthPass_Billboard) {
      for (Uint32 j = 0; j < g_depth_pass.billboard_run_count; ++j) {
        const Sim3DBillboardRun *run = &g_depth_pass.billboard_runs[j];
        pipeline = run->rim.color.a > 0 ? g_depth_pass.billboard_rim_pipeline
                                     : g_depth_pass.effect_pipeline;
        if (pipeline != bound) {
          SDL_BindGPUGraphicsPipeline(pass,pipeline);
          bound = pipeline;
        }
        if (run->rim.color.a > 0)
          SDL_PushGPUFragmentUniformData(commands,0,&run->rim,sizeof(run->rim));
        vertex_binding.offset = (first[i]+run->first)*(Uint32)sizeof(Sim3DGpuVertex);
        SDL_BindGPUVertexBuffers(pass,0,&vertex_binding,1);
        SDL_DrawGPUIndexedPrimitives(pass,run->count/4*6,1,0,0,0);
      }
      continue;
    }
    if (OrderedLayerSupported(i) && samples) {
      /* Retained samples mark their insertion point in the ordinary stream.
       * Adjacent ordinary appends remain one draw; no per-face command list
       * is needed. Equal-depth/alpha ordering survives every representation
       * switch, including a rejected optional sample followed by CPU data. */
      Uint32 cursor = 0;
      SDL_GPUGraphicsPipeline *ordinary_pipeline = PipelineForLayer(i);
      for (Uint32 j = 0; j <= g_depth_pass.sample_count; ++j) {
        const Sim3DMeshSample *sample = j < g_depth_pass.sample_count
            ? &g_depth_pass.samples[j] : NULL;
        if (sample && sample->layer != (Sim3DDepthPassLayer)i) continue;
        const Uint32 end = sample ? sample->ordinary_before : g_depth_pass.lists[i].count;
        if (end > cursor) {
          if (bound_texture != texture) {
            texture_binding.texture = bound_texture = texture;
            SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
          }
          if (bound != ordinary_pipeline) {
            SDL_BindGPUGraphicsPipeline(pass, ordinary_pipeline);
            bound = ordinary_pipeline;
          }
          vertex_binding.offset = (first[i] + cursor) * (Uint32)sizeof(Sim3DGpuVertex);
          SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
          SDL_DrawGPUIndexedPrimitives(pass, (end - cursor) / 4 * 6, 1, 0, 0, 0);
        }
        cursor = end;
        if (!sample) break;
        SDL_GPUTexture *sample_texture = sample->texture ? sample->texture : texture;
        if (bound_texture != sample_texture) {
          texture_binding.texture = bound_texture = sample_texture;
          SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
        }
        pipeline = sample->mesh->kind == kMeshGeometry
            ? ordinary_pipeline : sample->mesh->kind == kMeshSurface
            ? g_depth_pass.surface_pipeline[i == kSim3DDepthPass_Ground ||
                i == kSim3DDepthPass_Mountain || i == kSim3DDepthPass_WorldMountain ? 0 : 1]
            : MeshPipeline(sample->mesh->kind);
        if (bound != pipeline) {
          SDL_BindGPUGraphicsPipeline(pass, pipeline);
          bound = pipeline;
        }
        const SDL_GPUBufferBinding binding = {
          sample->mesh->kind == kMeshSurface && sample->mesh->surface_selected
              ? sample->mesh->selection : sample->mesh->positions,
          sample->first * MeshVertexBytes(sample->mesh)};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        if (sample->mesh->kind == kMeshSurface) {
          SDL_PushGPUVertexUniformData(commands, 0, &sample->surface, sizeof(sample->surface));
          SDL_DrawGPUIndexedPrimitives(pass, 6, sample->count / 4, 0, 0, 0);
          continue;
        }
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
        if (IsModelMesh(sample->mesh->kind))
          SDL_PushGPUVertexUniformData(commands, 0, &sample->model, sizeof(sample->model));
#endif
        if (sample->mesh->kind == kMeshRadial)
          SDL_PushGPUVertexUniformData(commands, 0, &sample->radial, sizeof(sample->radial));
        if (sample->mesh->kind == kMeshLinear)
          SDL_PushGPUVertexUniformData(commands, 0, &sample->linear, sizeof(sample->linear));
        if (sample->mesh->selection_count) {
          const SDL_GPUBufferBinding selected = {sample->mesh->selection, 0};
          SDL_BindGPUIndexBuffer(pass, &selected, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        SDL_DrawGPUIndexedPrimitives(pass, sample->count / 4 * 6, 1, 0, 0, 0);
        if (sample->mesh->selection_count)
          SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
      }
      continue;
    }
    if (samples) {
      for (Uint32 j = 0; j < g_depth_pass.sample_count; ++j) {
        const Sim3DMeshSample *sample = &g_depth_pass.samples[j];
        if (sample->layer != i) continue;
        const Uint32 shadow_batch = SurfaceShadowBatchSize(j);
        pipeline = sample->mesh->kind == kMeshSurface
            ? g_depth_pass.surface_pipeline[shadow_batch > 1 ? 2 : 1]
            : MeshPipeline(sample->mesh->kind);
        if (pipeline != bound) {
          SDL_BindGPUGraphicsPipeline(pass, pipeline);
          bound = pipeline;
        }
        if (sample->mesh->kind == kMeshSphericalBody) {
          const SDL_GPUBufferBinding binding = {sample->mesh->positions,0};
          SDL_BindGPUVertexBuffers(pass,0,&binding,1);
          SDL_PushGPUVertexUniformData(commands,0,&sample->body.view,sizeof(sample->body.view));
          SDL_PushGPUFragmentUniformData(commands, 0, &sample->body.sample,
                                         sizeof(sample->body.sample));
          SDL_DrawGPUIndexedPrimitives(pass,sample->mesh->count/4*6,1,0,0,0);
          continue;
        }
        if (sample->mesh->kind == kMeshSpherical || sample->mesh->kind == kMeshSurface) {
          const SDL_GPUBufferBinding binding = {
            sample->mesh->kind == kMeshSurface && sample->mesh->surface_selected
                ? sample->mesh->selection : sample->mesh->positions,
            sample->mesh->kind == kMeshSurface ? sample->first*MeshVertexBytes(sample->mesh) : 0};
          SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
          if (shadow_batch > 1) {
            Sim3DSurfaceShadowBatchUniform uniform = {.surface = sample->surface};
            for (Uint32 tap = 0; tap < shadow_batch; ++tap) {
              const Sim3DSphericalUniform *s = &g_depth_pass.samples[j+tap].surface.spherical;
              uniform.taps[tap][0] = s->offset_extent[0];
              uniform.taps[tap][1] = s->offset_extent[1];
              uniform.taps[tap][2] = s->color[3];
            }
            SDL_PushGPUVertexUniformData(commands, 0, &uniform, sizeof(uniform));
          } else if (sample->mesh->kind == kMeshSurface)
            SDL_PushGPUVertexUniformData(commands, 0, &sample->surface, sizeof(sample->surface));
          else
            SDL_PushGPUVertexUniformData(commands, 0, &sample->spherical,
                                         sizeof(sample->spherical));
          SDL_DrawGPUIndexedPrimitives(
              pass, 6,
              (sample->mesh->kind == kMeshSurface ? sample->count : sample->mesh->count) / 4, 0, 0,
              0);
          j += shadow_batch - 1;
          continue;
        }
        const SDL_GPUBufferBinding bindings[] = {
          { sample->mesh->positions, 0 },
          { g_depth_pass.sample_buffer, sample->first * (Uint32)sizeof(ArRenderPointF) },
          { g_depth_pass.sample_buffer,
            g_depth_pass.sample_vertices * (Uint32)sizeof(ArRenderPointF) +
                j * (Uint32)sizeof(ArRenderColorF) },
        };
        SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
        SDL_DrawGPUIndexedPrimitives(pass, sample->mesh->count / 4 * 6, 1, 0, 0, 0);
      }
    } else {
      vertex_binding.offset = first[i] * (Uint32)sizeof(Sim3DGpuVertex);
      SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
      SDL_DrawGPUIndexedPrimitives(pass,
          g_depth_pass.lists[i].count / kSim3DDepthVerticesPerQuad * kSim3DDepthIndicesPerQuad,
          1, 0, 0, 0);
    }
  }
  SDL_EndGPURenderPass(pass);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] command submission failed: %s\n",
            SDL_GetError());
    return ArRenderTexture_Invalid();
  }
  Sim3DPerformance_AddGeometryUpload(vertex_upload_bytes);
  Sim3DPerformance_AddGeometryCopy(vertex_copy_bytes,vertex_copy_calls);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    if (i == kSim3DDepthPass_Billboard) {
      for (Uint32 j = 0; j < g_depth_pass.billboard_run_count; ++j) {
        const Uint32 count = g_depth_pass.billboard_runs[j].count;
        Sim3DPerformance_AddDraw(count,count/4*6);
      }
      continue;
    }
    if (OrderedLayerSupported((Sim3DDepthPassLayer)i)) {
      Uint32 cursor = 0;
      for (Uint32 j = 0; j <= g_depth_pass.sample_count; ++j) {
        const Sim3DMeshSample *sample = j < g_depth_pass.sample_count
            ? &g_depth_pass.samples[j] : NULL;
        if (sample && sample->layer != (Sim3DDepthPassLayer)i) continue;
        const Uint32 end = sample ? sample->ordinary_before : g_depth_pass.lists[i].count;
        if (end > cursor) Sim3DPerformance_AddDraw(end - cursor, (end - cursor) / 4 * 6);
        cursor = end;
      }
      continue;
    }
    Sim3DPerformance_AddDraw(
        g_depth_pass.lists[i].count,
        g_depth_pass.lists[i].count / kSim3DDepthVerticesPerQuad *
            kSim3DDepthIndicesPerQuad);
  }
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i) {
    const Sim3DMeshSample *sample = &g_depth_pass.samples[i];
    const Uint32 count = GeometryLayerSupported(sample->layer) || sample->mesh->kind == kMeshSurface
        ? sample->count : sample->mesh->count;
    Sim3DPerformance_AddDraw(count, count / 4 * 6);
    i += SurfaceShadowBatchSize(i) - 1; /* Count actual draws, not logical taps. */
  }
  for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next)
    if (mesh->queued) mesh->dirty = mesh->selection_dirty = false;
  g_depth_pass.index_upload_required = false;
  return ArSdlRenderBackend_BorrowTexture(g_depth_pass.output_texture);
}

bool Sim3DDepthPass_IsCollecting(void) {
  return g_depth_pass.collecting;
}

void Sim3DDepthPass_Reset(ArRenderDevice *device) {
  (void)device;
  g_depth_pass.collecting = false;
  ReleaseTargets();
  ReleaseAtlasCacheStorage(s_atlas_cache);
  for (Sim3DDepthMesh *mesh = g_depth_pass_meshes; mesh; mesh = mesh->next)
    ReleaseMeshStorage(mesh);
  if (g_depth_pass.mesh_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.mesh_pipeline);
  if (g_depth_pass.spherical_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.spherical_pipeline);
  if (g_depth_pass.spherical_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.spherical_shader);
  for (unsigned i = 0; i < 3; ++i)
    if (g_depth_pass.surface_pipeline[i])
      SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.surface_pipeline[i]);
  if (g_depth_pass.surface_vertex)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.surface_vertex);
  if (g_depth_pass.surface_fragment)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.surface_fragment);
  if (g_depth_pass.shadow_batch_vertex)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.shadow_batch_vertex);
  if (g_depth_pass.shadow_batch_fragment)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.shadow_batch_fragment);
  if (g_depth_pass.body_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device,g_depth_pass.body_pipeline);
  if (g_depth_pass.body_vertex)
    SDL_ReleaseGPUShader(g_depth_pass.device,g_depth_pass.body_vertex);
  if (g_depth_pass.body_fragment)
    SDL_ReleaseGPUShader(g_depth_pass.device,g_depth_pass.body_fragment);
  for (unsigned variant = 0; variant < kModelPipelineCount; ++variant) {
    if (g_depth_pass.model_pipeline[variant])
      SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.model_pipeline[variant]);
    if (g_depth_pass.model_shader[variant])
      SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.model_shader[variant]);
  }
  if (g_depth_pass.model_clipped_fragment)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.model_clipped_fragment);
  if (g_depth_pass.sample_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.sample_buffer);
  if (g_depth_pass.sample_transfer)
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
  free(g_depth_pass.sample_uv);
  if (g_depth_pass.pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.pipeline);
  if (g_depth_pass.depth_occluder_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.depth_occluder_pipeline);
  if (g_depth_pass.effect_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.effect_pipeline);
  if (g_depth_pass.billboard_rim_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.billboard_rim_pipeline);
  if (g_depth_pass.billboard_rim_fragment)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.billboard_rim_fragment);
  if (g_depth_pass.vertex_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.vertex_shader);
  if (g_depth_pass.fragment_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.fragment_shader);
  if (g_depth_pass.nearest_sampler)
    SDL_ReleaseGPUSampler(g_depth_pass.device, g_depth_pass.nearest_sampler);
  if (g_depth_pass.linear_sampler)
    SDL_ReleaseGPUSampler(g_depth_pass.device, g_depth_pass.linear_sampler);
  if (g_depth_pass.vertex_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.vertex_buffer);
  if (g_depth_pass.transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.transfer_buffer);
  if (g_depth_pass.index_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.index_buffer);
  if (g_depth_pass.index_transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.index_transfer_buffer);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    Sim3DDepthAtlas *atlas = &g_depth_pass.atlases[i];
    if (atlas->texture) SDL_ReleaseGPUTexture(g_depth_pass.device, atlas->texture);
    if (atlas->transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, atlas->transfer);
  }
  if (g_depth_pass.white_texture)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.white_texture);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    free(g_depth_pass.lists[i].vertices);
    g_depth_pass.lists[i].vertices = NULL;
  }
  memset(&g_depth_pass, 0, sizeof(g_depth_pass));
}
