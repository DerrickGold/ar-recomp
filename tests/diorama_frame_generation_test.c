#include <math.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_gpu_policy.h"
#include "platform/sdl/action_effect_source_sdl.h"
#include "platform/sdl/render_sdl_internal.h"
#include "present/present.h"

static int failures;
#define CHECK(expression) do {                                           \
  if (!(expression)) {                                                   \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__,             \
            #expression, SDL_GetError());                                \
    failures++;                                                          \
  }                                                                      \
} while (0)

enum {
  kDisplayWidth = 32,
  kApron = 4,
  kSurfaceWidth = kDisplayWidth + kApron * 2,
  kHeight = 16,
  kSkipNoGpuRenderer = 77,
};

static bool RequireProductionGpuRenderer(void) {
  const char *value = getenv("AR_REQUIRE_GPU_RENDERER");
  return value && value[0] && strcmp(value, "0") != 0;
}

static uint32_t PatternPixel(int x, int y) {
  uint32_t hash = (uint32_t)(x + 1) * 73856093u ^
      (uint32_t)(y + 1) * 19349663u;
  hash ^= hash >> 13;
  return 0xff000000u | (hash & 0x00ffffffu);
}

static void FillFrame(uint32_t *pixels, int shift, bool change_marker) {
  for (int y = 0; y < kHeight; y++) {
    uint32_t *row = &pixels[y * kSurfaceWidth];
    for (int x = 0; x < kSurfaceWidth; x++) {
      if (x < kApron || x >= kApron + kDisplayWidth) {
        row[x] = 0xffff00ffu;
        continue;
      }
      const int source_x = x - kApron - shift;
      row[x] = source_x >= 0 && source_x < kDisplayWidth
          ? PatternPixel(source_x, y) : 0;
    }
  }
  for (int y = 7; y <= 9; y++) {
    for (int x = 8; x <= 11; x++) {
      const int shifted_x = x + (change_marker ? 2 : 0);
      pixels[y * kSurfaceWidth + kApron + shifted_x] =
          change_marker ? 0xff0000ffu : 0xffff0000u;
    }
  }
  }

/* Exercise the renderer implementation used by release builds. A GPU-less
 * test host falls back to SDL's default renderer so the ROM-free suite still
 * covers state restoration and synthesis math instead of becoming a skip. */
static SDL_Renderer *CreateTestRenderer(SDL_Window *window) {
  const char *override = getenv("AR_TEST_RENDERER");
  if (override && override[0])
    return SDL_CreateRenderer(window, override);
  SDL_Renderer *renderer = NULL;
  SDL_PropertiesID properties = SDL_CreateProperties();
  if (properties) {
    SDL_SetStringProperty(properties,
        SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetPointerProperty(properties,
        SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(properties,
        SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
    renderer = SDL_CreateRendererWithProperties(properties);
    SDL_DestroyProperties(properties);
  }
  if (!renderer) {
    if (RequireProductionGpuRenderer())
      return NULL;
    fprintf(stderr, "GPU renderer unavailable; testing SDL fallback: %s\n",
            SDL_GetError());
    renderer = SDL_CreateRenderer(window, NULL);
  }
  return renderer;
}

/* Download only in this oracle: live source projection never reads vertices
 * or motion back. Exercise particle sizing, leaf geometry and rejected motion
 * at all interpolation phases against independently computed screen points. */
static void TestSourcePrimitives(ArRenderDevice *device, SDL_GPUDevice *gpu, SDL_Renderer *renderer) {
  SDL_Texture *previous_target = SDL_GetRenderTarget(renderer);
  ArGpuEffectSource effect = {0};
  CHECK(ArGpuEffectSource_Init(&effect, gpu));
  if (!effect.device) return;
  SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, 64, 64);
  CHECK(target && SDL_SetRenderTarget(renderer, target));
  const SDL_GPUBufferCreateInfo bi = {.size = 256,
      .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
  SDL_GPUBuffer *motion = SDL_CreateGPUBuffer(gpu, &bi);
  const SDL_GPUTransferBufferCreateInfo ui = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 256};
  const SDL_GPUTransferBufferCreateInfo di = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 90 * 32};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &ui),
      *download = SDL_CreateGPUTransferBuffer(gpu, &di);
  CHECK(motion && upload && download);
  const DioramaProjection view = {.valid = true, .output_width = 64, .output_height = 64,
      .texture_width = 32, .texture_height = 16,
      .bg2_skybox = {.count = 1, .bands = {{0,0,32,16,0,1}}}};
  ActionEffectSourcePrimitive primitives[6] = {
    {.meta={kActionSourceTriangle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{0,0},{3,0},{0,3}}, .colors={{1,.2f,.3f,.5f},{1,.2f,.3f,.5f},{1,.2f,.3f,.5f}}},
    {.meta={kActionSourceParticle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{4,4},{4,3}}, .colors={{.7f,.4f,.2f,.6f}}, .extra={.4f,.7f}},
    {.meta={kActionSourceLeaf,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{8,4},{9,3},{11,3},{12,4},{11,5},{9,5}}, .colors={{.1f,.3f,.2f,.5f},{.6f,.7f,.3f,.2f}}},
    {.meta={kActionSourceQuad,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{2,3},{4,3},{4,5},{2,5}},
      .colors={{1,0,0,1},{0,1,0,1},{0,0,1,1}}, .extra={.1f,.2f,.3f,.4f}},
    {.meta={kActionSourceStar,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{3,4}}, .colors={{1,1,1,1}}, .extra={.5f}},
    {.meta={kActionSourceGlowTriangle,2}, .origin={5,2}, .clip={-100,-100,100,100},
      .points={{3,4},{.25f,2,3,2},{0,0,0,0},{1,0,0,0},{0,0,0,1}},
      .colors={{1,0,0,1},{0,1,0,.5f},{0,0,1,0}}}
  };
  ActionEffectSourceBatch batch = {.primitives=primitives,.count=6,.capacity=6};
  const float phases[]={0,.25f,.5f,.75f,1};
  const float brightnesses[]={1,.5f,.5f,0,1};
  for (unsigned valid=0; valid<2; ++valid) for (unsigned phase=0; phase<5; ++phase) {
    /* Repaints reuse one GPU packet while motion changes. The next capture
     * changes its revision/color; master brightness may change independently. */
    batch.revision = valid + 1;
    primitives[0].colors[0][0] = valid ? .6f : 1.0f;
    const float brightness = brightnesses[phase];
    ActionEffectSourcePrimitive original[6];
    memcpy(original, primitives, sizeof(original));
    int32_t *vectors = SDL_MapGPUTransferBuffer(gpu, upload, true);
    CHECK(vectors != NULL); if (!vectors) continue;
    memset(vectors,0,256); vectors[6*8]=3; vectors[6*8+1]=-3;
    vectors[6*8+2]=-2; vectors[6*8+3]=2; vectors[6*8+4]=valid;
    SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation from={.transfer_buffer=upload};
    const SDL_GPUBufferRegion to={.buffer=motion,.size=256};
    SDL_UploadToGPUBuffer(copy,&from,&to,true); SDL_EndGPUCopyPass(copy);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,1u<<6,phases[phase],&batch,&view,NULL,
        kArRenderBlendMode_Add,brightness));
    CHECK(effect.packets[0].revision == batch.revision);
    CHECK(memcmp(original, primitives, sizeof(original)) == 0);
    cmd=SDL_AcquireGPUCommandBuffer(gpu); copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion source={.buffer=effect.vertices,.size=90*32};
    const SDL_GPUTransferBufferLocation dest={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&source,&dest); SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1)); SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false); CHECK(out!=NULL);
    if (!out) continue;
    const float dx=valid?(phases[phase]<.5f?-2+3*phases[phase]:-2*(1-phases[phase])):0;
    const float dy=-dx;
    for(unsigned j=0;j<3;++j) {
      CHECK(fabsf(out[j*8]-(5+primitives[0].points[j][0]+dx)*2)<.0001f);
      CHECK(fabsf(out[j*8+1]-(2+primitives[0].points[j][1]+dy)*4)<.0001f);
      for (unsigned c=0;c<4;++c)
        CHECK(fabsf(out[j*8+4+c]-primitives[0].colors[j][c]*(c<3?brightness:1))<.0001f);
    }
    /* Unit X/Y project to 2/4 pixels, so billboard scale is their mean, 3. */
    CHECK(fabsf(out[15*8]-(9+dx)*2)<.0001f);
    CHECK(fabsf(out[15*8+1]-((6+dy)*4+2.1f))<.0001f);
    CHECK(fabsf(out[16*8]-((9+dx)*2-1.2f))<.0001f);
    CHECK(fabsf(out[30*8]-(13+dx)*2)<.0001f);
    CHECK(fabsf(out[44*8]-((14+dx)*2*.72f+(14+dx)*2*.28f))<.0001f);
    CHECK(fabsf(out[45*8]-(7+dx)*2)<.0001f);
    CHECK(fabsf(out[50*8+1]-(7+dy)*4)<.0001f);
    for (unsigned c=0;c<4;++c)
      CHECK(fabsf(out[50*8+4+c]-primitives[3].extra[c]*(c<3?brightness:1))<.0001f);
    CHECK(fabsf(out[60*8]-((8+dx)*2+1))<.0001f);
    CHECK(fabsf(out[66*8+1]-((6+dy)*4+2.7f))<.0001f);
    CHECK(fabsf(out[75*8]-(8+dx)*2)<.0001f);
    CHECK(fabsf(out[76*8]-((8+dx)*2+3))<.0001f); // screen minimum
    CHECK(fabsf(out[77*8+1]-((6+dy)*4+8))<.0001f);
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* World-space casters have independent camera/apron offsets. Verify both
   * their uploaded bounds and the resulting shadow, including retained
   * packets, fades, and removal of all casters after a populated upload. */
  const DioramaProjection shadow_view = {.valid=true,.texture_width=32,.texture_height=16,
      .output_width=64,.output_height=64,.aspect_x=2,.height_scale=2,
      .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
      .bg1_plane={.valid=true,.u1=1,.v1=1}};
  ActionEffectSourcePrimitive shadow_primitive = {.meta={kActionSourceTriangle,1,0,1},
      .origin={16,0},.clip={-100,-100,100,100},.points={{0,8},{1,8},{0,9}},
      .colors={{1,.4f,.2f,.6f},{1,.4f,.2f,.6f},{1,.4f,.2f,.6f}}};
  const ActionEffectSourcePrimitive original_shadow = shadow_primitive;
  ActionMoonlightOcclusion scenery = {.valid=true,.rectangles={{5,8,19,16},{-3,4,1,6}}};
  ActionEffectSourceBatch shadow_batch = {.primitives=&shadow_primitive,.count=1,.capacity=1,
      .context={.ws_extra=9,.bg1_camera_x=4,.ws_extra_top=7,.bg1_camera_y=10}};
  const float shadow_brightness[]={1,1,.5f,.5f,0,1};
  for (unsigned sample=0;sample<6;++sample) {
    scenery.count = sample && sample<5 ? 2 : 0;
    shadow_batch.revision = 100 + scenery.count;
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&shadow_batch,&shadow_view,&scenery,
        kArRenderBlendMode_Add,shadow_brightness[sample]));
    CHECK(memcmp(&original_shadow,&shadow_primitive,sizeof(shadow_primitive)) == 0);
    CHECK(effect.packets[0].shadow_count == scenery.count);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion vertices={.buffer=effect.vertices,.size=15*32};
    const SDL_GPUBufferRegion casters={.buffer=effect.occluders,.size=(scenery.count+1)*16};
    const SDL_GPUTransferBufferLocation vertex_to={.transfer_buffer=download},
        caster_to={.transfer_buffer=download,.offset=15*32};
    SDL_DownloadFromGPUBuffer(copy,&vertices,&vertex_to);
    SDL_DownloadFromGPUBuffer(copy,&casters,&caster_to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    const float *rects=out+15*8;
    if (scenery.count) {
      const float expected[]={2,1,24,13, 10,5,24,13, 2,1,6,3};
      for (unsigned i=0;i<12;++i) CHECK(rects[i] == expected[i]);
    } else {
      CHECK(rects[0] == INFINITY && rects[1] == INFINITY);
      CHECK(rects[2] == -INFINITY && rects[3] == -INFINITY);
    }
    for (unsigned j=0;j<3;++j) for (unsigned c=0;c<4;++c) {
      const float factor=c<3?shadow_brightness[sample]*(scenery.count?.14f:1):1;
      CHECK(fabsf(out[j*8+4+c]-shadow_primitive.colors[j][c]*factor)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* The same world transform must cover OBJ priorities, high BG planes,
   * folded waterfalls, and static/dynamic anchors spanning raster bands. */
  for (unsigned scenario = 0; scenario < 11; ++scenario) {
    DioramaProjection v = {.valid = true, .texture_width = 32, .texture_height = 16,
        .output_width = 64, .output_height = 64, .aspect_x = 2, .height_scale = 2,
        .matrix = {1,0,0,0, 0,1,0,0, .1f,.1f,1,.2f, 0,0,0,1}};
    const DioramaPlaneProjection plane = {.valid = true, .u1 = 1, .v1 = 1,
        .z_world = .1f, .rake = .1f, .bow = .05f, .world_y_offset = .03f};
    v.bg1_plane = v.bg2_plane = v.bg1_high_plane = v.bg2_high_plane = plane;
    for (unsigned i = 0; i < 4; ++i) {v.object_planes[i] = plane; v.object_planes[i].z_world += i*.1f;}
    ActionEffectSourcePrimitive p = {.meta = {kActionSourceQuad, scenario < 4 ?
        (scenario ? 5 + scenario : 0) : scenario < 6 ? scenario - 1 : 2},
        .origin = {5,2}, .clip = {-100,-100,100,100}, .points={{1,1},{3,1},{3,3},{1,3}},
        .colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}},.extra={1,1,1,1}};
    if (scenario == 6) {
      v.bg2_plane.overflow_valid = true; v.bg2_plane.overflow_fold_t = 1;
      v.bg2_plane.overflow_height = 1; v.bg2_plane.overflow_overlap_t = .2f;
      v.bg2_plane.overflow_handoff_z = .5f; v.bg2_plane.overflow_front_z = .9f;
      v.bg2_plane.overflow_front_drop = .18f; p.origin[1] = 18;
    } else if (scenario >= 7 && scenario < 10) {
      v.bg2_skybox = (DioramaSkyboxProjection){.count=2,.active_band=scenario==7?1:-1,
          .bands={{0,0,32,8,0,.5f},{-8,8,40,16,.5f,1}}};
      p.origin[1] = scenario == 9 ? 2 : 9;
      if (scenario == 9) {p.origin[2] = 2; for (unsigned j=0;j<4;++j)p.points[j][1]+=7;}
    } else if (scenario == 10) {
      /* A finite UV edge that does not round-trip exactly through pixel
       * bounds must keep an attached primitive whose normalized point is on
       * the edge. This dropped Marahna's torch at the capture's left border. */
      v.texture_width=640;v.texture_x_origin=64;v.bg2_plane.u0=.1f;
      p.origin[0]=0;
      for(unsigned j=0;j<4;++j)p.points[j][0]-=1;
    }
    const ActionEffectSourceBatch b = {.primitives=&p,.count=1,.capacity=1};
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&b,&v,NULL,kArRenderBlendMode_Add,1));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion from={.buffer=effect.vertices,.size=15*32};
    const SDL_GPUTransferBufferLocation to={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    const unsigned corners[]={0,1,2,0,2,3};
    for(unsigned i=0;i<6;++i) {
      const float x=p.origin[0]+p.points[corners[i]][0], y=p.origin[1]+p.points[corners[i]][1];
      ArRenderPointF expected;
      const bool ok=scenario<4?Diorama_ProjectCapturedPoint(&v,x,y,scenario,&expected,NULL,NULL):
          scenario==4?Diorama_ProjectCapturedBg1HighPoint(&v,x,y,&expected,NULL,NULL):
          scenario==5?Diorama_ProjectCapturedBg2HighPoint(&v,x,y,&expected,NULL,NULL):
          scenario==9?Diorama_ProjectSkyboxAnchorPoint(&v,p.origin[1],x,y,&expected):
          Diorama_ProjectCapturedBg2Point(&v,x,y,&expected,NULL,NULL);
      CHECK(ok);
      CHECK(fabsf(out[i*8]-expected.x)<.001f && fabsf(out[i*8+1]-expected.y)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  /* Synthetic moon receiver: the finite platform shades y=8 at half depth,
   * while the near y=2 receiver remains lit. A glowing insect ignores platform
   * transport, but uses the same projected angular light distribution. */
  for(unsigned kind=0;kind<4;++kind)for(unsigned blocked=0;blocked<2;++blocked) {
    DioramaProjection v={.valid=true,.texture_width=32,.texture_height=16,
        .output_width=64,.output_height=64,.aspect_x=2,.height_scale=2,
        .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
        .bg1_plane={.valid=true,.u1=1,.v1=1},
        .bg2_skybox={.count=1,.active_band=-1,.bands={{0,0,32,16,0,1}}}};
    const ActionMoonlightOcclusion occlusion={.valid=true,.count=blocked,
        .rectangles={{0,3,32,5}}};
    static ActionEffectSourceLightJob light;
    light=(ActionEffectSourceLightJob){.occlusion=&occlusion,
        .data={.meta={3,kind,10},.light={16,0,2},.receiver={16,0,2},
            .clip={-100,-100,100,100},.transport={.5f,.5f,.5f},.surface={.5f,.5f},
            .rays={{0,10,1},{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,1,0}},
            .points={{0,8},{8,8},{0,2}}}};
    ActionEffectSourcePrimitive triangles[3];
    for(unsigned i=0;i<3;++i) {
      triangles[i]=(ActionEffectSourcePrimitive){.meta={kActionSourceLitTriangle,2},
          .origin={16,0},.clip={-100,-100,100,100},.extra={10},
          .colors={{1,1,1,1},{1,1,1,1},{1,1,1,1}}};
      for(unsigned j=0;j<3;++j) {
        triangles[i].points[j][0]=light.data.points[i][0];
        triangles[i].points[j][1]=light.data.points[i][1];
        triangles[i].points[j][2]=i+1;
      }
    }
    const ActionEffectSourceBatch b={.primitives=triangles,.count=3,.capacity=3,
        .lights=&light,.light_count=1,.light_capacity=1};
    CHECK(ArGpuEffectSource_Draw(&effect,device,motion,0,.5f,&b,&v,NULL,kArRenderBlendMode_Add,1));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUBufferRegion from={.buffer=effect.vertices,.size=45*32};
    const SDL_GPUTransferBufferLocation to={.transfer_buffer=download};
    SDL_DownloadFromGPUBuffer(copy,&from,&to);SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    CHECK(fence && SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
    const float *out=SDL_MapGPUTransferBuffer(gpu,download,false);CHECK(out!=NULL);
    if(!out)continue;
    for(unsigned i=0;i<3;++i) {
      float expected=kind==0||i!=1?1:.96059601f;
      if(blocked&&kind!=3&&i<2)expected=0;
      if(fabsf(out[i*15*8+7]-expected)>=.001f)
        fprintf(stderr,"moon kind=%u blocked=%u point=%u actual=%f expected=%f\n",kind,blocked,i,out[i*15*8+7],expected);
      CHECK(fabsf(out[i*15*8+7]-expected)<.001f);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
  }
  CHECK(SDL_SetRenderTarget(renderer,previous_target));
  SDL_DestroyTexture(target); ArGpuEffectSource_Destroy(&effect);
  SDL_ReleaseGPUBuffer(gpu,motion); SDL_ReleaseGPUTransferBuffer(gpu,upload);
  SDL_ReleaseGPUTransferBuffer(gpu,download);
}

static void TestProductionPolicy(void) {
  unsigned rooms=0;
  for(unsigned group=1;group<=7;++group)for(unsigned room=1;room<=ActRaiser_ActionMapLast(group);++room) {
    const DioramaGpuPolicy p=DioramaGpuPolicy_Resolve(group,room,NULL,NULL,NULL,NULL,NULL);
    CHECK(p.capture==kDioramaGpuCapture_Owned && p.motion==kDioramaGpuMotion_Owned &&
        p.pack && p.unpack && p.resident);
    ++rooms;
  }
  CHECK(rooms==49);
  const DioramaGpuPolicy reference=DioramaGpuPolicy_Resolve(1,1,"reference","owned","owned","all","resident");
  CHECK(!reference.capture&&!reference.motion&&!reference.pack&&!reference.unpack&&!reference.resident);
  const DioramaGpuPolicy sim=DioramaGpuPolicy_Resolve(0,1,NULL,NULL,NULL,NULL,NULL);
  CHECK(!sim.capture&&!sim.motion&&!sim.pack&&!sim.resident);
  const DioramaGpuPolicy invalid=DioramaGpuPolicy_Resolve(1,99,NULL,NULL,NULL,NULL,NULL);
  CHECK(!invalid.capture&&!invalid.motion&&!invalid.resident);
  const DioramaGpuPolicy explicit_reference=DioramaGpuPolicy_Resolve(1,1,NULL,"0","0","0","reference");
  CHECK(!explicit_reference.capture&&!explicit_reference.motion&&!explicit_reference.pack&&!explicit_reference.resident);
  const DioramaGpuPolicy incomplete=DioramaGpuPolicy_Resolve(1,1,NULL,NULL,"compute","all","resident");
  CHECK(!incomplete.resident);
}

int main(void) {
  TestProductionPolicy();
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Texture *current = NULL;
  SDL_Texture *scene = NULL;
  ArRenderDevice render_device = {0};
  ArSdlRenderBackend render_backend = {0};
  const bool resident = getenv("AR_GPU_EFFECT_PROJECTION") != NULL;
  DioramaFrameGeneration_AllowSourceProjection(resident);
  const bool gpu_motion = getenv("AR_GPU_BG_MOTION") != NULL;
  const bool gpu_owned = gpu_motion && strcmp(getenv("AR_GPU_BG_MOTION"), "owned") == 0;
  const int test_plane = gpu_motion ? SR_PPU_OVERLAY_BG1 : kDioramaPlane_Backdrop;
  DioramaPlaneCaptureRegion region;
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      kDioramaPlane_Backdrop, kSurfaceWidth, kHeight, kApron, 3, &region));
  CHECK(region.x == kApron && region.width == kDisplayWidth &&
        region.height == kHeight);
  CHECK(DioramaPlaneCaptureRegion_Resolve(
      SR_PPU_OVERLAY_OBJ, kSurfaceWidth, kHeight, kApron, 0, &region));
  CHECK(region.x == 0 && region.width == kSurfaceWidth &&
        region.height == kHeight);
  const int backgrounds[] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi,
      kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far};
  for (unsigned i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]); i++) {
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, 0, &region));
    CHECK(region.x == kApron && region.width == kDisplayWidth);
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, i < 3 ? 1 : 2, &region));
    CHECK(region.x == 0 && region.width == kSurfaceWidth);
    CHECK(DioramaPlaneCaptureRegion_Resolve(
        backgrounds[i], kSurfaceWidth, kHeight, kApron, i < 3 ? 2 : 1, &region));
    CHECK(region.x == kApron && region.width == kDisplayWidth);
  }
  if (!SDL_Init(SDL_INIT_VIDEO) && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    return kSkipNoGpuRenderer;
  }
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) != 0);
  window = SDL_CreateWindow("frame generation test", 64, 64, 0);
  if (!window && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  CHECK(window != NULL);
  if (window && gpu_motion) {
    if (ArSdlRenderBackend_CreateForWindow(&render_device, window, NULL))
      renderer = ArSdlRenderBackend_Renderer(&render_device);
  } else renderer = window ? CreateTestRenderer(window) : NULL;
  if (!renderer && RequireProductionGpuRenderer()) {
    fprintf(stderr, "GPU frame-generation test skipped: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return kSkipNoGpuRenderer;
  }
  CHECK(renderer != NULL);
  if (!renderer) goto done;
  SDL_GPUDevice *gpu_device = SDL_GetGPURendererDevice(renderer);
  printf("diorama frame-generation renderer=%s gpu=%s\n",
         SDL_GetRendererName(renderer),
         gpu_device ? SDL_GetGPUDeviceDriver(gpu_device) : "fallback");
  if (!gpu_motion) CHECK(ArSdlRenderBackend_Bind(
      &render_device, &render_backend, renderer));

  if (resident && gpu_device) TestSourcePrimitives(&render_device, gpu_device, renderer);

  current = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kFrameSlotLayerTextureWidth, kFrameSlotLayerTextureHeight);
  scene = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 64, 64);
  CHECK(current != NULL && scene != NULL);
  if (!current || !scene) goto done;

  static uint32_t previous_pixels[kSurfaceWidth * kHeight];
  static uint32_t current_pixels[kSurfaceWidth * kHeight];
  FillFrame(previous_pixels, 0, false);
  FillFrame(current_pixels, 2, true);
  CHECK(current_pixels[8 * kSurfaceWidth + kApron + 11] == 0xff0000ffu);
  const uint8_t *planes[kDioramaPlane_Count] = {0};
  size_t plane_pitches[kDioramaPlane_Count] = {0};
  /* Endpoints are now copied from the compositor texture on the GPU, so the
   * source texture has to hold this frame's pixels BEFORE Capture runs --
   * exactly the order Diorama_Upload establishes in the real path. */
  ArRenderTexture sources[kDioramaPlane_Count] = {0};
  sources[test_plane] =
      ArSdlRenderBackend_BorrowTexture(current);
  const SDL_Rect source_rect = {kApron, 0, kDisplayWidth, kHeight};
  planes[test_plane] = (uint8_t *)previous_pixels;
  plane_pitches[test_plane] =
      kSurfaceWidth * sizeof(uint32_t);

  static FrameSlot slot;
  static SrPpuBgPacket packet;
  if (gpu_owned) {
    packet.owned_sources = packet.words[2] = 7;
    slot.background_packet = &packet;
    planes[test_plane] = NULL;
  }
  slot.diorama_active = true;
  slot.interp_setting_enabled = true;
  slot.snes_width = kDisplayWidth;
  slot.snes_height = kHeight;
  slot.obj_apron = kApron;
  slot.capture_ticks = 1;
  slot.bg_mode = 1;
  slot.diorama_plane_request_mask = 1u << test_plane;
  slot.diorama_plane_content_mask = slot.diorama_plane_request_mask;
  slot.timestamp_ns = 1000000;
  CHECK(SDL_UpdateTexture(
      current, &source_rect, &previous_pixels[kApron],
      kSurfaceWidth * sizeof(uint32_t)));
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches,
      1u << test_plane);

  planes[test_plane] = gpu_owned ? NULL : (uint8_t *)current_pixels;
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_FinishCapture();
  CHECK(SDL_UpdateTexture(
      current, &source_rect, &current_pixels[kApron],
      kSurfaceWidth * sizeof(uint32_t)));
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches,
      1u << test_plane);

  CHECK(SDL_SetRenderTarget(renderer, scene));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 32, 32, SDL_LOGICAL_PRESENTATION_STRETCH));
  const SDL_Rect viewport = {1, 2, 30, 28};
  const SDL_Rect clip = {3, 4, 20, 18};
  CHECK(SDL_SetRenderViewport(renderer, &viewport));
  CHECK(SDL_SetRenderClipRect(renderer, &clip));
  CHECK(SDL_SetRenderDrawColor(renderer, 11, 22, 33, 44));

  ArRenderTexture raw[kDioramaPlane_Count] = {0};
  ArRenderTexture resolved[kDioramaPlane_Count] = {0};
  raw[test_plane] =
      ArSdlRenderBackend_BorrowTexture(current);
  const uint32_t generated = DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved);
  CHECK(generated == (1u << test_plane));
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == generated);
  CHECK(DioramaFrameGeneration_GpuPlaneMask() == (gpu_motion ? (1u << test_plane) : 0));
  ArRenderPointF offset = DioramaFrameGeneration_PlaneOffset(test_plane);
  CHECK(DioramaFrameGeneration_SourceProjectionActive() == resident);
  CHECK(!resident || DioramaFrameGeneration_MetadataReadbackCount() == 0);
  CHECK(fabsf(offset.x+(resident ? 0 : 1)) < .001f && fabsf(offset.y) < .001f);
  CHECK(DioramaFrameGeneration_PlaneOffset(SR_PPU_OVERLAY_BG2).x == 0);
  CHECK(DioramaFrameGeneration_PlaneOffset(-1).x == 0);
  CHECK(!ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  CHECK(SDL_GetRenderTarget(renderer) == scene);
  int logical_width = 0, logical_height = 0;
  SDL_RendererLogicalPresentation logical_mode =
      SDL_LOGICAL_PRESENTATION_DISABLED;
  CHECK(SDL_GetRenderLogicalPresentation(
      renderer, &logical_width, &logical_height, &logical_mode));
  CHECK(logical_width == 32 && logical_height == 32 &&
        logical_mode == SDL_LOGICAL_PRESENTATION_STRETCH);
  SDL_Rect restored;
  CHECK(SDL_GetRenderViewport(renderer, &restored));
  CHECK(SDL_RectsEqual(&viewport, &restored));
  CHECK(SDL_GetRenderClipRect(renderer, &restored));
  CHECK(SDL_RectsEqual(&clip, &restored));
  Uint8 r = 0, g = 0, b = 0, a = 0;
  CHECK(SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a));
  CHECK(r == 11 && g == 22 && b == 33 && a == 44);

  /* At phase 0.5 the nearer (current) endpoint warps back by one pixel. The
   * marker therefore lands at x=10 as exact opaque blue. Drawing/blending the
   * farther red endpoint would turn this purple and recreate sprite ghosting. */
  CHECK(SDL_SetRenderTarget(
      renderer, ArSdlRenderBackend_UnwrapTexture(
                    resolved[test_plane])));
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
  CHECK(SDL_SetRenderViewport(renderer, NULL));
  CHECK(SDL_SetRenderClipRect(renderer, NULL));
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, &source_rect);
  CHECK(readback != NULL);
  SDL_Surface *argb = readback
      ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
  CHECK(argb != NULL);
  if (argb) {
    const uint32_t *row = (const uint32_t *)(
        (const uint8_t *)argb->pixels + (size_t)8 * argb->pitch);
    const uint32_t marker = row[10];
    if ((marker >> 24) < 250u || ((marker >> 16) & 0xffu) > 4u ||
        (marker & 0xffu) < 250u)
      fprintf(stderr, "generated marker=%08x\n", marker);
    CHECK((marker >> 24) >= 250u);
    CHECK(((marker >> 16) & 0xffu) <= 4u);
    CHECK((marker & 0xffu) >= 250u);
  }
  SDL_DestroySurface(argb);
  SDL_DestroySurface(readback);

  /* The non-OBJ backdrop excludes the apron entirely. Sentinel magenta in the
   * source apron must therefore leave the fixed compositor padding clear. */
  static const SDL_Rect apron_rects[] = {
    {0, 0, kApron, kHeight},
    {kApron + kDisplayWidth, 0, kApron, kHeight},
  };
  for (size_t side = 0;
       side < sizeof(apron_rects) / sizeof(apron_rects[0]); side++) {
    readback = SDL_RenderReadPixels(renderer, &apron_rects[side]);
    argb = readback
        ? SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888) : NULL;
    CHECK(argb != NULL);
    if (argb) {
      const uint32_t *row = argb->pixels;
      CHECK((row[0] >> 24) == 0);
    }
    SDL_DestroySurface(argb);
    SDL_DestroySurface(readback);
  }

  /* Both endpoint owners publish the same current-capture-to-image motion.
   * A multi-tick pair uses the same phase mapping as texture generation. */
  const float phases[] = {.25f,.75f};
  for (unsigned i = 0; i < 2; i++) {
    CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,phases[i],raw,
        1u << test_plane,resolved) == (1u << test_plane));
    offset = DioramaFrameGeneration_PlaneOffset(test_plane);
    CHECK(fabsf(offset.x+(resident ? 0 : 2*(1-phases[i]))) < .001f && fabsf(offset.y) < .001f);
  }
  slot.capture_ticks = 2;
  CHECK(DioramaFrameGeneration_Prepare(&render_device,&slot,.5f,raw,
      1u << test_plane,resolved) == (1u << test_plane));
  CHECK(fabsf(DioramaFrameGeneration_PlaneOffset(test_plane).x+(resident ? 0 : .5f)) < .001f);
  slot.capture_ticks = 1;

  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, kPresentationFrameGenerationPhaseNone, raw,
      1u << test_plane, resolved) == 0);
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == 0);
  CHECK(DioramaFrameGeneration_PlaneOffset(test_plane).x == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* A synchronized but byte-identical raw endpoint is already authoritative.
   * It must not create a redundant private pair or generated plane. */
  slot.timestamp_ns += 16666667;
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* A room-key discontinuity seeds a new endpoint but never blends across the
   * transition. */
  slot.timestamp_ns += 16666667;
  slot.diorama_map_number++;
  DioramaFrameGeneration_Capture(
      &render_device, &slot, sources, planes, plane_pitches, 0);
  memset(resolved, 0, sizeof(resolved));
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(
      resolved[test_plane], raw[test_plane]));

  /* Shrinking and restoring the capture reallocates the compute atlas. Neither
   * its old field nor its old endpoint can survive the geometry change. */
  for (int pass = 0; pass < 3; ++pass) {
    slot.snes_height = pass == 0 ? kHeight - 4 : kHeight;
    const uint8_t *endpoint = (const uint8_t *)(pass == 2 ? current_pixels : previous_pixels);
    planes[test_plane] = gpu_owned ? NULL : endpoint;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &source_rect,
        endpoint + kApron * sizeof(uint32_t), kSurfaceWidth * sizeof(uint32_t)));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_Capture(&render_device, &slot, sources, planes,
        plane_pitches, 1u << test_plane);
    const uint32_t expected = pass == 2 ? 1u << test_plane : 0;
    CHECK(DioramaFrameGeneration_Prepare(&render_device, &slot, 0.5f, raw,
        1u << test_plane, resolved) == expected);
    CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == expected);
    if (gpu_motion) CHECK(DioramaFrameGeneration_GpuPlaneMask() == expected);
  }

  /* A changed but uniform pair has no reliable motion. GPU preparation may
   * already have copied its output before confidence reaches the CPU; that
   * private copy must not replace the current image or publish an offset. */
  DioramaFrameGeneration_Reset();
  for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
    for (unsigned i = 0; i < kSurfaceWidth * kHeight; ++i)
      current_pixels[i] = endpoint ? 0xff557799u : 0xff113355u;
    planes[test_plane] = gpu_owned ? NULL : (uint8_t *)current_pixels;
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &source_rect, &current_pixels[kApron],
        kSurfaceWidth * sizeof(uint32_t)));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_Capture(&render_device, &slot, sources, planes,
        plane_pitches, 1u << test_plane);
  }
  CHECK(DioramaFrameGeneration_Prepare(&render_device, &slot, 0.5f, raw,
      1u << test_plane, resolved) == 0);
  CHECK(ArRenderTexture_Equals(resolved[test_plane], raw[test_plane]));
  CHECK(DioramaFrameGeneration_GpuPlaneMask() == 0);
  CHECK(DioramaFrameGeneration_GeneratedPlaneMask() == 0);
  offset = DioramaFrameGeneration_PlaneOffset(test_plane);
  CHECK(offset.x == 0 && offset.y == 0);

  /* The clamped skybox is independent of the gameplay planes and has no
   * apron. A compact source pitch must remain valid through interpolation. */
  CHECK(SDL_SetRenderTarget(renderer, scene));
  DioramaFrameGeneration_Reset();
  uint32_t sky_pixels[kDisplayWidth * kHeight];
  const SDL_Rect sky_rect = {0, 0, kDisplayWidth, kHeight};
  slot.diorama_plane_request_mask = slot.diorama_plane_content_mask = 0;
  slot.diorama_skybox_surface = (SrPpuSurfaceView){
    .data = gpu_owned ? NULL : (const uint8_t *)sky_pixels,
    .width_pixels = kDisplayWidth, .height_pixels = kHeight,
    .pitch_bytes = kDisplayWidth * sizeof(uint32_t),
  };
  const ArRenderTexture sky_raw = ArSdlRenderBackend_BorrowTexture(current);
  // Drain multiple source frames without presenting. The queued GPU result
  // must belong to the newest pair (+4), never an earlier discarded fence.
  const int sky_shifts[] = {0, 2, 1, 5};
  for (int endpoint = 0; endpoint < 4; ++endpoint) {
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kDisplayWidth; ++x)
        sky_pixels[y * kDisplayWidth + x] = PatternPixel(x - sky_shifts[endpoint], y);
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &sky_rect, sky_pixels, kDisplayWidth * 4));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, sky_raw, true);
  }
  ArRenderTexture sky_resolved;
  CHECK(DioramaFrameGeneration_PrepareWithSkybox(
      &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) ==
      (1u << kDioramaFrameGenerationSkybox));
  CHECK(!ArRenderTexture_Equals(sky_resolved, sky_raw));
  const ArRenderPointF sky_offset = DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox);
  CHECK(fabsf(sky_offset.x+(resident ? 0 : 2)) < .001f && fabsf(sky_offset.y) < .001f);
  CHECK(SDL_GetRenderTarget(renderer) == scene);
  /* An old caller must not accidentally address the private extra slot. */
  CHECK(DioramaFrameGeneration_Prepare(
      &render_device, &slot, 0.5f, raw, UINT32_MAX, resolved) == 0);
  /* A periodic source animates in world space; falling water is not camera
   * motion. Switching from a finite source must discard its private pair. */
  slot.diorama_skybox_periodic = true;
  for (int endpoint = 0; endpoint < 2; ++endpoint) {
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kDisplayWidth; ++x)
        sky_pixels[y * kDisplayWidth + x] = PatternPixel(x, y - endpoint * 2);
    DioramaFrameGeneration_FinishCapture();
    CHECK(SDL_UpdateTexture(current, &sky_rect, sky_pixels, kDisplayWidth * 4));
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, sky_raw, true);
    CHECK(DioramaFrameGeneration_PrepareWithSkybox(
        &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) == 0);
    CHECK(ArRenderTexture_Equals(sky_resolved, sky_raw));
    CHECK(DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox).x == 0);
    CHECK(DioramaFrameGeneration_PlaneOffset(kDioramaFrameGenerationSkybox).y == 0);
  }
  DioramaFrameGeneration_Reset();
  CHECK(DioramaFrameGeneration_PrepareWithSkybox(
      &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved) == 0);
  CHECK(ArRenderTexture_Equals(sky_resolved, sky_raw));

  if (resident) {
    /* Room qualification can change without destroying the renderer. A
     * reference frame must download its metadata; returning to resident mode
     * must stop downloading without inheriting a stale pending result. */
    slot.diorama_skybox_periodic = false;
    unsigned downloads = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
      DioramaFrameGeneration_AllowSourceProjection(mode != 0);
      for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
        slot.timestamp_ns += 16666667;
        DioramaFrameGeneration_CaptureWithSkybox(
            &render_device, &slot, sources, planes, plane_pitches, 0, sky_raw, true);
        DioramaFrameGeneration_FinishCapture();
        CHECK(DioramaFrameGeneration_SourceProjectionActive() == (mode != 0));
        DioramaFrameGeneration_PrepareWithSkybox(
            &render_device, &slot, 0.5f, raw, 0, resolved, sky_raw, &sky_resolved);
      }
      if (!mode) {
        downloads = DioramaFrameGeneration_MetadataReadbackCount();
        CHECK(downloads > 0);
      } else {
        CHECK(DioramaFrameGeneration_MetadataReadbackCount() == downloads);
      }
    }
    /* Projection also runs with interpolation disabled, without creating a
     * motion pair. A failed batch must recover and latch until reset. */
    DioramaFrameGeneration_Reset();
    DioramaFrameGeneration_AllowSourceProjection(true);
    slot.interp_setting_enabled=false;
    for(unsigned attempt=0;attempt<3;++attempt) {
      slot.timestamp_ns+=16666667;
      DioramaFrameGeneration_CaptureWithSkybox(
          &render_device,&slot,sources,planes,plane_pitches,0,sky_raw,true);
      CHECK(DioramaFrameGeneration_SourceProjectionActive()==(attempt!=1));
      CHECK(DioramaFrameGeneration_SourceProjectionFallback()==(attempt==1));
      CHECK(DioramaFrameGeneration_PrepareWithSkybox(
          &render_device,&slot,.5f,raw,0,resolved,sky_raw,&sky_resolved)==0);
      CHECK(DioramaFrameGeneration_MetadataReadbackCount()==0);
      if(attempt==0) {
        const ActionEffectSourceBatch failed={.failed=true};
        CHECK(!DioramaFrameGeneration_DrawSource(&render_device,&failed,NULL,NULL,kArRenderBlendMode_Add,1));
        CHECK(DioramaFrameGeneration_SourceProjectionFailed());
        DioramaFrameGeneration_RecoverSourceProjection(&render_device);
        CHECK(!DioramaFrameGeneration_SourceProjectionActive());
        CHECK(DioramaFrameGeneration_SourceProjectionFallback());
        DioramaFrameGeneration_AllowSourceProjection(false);
        CHECK(!DioramaFrameGeneration_SourceProjectionFallback());
        DioramaFrameGeneration_AllowSourceProjection(true);
      } else if(attempt==1) DioramaFrameGeneration_Reset();
    }
    slot.interp_setting_enabled=true;
  }

  /* Teardown may happen immediately after dispatch, without Prepare ever
   * joining it. Repeated lifetimes must retire the borrow before destruction. */
  slot.diorama_skybox_periodic = false;
  for (unsigned reset = 0; reset < 3; ++reset) {
    slot.timestamp_ns += 16666667;
    DioramaFrameGeneration_CaptureWithSkybox(
        &render_device, &slot, sources, planes, plane_pitches, 0, sky_raw, true);
    DioramaFrameGeneration_Reset();
  }

done:
  DioramaFrameGeneration_Shutdown();
  SDL_DestroyTexture(scene);
  SDL_DestroyTexture(current);
  ArSdlRenderBackend_Destroy(&render_device);
  if (!gpu_motion) SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("diorama frame-generation test: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
