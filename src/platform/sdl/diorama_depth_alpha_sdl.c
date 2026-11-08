#include "diorama_depth_alpha_sdl.h"
#include "diorama_depth_alpha_util.h"
#include "gpu_shader_blob.h"
#include "diorama/diorama_scenery_uniforms.h"
#include "shaders/diorama_alpha_vert.h"
#include "shaders/diorama_alpha_frag.h"
#include "shaders/scenery_alpha_frag.h"
#include "shaders/actor_alpha_frag.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Pipelines: 0 opaque writers (color + order MRT, depth), 1 alpha tail, 2 additive tail. */
enum { kAlphaPipelineCount = 3 };
typedef struct AlphaVertex { DioramaDepthVertex vertex; float ordinal; } AlphaVertex;
_Static_assert(sizeof(AlphaVertex)==52 && offsetof(AlphaVertex,ordinal)==48,
    "fractional world vertex layout");
typedef struct ArGpuDioramaDepthAlpha {
  SDL_GPUGraphicsPipeline *pipeline[3][kAlphaPipelineCount];
  SDL_GPUTexture *opaque_order;
  SDL_GPUBuffer *vertices;
  SDL_GPUTransferBuffer *upload;
  unsigned width, height, capacity;
  SDL_GPUTextureFormat order_format;
} ArGpuDioramaDepthAlpha;

#define BLOBS(n) (GpuShaderBlobs){k##n##MSL,k##n##MSLSize,k##n##SPV,k##n##SPVSize,k##n##DXIL,k##n##DXILSize}

void ArGpuDioramaDepthAlpha_Destroy(ArGpuDioramaDepthPass *p) {
  ArGpuDioramaDepthAlpha *a=p?p->alpha:NULL;
  if(!a)return;
  for(unsigned m=0;m<3;++m)for(unsigned k=0;k<kAlphaPipelineCount;++k)
    if(a->pipeline[m][k])SDL_ReleaseGPUGraphicsPipeline(p->device,a->pipeline[m][k]);
  if(a->opaque_order)SDL_ReleaseGPUTexture(p->device,a->opaque_order);
  if(a->vertices)SDL_ReleaseGPUBuffer(p->device,a->vertices);
  if(a->upload)SDL_ReleaseGPUTransferBuffer(p->device,a->upload);
  free(a);p->alpha=NULL;
}

static SDL_GPUGraphicsPipeline *Pipeline(ArGpuDioramaDepthPass *p,
    SDL_GPUShader *vs,SDL_GPUShader *fs,unsigned kind) {
  const bool tail=kind!=0;
  const SDL_GPUVertexBufferDescription buffer={.slot=0,.pitch=sizeof(AlphaVertex),
    .input_rate=SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attributes[]={
    {0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,0},
    {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,16},
    {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,32},
    {3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,48},
    {4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,40}};
  SDL_GPUColorTargetDescription colors[2]={{.format=p->color_format},{.format=p->alpha->order_format}};
  if(tail)colors[0].blend_state=(SDL_GPUColorTargetBlendState){.enable_blend=true,
    .src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA,
    .dst_color_blendfactor=kind==2?SDL_GPU_BLENDFACTOR_ONE:SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    .color_blend_op=SDL_GPU_BLENDOP_ADD,
    .src_alpha_blendfactor=kind==2?SDL_GPU_BLENDFACTOR_ZERO:SDL_GPU_BLENDFACTOR_ONE,
    .dst_alpha_blendfactor=kind==2?SDL_GPU_BLENDFACTOR_ONE:SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    .alpha_blend_op=SDL_GPU_BLENDOP_ADD};
  const SDL_GPUGraphicsPipelineCreateInfo info={.vertex_shader=vs,.fragment_shader=fs,
    .vertex_input_state=(SDL_GPUVertexInputState){&buffer,1,attributes,5},
    .primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    .rasterizer_state={.fill_mode=SDL_GPU_FILLMODE_FILL,.cull_mode=SDL_GPU_CULLMODE_NONE,
      .front_face=SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,.enable_depth_clip=true},
    .multisample_state={.sample_count=SDL_GPU_SAMPLECOUNT_1},
    .depth_stencil_state={.compare_op=SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
      .enable_depth_test=!tail,.enable_depth_write=!tail},
    .target_info={.color_target_descriptions=colors,.num_color_targets=tail?1:2,
      .has_depth_stencil_target=!tail,
      .depth_stencil_format=tail?SDL_GPU_TEXTUREFORMAT_INVALID:SDL_GPU_TEXTUREFORMAT_D32_FLOAT}};
  return GpuShaderBlob_CreateGraphicsPipeline(p->device,&info,"action fractional world");
}

static bool Init(ArGpuDioramaDepthPass *p) {
  if(p->alpha)return true;
  p->alpha=calloc(1,sizeof(*p->alpha));
  if(!p->alpha)return false;
  const SDL_GPUTextureUsageFlags usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
  bool supports_order=SDL_GPUTextureSupportsFormat(p->device,SDL_GPU_TEXTUREFORMAT_R32_FLOAT,SDL_GPU_TEXTURETYPE_2D,usage);
#ifdef AR_DIORAMA_ALPHA_TEST_FORCE_RGBA32
  /* Defined only by the independent GPU parity test translation unit. */
  supports_order=false;
#endif
  p->alpha->order_format=DioramaDepthAlpha_SelectOrderFormat(supports_order);
  const GpuShaderBlobs vertex=BLOBS(DioramaAlphaVert);
  const GpuShaderBlobs fragment[]={BLOBS(DioramaAlphaFrag),BLOBS(SceneryAlphaFrag),BLOBS(ActorAlphaFrag)};
  SDL_GPUShader *vs=GpuShaderBlob_Create(p->device,&vertex,"action fractional world",SDL_GPU_SHADERSTAGE_VERTEX,0,0);
  bool ok=vs!=NULL;
  for(unsigned m=0;ok&&m<3;++m) {
    SDL_GPUShader *fs=GpuShaderBlob_CreateFragment(p->device,&fragment[m],"action fractional material",
        m==2?6:m==1?4:3,m?2:1);
    ok=fs!=NULL;
    for(unsigned k=0;ok&&k<kAlphaPipelineCount;++k) {
      p->alpha->pipeline[m][k]=Pipeline(p,vs,fs,k);
      ok=p->alpha->pipeline[m][k]!=NULL;
    }
    if(fs)SDL_ReleaseGPUShader(p->device,fs);
  }
  if(vs)SDL_ReleaseGPUShader(p->device,vs);
  if(ok)return true;
  ArGpuDioramaDepthAlpha_Destroy(p);return false;
}

static SDL_GPUTexture *Texture(ArGpuDioramaDepthPass *p,unsigned width,unsigned height,
    SDL_GPUTextureFormat format) {
  const SDL_GPUTextureCreateInfo info={.type=SDL_GPU_TEXTURETYPE_2D,.format=format,
    .width=width,.height=height,.layer_count_or_depth=1,.num_levels=1,
    .usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER};
  return SDL_CreateGPUTexture(p->device,&info);
}

bool ArGpuDioramaDepthPass_PrepareHybrid(ArGpuDioramaDepthPass *p,unsigned width,unsigned height,
    unsigned vertices,unsigned indices) {
  if(!ArGpuDioramaDepthPass_Prepare(p,width,height,vertices,indices)||!Init(p))return false;
  ArGpuDioramaDepthAlpha *a=p->alpha;
  if(a->width!=width||a->height!=height) {
    SDL_GPUTexture *order=Texture(p,width,height,a->order_format);
    if(!order)return false;
    if(a->opaque_order)SDL_ReleaseGPUTexture(p->device,a->opaque_order);
    a->opaque_order=order;
    a->width=width;a->height=height;
  }
  if(indices>a->capacity) {
    unsigned capacity=256;
    while(capacity<indices&&capacity<kDioramaDepthMaximumIndices)capacity*=2;
    if(capacity>kDioramaDepthMaximumIndices)capacity=kDioramaDepthMaximumIndices;
    const SDL_GPUBufferCreateInfo buffer={.usage=SDL_GPU_BUFFERUSAGE_VERTEX,.size=capacity*sizeof(AlphaVertex)};
    const SDL_GPUTransferBufferCreateInfo transfer={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=buffer.size};
    SDL_GPUBuffer *v=SDL_CreateGPUBuffer(p->device,&buffer);
    SDL_GPUTransferBuffer *t=SDL_CreateGPUTransferBuffer(p->device,&transfer);
    if(!v||!t) {
      if(v)SDL_ReleaseGPUBuffer(p->device,v);
      if(t)SDL_ReleaseGPUTransferBuffer(p->device,t);
      return false;
    }
    if(a->vertices)SDL_ReleaseGPUBuffer(p->device,a->vertices);
    if(a->upload)SDL_ReleaseGPUTransferBuffer(p->device,a->upload);
    a->vertices=v;a->upload=t;a->capacity=capacity;
  }
  return true;
}

static bool Finite(const void *data,size_t size) {
  for(size_t i=0;i<size;i+=sizeof(float)) {
    float f;memcpy(&f,(const char *)data+i,sizeof(f));if(!isfinite(f))return false;
  }
  return true;
}

static bool Validate(ArGpuDioramaDepthPass *p,SDL_GPUTexture *target,unsigned width,unsigned height,
    const DioramaDepthDraw *draws,SDL_GPUTexture *const *sources,const ArGpuDioramaDepthMaterial *materials,
    unsigned count,unsigned *nv,unsigned *ni) {
  *nv=*ni=0;
  if(!p||!p->device||!target||target==p->depth||!width||!height||width>16384||height>16384||
      count>kDioramaDepthMaximumDraws||(count&&(!draws||!sources)))return false;
  for(unsigned i=0;i<count;++i) {
    const DioramaDepthDraw *d=&draws[i];
    if(!d->vertices||!d->indices||!d->vertex_count||!d->index_count||d->index_count%3||
        d->vertex_count>kDioramaDepthMaximumVertices-*nv||d->index_count>kDioramaDepthMaximumIndices-*ni||
        (unsigned)d->depth_mode>=kDioramaDepth_Count||
        (d->blend!=kArRenderBlendMode_Opaque&&d->blend!=kArRenderBlendMode_Alpha&&d->blend!=kArRenderBlendMode_Add)||
        (d->depth_mode==kDioramaDepth_Write&&d->blend==kArRenderBlendMode_Add)||
        (d->textured&&!sources[i])||sources[i]==target||(sources[i]&&sources[i]==p->depth)||
        (d->clip_enabled&&(d->clip.x<0||d->clip.y<0||d->clip.w<=0||d->clip.h<=0||
          (int64_t)d->clip.x+d->clip.w>width||(int64_t)d->clip.y+d->clip.h>height)))return false;
    const bool material=materials&&materials[i].scenery;
    if(material) {
      const ArGpuDioramaDepthMaterial *m=&materials[i];
      if(!d->textured||m->scenery->mode>2||(!m->actor&&ArRenderTexture_IsValid(m->scenery->attributes))||
          (!m->actor&&m->scenery->mode==2&&(m->scenery->priority.width<=0||m->scenery->priority.height<=0))||
          m->illumination==target||m->attributes==target||m->enemy_illumination==target||
          (m->illumination&&m->illumination==p->depth)||(m->attributes&&m->attributes==p->depth)||
          (m->enemy_illumination&&m->enemy_illumination==p->depth))return false;
      if(m->actor) {
        DioramaActorUniforms u;DioramaActorUniforms_Init(&u,m->scenery,width,height);
        if(!Finite(&u,sizeof(u)))return false;
      } else {
        DioramaSceneryUniforms u;DioramaSceneryUniforms_Init(&u,m->scenery,width,height);
        if(m->attributes||m->enemy_illumination||!Finite(&u,sizeof(u)))return false;
      }
    } else if(materials&&(materials[i].illumination||materials[i].attributes||
        materials[i].enemy_illumination||materials[i].actor))return false;
    for(unsigned j=0;j<d->vertex_count;++j)
      if(!Finite(&d->vertices[j],sizeof(d->vertices[j]))||d->vertices[j].clip[3]<=0||
          (d->textured&&!material&&d->vertices[j].uv.x<0))return false;
    for(unsigned j=0;j<d->index_count;++j)
      if(d->indices[j]<0||(unsigned)d->indices[j]>=d->vertex_count)return false;
    *nv+=d->vertex_count;*ni+=d->index_count;
  }
  return true;
}

static void DrawRange(ArGpuDioramaDepthPass *p,SDL_GPUCommandBuffer *cmd,SDL_GPURenderPass *pass,
    const DioramaDepthDraw *draws,SDL_GPUTexture *const *sources,const ArGpuDioramaDepthMaterial *materials,
    const unsigned *offset,unsigned first,unsigned last,unsigned mode) {
  ArGpuDioramaDepthAlpha *a=p->alpha;
  const SDL_GPUBufferBinding vertices={.buffer=a->vertices};
  SDL_BindGPUVertexBuffers(pass,0,&vertices,1);
  for(unsigned i=first;i<last;++i) {
    const DioramaDepthDraw *d=&draws[i];
    const unsigned material=materials&&materials[i].scenery?(materials[i].actor?2:1):0;
    const unsigned blend=d->blend==kArRenderBlendMode_Add?2:d->blend==kArRenderBlendMode_Alpha?1:0;
    const unsigned pipeline=mode?(blend==2?2:1):0;
    SDL_BindGPUGraphicsPipeline(pass,a->pipeline[material][pipeline]);
    SDL_GPUTextureSamplerBinding samples[6]={{d->textured?sources[i]:p->white,d->nearest?p->nearest_sampler:p->sampler}};
    const unsigned n=material==2?4:material==1?2:1;
    if(material==2) {
      DioramaActorUniforms u;DioramaActorUniforms_Init(&u,materials[i].scenery,a->width,a->height);
      SDL_PushGPUFragmentUniformData(cmd,0,&u,sizeof(u));
      samples[1]=(SDL_GPUTextureSamplerBinding){materials[i].attributes?materials[i].attributes:p->neutral_attributes,p->sampler};
      samples[2]=(SDL_GPUTextureSamplerBinding){materials[i].illumination?materials[i].illumination:p->black,p->sampler};
      samples[3]=(SDL_GPUTextureSamplerBinding){materials[i].enemy_illumination?materials[i].enemy_illumination:p->black,p->sampler};
    } else if(material==1) {
      DioramaSceneryUniforms u;DioramaSceneryUniforms_Init(&u,materials[i].scenery,a->width,a->height);
      SDL_PushGPUFragmentUniformData(cmd,0,&u,sizeof(u));
      samples[1]=(SDL_GPUTextureSamplerBinding){materials[i].illumination?materials[i].illumination:p->black,p->sampler};
    }
    samples[n]=(SDL_GPUTextureSamplerBinding){mode?p->depth:p->black,p->nearest_sampler};
    samples[n+1]=(SDL_GPUTextureSamplerBinding){mode?a->opaque_order:p->black,p->nearest_sampler};
    SDL_BindGPUFragmentSamplers(pass,0,samples,n+2);
    const float options[]={(float)mode,0,(float)blend,(float)(d->depth_mode==kDioramaDepth_Write)};
    SDL_PushGPUFragmentUniformData(cmd,material?1:0,options,sizeof(options));
    const SDL_Rect clip=d->clip_enabled?(SDL_Rect){d->clip.x,d->clip.y,d->clip.w,d->clip.h}:
      (SDL_Rect){0,0,(int)a->width,(int)a->height};
    SDL_SetGPUScissor(pass,&clip);
    SDL_DrawGPUPrimitives(pass,d->index_count,1,offset[i],0);
  }
}

static bool EncodeBand(ArGpuDioramaDepthPass *p,SDL_GPUCommandBuffer *cmd,SDL_GPUTexture *target,
    const DioramaDepthDraw *draws,SDL_GPUTexture *const *sources,const ArGpuDioramaDepthMaterial *materials,
    const unsigned *offset,unsigned first,unsigned last) {
  ArGpuDioramaDepthAlpha *a=p->alpha;
  const SDL_GPUColorTargetInfo color={.texture=target,.load_op=SDL_GPU_LOADOP_LOAD,.store_op=SDL_GPU_STOREOP_STORE};
  const SDL_GPUDepthStencilTargetInfo opaque={.texture=p->depth,.load_op=SDL_GPU_LOADOP_LOAD,
    .store_op=SDL_GPU_STOREOP_STORE,.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE,.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE};
  const SDL_GPUColorTargetInfo opaque_colors[]={color,
    {.texture=a->opaque_order,.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE,.clear_color={-1,0,0,0}}};
  SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(cmd,opaque_colors,2,&opaque);
  if(!pass)return false;
  DrawRange(p,cmd,pass,draws,sources,materials,offset,first,last,0);
  SDL_EndGPURenderPass(pass);
  /* Everything the opaque pass rejected: coverage edges, fades and additive
   * light, blended in band draw order where it lies in front of opaque depth. */
  pass=SDL_BeginGPURenderPass(cmd,&color,1,NULL);
  if(!pass)return false;
  DrawRange(p,cmd,pass,draws,sources,materials,offset,first,last,1);
  SDL_EndGPURenderPass(pass);return true;
}

bool ArGpuDioramaDepthPass_EncodeHybridMaterials(ArGpuDioramaDepthPass *p,SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *target,unsigned width,unsigned height,const DioramaDepthDraw *draws,
    SDL_GPUTexture *const *sources,const ArGpuDioramaDepthMaterial *materials,unsigned count,
    SDL_GPULoadOp color_load,ArRenderColorF clear_color,bool clear_depth) {
  unsigned nv,ni;
  if(!cmd||!Validate(p,target,width,height,draws,sources,materials,count,&nv,&ni)||
      (color_load!=SDL_GPU_LOADOP_CLEAR&&color_load!=SDL_GPU_LOADOP_LOAD)||!Finite(&clear_color,sizeof(clear_color))||
      (!clear_depth&&(!p->depth_initialized||p->width!=width||p->height!=height))||
      !ArGpuDioramaDepthPass_PrepareHybrid(p,width,height,nv,ni))return false;
  unsigned offsets[kDioramaDepthMaximumDraws];
  ArGpuDioramaDepthAlpha *a=p->alpha;
  unsigned retained_indices=0;
  for(unsigned i=0;i<count;++i) {
    offsets[i]=retained_indices;
    if(draws[i].depth_mode==kDioramaDepth_Write||draws[i].depth_mode==kDioramaDepth_Read)
      retained_indices+=draws[i].index_count;
  }
  /* Native/disabled bands upload through EncodeMaterials below. Keep only
   * world geometry here, while preserving its original triangle ordinals. */
  if(retained_indices) {
    AlphaVertex *mapped=SDL_MapGPUTransferBuffer(p->device,a->upload,true);
    if(!mapped)return false;
    unsigned logical_index=0;
    for(unsigned i=0;i<count;++i) {
      if(draws[i].depth_mode==kDioramaDepth_Write||draws[i].depth_mode==kDioramaDepth_Read)
        for(unsigned j=0;j<draws[i].index_count;++j) {
          AlphaVertex *vertex=&mapped[offsets[i]+j];
          *vertex=(AlphaVertex){.vertex=draws[i].vertices[draws[i].indices[j]],
            .ordinal=(float)((logical_index+j)/3)};
          if(!draws[i].textured)vertex->vertex.uv.x=-1;
        }
      logical_index+=draws[i].index_count;
    }
    SDL_UnmapGPUTransferBuffer(p->device,a->upload);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    if(!copy)return false;
    const SDL_GPUTransferBufferLocation source={.transfer_buffer=a->upload};
    const SDL_GPUBufferRegion destination={.buffer=a->vertices,.size=retained_indices*sizeof(AlphaVertex)};
    SDL_UploadToGPUBuffer(copy,&source,&destination,true);SDL_EndGPUCopyPass(copy);
  }
  /* Prepare/Validate above already guarantees initialized matching depth for
   * a LOAD/no-clear call. With no draws this pass would only load/store the
   * unchanged color and depth targets; all actual bands retain their passes. */
  if(DioramaDepthAlpha_NeedsInitialPass(color_load,clear_depth)&&
      !ArGpuDioramaDepthPass_EncodeMaterials(p,cmd,target,width,height,NULL,NULL,NULL,0,
          color_load,clear_color,clear_depth))return false;
  for(unsigned first=0;first<count;) {
    const DioramaDepthMode mode=draws[first].depth_mode;
    if(mode==kDioramaDepth_NativeActor||mode==kDioramaDepth_Disabled) {
      unsigned last=first+1;
      while(last<count&&draws[last].depth_mode==mode)++last;
      if(!ArGpuDioramaDepthPass_EncodeMaterials(p,cmd,target,width,height,draws+first,sources+first,
          materials?materials+first:NULL,last-first,SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false))return false;
      first=last;
    } else {
      unsigned last=first+1;
      while(last<count&&draws[last].depth_mode!=kDioramaDepth_NativeActor&&draws[last].depth_mode!=kDioramaDepth_Disabled)++last;
      if(!EncodeBand(p,cmd,target,draws,sources,materials,offsets,first,last))return false;
      first=last;
    }
  }
  return true;
}
