/* Complete motion -> warp -> source-space scene, with no readback until the
 * finished image. CPU analysis/projection and SDL geometry are the oracle.
 * No ROM, live FrameSlot, window renderer or production setting is required. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/sdl/action_scene_pass_sdl.h"
#include "platform/sdl/gpu_global_motion_sdl.h"
#include "platform/sdl/gpu_texture_upload_layout.h"
#include "present/presentation_frame_generation.h"
#include "render/scene3d_math.h"
#include "support/test_assert.h"

enum { W = 128, H = 64, N = 2, GX = 8, GY = 6, NV = (GX + 1) * (GY + 1), NI = GX * GY * 6 };
static SDL_FPoint reference_edges[6 * NI];
static unsigned reference_edge_points;

static bool OnReferenceEdge(float x, float y) {
  for (unsigned i = 0; i < reference_edge_points; i += 2) {
    SDL_FPoint a = reference_edges[i], b = reference_edges[i+1];
    float dx = b.x-a.x, dy = b.y-a.y, length = hypotf(dx,dy);
    if (length == 0) continue;
    float t = ((x-a.x)*dx + (y-a.y)*dy)/(length*length);
    if (t >= 0 && t <= 1 && fabsf((x-a.x)*dy-(y-a.y)*dx)/length <= .01f) return true;
  }
  return false;
}

static uint32_t Pattern(int x, int y) {
  unsigned a = (unsigned)(x + 1) * 73856093u ^ (unsigned)(y + 1) * 19349663u;
  a ^= a >> 13;
  return 0xff000000u | (a & 0xffffffu);
}

static SDL_GPUTexture *Texture(SDL_GPUDevice *gpu, unsigned w, unsigned h, bool target) {
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .width = w, .height = h,
      .layer_count_or_depth = 1, .num_levels = 1,
      .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0)};
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(gpu, &info);
  assert(texture);
  return texture;
}

static void Grid(DioramaSceneVertex *vertices, int32_t *indices) {
  for (unsigned y = 0; y <= GY; ++y) for (unsigned x = 0; x <= GX; ++x) {
    vertices[y * (GX + 1) + x] = (DioramaSceneVertex){
        {(float)x / GX * W, (float)y / GY * H}, {1,1,1,1}, {(float)x / GX, (float)y / GY}};
  }
  unsigned count = 0;
  for (unsigned y = 0; y < GY; ++y) for (unsigned x = 0; x < GX; ++x) {
    int32_t a = (int32_t)(y * (GX + 1) + x), b = a + GX + 1;
    int32_t cell[] = {a, a+1, b, a+1, b+1, b};
    memcpy(indices + count, cell, sizeof(cell)); count += 6;
  }
}

static ArRenderPointF Offset(const PresentationFrameGenerationMotionField *field, float phase) {
  if (!field->valid) return (ArRenderPointF){0};
  float x = field->backward_dx[0], y = field->backward_dy[0];
  return phase < .5f ? (ArRenderPointF){x + field->forward_dx[0] * phase, y + field->forward_dy[0] * phase}
      : (ArRenderPointF){x * (1 - phase), y * (1 - phase)};
}

static uint32_t Sample(const uint32_t *pixels, float x, float y) {
  if (x < 0 || y < 0 || x >= W || y >= H) return 0;
  x = fmaxf(0, fminf(W - 1, x - .5f)); y = fmaxf(0, fminf(H - 1, y - .5f));
  unsigned x0 = (unsigned)x, y0 = (unsigned)y, x1 = x0 < W-1 ? x0+1 : x0, y1 = y0 < H-1 ? y0+1 : y0;
  float fx = x - x0, fy = y - y0;
  uint32_t result = 0;
  for (unsigned s = 0; s < 32; s += 8) {
    float a = (pixels[y0*W+x0] >> s) & 255, b = (pixels[y0*W+x1] >> s) & 255;
    float c = (pixels[y1*W+x0] >> s) & 255, d = (pixels[y1*W+x1] >> s) & 255;
    result |= (uint32_t)lroundf((a + (b-a)*fx)*(1-fy) + (c + (d-c)*fx)*fy) << s;
  }
  return result;
}

/* Use the validated GPU warp bytes for the projection oracle. Quantization
 * at the intermediate RGBA8 texture is checked independently below. */
static SDL_Texture *ReferenceTexture(SDL_Renderer *renderer, const uint32_t *pixels) {
  SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, W, H);
  assert(t && SDL_UpdateTexture(t, NULL, pixels, W*4));
  assert(SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR));
  return t;
}

static void CheckWarp(const uint32_t *actual, const uint32_t *a, const uint32_t *b,
    const PresentationFrameGenerationMotionField *field, unsigned scene, float phase) {
  unsigned maximum = 0;
  for (unsigned plane = 0; plane < N; ++plane) {
    const bool forward = phase < .5f;
    const float factor = forward ? phase : 1 - phase;
    const float dx = (forward ? field[plane].forward_dx[0] : field[plane].backward_dx[0]) * factor;
    const float dy = (forward ? field[plane].forward_dy[0] : field[plane].backward_dy[0]) * factor;
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
      const unsigned i = (plane * H + y) * W + x;
      const uint32_t expected = field[plane].valid
          ? Sample((forward ? a : b) + plane * W * H, x + .5f - dx, y + .5f - dy) : b[i];
      for (unsigned channel = 0; channel < 4; ++channel) {
        const unsigned shift = channel * 8;
        const unsigned error = (unsigned)abs((int)((actual[i] >> shift) & 255) -
                                            (int)((expected >> shift) & 255));
        if (!field[plane].valid) assert(error == 0); /* Unfiltered fallback. */
        if (error > maximum) maximum = error;
      }
    }
  }
  printf("action-warp case=%u phase=%.2f max=%u\n", scene, phase, maximum);
  /* The CPU rounds ideal bilinear samples, while hardware filtering and
   * storage quantize normalized channels. Windows NVIDIA and Apple Metal
   * differ by up to one byte at this intermediate stage. Integer endpoints
   * must remain exact; no projection or composition error is hidden here. */
  assert(maximum <= (phase == 0 || phase == 1 ? 0u : 1u));
}

static SDL_Surface *Reference(SDL_Renderer *renderer, unsigned width, unsigned height,
    const DioramaSceneDraw *draws, unsigned count, SDL_Texture *const *textures,
    const PresentationFrameGenerationMotionField *motion, float phase) {
  SDL_Texture *output = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
  assert(output && SDL_SetRenderTarget(renderer, output));
  assert(SDL_SetRenderClipRect(renderer, NULL));
  assert(SDL_SetRenderDrawColor(renderer, 0,0,0,255) && SDL_RenderClear(renderer));
  reference_edge_points = 0;
  for (unsigned i = 0; i < count; ++i) {
    const DioramaSceneDraw *d = &draws[i];
    DioramaProjection projection = *d->view;
    ArRenderPointF offset = d->motion_slot < 0 ? (ArRenderPointF){0} : Offset(&motion[d->motion_slot], phase);
    projection.bg1_plane = d->plane;
    projection.bg1_plane.capture_offset.x += offset.x;
    projection.bg1_plane.capture_offset.y += offset.y;
    projection.bg2_skybox.count = 1; projection.bg2_skybox.active_band = 0;
    projection.bg2_skybox.bands[0] = d->skybox;
    projection.bg2_skybox.bands[0].x0 -= offset.x; projection.bg2_skybox.bands[0].x1 -= offset.x;
    projection.bg2_skybox.bands[0].y0 -= offset.y; projection.bg2_skybox.bands[0].y1 -= offset.y;
    SDL_Vertex vertices[NV];
    assert(d->vertex_count <= NV);
    for (unsigned j = 0; j < d->vertex_count; ++j) {
      const DioramaSceneVertex *v = &d->vertices[j];
      ArRenderPointF point;
      assert(d->use_skybox ? Diorama_ProjectCapturedBg2Point(&projection, v->source.x, v->source.y, &point, NULL, NULL)
          : Diorama_ProjectCapturedBg1Point(&projection, v->source.x, v->source.y, &point, NULL, NULL));
      vertices[j] = (SDL_Vertex){{point.x, point.y}, {v->color.r,v->color.g,v->color.b,v->color.a}, {v->uv.x,v->uv.y}};
    }
    for (unsigned j=0; j<d->index_count; j+=3) for (unsigned edge=0; edge<3; ++edge) {
      assert(reference_edge_points + 2 <= sizeof(reference_edges)/sizeof(reference_edges[0]));
      reference_edges[reference_edge_points++] = vertices[d->indices[j+edge]].position;
      reference_edges[reference_edge_points++] = vertices[d->indices[j+(edge+1)%3]].position;
    }
    SDL_BlendMode blend = d->blend == kArRenderBlendMode_Opaque ? SDL_BLENDMODE_NONE :
        d->blend == kArRenderBlendMode_Add ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND;
    assert(SDL_SetRenderDrawBlendMode(renderer, blend));
    if (d->textured) assert(SDL_SetTextureBlendMode(textures[i], blend));
    if (d->clip_enabled) {
      assert(d->use_skybox);
      float sx=projection.output_width/(d->skybox.x1-d->skybox.x0);
      float sy=projection.output_height*(d->skybox.output_y1-d->skybox.output_y0)/(d->skybox.y1-d->skybox.y0);
      float x0=projection.output_x+(d->clip.x-d->skybox.x0)*sx;
      float y0=projection.output_y+projection.output_height*d->skybox.output_y0+(d->clip.y-d->skybox.y0)*sy;
      int x=(int)ceilf(x0-.5f), y=(int)ceilf(y0-.5f);
      SDL_Rect clip={x,y,(int)floorf(x0+d->clip.w*sx-.5f)+1-x,
          (int)floorf(y0+d->clip.h*sy-.5f)+1-y};
      assert(SDL_SetRenderClipRect(renderer,&clip));
    } else assert(SDL_SetRenderClipRect(renderer,NULL));
    assert(SDL_RenderGeometry(renderer, d->textured ? textures[i] : NULL,
        vertices, d->vertex_count, d->indices, d->index_count));
  }
  SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
  assert(raw);
  SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
  assert(rgba); SDL_DestroySurface(raw);
  assert(SDL_SetRenderTarget(renderer, NULL));
  SDL_DestroyTexture(output);
  return rgba;
}

static void Compare(SDL_GPUDevice *gpu, SDL_Renderer *renderer, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *output, SDL_GPUTexture *warped, unsigned w, unsigned h,
    const DioramaSceneDraw *draws, unsigned count, const uint32_t *a, const uint32_t *b,
    const PresentationFrameGenerationMotionField *motion, unsigned label, float phase) {
  const unsigned pitch = (w*4 + 255u) & ~255u;
  const unsigned warp_offset = (pitch*h + 511u) & ~511u;
  const SDL_GPUTransferBufferCreateInfo info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = warp_offset + W*H*N*4,
  };
  SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu, &info); assert(download);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); assert(copy);
  const SDL_GPUTextureRegion src = {.texture=output, .w=w, .h=h, .d=1};
  const SDL_GPUTextureTransferInfo dst = {.transfer_buffer=download, .pixels_per_row=pitch/4, .rows_per_layer=h};
  SDL_DownloadFromGPUTexture(copy, &src, &dst);
  const SDL_GPUTextureRegion warp_src = {.texture=warped, .w=W, .h=H*N, .d=1};
  const SDL_GPUTextureTransferInfo warp_dst = {.transfer_buffer=download, .offset=warp_offset,
      .pixels_per_row=W, .rows_per_layer=H*N};
  SDL_DownloadFromGPUTexture(copy, &warp_src, &warp_dst);
  SDL_EndGPUCopyPass(copy);
  /* Both native passes finish before this single readback/fence. The fixture
   * still exercises GPU-only motion -> warp -> scene resource ordering. */
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); assert(fence);
  assert(SDL_WaitForGPUFences(gpu, true, &fence, 1)); SDL_ReleaseGPUFence(gpu, fence);
  const unsigned char *actual = SDL_MapGPUTransferBuffer(gpu, download, false); assert(actual);
  const uint32_t *warp = (const uint32_t *)(actual + warp_offset);
  CheckWarp(warp, a, b, motion, label, phase);
  SDL_Texture *refs[] = {ReferenceTexture(renderer, warp), ReferenceTexture(renderer, warp + W*H), NULL, NULL};
  SDL_Surface *reference = Reference(renderer, w, h, draws, count, refs, motion, phase);
  assert(SDL_RenderPresent(renderer));
  unsigned max = 0, over = 0, edge_channels = 0, edge_pixels = 0, last_edge_pixel = UINT32_MAX; uint64_t total = 0;
  for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w*4; ++x) {
    int difference = abs(actual[y*pitch+x] - ((const unsigned char *)reference->pixels)[y*reference->pitch+x]);
    if ((unsigned)difference > max) max = (unsigned)difference;
    total += (unsigned)difference; over += difference > 3;
    if (difference > 3 && OnReferenceEdge((float)(x/4)+.5f,(float)y+.5f)) {
      ++edge_channels;
      if (last_edge_pixel != y*w+x/4) { ++edge_pixels; last_edge_pixel=y*w+x/4; }
    }
    else if (difference > 3 && over <= 8)
      fprintf(stderr, "different pixel (%u,%u) channel=%u expected=%u actual=%u\n", x/4,y,x%4,
          ((const unsigned char *)reference->pixels)[y*reference->pitch+x], actual[y*pitch+x]);
  }
  printf("action-scene case=%u phase=%.2f max=%u channels-over-3=%u edge-channels=%u mean=%.6f\n",
      label, phase, max, over, edge_channels, (double)total/(w*h*4));
  /* CPU/GPU float projection can put an exact pixel-centre edge on opposite
   * sides of the rasterizer's subpixel grid. Permit at most four such pixels,
   * only within .01 pixel of an independently projected reference edge.
   * Interior filtering/composition still has a strict 3/255 channel limit. */
  assert(over == edge_channels && edge_pixels <= 4);
  assert((double)total/(w*h*4) < .25);
  SDL_DestroySurface(reference); SDL_DestroyTexture(refs[0]); SDL_DestroyTexture(refs[1]);
  SDL_UnmapGPUTransferBuffer(gpu, download); SDL_ReleaseGPUTransferBuffer(gpu, download);
}

int main(void) {
  /* Preserve the failing case's pixel diagnostics when assert aborts. */
  setvbuf(stdout, NULL, _IONBF, 0);
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
  if (!gpu) { SDL_Quit(); return 77; }
  printf("action-scene backend=%s SDL=%d\n", SDL_GetGPUDeviceDriver(gpu), SDL_GetVersion());
  SDL_Renderer *renderer = SDL_CreateGPURenderer(gpu, NULL); assert(renderer);
  ArGpuActionScenePass pass; assert(ArGpuActionScenePass_Init(&pass, gpu));
  ArGpuGlobalMotion motion; assert(ArGpuGlobalMotion_Init(&motion, gpu));
  motion.plane_count=N; assert(ArGpuGlobalMotion_Resize(&motion, W, H));
  SDL_GPUTexture *previous=Texture(gpu,W,H*N,false), *current=Texture(gpu,W,H*N,false);
  const SDL_GPUTransferBufferCreateInfo ui={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=W*H*N*4*2};
  SDL_GPUTransferBuffer *upload=SDL_CreateGPUTransferBuffer(gpu,&ui); assert(upload);
  DioramaSceneVertex grid[NV]; int32_t indices[NI]; Grid(grid,indices);
  const DioramaSceneVertex rays[] = {{{36,9},{.2f,.4f,.9f,.4f},{0}},
    {{60,59},{.1f,.3f,.8f,.1f},{0}}, {{94,59},{.1f,.2f,.7f,.3f},{0}}};
  const DioramaSceneVertex mote[] = {{{65,28},{1,.6f,.3f,.7f},{0}},
    {{68,33},{.4f,.2f,.1f,.2f},{0}}, {{71,28},{1,.6f,.3f,.7f},{0}}};
  const int32_t triangle[]={0,1,2};
  const float phases[]={0,.25f,.5f,.75f,1};
  SDL_GPUBuffer *retained_vertices=NULL, *retained_indices=NULL;
  for (unsigned scene=0; scene<7; ++scene) {
    uint32_t a[W*H*N], b[W*H*N];
    PresentationFrameGenerationMotionField field[N];
    for (unsigned band=0; band<N; ++band) {
      int dx=band ? -3 : 2, dy=band ? 1 : -2;
      for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
        a[(band*H+y)*W+x]=Pattern(x,y+band*H);
        b[(band*H+y)*W+x]=(x-dx>=0 && x-dx<W && y-dy>=0 && y-dy<H) ? Pattern(x-dx,y-dy+band*H) : 0;
        if (scene==5) { a[(band*H+y)*W+x]=0xff335577; b[(band*H+y)*W+x]=0xff7799aa; }
      }
      bool valid=PresentationFrameGeneration_Analyze(a+band*W*H,b+band*W*H,W,H,W,W,
          kPresentationFrameGenerationAnalysis_Global,&field[band]);
      assert(valid == (scene!=5));
    }
    void *mapped=SDL_MapGPUTransferBuffer(gpu,upload,true); assert(mapped);
    memcpy(mapped,a,sizeof(a)); memcpy((unsigned char *)mapped+sizeof(a),b,sizeof(b)); SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd); assert(copy);
    for (unsigned i=0;i<2;++i) {
      const SDL_GPUTextureTransferInfo from={.transfer_buffer=upload,.offset=i*sizeof(a),.pixels_per_row=W,.rows_per_layer=H*N};
      const SDL_GPUTextureRegion to={.texture=i?current:previous,.w=W,.h=H*N,.d=1};
      SDL_UploadToGPUTexture(copy,&from,&to,true);
    }
    SDL_EndGPUCopyPass(copy);
    assert(ArGpuGlobalMotion_Analyze(&motion,cmd,previous,current));
    assert(SDL_SubmitGPUCommandBuffer(cmd)); // No wait or metadata download.
    unsigned width=scene%2 ? 512 : 320, height=scene%3 ? 256 : 384;
    SDL_GPUTexture *output=Texture(gpu,width,height,true);
    DioramaProjection view={.valid=true,.aspect_x=2,.height_scale=1,.texture_width=W,.texture_height=H,
      .output_x=9,.output_y=7,.output_width=(int)width-18,.output_height=(int)height-14};
    const Scene3DCamera camera={.tilt_x=.13f*scene,.tilt_y=-.04f*scene,.distance=1.6f+.05f*scene,.fov_y=.9f};
    Scene3D_BuildViewProjection(&camera,view.output_width,view.output_height,view.matrix);
    const DioramaPlaneProjection plane={.valid=true,.u0=0,.v0=0,.u1=1,.v1=1,
      .z_world=.1f,.rake=scene?.2f:0,.bow=scene?-.1f:0,.world_y_offset=.01f*scene,
      .overflow_valid=scene==4,.overflow_fold_t=.6f,.overflow_height=.4f,.overflow_overlap_t=.2f,
      .overflow_handoff_z=.3f,.overflow_front_z=.6f,.overflow_front_drop=.1f};
    DioramaSceneDraw draws[]={
      {.vertices=grid,.indices=indices,.vertex_count=NV,.index_count=NI,.view=&view,
        .use_skybox=true,.skybox={0,0,W,H,0,1},.textured=true,.motion_slot=-1,.blend=kArRenderBlendMode_Opaque},
      {.vertices=grid,.indices=indices,.vertex_count=NV,.index_count=NI,.view=&view,
        .plane=plane,.textured=true,.motion_slot=-1,.blend=kArRenderBlendMode_Alpha},
      {.vertices=rays,.indices=triangle,.vertex_count=3,.index_count=3,.view=&view,
        .plane=plane,.motion_slot=1,.blend=kArRenderBlendMode_Add},
      {.vertices=mote,.indices=triangle,.vertex_count=3,.index_count=3,.view=&view,
        .use_skybox=true,.skybox={0,0,W,H,0,1},.motion_slot=0,.blend=kArRenderBlendMode_Alpha}};
    if (scene==3) {
      view.texture_width=W+16; view.texture_x_origin=8; view.height_scale=352.0f/224.0f;
      for (unsigned i=1; i<=2; ++i) {
        draws[i].plane.u0=8.0f/(W+16); draws[i].plane.u1=(W+8.0f)/(W+16);
      }
      draws[0].skybox=draws[3].skybox=(DioramaSkyboxBandProjection){-12,7,W+14,H-9,.15f,.9f};
    }
    if (scene==6) { draws[3].clip_enabled=true; draws[3].clip=(ArRenderRectF){68,29,2.5f,8}; }
    ArGpuActionSceneTexture textures[4];
    for (unsigned i=0;i<4;++i) textures[i]=(ArGpuActionSceneTexture){motion.output,W,H*N,{0,i==1?H:0,W,H}};
    for (unsigned step=0;step<5;++step) {
      float phase=phases[step];
      cmd=SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
      assert(ArGpuGlobalMotion_Warp(&motion,cmd,previous,current,phase));
      assert(ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,phase,draws,textures,4,
          SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
      if (retained_vertices) assert(pass.vertices==retained_vertices && pass.indices==retained_indices);
      retained_vertices=pass.vertices; retained_indices=pass.indices;
      Compare(gpu,renderer,cmd,output,motion.output,width,height,draws,4,a,b,field,scene,phase);
    }
    // Fail before recording a partial pass. Reject invalid geometry/mapping.
    cmd=SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
    draws[0].motion_slot=8;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    draws[0].motion_slot=-1; draws[2].plane.capture_offset.x=1;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    draws[2].plane.capture_offset.x=0; draws[1].plane.u1=NAN;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    draws[1].plane=plane;
    draws[0].vertex_count=kDioramaSceneMaximumVertices+1;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    draws[0].vertex_count=NV;
    const int32_t invalid_indices[]={0,1,-1};
    draws[0].indices=invalid_indices; draws[0].index_count=3;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    draws[0].indices=indices; draws[0].index_count=NI; textures[0].texture=output;
    assert(!ArGpuActionScenePass_Encode(&pass,cmd,output,width,height,motion.motion,.5f,draws,textures,4,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0,0,0,1}));
    SDL_CancelGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTexture(gpu,output);
  }
  assert(SDL_WaitForGPUIdle(gpu));
  SDL_ReleaseGPUTexture(gpu,previous); SDL_ReleaseGPUTexture(gpu,current);
  SDL_ReleaseGPUTransferBuffer(gpu,upload);
  ArGpuGlobalMotion_Destroy(&motion); ArGpuActionScenePass_Destroy(&pass);
  SDL_DestroyRenderer(renderer); SDL_DestroyGPUDevice(gpu); SDL_Quit();
  return 0;
}
