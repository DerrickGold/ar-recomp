#include "action_effect_source_sdl.h"
#include "render_sdl_internal.h"
#include "gpu_shader_blob.h"
#include "shaders/action_effect_project_comp.h"
#include "shaders/action_effect_shadow_comp.h"
#include "shaders/action_effect_moon_comp.h"
#include "shaders/action_effect_draw_vert.h"
#include "shaders/action_effect_draw_frag.h"
#include "shaders/action_skybox_vert.h"
#include "shaders/action_skybox_frag.h"
#include <math.h>
#include <string.h>

typedef struct EffectPlaneUniform {
  float uv[4], shape[4], offset[4], bounds[4], sky[4], fold[4], front[4];
} EffectPlaneUniform;
typedef struct EffectUniform {
  float matrix[16], viewport[4], capture[4], geometry[4], clock[4], follow[4];
  EffectPlaneUniform planes[9];
  float sky_meta[4];
  float sky_bands[kDioramaBgMaxValidSpans][8];
  float moon[4];
  DioramaSkyboxSourceMapping sky_mapping;
} EffectUniform;
_Static_assert(sizeof(EffectPlaneUniform) == 112 && sizeof(EffectUniform) == 1792,
               "effect projection std140");
_Static_assert(kDioramaBgMaxValidSpans == kDioramaSkyboxSourceBands, "sky band ABI");
_Static_assert(sizeof(ActionEffectSourceLightData) == 35440, "moon transport ABI");
_Static_assert(kActionShadowWidth == 400 && kActionShadowHeight == 176, "GPU shadow coverage grid");
#define BLOBS(name)                                                                                \
  {k##name##MSL, k##name##MSLSize, k##name##SPV, k##name##SPVSize, k##name##DXIL, k##name##DXILSize}

enum {
  kPrimitiveBytes = kActionSourceMaximumPrimitives * sizeof(ActionEffectSourcePrimitive),
  kOccluderBytes = (kActionMoonlightMaxOccluders + 1) * 16,
  kOutputBytes = kActionSourceMaximumPrimitives * kActionSourceVerticesPerPrimitive * 32,
  kUploadBytes = kPrimitiveBytes + kOccluderBytes +
      kActionSourceMaxLightJobs * (sizeof(ActionEffectSourceLightData) + kOccluderBytes)
};

void ArGpuEffectSource_Destroy(ArGpuEffectSource *p) {
  if (!p || !p->device) return;
  for (unsigned f = 0; f < 2; ++f) {
    for (unsigned b = 0; b < 3; ++b)
      SDL_ReleaseGPUGraphicsPipeline(p->device, p->draw[f][b]);
    for (unsigned b = 0; b < 2; ++b)
      SDL_ReleaseGPUGraphicsPipeline(p->device, p->skybox[f][b]);
  }
  SDL_ReleaseGPUComputePipeline(p->device, p->project);
  SDL_ReleaseGPUComputePipeline(p->device, p->shadow);
  SDL_ReleaseGPUComputePipeline(p->device, p->moon);
  SDL_ReleaseGPUBuffer(p->device, p->moon_job);
  SDL_ReleaseGPUBuffer(p->device, p->moon_work);
  SDL_ReleaseGPUBuffer(p->device, p->moon_mask);
  SDL_ReleaseGPUBuffer(p->device, p->moon_light);
  SDL_ReleaseGPUBuffer(p->device, p->primitives);
  for (unsigned i = 0; i < kActionSourcePacketSlots; ++i)
    SDL_ReleaseGPUBuffer(p->device, p->packets[i].primitives);
  SDL_ReleaseGPUBuffer(p->device, p->vertices);
  SDL_ReleaseGPUBuffer(p->device, p->occluders);
  SDL_ReleaseGPUBuffer(p->device, p->coverage);
  SDL_ReleaseGPUBuffer(p->device, p->sky_vertices);
  SDL_ReleaseGPUTransferBuffer(p->device, p->upload);
  SDL_ReleaseGPUSampler(p->device, p->sampler);
  SDL_ReleaseGPUSampler(p->device, p->nearest_sampler);
  SDL_ReleaseGPUSampler(p->device, p->wrap_sampler);
  SDL_ReleaseGPUSampler(p->device, p->wrap_nearest_sampler);
  *p = (ArGpuEffectSource){0};
}

static SDL_GPUGraphicsPipeline *Pipeline(SDL_GPUDevice *gpu, SDL_GPUShader *vs, SDL_GPUShader *fs,
                                         SDL_GPUTextureFormat format, int blend, bool sky) {
  const SDL_GPUVertexBufferDescription vb = {
      .slot = 0, .pitch = sizeof(ArRenderVertex2D), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attr[] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
                                         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 8},
                                         {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24}};
  const SDL_GPUColorTargetDescription color = {
      .format = format,
      .blend_state = {.enable_blend = blend != 3,
                      .src_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_DST_COLOR
                                                          : SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                      .dst_color_blendfactor = blend == 0 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
                                                          : SDL_GPU_BLENDFACTOR_ONE,
                      .color_blend_op = SDL_GPU_BLENDOP_ADD,
                      .src_alpha_blendfactor =
                          blend == 0 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ZERO,
                      .dst_alpha_blendfactor = blend == 0 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
                                                          : SDL_GPU_BLENDFACTOR_ONE,
                      .alpha_blend_op = SDL_GPU_BLENDOP_ADD}};
  const SDL_GPUGraphicsPipelineCreateInfo info = {
      .vertex_shader = vs,
      .fragment_shader = fs,
      .vertex_input_state = {.vertex_buffer_descriptions = sky ? &vb : NULL,
                             .num_vertex_buffers = sky ? 1 : 0,
                             .vertex_attributes = sky ? attr : NULL,
                             .num_vertex_attributes = sky ? 3 : 0},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                           .cull_mode = SDL_GPU_CULLMODE_NONE,
                           .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                           .enable_depth_clip = true},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = {.color_target_descriptions = &color, .num_color_targets = 1}};
  return SDL_CreateGPUGraphicsPipeline(gpu, &info);
}

bool ArGpuEffectSource_Init(ArGpuEffectSource *p, SDL_GPUDevice *gpu) {
  *p = (ArGpuEffectSource){.device = gpu};
  const GpuShaderBlobs project = BLOBS(ActionEffectProjectComp),
                       shadow = BLOBS(ActionEffectShadowComp), moon = BLOBS(ActionEffectMoonComp), drawvs = BLOBS(ActionEffectDrawVert),
                       drawfs = BLOBS(ActionEffectDrawFrag), skyvs = BLOBS(ActionSkyboxVert),
                       skyfs = BLOBS(ActionSkyboxFrag);
  p->project = GpuShaderBlob_CreateCompute(gpu, &project, 0, 4, 1, 0, 1, 64, 1);
  p->shadow = GpuShaderBlob_CreateCompute(gpu, &shadow, 0, 2, 1, 0, 1, 64, 1);
  p->moon = GpuShaderBlob_CreateCompute(gpu, &moon, 0, 3, 3, 0, 1, 64, 1);
  SDL_GPUShader *vs = GpuShaderBlob_CreateWithStorage(gpu, &drawvs, "effect projection",
                                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
  SDL_GPUShader *fs = GpuShaderBlob_CreateFragment(gpu, &drawfs, "effect color", 0, 0);
  SDL_GPUShader *sv = GpuShaderBlob_CreateWithStorage(gpu, &skyvs, "resident skybox",
                                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
  SDL_GPUShader *sf = GpuShaderBlob_CreateFragment(gpu, &skyfs, "resident skybox blur", 1, 1);
  bool ok = p->project && p->shadow && p->moon && vs && fs && sv && sf;
  for (unsigned f = 0; f < 2 && ok; ++f) {
    const SDL_GPUTextureFormat format =
        f ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    for (unsigned b = 0; b < 3; ++b) {
      p->draw[f][b] = Pipeline(gpu, vs, fs, format, (int)b, false);
      ok = ok && p->draw[f][b];
    }
    for (unsigned b = 0; b < 2; ++b) {
      p->skybox[f][b] = Pipeline(gpu, sv, sf, format, b ? 0 : 3, true);
      ok = ok && p->skybox[f][b];
    }
  }
  SDL_ReleaseGPUShader(gpu, vs);
  SDL_ReleaseGPUShader(gpu, fs);
  SDL_ReleaseGPUShader(gpu, sv);
  SDL_ReleaseGPUShader(gpu, sf);
  SDL_GPUBufferCreateInfo buffer = {.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                    .size = kPrimitiveBytes};
  p->primitives = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.size = kOccluderBytes;
  p->occluders = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.size = sizeof(ActionEffectSourceLightData);
  p->moon_job = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
  buffer.size = (400 * 176 + 4) * 4;
  p->coverage = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.size = (16 + kActionSourceMaxLightPoints) * 16;
  p->moon_work = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.size = 800 * 224 * 4;
  p->moon_mask = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.size = kActionSourceMaxLightJobs * kActionSourceMaxLightPoints * 4;
  p->moon_light = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.usage =
      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
  buffer.size = kOutputBytes;
  p->vertices = SDL_CreateGPUBuffer(gpu, &buffer);
  buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  buffer.size = kDioramaSkyboxSourceMaxIndices * sizeof(ArRenderVertex2D);
  p->sky_vertices = SDL_CreateGPUBuffer(gpu, &buffer);
  const SDL_GPUTransferBufferCreateInfo upload = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                  .size = kUploadBytes};
  p->upload = SDL_CreateGPUTransferBuffer(gpu, &upload);
  SDL_GPUSamplerCreateInfo sampler = {.min_filter = SDL_GPU_FILTER_LINEAR,
                                      .mag_filter = SDL_GPU_FILTER_LINEAR,
                                      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  p->sampler = SDL_CreateGPUSampler(gpu, &sampler);
  sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  p->nearest_sampler = SDL_CreateGPUSampler(gpu, &sampler);
  sampler.address_mode_u = sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  p->wrap_nearest_sampler = SDL_CreateGPUSampler(gpu, &sampler);
  sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  p->wrap_sampler = SDL_CreateGPUSampler(gpu, &sampler);
  if (ok && p->wrap_sampler && p->wrap_nearest_sampler && p->primitives && p->occluders && p->coverage && p->vertices && p->sky_vertices &&
      p->upload && p->sampler && p->nearest_sampler && p->moon_job && p->moon_work &&
      p->moon_mask && p->moon_light)
    return true;
  ArGpuEffectSource_Destroy(p);
  return false;
}

static bool Target(ArRenderDevice *device, SDL_GPUTexture **out, unsigned *width, unsigned *height,
                   unsigned *format, SDL_Rect *viewport, SDL_Rect *scissor) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *target = renderer ? SDL_GetRenderTarget(renderer) : NULL;
  if (!target) return false;
  SDL_PropertiesID props = SDL_GetTextureProperties(target);
  const SDL_PixelFormat pixels =
      (SDL_PixelFormat)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, 0);
  if (pixels != SDL_PIXELFORMAT_RGBA32 && pixels != SDL_PIXELFORMAT_BGRA32) return false;
  *format = pixels == SDL_PIXELFORMAT_BGRA32;
  *out = SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
  *width = (unsigned)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0);
  *height = (unsigned)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0);
  if (!*out || !*width || !*height || !SDL_GetRenderViewport(renderer, viewport)) return false;
  const SDL_Rect bounds = {0, 0, (int)*width, (int)*height};
  if (!SDL_GetRectIntersection(viewport, &bounds, scissor)) *scissor = (SDL_Rect){0};
  if (SDL_RenderClipEnabled(renderer)) {
    SDL_Rect clip;
    if (!SDL_GetRenderClipRect(renderer, &clip)) return false;
    clip.x += viewport->x; clip.y += viewport->y;
    if (!SDL_GetRectIntersection(scissor, &clip, scissor)) *scissor = (SDL_Rect){0};
  }
  return true;
}

static EffectPlaneUniform Plane(const DioramaProjection *v, const DioramaPlaneProjection *p,
                                int slot) {
  return (EffectPlaneUniform){
      .fold = {p->overflow_valid, p->overflow_fold_t, p->overflow_height, p->overflow_overlap_t},
      .front = {p->overflow_handoff_z, p->overflow_front_z, p->overflow_front_drop, 0},
      .uv = {p->u0, p->v0, p->u1, p->v1},
      .shape = {p->z_world, p->rake, p->bow, p->world_y_offset},
      .offset = {p->capture_offset.x, p->capture_offset.y, (float)slot, p->valid ? 1 : 0},
      .bounds = {p->u0 * v->texture_width - v->texture_x_origin - p->capture_offset.x,
                 p->v0 * v->texture_height - p->capture_offset.y,
                 p->u1 * v->texture_width - v->texture_x_origin - p->capture_offset.x,
                 p->v1 * v->texture_height - p->capture_offset.y}};
}

static bool Uniform(const DioramaProjection *v, unsigned width, unsigned height, unsigned count,
                    uint32_t mask, float phase, EffectUniform *u) {
  if (!v || !v->valid || !width || !height || v->texture_width <= 0 || v->texture_height <= 0 ||
      v->bg2_skybox.count > kDioramaBgMaxValidSpans ||
      !(phase >= 0 && phase <= 1))
    return false;
  *u = (EffectUniform){
      .viewport = {v->output_x, v->output_y, v->output_width, v->output_height},
      .capture = {v->texture_width, v->texture_height, v->texture_x_origin, v->aspect_x},
      .geometry = {v->height_scale, width, height, count},
      .clock = {phase, mask, 0, 0}};
  memcpy(u->matrix, v->matrix, sizeof(u->matrix));
  u->planes[1] = Plane(v, &v->bg1_plane, 0);
  u->planes[2] = Plane(v, &v->bg2_plane, 3);
  u->planes[3] = Plane(v, &v->bg1_high_plane, 1);
  u->planes[4] = Plane(v, &v->bg2_high_plane, 4);
  for (unsigned i = 0; i < kDioramaObjectPriorityCount; ++i)
    u->planes[i ? 5 + i : 0] = Plane(v, &v->object_planes[i], -1);
  if (v->bg2_skybox.count) {
    const DioramaSkyboxProjection *sky = &v->bg2_skybox;
    const int motion_slot = sky->motion_source == 1 ? -1 : sky->motion_source == 2 ? 0 :
        sky->motion_source == 3 ? 3 : 6;
    u->sky_mapping = sky->resident;
    u->sky_meta[0] = sky->count; u->sky_meta[1] = sky->active_band;
    if (sky->world_plane.valid) {
      u->planes[2] = Plane(v, &sky->world_plane, motion_slot);
      u->planes[2].sky[2] = 2;
    } else u->planes[2] = (EffectPlaneUniform){.offset = {0, 0, motion_slot, 1}, .sky = {0, 1, 1, 1}};
    for (unsigned i = 0; i < sky->count; ++i) {
      memcpy(u->sky_bands[i], &sky->bands[i], sizeof(sky->bands[i]));
    }
    memcpy(u->follow, sky->motion_follow, sizeof(u->follow));
  }
  u->planes[5] = u->planes[2];
  return true;
}

bool ArGpuEffectSource_Draw(ArGpuEffectSource *p, ArRenderDevice *device, SDL_GPUBuffer *motion,
                            uint32_t mask, float phase, const ActionEffectSourceBatch *batch,
                            const DioramaProjection *view, const ActionMoonlightOcclusion *scenery,
                            ArRenderBlendMode blend, float brightness) {
  if (!batch || batch->failed || batch->count > kActionSourceMaximumPrimitives ||
      batch->light_count > kActionSourceMaxLightJobs || (batch->light_count && !batch->lights)) return false;
  if (!batch->count) return true;
  if (!batch->primitives) return false;
  const int b = blend == kArRenderBlendMode_Alpha   ? 0
                : blend == kArRenderBlendMode_Add   ? 1
                : blend == kArRenderBlendMode_Light ? 2
                                                    : -1;
  unsigned width, height, format;
  SDL_GPUTexture *target;
  SDL_Rect viewport, scissor;
  EffectUniform u;
  if (b < 0 || !p->device || !motion ||
      !Target(device, &target, &width, &height, &format, &viewport, &scissor) ||
      !Uniform(view, width, height, batch->count, mask, phase, &u))
    return false;
  const unsigned bytes = batch->count * sizeof(*batch->primitives);
  ArGpuEffectPacket *packet = batch->revision && batch->packet_slot < kActionSourcePacketSlots
      ? &p->packets[batch->packet_slot] : NULL;
  const bool upload_primitives = !packet || packet->revision != batch->revision ||
      packet->brightness != brightness;
  unsigned shadow_count = packet && !upload_primitives ? packet->shadow_count : 0;
  if (upload_primitives) {
    for (unsigned i = 0; i < batch->count; ++i) {
      const ActionEffectSourcePrimitive *s = &batch->primitives[i];
      if (s->meta[0] > kActionSourceFloorTriangle || s->meta[1] > 8) return false;
      if (s->meta[3] && scenery && scenery->valid) shadow_count = scenery->count;
    }
    if (shadow_count > kActionMoonlightMaxOccluders) return false;
    if (packet && bytes > packet->capacity) {
      /* Grow geometrically within the existing hard bound. SDL defers release
       * while prior submissions still reference the old allocation. */
      unsigned capacity = packet->capacity ? packet->capacity * 2 : 4096;
      if (capacity < bytes) capacity = bytes;
      if (capacity > kPrimitiveBytes) capacity = kPrimitiveBytes;
      const SDL_GPUBufferCreateInfo info = {
          .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, .size = capacity};
      SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(p->device, &info);
      if (!buffer) return false;
      SDL_ReleaseGPUBuffer(p->device, packet->primitives);
      packet->primitives = buffer;
      packet->capacity = capacity;
      packet->revision = 0;
    }
  }
  SDL_GPUBuffer *primitives = packet ? packet->primitives : p->primitives;
  const unsigned primitive_upload_bytes = upload_primitives ? bytes : 0;
  unsigned char *mapped = SDL_MapGPUTransferBuffer(p->device, p->upload, true);
  if (!mapped) return false;
  if (upload_primitives) {
    memcpy(mapped, batch->primitives, bytes);
    if (brightness != 1.0f)
      for (unsigned i = 0; i < batch->count; ++i)
        for (unsigned c = 0; c < (batch->primitives[i].meta[0] == kActionSourceQuad ? 4u : 3u); ++c)
          for (unsigned rgb = 0; rgb < 3; ++rgb)
            (c < 3 ? ((ActionEffectSourcePrimitive *)mapped)[i].colors[c] :
                     ((ActionEffectSourcePrimitive *)mapped)[i].extra)[rgb] *= brightness;
  }
  float (*rects)[4] = (float (*)[4])(mapped + primitive_upload_bytes);
  rects[0][0] = rects[0][1] = INFINITY;
  rects[0][2] = rects[0][3] = -INFINITY;
  const int ox = batch->context.ws_extra - batch->context.bg1_camera_x;
  const int oy = batch->context.ws_extra_top - batch->context.bg1_camera_y;
  for (unsigned i = 0; i < shadow_count; ++i) {
    const ActionMoonlightOccluder *r = &scenery->rectangles[i];
    rects[i + 1][0] = r->x0 + ox;
    rects[i + 1][1] = r->y0 + oy;
    rects[i + 1][2] = r->x1 + ox;
    rects[i + 1][3] = r->y1 + oy;
    rects[0][0] = fminf(rects[0][0], rects[i + 1][0]);
    rects[0][1] = fminf(rects[0][1], rects[i + 1][1]);
    rects[0][2] = fmaxf(rects[0][2], rects[i + 1][2]);
    rects[0][3] = fmaxf(rects[0][3], rects[i + 1][3]);
  }
  unsigned light_offset[kActionSourceMaxLightJobs], light_occluders[kActionSourceMaxLightJobs];
  unsigned upload_end = primitive_upload_bytes + (shadow_count + 1) * 16;
  for (unsigned i = 0; i < batch->light_count; ++i) {
    const ActionEffectSourceLightJob *job = &batch->lights[i];
    const unsigned count = job->occlusion && job->occlusion->valid ? job->occlusion->count : 0;
    if (count > kActionMoonlightMaxOccluders || job->data.meta[0] < 0 ||
        job->data.meta[0] > kActionSourceMaxLightPoints ||
        !isfinite(job->data.meta[0]) || floorf(job->data.meta[0]) != job->data.meta[0] ||
        !isfinite(job->data.meta[1]) || job->data.meta[1] < 0 || job->data.meta[1] > 3 ||
        floorf(job->data.meta[1]) != job->data.meta[1]) {
      SDL_UnmapGPUTransferBuffer(p->device, p->upload); return false;
    }
    light_offset[i] = upload_end; light_occluders[i] = count;
    memcpy(mapped + upload_end, &job->data, sizeof(job->data));
    upload_end += sizeof(job->data);
    float (*caster)[4] = (float (*)[4])(mapped + upload_end);
    for (unsigned j = 0; j < count; ++j) {
      const ActionMoonlightOccluder *r = &job->occlusion->rectangles[j];
      caster[j][0] = r->x0 + ox; caster[j][1] = r->y0 + oy;
      caster[j][2] = r->x1 + ox; caster[j][3] = r->y1 + oy;
    }
    upload_end += count * 16;
  }
  SDL_UnmapGPUTransferBuffer(p->device, p->upload);
  if (!ArSdlRenderBackend_SubmitPending(device)) return false;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device);
  if (!cmd) return false;
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) goto failed;
  if (upload_primitives) {
    const SDL_GPUTransferBufferLocation from = {.transfer_buffer = p->upload};
    const SDL_GPUBufferRegion to = {.buffer = primitives, .size = bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
  }
  const SDL_GPUTransferBufferLocation rect_from = {
      .transfer_buffer = p->upload, .offset = primitive_upload_bytes};
  const SDL_GPUBufferRegion rect_to = {.buffer = p->occluders, .size = (shadow_count + 1) * 16};
  SDL_UploadToGPUBuffer(copy, &rect_from, &rect_to, true);
  SDL_EndGPUCopyPass(copy);
  u.clock[2] = shadow_count;
  if (shadow_count)
    for (unsigned stage = 0; stage < 2; ++stage) {
      u.clock[3] = stage;
      SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
      const SDL_GPUStorageBufferReadWriteBinding out = {.buffer = p->coverage, .cycle = stage == 0};
      SDL_GPUComputePass *compute = SDL_BeginGPUComputePass(cmd, NULL, 0, &out, 1);
      if (!compute) goto failed;
      SDL_BindGPUComputePipeline(compute, p->shadow);
      SDL_GPUBuffer *inputs[] = {p->occluders, motion};
      SDL_BindGPUComputeStorageBuffers(compute, 0, inputs, 2);
      SDL_DispatchGPUCompute(compute, stage ? shadow_count : (400 * 176 + 63) / 64, 1, 1);
      SDL_EndGPUComputePass(compute);
    }
  bool light_cycled = false;
  for (unsigned i = 0; i < batch->light_count; ++i) {
    copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) goto failed;
    const SDL_GPUTransferBufferLocation job_from = {.transfer_buffer=p->upload,.offset=light_offset[i]};
    const SDL_GPUBufferRegion job_to = {.buffer=p->moon_job,.size=sizeof(ActionEffectSourceLightData)};
    SDL_UploadToGPUBuffer(copy,&job_from,&job_to,true);
    if (light_occluders[i]) {
      const SDL_GPUTransferBufferLocation caster_from = {.transfer_buffer=p->upload,
          .offset=light_offset[i]+sizeof(ActionEffectSourceLightData)};
      const SDL_GPUBufferRegion caster_to = {.buffer=p->occluders,.size=light_occluders[i]*16};
      SDL_UploadToGPUBuffer(copy,&caster_from,&caster_to,true);
    }
    SDL_EndGPUCopyPass(copy);
    u.moon[0] = i*kActionSourceMaxLightPoints; u.moon[1] = light_occluders[i];
    const unsigned count = (unsigned)batch->lights[i].data.meta[0];
    if (!count) continue;
    for (unsigned stage=0; stage<3; ++stage) {
      if (stage==1 && !light_occluders[i]) continue;
      u.clock[3] = stage;
      SDL_PushGPUComputeUniformData(cmd,0,&u,sizeof(u));
      const SDL_GPUStorageBufferReadWriteBinding outputs[] = {
          {.buffer=p->moon_work,.cycle=stage==0}, {.buffer=p->moon_mask,.cycle=stage==0},
          {.buffer=p->moon_light,.cycle=!light_cycled&&stage==0}};
      SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd,NULL,0,outputs,3);
      if (!pass) goto failed;
      SDL_BindGPUComputePipeline(pass,p->moon);
      SDL_GPUBuffer *inputs[] = {p->moon_job,motion,p->occluders};
      SDL_BindGPUComputeStorageBuffers(pass,0,inputs,3);
      const unsigned groups = stage==0 ? (batch->lights[i].data.meta[1]==3 ? 1 : (800*224+63)/64) :
          stage==1 ? light_occluders[i] : (count+63)/64;
      SDL_DispatchGPUCompute(pass,groups,1,1);SDL_EndGPUComputePass(pass);
    }
    light_cycled = true;
  }
  SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
  const SDL_GPUStorageBufferReadWriteBinding out = {.buffer = p->vertices, .cycle = true};
  SDL_GPUComputePass *compute = SDL_BeginGPUComputePass(cmd, NULL, 0, &out, 1);
  if (!compute) goto failed;
  SDL_BindGPUComputePipeline(compute, p->project);
  SDL_GPUBuffer *inputs[] = {primitives, motion, p->coverage, p->moon_light};
  SDL_BindGPUComputeStorageBuffers(compute, 0, inputs, 4);
  SDL_DispatchGPUCompute(compute, (batch->count + 63) / 64, 1, 1);
  SDL_EndGPUComputePass(compute);
  const SDL_GPUColorTargetInfo color = {
      .texture = target, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE};
  SDL_GPURenderPass *draw = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
  if (!draw) goto failed;
  SDL_BindGPUGraphicsPipeline(draw, p->draw[format][b]);
  SDL_BindGPUVertexStorageBuffers(draw, 0, &p->vertices, 1);
  const float size[4] = {width, height, 0, 0};
  SDL_PushGPUVertexUniformData(cmd, 0, size, sizeof(size));
  SDL_SetGPUScissor(draw, &scissor);
  SDL_DrawGPUPrimitives(draw, batch->count * 15, 1, 0, 0);
  SDL_EndGPURenderPass(draw);
  if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;
  if (packet) {
    packet->revision = batch->revision;
    packet->brightness = brightness;
    packet->shadow_count = shadow_count;
  }
  return true;
failed:
  SDL_CancelGPUCommandBuffer(cmd);
  return false;
}

bool ArGpuEffectSource_Skybox(ArGpuEffectSource *p, ArRenderDevice *device, SDL_GPUBuffer *motion,
                              int slot, float phase, ArRenderTexture image,
                              const DioramaSkyboxSourceDraw *input) {
  const ArRenderVertex2D *vertices = input->vertices;
  const ArRenderBlendMode blend = input->blend;
  const int texture_width = input->texture_width, texture_height = input->texture_height;
  if (!vertices || !input->indices || !input->index_count ||
      input->index_count > kDioramaSkyboxSourceMaxIndices || texture_width <= 0 || texture_height <= 0)
    return false;
  for (unsigned i=0;i<input->index_count;++i)
    if (input->indices[i] < 0 || (unsigned)input->indices[i] >= input->vertex_count) return false;
  unsigned width, height, format;
  SDL_GPUTexture *target;
  SDL_Rect viewport, scissor;
  SDL_Texture *texture = ArSdlRenderBackend_UnwrapTexture(image);
  if (!p->device || !motion || !texture ||
      !Target(device, &target, &width, &height, &format, &viewport, &scissor))
    return false;
  SDL_GPUTexture *source = SDL_GetPointerProperty(SDL_GetTextureProperties(texture),
                                                  SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
  if (!source || source == target ||
      (blend != kArRenderBlendMode_Opaque && blend != kArRenderBlendMode_Alpha))
    return false;
  ArRenderVertex2D *mapped = SDL_MapGPUTransferBuffer(p->device, p->upload, true);
  if (!mapped) return false;
  for (unsigned i = 0; i < input->index_count; ++i)
    mapped[i] = vertices[input->indices[i]];
  SDL_UnmapGPUTransferBuffer(p->device, p->upload);
  if (!ArSdlRenderBackend_SubmitPending(device)) return false;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device);
  if (!cmd) return false;
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) goto failed;
  const SDL_GPUTransferBufferLocation from = {.transfer_buffer = p->upload};
  const SDL_GPUBufferRegion to = {.buffer = p->sky_vertices, .size = input->index_count * sizeof(*vertices)};
  SDL_UploadToGPUBuffer(copy, &from, &to, true);
  SDL_EndGPUCopyPass(copy);
  const SDL_GPUColorTargetInfo color = {
      .texture = target, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE};
  SDL_GPURenderPass *draw = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
  if (!draw) goto failed;
  SDL_BindGPUGraphicsPipeline(draw, p->skybox[format][blend == kArRenderBlendMode_Alpha]);
  const SDL_GPUBufferBinding vb = {.buffer = p->sky_vertices};
  SDL_BindGPUVertexBuffers(draw, 0, &vb, 1);
  SDL_BindGPUVertexStorageBuffers(draw, 0, &motion, 1);
  struct {
    float target[4], motion[4];
    DioramaSkyboxSourceMapping mapping;
  } u = {.target = {width, height, viewport.x, viewport.y},
         .motion = {slot, phase, input->periodic, input->band}, .mapping = input->mapping};
  SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
  const float f[4] = {1.f / texture_width, 1.f / texture_height, input->radius, 0};
  SDL_PushGPUFragmentUniformData(cmd, 0, f, sizeof(f));
  SDL_ScaleMode scale;
  if (!SDL_GetTextureScaleMode(texture, &scale)) {
    SDL_EndGPURenderPass(draw);
    goto failed;
  }
  const SDL_GPUTextureSamplerBinding sample = {
      source, input->periodic ? (scale == SDL_SCALEMODE_NEAREST ? p->wrap_nearest_sampler : p->wrap_sampler) :
          (scale == SDL_SCALEMODE_NEAREST ? p->nearest_sampler : p->sampler)};
  SDL_BindGPUFragmentSamplers(draw, 0, &sample, 1);
  SDL_SetGPUScissor(draw, &scissor);
  SDL_DrawGPUPrimitives(draw, input->index_count, 1, 0, 0);
  SDL_EndGPURenderPass(draw);
  return SDL_SubmitGPUCommandBuffer(cmd);
failed:
  SDL_CancelGPUCommandBuffer(cmd);
  return false;
}
