/* Real pixel oracle for terrain/prop visibility. Two intersecting surfaces
 * change which is nearer within the same quad, so no face/owner painter sort
 * can satisfy both halves. All permutations must match the independent CPU
 * depth/alpha reference. No ROM or user settings are needed. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "platform/sdl/diorama_depth_pass_sdl.h"
#include "platform/sdl/gpu_texture_upload_layout.h"
#include "support/test_assert.h"

enum { W = 128, H = 64, TW = 4, TH = 4 };
static const int32_t indices[] = {0,1,2,0,2,3};
static const unsigned orders[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
static unsigned renders, checked_pixels;

static SDL_GPUTexture *Texture(SDL_GPUDevice *gpu, unsigned w, unsigned h, bool target) {
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .width = w, .height = h,
      .layer_count_or_depth = 1, .num_levels = 1,
      .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0)};
  SDL_GPUTexture *t = SDL_CreateGPUTexture(gpu, &info); assert(t); return t;
}

static void Upload(SDL_GPUDevice *gpu, SDL_GPUTexture *texture, const uint8_t *pixels) {
  uint32_t size = 0;
  ArSdlTextureUploadLayout layout;
  assert(ArSdlTextureUploadLayout_Append(TW, TH,
      strcmp(SDL_GetGPUDeviceDriver(gpu), "direct3d12") == 0, &size, &layout));
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size};
  SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(gpu, &info); assert(buffer);
  uint8_t *bytes = SDL_MapGPUTransferBuffer(gpu, buffer, false); assert(bytes);
  for (unsigned y = 0; y < TH; ++y) memcpy(bytes + layout.offset + y * layout.row_pitch, pixels + y*TW*4, TW*4);
  SDL_UnmapGPUTransferBuffer(gpu, buffer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); assert(copy);
  const SDL_GPUTextureTransferInfo src = {.transfer_buffer = buffer, .offset = layout.offset,
      .pixels_per_row = layout.row_pitch / 4, .rows_per_layer = TH};
  const SDL_GPUTextureRegion dst = {.texture = texture, .w = TW, .h = TH, .d = 1};
  SDL_UploadToGPUTexture(copy, &src, &dst, false); SDL_EndGPUCopyPass(copy);
  assert(SDL_SubmitGPUCommandBuffer(cmd)); SDL_ReleaseGPUTransferBuffer(gpu, buffer);
}

static void Quad(DioramaDepthVertex v[4], float left, float right, ArRenderColorF color) {
  for (unsigned i = 0; i < 4; ++i) {
    const bool r = i == 1 || i == 2, b = i >= 2;
    v[i] = (DioramaDepthVertex){.clip = {r ? .875f : -.875f, b ? -.75f : .75f, r ? right : left, 1},
        .color = color, .uv = {r, b}};
  }
}

static DioramaDepthDraw Draw(const DioramaDepthVertex *v) {
  return (DioramaDepthDraw){.vertices = v, .indices = indices, .vertex_count = 4, .index_count = 6,
      .depth_mode = kDioramaDepth_Write, .blend = kArRenderBlendMode_Alpha, .textured = true, .nearest = true};
}

/* This reference uses pixel coordinates and the known rectangular fixtures,
 * independently of the GPU vertex transform and upload packing. */
static void Reference(uint8_t *out, const DioramaDepthDraw *d, const uint8_t *const *textures, unsigned count) {
  for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
    float rgb[4] = {0,0,0,0}, depth = 1;
    const float u = (x + .5f - 8) / 112, v = (y + .5f - 8) / 48;
    for (unsigned j = 0; j < count; ++j) {
      if (u <= 0 || u >= 1 || v <= 0 || v >= 1) continue;
      if (d[j].clip_enabled && (x < (unsigned)d[j].clip.x || y < (unsigned)d[j].clip.y ||
          x >= (unsigned)(d[j].clip.x + d[j].clip.w) || y >= (unsigned)(d[j].clip.y + d[j].clip.h))) continue;
      const float z = d[j].vertices[0].clip[2] * (1-u) + d[j].vertices[1].clip[2] * u;
      if (d[j].depth_mode != kDioramaDepth_Disabled &&
          d[j].depth_mode != kDioramaDepth_NativeActor && z > depth) continue;
      const uint8_t *texel = textures[j] + ((unsigned)(v*TH)*TW + (unsigned)(u*TW))*4;
      const ArRenderColorF color = d[j].vertices[0].color;
      float c[] = {color.r, color.g, color.b, color.a};
      for (unsigned k = 0; k < 4; ++k) if (d[j].textured) c[k] *= texel[k]/255.f;
      if (c[3] <= .5f/255) continue;
      if (d[j].depth_mode == kDioramaDepth_Write) depth = z;
      if (d[j].depth_mode == kDioramaDepth_NativeActor) depth = 1;
      const float keep = d[j].blend == kArRenderBlendMode_Add ? 1 : 1-c[3];
      if (d[j].blend == kArRenderBlendMode_Opaque) memcpy(rgb, c, sizeof(rgb));
      else {
        for (unsigned k = 0; k < 3; ++k) rgb[k] = c[k]*c[3] + rgb[k]*keep;
        if (d[j].blend != kArRenderBlendMode_Add) rgb[3] = c[3] + rgb[3]*keep;
      }
      /* RGBA8 is quantized after each blend, just like the GPU target. */
      for (unsigned k = 0; k < 4; ++k) rgb[k] = roundf(fminf(1,rgb[k])*255)/255;
    }
    for (unsigned k = 0; k < 4; ++k) out[(y*W+x)*4+k] = (uint8_t)lroundf(rgb[k]*255);
  }
}

static void Check(ArGpuDioramaDepthPass *pass, SDL_GPUTexture *target,
    const DioramaDepthDraw *draws, SDL_GPUTexture *const *textures,
    const uint8_t *const *pixels, unsigned count, unsigned split, uint8_t *result) {
  SDL_GPUDevice *gpu = pass->device;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
  const unsigned first = split == 3 ? 1 : split;
  if (first && first < count) {
    assert(ArGpuDioramaDepthPass_Encode(pass, cmd, target, W, H, draws, textures, first,
        SDL_GPU_LOADOP_CLEAR, (ArRenderColorF){0}, true));
    /* Continue through a separately uploaded batch; world depth must survive. */
    if (split == 3) {
      assert(SDL_SubmitGPUCommandBuffer(cmd));
      /* Re-preparation of an unchanged target must retain its depth contents. */
      assert(ArGpuDioramaDepthPass_Prepare(pass,W,H,12,18));
      cmd = SDL_AcquireGPUCommandBuffer(gpu); assert(cmd);
    }
    assert(ArGpuDioramaDepthPass_Encode(pass, cmd, target, W, H, draws+first, textures+first, count-first,
        SDL_GPU_LOADOP_LOAD, (ArRenderColorF){0}, false));
  } else assert(ArGpuDioramaDepthPass_Encode(pass, cmd, target, W, H, draws, textures, count,
      SDL_GPU_LOADOP_CLEAR, (ArRenderColorF){0}, true));
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = W*H*4};
  SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu, &info); assert(download);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); assert(copy);
  const SDL_GPUTextureRegion src = {.texture = target, .w = W, .h = H, .d = 1};
  const SDL_GPUTextureTransferInfo dst = {.transfer_buffer = download, .pixels_per_row = W, .rows_per_layer = H};
  SDL_DownloadFromGPUTexture(copy, &src, &dst); SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); assert(fence);
  assert(SDL_WaitForGPUFences(gpu, true, &fence, 1)); SDL_ReleaseGPUFence(gpu, fence);
  const uint8_t *actual = SDL_MapGPUTransferBuffer(gpu, download, false); assert(actual);
  uint8_t expected[W*H*4]; Reference(expected, draws, pixels, count);
  for (unsigned i = 0; i < sizeof(expected); ++i) {
    const int delta = (int)actual[i] - expected[i];
    if (delta < -1 || delta > 1) fprintf(stderr, "render=%u pixel=%u,%u channel=%u actual=%u expected=%u\n",
        renders, i/4%W, i/4/W, i%4, actual[i], expected[i]);
    assert(delta >= -1 && delta <= 1);
  }
  if (result) memcpy(result, actual, sizeof(expected));
  SDL_UnmapGPUTransferBuffer(gpu, download); SDL_ReleaseGPUTransferBuffer(gpu, download);
  ++renders; checked_pixels += W*H;
}

static void Rejections(ArGpuDioramaDepthPass *pass, SDL_GPUTexture *target, DioramaDepthDraw draw,
    SDL_GPUTexture *texture) {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(pass->device); assert(cmd);
  const DioramaDepthDraw good = draw;
  DioramaDepthVertex bad[4]; memcpy(bad, draw.vertices, sizeof(bad));
  const int32_t invalid[] = {0,1,4};
  SDL_GPUBuffer *vertices = pass->vertices; SDL_GPUTexture *depth = pass->depth;
  for (unsigned i = 0; i < 11; ++i) {
    draw = good; SDL_GPUTexture *source = texture;
    if (i == 0) draw.depth_mode = kDioramaDepth_Count;
    if (i == 1) draw.blend = kArRenderBlendMode_Add;
    if (i == 2) source = NULL;
    if (i == 3) source = target;
    if (i == 4) { draw.indices = invalid; draw.index_count = 3; }
    if (i == 5) { draw.vertices = bad; bad[0].clip[2] = NAN; }
    if (i == 6) draw.vertex_count = kDioramaDepthMaximumVertices + 1;
    if (i == 7) draw.index_count = 5;
    if (i == 8) { draw.clip_enabled = true; draw.clip = (ArRenderRectI){0,0,W+1,H}; }
    if (i == 9) { draw.vertices = bad; bad[0] = good.vertices[0]; bad[0].clip[3] = 0; }
    if (i == 10) { draw.vertices = bad; bad[0] = good.vertices[0]; bad[0].uv.x = -.1f; }
    assert(!ArGpuDioramaDepthPass_Encode(pass, cmd, target, W,H, &draw,&source,1,
        SDL_GPU_LOADOP_CLEAR, (ArRenderColorF){0}, true));
    assert(pass->vertices == vertices && pass->depth == depth);
  }
  assert(!ArGpuDioramaDepthPass_Encode(pass, cmd, target, W+1,H, &good,&texture,1,
      SDL_GPU_LOADOP_LOAD, (ArRenderColorF){0}, false));
  assert(SDL_CancelGPUCommandBuffer(cmd));
}

int main(void) {
  setvbuf(stdout,NULL,_IONBF,0);
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL, true, NULL);
  if (!gpu) { SDL_Quit(); return 77; }
  printf("shared action depth backend=%s SDL=%d\n", SDL_GetGPUDeviceDriver(gpu), SDL_GetVersion());
  ArGpuDioramaDepthPass pass; assert(ArGpuDioramaDepthPass_Init(&pass,gpu));
  SDL_GPUTexture *target = Texture(gpu,W,H,true), *textures[3];
  assert(ArGpuDioramaDepthPass_Prepare(&pass,W,H,256,512));
  assert(!pass.depth_initialized);
  for(unsigned m=0;m<3;++m)for(unsigned d=0;d<kDioramaDepth_Count;++d)for(unsigned b=0;b<3;++b)
    assert((pass.pipelines[m][d][b]!=NULL)==!(d==kDioramaDepth_Write && b==2));
  const ArGpuDioramaDepthPass prepared=pass;
  assert(!ArGpuDioramaDepthPass_Prepare(&pass,0,H,256,512));
  assert(!ArGpuDioramaDepthPass_Prepare(&pass,W,16385,256,512));
  assert(!ArGpuDioramaDepthPass_Prepare(&pass,W,H,kDioramaDepthMaximumVertices+1,512));
  assert(!ArGpuDioramaDepthPass_Prepare(&pass,W,H,256,kDioramaDepthMaximumIndices+1));
  assert(!memcmp(&pass,&prepared,sizeof(pass)));
  SDL_GPUCommandBuffer *uncleared=SDL_AcquireGPUCommandBuffer(gpu);assert(uncleared);
  assert(!ArGpuDioramaDepthPass_Encode(&pass,uncleared,target,W,H,NULL,NULL,0,
      SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false));
  assert(SDL_CancelGPUCommandBuffer(uncleared));
  uint8_t art[3][TW*TH*4]; memset(art,255,sizeof(art));
  /* Distinct holes in the prop and terrain; black is a solid material color. */
  for (unsigned y=0;y<TH;++y) {
    art[0][(y*TW+1)*4+3] = 0;
    art[1][(y*TW+2)*4+3] = 0;
    if (y==1 || y==2) for (unsigned k=0;k<3;++k) art[0][(y*TW+3)*4+k] = 0;
  }
  for (unsigned i=0;i<3;++i) { textures[i]=Texture(gpu,TW,TH,false); Upload(gpu,textures[i],art[i]); }
  const uint8_t *pixels[] = {art[0],art[1],art[2]};
  DioramaDepthVertex vertices[3][4];
  DioramaDepthDraw draws[3];
  uint8_t baseline[W*H*4], result[W*H*4];
  for (unsigned angle=0;angle<2;++angle) {
    Quad(vertices[0],angle?.78f:.18f,angle?.18f:.78f,(ArRenderColorF){1,0,0,1});
    Quad(vertices[1],angle?.25f:.73f,angle?.73f:.25f,(ArRenderColorF){0,0,1,1});
    Quad(vertices[2],.9f,.9f,(ArRenderColorF){0,1,0,1});
    for (unsigned i=0;i<3;++i) draws[i]=Draw(vertices[i]);
    for (unsigned order=0;order<6;++order) for (unsigned split=0;split<4;++split) {
      DioramaDepthDraw d[3]; SDL_GPUTexture *t[3]; const uint8_t *a[3];
      for (unsigned i=0;i<3;++i) { unsigned j=orders[order][i]; d[i]=draws[j]; t[i]=textures[j]; a[i]=pixels[j]; }
      Check(&pass,target,d,t,a,3,split,result);
      if (!order && !split) memcpy(baseline,result,sizeof(result));
      else assert(!memcmp(baseline,result,sizeof(result)));
    }
  }
  /* Negative control: with the background correctly drawn first, neither
   * whole-face ordering can reproduce the intersecting surfaces. This guards
   * against accidentally turning the test into a batch-sort regression. */
  for (unsigned front=0;front<2;++front) {
    const unsigned order[]={2,front,1-front};
    DioramaDepthDraw d[3]; SDL_GPUTexture *t[3]; const uint8_t *a[3];
    for (unsigned i=0;i<3;++i) {
      const unsigned j=order[i]; d[i]=draws[j]; d[i].depth_mode=kDioramaDepth_Disabled;
      t[i]=textures[j]; a[i]=pixels[j];
    }
    Check(&pass,target,d,t,a,3,0,result);
    unsigned different=0;
    for (unsigned i=0;i<W*H;++i) different+=memcmp(result+i*4,baseline+i*4,4)!=0;
    assert(different>500);
    printf("painter negative control=%u mismatched-pixels=%u\n",front,different);
  }
  /* Equal depth obeys native submission order, but native priority cannot
   * make a physically farther opaque face cover a nearer face. */
  Quad(vertices[0],.5f,.5f,(ArRenderColorF){1,0,0,1});
  Quad(vertices[1],.5f,.5f,(ArRenderColorF){0,0,1,1});
  draws[0].textured = draws[1].textured = false;
  Check(&pass,target,draws,textures,pixels,2,1,NULL);
  DioramaDepthDraw reverse[]={draws[1],draws[0]};
  Check(&pass,target,reverse,textures,pixels,2,0,NULL);
  Quad(vertices[0],.4f,.4f,(ArRenderColorF){1,0,0,1});
  Check(&pass,target,draws,textures,pixels,2,0,NULL);
  /* Read-only alpha effects never overwrite the world depth. This includes a
   * later translucent surface behind the first effect but ahead of the world. */
  Quad(vertices[1],.1f,.1f,(ArRenderColorF){0,0,1,.5f});
  Quad(vertices[2],.2f,.2f,(ArRenderColorF){0,1,0,.5f});
  draws[1].depth_mode = draws[2].depth_mode = kDioramaDepth_Read;
  draws[2].textured = false;
  Check(&pass,target,draws,textures,pixels,3,2,NULL);
  draws[1].depth_mode = kDioramaDepth_Disabled;
  Quad(vertices[1],.99f,.99f,(ArRenderColorF){0,0,1,1});
  draws[1].clip_enabled=true; draws[1].clip=(ArRenderRectI){37,11,43,31};
  Check(&pass,target,draws,textures,pixels,2,0,NULL);
  draws[1].depth_mode=kDioramaDepth_Read; draws[1].blend=kArRenderBlendMode_Add;
  Quad(vertices[1],.1f,.1f,(ArRenderColorF){0,0,.5f,.5f});
  Check(&pass,target,draws,textures,pixels,2,1,NULL);
  Rejections(&pass,target,Draw(vertices[0]),textures[0]);
  /* Native actors override earlier walking surfaces and establish their draw
   * slot only at covered pixels. Later foreground art may cover those pixels;
   * actor holes keep the previous world depth. Modeled props remain geometric.
   * Disabling depth for actors alone fails the later-foreground requirement. */
  Quad(vertices[0],.2f,.2f,(ArRenderColorF){1,0,0,1});
  Quad(vertices[1],.7f,.7f,(ArRenderColorF){0,0,1,1});
  Quad(vertices[2],.6f,.6f,(ArRenderColorF){0,1,0,1});
  DioramaDepthDraw native[3]={Draw(vertices[0]),Draw(vertices[1]),Draw(vertices[2])};
  native[0].textured=false;native[1].depth_mode=kDioramaDepth_NativeActor;
  Check(&pass,target,native,textures,pixels,3,3,baseline);
  for(unsigned control=0;control<2;++control) {
    native[1].depth_mode=control?kDioramaDepth_Write:kDioramaDepth_Disabled;
    Check(&pass,target,native,textures,pixels,3,3,result);
    unsigned different=0;
    for(unsigned i=0;i<W*H;++i)different+=memcmp(result+i*4,baseline+i*4,4)!=0;
    assert(different>500);
    printf("actor priority negative control=%u mismatched-pixels=%u\n",control,different);
  }
  /* New dimensions cannot reuse stale depth. Shrink then restore between
   * command buffers; buffers are retained while the attachment is recreated. */
  SDL_GPUTexture *small=Texture(gpu,32,16,true);
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
  assert(ArGpuDioramaDepthPass_Encode(&pass,cmd,small,32,16,NULL,NULL,0,
      SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
  assert(SDL_SubmitGPUCommandBuffer(cmd)); SDL_ReleaseGPUTexture(gpu,small);
  Check(&pass,target,draws,textures,pixels,2,0,NULL);
  SDL_GPUBuffer *retained=pass.vertices;
  Check(&pass,target,draws,textures,pixels,2,0,NULL); assert(pass.vertices==retained);
  assert(pass.vertices==prepared.vertices && pass.indices==prepared.indices && pass.upload==prepared.upload);
  assert(!memcmp(pass.pipelines,prepared.pipelines,sizeof(pass.pipelines)));
  assert(SDL_WaitForGPUIdle(gpu));
  ArGpuDioramaDepthPass_Destroy(&pass); assert(!pass.device && !pass.depth);
  for (unsigned i=0;i<3;++i) SDL_ReleaseGPUTexture(gpu,textures[i]);
  SDL_ReleaseGPUTexture(gpu,target); SDL_DestroyGPUDevice(gpu); SDL_Quit();
  printf("shared action depth: %u renders, %u pixels; crossing meshes, all permutations, alpha holes, black interiors, native ties, multi-batch depth, effects, clipping and resize passed\n",renders,checked_pixels);
  return 0;
}
