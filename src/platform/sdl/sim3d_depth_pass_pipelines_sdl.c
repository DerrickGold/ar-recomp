/* Sim3DDepthPass pipelines (SDL): the pass's GPU device resources: render
 * targets, the white texture, and a graphics pipeline per layer family
 * (depth, body, surface, model, linear, radial) from the generated shaders.
 * Phase: present (render owner thread).
 * Tests: tests/sim3d_depth_pass_gpu_test.c */
#include "platform/sdl/sim3d_depth_pass_sdl_internal.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/sim3d_depth_vert.h"
#include "shaders/sim3d_billboard_rim_frag.h"
#include "shaders/sim3d_spherical_vert.h"
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
#include "shaders/sim3d_model_vert.h"
#include "shaders/sim3d_model_clipped_vert.h"
#endif
#include "shaders/sim3d_model_clipped_frag.h"
#include "shaders/sim3d_linear_vert.h"
#include "shaders/sim3d_radial_vert.h"
#include "shaders/sim3d_surface_vert.h"
#include "shaders/sim3d_surface_frag.h"
#include "shaders/sim3d_shadow_batch_vert.h"
#include "shaders/sim3d_shadow_batch_frag.h"
#include "shaders/sim3d_spherical_body_vert.h"
#include "shaders/sim3d_spherical_body_frag.h"

static const GpuShaderBlobs kVertexBlobs = {
  kSim3dDepthVertMSL, kSim3dDepthVertMSLSize,
  kSim3dDepthVertSPV, kSim3dDepthVertSPVSize,
  kSim3dDepthVertDXIL, kSim3dDepthVertDXILSize,
};
static const GpuShaderBlobs kFragmentBlobs = {
  kSim3dDepthFragMSL, kSim3dDepthFragMSLSize,
  kSim3dDepthFragSPV, kSim3dDepthFragSPVSize,
  kSim3dDepthFragDXIL, kSim3dDepthFragDXILSize,
};
static const GpuShaderBlobs kBillboardRimBlobs = {
  kSim3dBillboardRimFragMSL, kSim3dBillboardRimFragMSLSize,
  kSim3dBillboardRimFragSPV, kSim3dBillboardRimFragSPVSize,
  kSim3dBillboardRimFragDXIL, kSim3dBillboardRimFragDXILSize,
};
static const GpuShaderBlobs kSphericalBlobs = {
  kSim3dSphericalVertMSL, kSim3dSphericalVertMSLSize,
  kSim3dSphericalVertSPV, kSim3dSphericalVertSPVSize,
  kSim3dSphericalVertDXIL, kSim3dSphericalVertDXILSize,
};
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
static const GpuShaderBlobs kModelBlobs = {
  kSim3dModelVertMSL, kSim3dModelVertMSLSize,
  kSim3dModelVertSPV, kSim3dModelVertSPVSize,
  kSim3dModelVertDXIL, kSim3dModelVertDXILSize,
};
static const GpuShaderBlobs kModelClippedBlobs = {
  kSim3dModelClippedVertMSL, kSim3dModelClippedVertMSLSize,
  kSim3dModelClippedVertSPV, kSim3dModelClippedVertSPVSize,
  kSim3dModelClippedVertDXIL, kSim3dModelClippedVertDXILSize,
};
#endif
static const GpuShaderBlobs kModelClippedFragmentBlobs = {
  kSim3dModelClippedFragMSL, kSim3dModelClippedFragMSLSize,
  kSim3dModelClippedFragSPV, kSim3dModelClippedFragSPVSize,
  kSim3dModelClippedFragDXIL, kSim3dModelClippedFragDXILSize,
};
static const GpuShaderBlobs kRadialBlobs = {
  kSim3dRadialVertMSL, kSim3dRadialVertMSLSize,
  kSim3dRadialVertSPV, kSim3dRadialVertSPVSize,
  kSim3dRadialVertDXIL, kSim3dRadialVertDXILSize,
};
static const GpuShaderBlobs kLinearBlobs = {
  kSim3dLinearVertMSL, kSim3dLinearVertMSLSize,
  kSim3dLinearVertSPV, kSim3dLinearVertSPVSize,
  kSim3dLinearVertDXIL, kSim3dLinearVertDXILSize,
};
static const GpuShaderBlobs kSurfaceVertexBlobs = {
  kSim3dSurfaceVertMSL, kSim3dSurfaceVertMSLSize,
  kSim3dSurfaceVertSPV, kSim3dSurfaceVertSPVSize,
  kSim3dSurfaceVertDXIL, kSim3dSurfaceVertDXILSize,
};
static const GpuShaderBlobs kSurfaceFragmentBlobs = {
  kSim3dSurfaceFragMSL, kSim3dSurfaceFragMSLSize,
  kSim3dSurfaceFragSPV, kSim3dSurfaceFragSPVSize,
  kSim3dSurfaceFragDXIL, kSim3dSurfaceFragDXILSize,
};
static const GpuShaderBlobs kShadowBatchVertexBlobs = {
  kSim3dShadowBatchVertMSL, kSim3dShadowBatchVertMSLSize,
  kSim3dShadowBatchVertSPV, kSim3dShadowBatchVertSPVSize,
  kSim3dShadowBatchVertDXIL, kSim3dShadowBatchVertDXILSize,
};
static const GpuShaderBlobs kShadowBatchFragmentBlobs = {
  kSim3dShadowBatchFragMSL, kSim3dShadowBatchFragMSLSize,
  kSim3dShadowBatchFragSPV, kSim3dShadowBatchFragSPVSize,
  kSim3dShadowBatchFragDXIL, kSim3dShadowBatchFragDXILSize,
};
static const GpuShaderBlobs kBodyVertexBlobs = {
  kSim3dSphericalBodyVertMSL, kSim3dSphericalBodyVertMSLSize,
  kSim3dSphericalBodyVertSPV, kSim3dSphericalBodyVertSPVSize,
  kSim3dSphericalBodyVertDXIL, kSim3dSphericalBodyVertDXILSize,
};
static const GpuShaderBlobs kBodyFragmentBlobs = {
  kSim3dSphericalBodyFragMSL, kSim3dSphericalBodyFragMSLSize,
  kSim3dSphericalBodyFragSPV, kSim3dSphericalBodyFragSPVSize,
  kSim3dSphericalBodyFragDXIL, kSim3dSphericalBodyFragDXILSize,
};

bool CreateBodyPipeline(void) {
  if (g_depth_pass.body_pipeline_attempted) return g_depth_pass.body_pipeline != NULL;
  g_depth_pass.body_pipeline_attempted = true;
  g_depth_pass.body_vertex = GpuShaderBlob_Create(g_depth_pass.device, &kBodyVertexBlobs,
      "SIM3D spherical body", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  g_depth_pass.body_fragment = GpuShaderBlob_Create(g_depth_pass.device, &kBodyFragmentBlobs,
      "SIM3D continuous spherical material", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
  if (!g_depth_pass.body_vertex || !g_depth_pass.body_fragment) return false;
  const SDL_GPUVertexBufferDescription buffer = {
    0, sizeof(Sim3DDepthSphericalBodyVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0,
  };
  const SDL_GPUVertexAttribute attribute = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0};
  const SDL_GPUColorTargetDescription color = {
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .blend_state = {
      .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
      .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .color_blend_op = SDL_GPU_BLENDOP_ADD,
      .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
      .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .alpha_blend_op = SDL_GPU_BLENDOP_ADD, .enable_blend = true,
    },
  };
  const SDL_GPUGraphicsPipelineCreateInfo info = {
    .vertex_shader = g_depth_pass.body_vertex, .fragment_shader = g_depth_pass.body_fragment,
    .vertex_input_state = {&buffer, 1, &attribute, 1},
    .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
      .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
    .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
    .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
      .enable_depth_test = true, .enable_depth_write = false},
    .target_info = {.color_target_descriptions = &color, .num_color_targets = 1,
      .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true},
  };
  g_depth_pass.body_pipeline = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  return g_depth_pass.body_pipeline != NULL;
}

bool CreateSurfacePipelines(void) {
  if (g_depth_pass.surface_pipeline_attempted)
    return g_depth_pass.surface_pipeline[0] && g_depth_pass.surface_pipeline[1] &&
        g_depth_pass.surface_pipeline[2];
  g_depth_pass.surface_pipeline_attempted = true;
  g_depth_pass.surface_vertex = GpuShaderBlob_Create(g_depth_pass.device, &kSurfaceVertexBlobs,
      "SIM3D shared radial surface", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  g_depth_pass.surface_fragment = GpuShaderBlob_Create(g_depth_pass.device, &kSurfaceFragmentBlobs,
      "SIM3D affine surface material", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  g_depth_pass.shadow_batch_vertex =
      GpuShaderBlob_Create(g_depth_pass.device, &kShadowBatchVertexBlobs,
                           "SIM3D batched surface shadows", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  g_depth_pass.shadow_batch_fragment =
      GpuShaderBlob_Create(g_depth_pass.device, &kShadowBatchFragmentBlobs,
                           "SIM3D shadow transmittance", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  if (!g_depth_pass.surface_vertex || !g_depth_pass.surface_fragment ||
      !g_depth_pass.shadow_batch_vertex || !g_depth_pass.shadow_batch_fragment) return false;
  const SDL_GPUVertexBufferDescription buffer = {
    0, sizeof(Sim3DSurfaceGpuQuad), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0,
  };
  SDL_GPUVertexAttribute attributes[16];
  for (Uint32 i = 0; i < 16; ++i)
    attributes[i] = (SDL_GPUVertexAttribute){i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, i * 16};
  const SDL_GPUColorTargetDescription color = {
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .blend_state = {
      .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
      .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .color_blend_op = SDL_GPU_BLENDOP_ADD,
      .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
      .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .alpha_blend_op = SDL_GPU_BLENDOP_ADD, .enable_blend = true,
    },
  };
  SDL_GPUGraphicsPipelineCreateInfo info = {
    .vertex_shader = g_depth_pass.surface_vertex, .fragment_shader = g_depth_pass.surface_fragment,
    .vertex_input_state = {&buffer, 1, attributes, 16},
    .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
      .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
    .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
    .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
      .enable_depth_test = true, .enable_depth_write = true},
    .target_info = {.color_target_descriptions = &color, .num_color_targets = 1,
      .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true},
  };
  g_depth_pass.surface_pipeline[0] = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  info.depth_stencil_state.enable_depth_write = false;
  g_depth_pass.surface_pipeline[1] = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  info.vertex_shader = g_depth_pass.shadow_batch_vertex;
  info.fragment_shader = g_depth_pass.shadow_batch_fragment;
  info.vertex_input_state.num_vertex_attributes = 8;
  g_depth_pass.surface_pipeline[2] = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  return g_depth_pass.surface_pipeline[0] && g_depth_pass.surface_pipeline[1] &&
      g_depth_pass.surface_pipeline[2];
}

/* Prepared at video boot/reset. Attempts are retained so a rejected variant
 * cannot trigger shader compilation or capability discovery during gameplay. */
bool CreateModelPipeline(Sim3DModelPipeline variant) {
  if ((unsigned)variant >= kModelPipelineCount) return false;
  if (g_depth_pass.model_pipeline_attempted[variant])
    return g_depth_pass.model_pipeline[variant] != NULL;
  g_depth_pass.model_pipeline_attempted[variant] = true;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
  if (variant == kModelPipeline_Linear && g_depth_pass_reject_linear_preparation) return false;
  const SDL_GPUVertexAttribute attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Sim3DDepthModelVertex, color)},
  };
#endif
  const SDL_GPUVertexAttribute radial_attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Sim3DDepthRadialVertex, normal)},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Sim3DDepthRadialVertex, elevation)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Sim3DDepthRadialVertex, color)},
    {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Sim3DDepthRadialVertex, variant)},
  };
  const SDL_GPUVertexAttribute linear_attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Sim3DDepthLinearVertex, position)},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Sim3DDepthLinearVertex, color)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Sim3DDepthLinearVertex, axis)},
  };
  const GpuShaderBlobs *blobs;
  const char *label;
  const SDL_GPUVertexAttribute *vertex_attributes;
  Uint32 stride, attribute_count;
  bool hardware_clipping = true;
  switch (variant) {
    case kModelPipeline_Radial:
      blobs = &kRadialBlobs;
      label = "SIM3D radial models";
      vertex_attributes = radial_attributes;
      attribute_count = 4;
      stride = sizeof(Sim3DDepthRadialVertex);
      break;
    case kModelPipeline_Linear:
      blobs = &kLinearBlobs;
      label = "SIM3D linear models";
      vertex_attributes = linear_attributes;
      attribute_count = 3;
      stride = sizeof(Sim3DDepthLinearVertex);
      break;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
    case kModelPipeline_ReferenceAffine:
    case kModelPipeline_ReferenceClipped:
      hardware_clipping = variant == kModelPipeline_ReferenceClipped;
      blobs = hardware_clipping ? &kModelClippedBlobs : &kModelBlobs;
      label = "SIM3D test reference models";
      vertex_attributes = attributes;
      attribute_count = 2;
      stride = sizeof(Sim3DDepthModelVertex);
      break;
#endif
    default: return false;
  }
  g_depth_pass.model_shader[variant] = GpuShaderBlob_Create(g_depth_pass.device,
      blobs, label, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  if (!g_depth_pass.model_shader[variant]) return false;
  if (hardware_clipping && !g_depth_pass.model_clipped_fragment) {
    g_depth_pass.model_clipped_fragment = GpuShaderBlob_Create(g_depth_pass.device,
        &kModelClippedFragmentBlobs, "SIM3D screen-linear model color",
        SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!g_depth_pass.model_clipped_fragment) return false;
  }
  const SDL_GPUVertexBufferDescription buffer = {
    0, stride, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0,
  };
  const SDL_GPUColorTargetDescription color = {
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .blend_state = {
      .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
      .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .color_blend_op = SDL_GPU_BLENDOP_ADD,
      .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
      .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      .alpha_blend_op = SDL_GPU_BLENDOP_ADD, .enable_blend = true,
    },
  };
  const SDL_GPUGraphicsPipelineCreateInfo info = {
    .vertex_shader = g_depth_pass.model_shader[variant],
    .fragment_shader = hardware_clipping ? g_depth_pass.model_clipped_fragment
                                         : g_depth_pass.fragment_shader,
    .vertex_input_state = {&buffer, 1, vertex_attributes, attribute_count},
    .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    .rasterizer_state = {
      .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
      .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true,
    },
    .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
    .depth_stencil_state = {
      .compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
      .enable_depth_test = true, .enable_depth_write = true,
    },
    .target_info = { .color_target_descriptions = &color, .num_color_targets = 1,
      .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true },
  };
  g_depth_pass.model_pipeline[variant] = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  return g_depth_pass.model_pipeline[variant] != NULL;
}

SDL_GPUGraphicsPipeline *MeshPipeline(Sim3DMeshKind kind) {
  switch (kind) {
    case kMeshScreen: return g_depth_pass.mesh_pipeline;
    case kMeshSpherical: return g_depth_pass.spherical_pipeline;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
    case kMeshModel: return g_depth_pass.model_pipeline[kModelPipeline_ReferenceAffine];
    case kMeshModelClipped: return g_depth_pass.model_pipeline[kModelPipeline_ReferenceClipped];
#endif
    case kMeshRadial: return g_depth_pass.model_pipeline[kModelPipeline_Radial];
    case kMeshLinear: return g_depth_pass.model_pipeline[kModelPipeline_Linear];
    case kMeshSurface: return g_depth_pass.surface_pipeline[0];
    case kMeshGeometry: return g_depth_pass.pipeline;
    case kMeshSphericalBody: return g_depth_pass.body_pipeline;
  }
  return NULL;
}

SDL_GPUTexture *GpuTexture(SDL_Texture *texture) {
  if (!texture) return NULL;
  /* SDL_GPU resources are device-owned. Binding a texture created by a
   * different renderer can hand one backend a handle owned by another
   * device, which is an API contract violation rather than a recoverable
   * draw error. All current callers use this renderer, but keep the boundary
   * explicit so future material layers fail closed. */
  if (SDL_GetRendererFromTexture(texture) != g_depth_pass.renderer) {
    SDL_SetError("SIM3D depth texture belongs to another renderer");
    return NULL;
  }
  SDL_PropertiesID props = SDL_GetTextureProperties(texture);
  return props ? SDL_GetPointerProperty(
      props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL) : NULL;
}

void ReleaseTargets(void) {
  if (g_depth_pass.output_texture)
    SDL_DestroyTexture(g_depth_pass.output_texture);
  if (g_depth_pass.color_target)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.color_target);
  if (g_depth_pass.depth_target)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.depth_target);
  g_depth_pass.output_texture = NULL;
  g_depth_pass.color_target = NULL;
  g_depth_pass.depth_target = NULL;
  g_depth_pass.width = 0;
  g_depth_pass.height = 0;
}

bool CreateTargets(SDL_Renderer *renderer, int width, int height,
                          SDL_ScaleMode scale_mode) {
  if (g_depth_pass.output_texture && g_depth_pass.width == width &&
      g_depth_pass.height == height) {
    return SDL_SetTextureScaleMode(g_depth_pass.output_texture, scale_mode);
  }
  ReleaseTargets();

  SDL_GPUTextureCreateInfo color_info;
  SDL_zero(color_info);
  color_info.type = SDL_GPU_TEXTURETYPE_2D;
  color_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
      SDL_GPU_TEXTUREUSAGE_SAMPLER;
  color_info.width = (Uint32)width;
  color_info.height = (Uint32)height;
  color_info.layer_count_or_depth = 1;
  color_info.num_levels = 1;
  color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  g_depth_pass.color_target = SDL_CreateGPUTexture(
      g_depth_pass.device, &color_info);

  SDL_GPUTextureCreateInfo depth_info = color_info;
  depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
  /* D3D12 records an optimized clear value when the resource is created.
   * SDL still produces the correct result if this differs from the render
   * pass, but the mismatch triggers the D3D12 validation layer and can force
   * a slower clear. Other backends ignore this documented property. */
  SDL_PropertiesID depth_props = SDL_CreateProperties();
  if (depth_props) {
    SDL_SetFloatProperty(depth_props,
        SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT, 1.0f);
    depth_info.props = depth_props;
  }
  g_depth_pass.depth_target = SDL_CreateGPUTexture(
      g_depth_pass.device, &depth_info);
  if (depth_props) SDL_DestroyProperties(depth_props);
  if (!g_depth_pass.color_target || !g_depth_pass.depth_target) {
    fprintf(stderr, "[sim3d-depth] render target creation failed: %s\n",
            SDL_GetError());
    ReleaseTargets();
    return false;
  }

  SDL_PropertiesID props = SDL_CreateProperties();
  if (props) {
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
        SDL_GetPixelFormatFromGPUTextureFormat(color_info.format));
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
        SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,
                           g_depth_pass.color_target);
    g_depth_pass.output_texture =
        SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
  }
  if (!g_depth_pass.output_texture ||
      !SDL_SetTextureBlendMode(g_depth_pass.output_texture,
                               SDL_BLENDMODE_BLEND) ||
      !SDL_SetTextureScaleMode(g_depth_pass.output_texture, scale_mode)) {
    fprintf(stderr, "[sim3d-depth] SDL target wrapper failed: %s\n",
            SDL_GetError());
    ReleaseTargets();
    return false;
  }
  g_depth_pass.width = width;
  g_depth_pass.height = height;
  return true;
}

static bool CreatePipeline(void) {
  g_depth_pass.vertex_shader = GpuShaderBlob_Create(
      g_depth_pass.device, &kVertexBlobs, "SIM3D depth vertex",
      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  g_depth_pass.fragment_shader = GpuShaderBlob_CreateFragment(
      g_depth_pass.device, &kFragmentBlobs, "SIM3D depth fragment", 1, 0);
  if (!g_depth_pass.vertex_shader || !g_depth_pass.fragment_shader)
    return false;

  const SDL_GPUVertexBufferDescription buffer = {
    .slot = 0,
    .pitch = sizeof(Sim3DGpuVertex),
    .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
  };
  const SDL_GPUVertexAttribute attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     offsetof(Sim3DGpuVertex, position)},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     offsetof(Sim3DGpuVertex, color)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     offsetof(Sim3DGpuVertex, uv)},
  };
  SDL_GPUColorTargetDescription color_target;
  SDL_zero(color_target);
  color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineCreateInfo info;
  SDL_zero(info);
  info.vertex_shader = g_depth_pass.vertex_shader;
  info.fragment_shader = g_depth_pass.fragment_shader;
  info.vertex_input_state.vertex_buffer_descriptions = &buffer;
  info.vertex_input_state.num_vertex_buffers = 1;
  info.vertex_input_state.vertex_attributes = attributes;
  info.vertex_input_state.num_vertex_attributes =
      (Uint32)(sizeof(attributes) / sizeof(attributes[0]));
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
  info.rasterizer_state.enable_depth_clip = true;
  info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  info.depth_stencil_state.enable_depth_test = true;
  info.depth_stencil_state.enable_depth_write = true;
  info.target_info.color_target_descriptions = &color_target;
  info.target_info.num_color_targets = 1;
  info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  info.target_info.has_depth_stencil_target = true;
  g_depth_pass.pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.pipeline) {
    fprintf(stderr, "[sim3d-depth] pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  /* The town ground is already rendered with its native texture through
   * SDL_RenderGeometry.  This pipeline contributes the identical surface to
   * D32 without touching the transparent composite's color target. */
  color_target.blend_state.color_write_mask = 0;
  color_target.blend_state.enable_color_write_mask = true;
  g_depth_pass.depth_occluder_pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.depth_occluder_pipeline) {
    fprintf(stderr, "[sim3d-depth] depth-only pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  color_target.blend_state.enable_color_write_mask = false;
  info.depth_stencil_state.enable_depth_write = false;
  g_depth_pass.effect_pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.effect_pipeline) {
    fprintf(stderr, "[sim3d-depth] effect pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }

  /* Prepared with the other depth pipelines at boot, not on the first actor.
   * Uses the same packed vertices and read-only world depth as ordinary art. */
  g_depth_pass.billboard_rim_fragment = GpuShaderBlob_CreateFragment(
      g_depth_pass.device, &kBillboardRimBlobs, "SIM3D billboard rim", 1, 1);
  if (!g_depth_pass.billboard_rim_fragment) return false;
  info.fragment_shader = g_depth_pass.billboard_rim_fragment;
  color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  g_depth_pass.billboard_rim_pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.billboard_rim_pipeline) {
    fprintf(stderr, "[sim3d-depth] billboard rim pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  info.fragment_shader = g_depth_pass.fragment_shader;
  color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;

  /* Same shaders and blend/depth rules, split inputs: retained positions,
   * streamed UVs, and one constant color per sample (instance-rate input). */
  const SDL_GPUVertexBufferDescription mesh_buffers[] = {
    {0, 4 * sizeof(float), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
    {1, sizeof(ArRenderPointF), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
    {2, sizeof(ArRenderColorF), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
  };
  const SDL_GPUVertexAttribute mesh_attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
    {1, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
    {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
  };
  info.vertex_input_state.vertex_buffer_descriptions = mesh_buffers;
  info.vertex_input_state.num_vertex_buffers = 3;
  info.vertex_input_state.vertex_attributes = mesh_attributes;
  g_depth_pass.mesh_pipeline = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  /* Optional optimization: unavailable split-input pipelines keep the
   * ordinary append path, without making the whole renderer unavailable. */

  g_depth_pass.spherical_shader = GpuShaderBlob_Create(g_depth_pass.device,
      &kSphericalBlobs, "SIM3D spherical mapping", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  if (g_depth_pass.spherical_shader) {
    const SDL_GPUVertexBufferDescription spherical_buffer = {
      0, sizeof(Sim3DSphericalGpuQuad), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0,
    };
    SDL_GPUVertexAttribute spherical_attributes[10];
    for (Uint32 i = 0; i < 10; ++i)
      spherical_attributes[i] = (SDL_GPUVertexAttribute){
        i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, i * 4 * (Uint32)sizeof(float),
      };
    info.vertex_shader = g_depth_pass.spherical_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &spherical_buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = spherical_attributes;
    info.vertex_input_state.num_vertex_attributes = 10;
    g_depth_pass.spherical_pipeline = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  }

  SDL_GPUSamplerCreateInfo sampler;
  SDL_zero(sampler);
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  g_depth_pass.nearest_sampler = SDL_CreateGPUSampler(
      g_depth_pass.device, &sampler);
  if (!g_depth_pass.nearest_sampler) {
    fprintf(stderr, "[sim3d-depth] nearest sampler creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  g_depth_pass.linear_sampler = SDL_CreateGPUSampler(
      g_depth_pass.device, &sampler);
  if (!g_depth_pass.linear_sampler) {
    fprintf(stderr, "[sim3d-depth] linear sampler creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

bool TextureUploadsNeedAlignment(void) {
  const char *driver = SDL_GetGPUDeviceDriver(g_depth_pass.device);
  return driver && !SDL_strcmp(driver, "direct3d12");
}

static bool CreateWhiteTexture(SDL_Renderer *renderer) {
  Uint32 upload_size = 0;
  ArSdlTextureUploadLayout layout;
  if (!ArSdlTextureUploadLayout_Append(1, 1, TextureUploadsNeedAlignment(),
          &upload_size, &layout)) return false;
  SDL_GPUTextureCreateInfo texture_info;
  SDL_zero(texture_info);
  texture_info.type = SDL_GPU_TEXTURETYPE_2D;
  texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texture_info.width = 1;
  texture_info.height = 1;
  texture_info.layer_count_or_depth = 1;
  texture_info.num_levels = 1;
  texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(
      g_depth_pass.device, &texture_info);
  SDL_GPUTransferBufferCreateInfo transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = upload_size,
  };
  SDL_GPUTransferBuffer *transfer = texture ? SDL_CreateGPUTransferBuffer(
      g_depth_pass.device, &transfer_info) : NULL;
  uint8_t *mapped = transfer ? SDL_MapGPUTransferBuffer(
      g_depth_pass.device, transfer, false) : NULL;
  if (!mapped) {
    if (transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    if (texture) SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  memset(mapped, 0xFF, kSim3DDepthRgbaBytesPerPixel);
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, transfer);
  if (!SDL_FlushRenderer(renderer)) {
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_GPUTextureTransferInfo source = {
    .transfer_buffer = transfer,
    .pixels_per_row = layout.row_pitch / kSim3DDepthRgbaBytesPerPixel,
    .rows_per_layer = 1,
  };
  SDL_GPUTextureRegion destination = {
    .texture = texture,
    .w = 1,
    .h = 1,
    .d = 1,
  };
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  PerformanceMetrics_AddTextureUpload(1, kSim3DDepthRgbaBytesPerPixel);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
  g_depth_pass.white_texture = texture;
  return true;
}

bool EnsureInitialized(SDL_Renderer *renderer) {
  if (g_depth_pass.failed) return false;
  if (g_depth_pass.renderer == renderer && g_depth_pass.pipeline) return true;
  if (g_depth_pass.renderer && g_depth_pass.renderer != renderer)
    Sim3DDepthPass_Reset(NULL);

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  g_depth_pass.device = props ? SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL) : NULL;
  if (!g_depth_pass.device) {
    fprintf(stderr, "[sim3d-depth] renderer has no SDL_GPU device\n");
    g_depth_pass.failed = true;
    return false;
  }
  if (!SDL_GPUTextureSupportsFormat(
          g_depth_pass.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
          SDL_GPU_TEXTURETYPE_2D,
          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
    fprintf(stderr, "[sim3d-depth] D32 depth targets are unsupported\n");
    g_depth_pass.failed = true;
    return false;
  }
  if (!SDL_GPUTextureSupportsFormat(
          g_depth_pass.device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
          SDL_GPU_TEXTURETYPE_2D,
          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
              SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
    fprintf(stderr, "[sim3d-depth] RGBA8 sampled color targets are "
                    "unsupported\n");
    g_depth_pass.failed = true;
    return false;
  }
  g_depth_pass.renderer = renderer;
  if (!CreatePipeline() || !CreateWhiteTexture(renderer)) {
    fprintf(stderr, "[sim3d-depth] required GPU resources failed: %s\n",
            SDL_GetError());
    g_depth_pass.failed = true;
    return false;
  }
  return true;
}

bool Sim3DDepthPass_Require(ArRenderDevice *device) {
  const ArRenderCapabilities *capabilities =
      ArRenderDevice_Capabilities(device);
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  return renderer && ArRenderCapabilities_Has(
      capabilities, kArRenderCapability_Depth |
                        kArRenderCapability_CustomShaders) &&
      EnsureInitialized(renderer);
}

const char *Sim3DDepthPass_LastError(void) {
  const char *error = SDL_GetError();
  return error && error[0] ? error : "required SDL_GPU depth pass unavailable";
}

Sim3DPreparedPipelines Sim3DDepthPass_PreparePipelines(ArRenderDevice *device) {
  Sim3DPreparedPipelines ready = {0};
  ready.depth = Sim3DDepthPass_Require(device);
  if (!ready.depth) return ready;
  ready.linear_models = CreateModelPipeline(kModelPipeline_Linear);
  ready.radial = CreateModelPipeline(kModelPipeline_Radial);
  ready.surfaces = CreateSurfacePipelines();
  ready.spherical_body = CreateBodyPipeline();
  return ready;
}
