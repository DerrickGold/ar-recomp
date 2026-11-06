/* Compare the shared-depth material against the actual ordinary SDL material,
 * then verify that only its final filtered coverage writes world depth. No ROM
 * or user settings are read. This is material/cutout evidence, not actor policy
 * or order-independent transparency acceptance. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "diorama/diorama_effect_backend.h"
#include "platform/sdl/diorama_depth_pass_sdl.h"
#include "platform/sdl/diorama_depth_alpha_sdl.h"
#include "platform/sdl/render_sdl_interop.h"
#include "support/test_assert.h"

enum { W=128, H=96, T=8 };
static const int32_t indices[]={0,1,2,0,2,3};
static unsigned renders, pixels_checked, covered, holes;

static SDL_GPUTexture *GpuTexture(ArRenderTexture texture) {
  SDL_Texture *sdl=ArSdlRenderBackend_UnwrapTexture(texture);assert(sdl);
  SDL_GPUTexture *gpu=SDL_GetPointerProperty(SDL_GetTextureProperties(sdl),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);assert(gpu);return gpu;
}

static ArRenderTexture Texture(ArRenderDevice *device, bool target, unsigned kind, bool linear) {
  const ArRenderTextureDesc desc={.width=target?W:T,.height=target?H:T,
      .format=target?kArRenderPixelFormat_Abgr8888:kArRenderPixelFormat_Argb8888,
      .usage=target?kArRenderTextureUsage_Target:kArRenderTextureUsage_Static,
      .filter=linear?kArRenderFilter_Linear:kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  ArRenderTexture texture;assert(ArRenderDevice_CreateTexture(device,&desc,&texture));
  if(!target) {
    uint32_t art[T*T];
    for(unsigned y=0;y<T;++y)for(unsigned x=0;x<T;++x) {
      const unsigned alpha=x==1||x==2||y==0?0:x==4?128:255;
      art[y*T+x]=kind==0?(alpha<<24)|((24+x*17)<<16)|((16+y*19)<<8)|(20+x*9+y*5)
          :kind==1?0xff000000u|((x*4)<<16)|((y*4)<<8)|(x+y)*2
          :kind==3?(alpha<<24)|(((32+(x%4)*8)*alpha/255)<<16)|
              ((x<4?alpha:0)<<8)|(x>=4?alpha:0)
          :kind==4?0xff000000u|((y*5)<<16)|((x*2)<<8)|24:0xff000000u;
      if(kind==0 && x==6 && y>=2 && y<=5)art[y*T+x]=0xff000000u;
    }
    assert(ArRenderDevice_UpdateTexture(device,texture,NULL,art,T*4));
  }
  return texture;
}

static void Read(ArRenderDevice *device, ArRenderTexture target, uint8_t out[W*H*4]) {
  assert(ArRenderDevice_SetRenderTarget(device,target));
  SDL_Surface *raw=SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(device),NULL);assert(raw);
  SDL_Surface *rgba=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_RGBA32);assert(rgba);
  for(unsigned y=0;y<H;++y)memcpy(out+y*W*4,(uint8_t *)rgba->pixels+y*rgba->pitch,W*4);
  SDL_DestroySurface(rgba);SDL_DestroySurface(raw);++renders;
}

static DioramaSceneryLightEffectParams Material(unsigned scenario, ArRenderTexture light) {
  DioramaSceneryLightEffectParams p={.illumination=light,.texel_width=1.f/T,.texel_height=1.f/T,
      .shade={.9f,.8f,.7f,1},.light_uv={.13f,.07f,.71f,.83f},
      .attributes_uv={.1f,.05f,.8f,.9f}};
  if(scenario==1 || scenario==7) {p.mode=1;p.blur_radius=1.2f;}
  if(scenario==2 || scenario==3) {
    p.mode=2;p.priority=(DioramaPrioritySurfaceEffectParams){T,T/2,scenario==3,false};
  }
  if(scenario==4)p.grade=(DioramaSceneryGrade){.strength=.75f,.saturation=.6f,
      .shadows=0x708de5,.highlights=0xffcf85};
  if(scenario==5)p.heat=(DioramaHeat){.source={0,0,64,64},
      .bounds={{0,0,64,64}},.motion={{3.5f,.7f,0,0}}};
  if(scenario==6)p.cloud_light=(DioramaCloudLight){.source={0,0,64,64},
      .meta={1,.4f,0,0},.background_dim={.35f},.regions={{.bounds={0,0,64,64},
          .core={.2f,.12f,.1f,.1f},.shape={.3f,.4f,.6f,.75f},
          .motion={.1f,.7f,19,3},.color={.6f,.8f,1,.5f},.spill={.7f,.5f,.3f,.2f}}}};
  if(scenario==7)p.lower_content_v_max=.625f;
  return p;
}

static void Compare(const uint8_t *actual,const uint8_t *expected,unsigned scenario) {
  for(unsigned i=0;i<W*H*4;++i) {
    const int difference=(int)actual[i]-expected[i];
    if(difference < -1 || difference > 1)fprintf(stderr,
        "scenario=%u pixel=%u,%u channel=%u actual=%u expected=%u\n",
        scenario,i/4%W,i/4/W,i%4,actual[i],expected[i]);
    assert(difference>=-1 && difference<=1);
  }
  pixels_checked+=W*H;
}

static void Reject(ArGpuDioramaDepthPass *pass,SDL_GPUTexture *target,
    DioramaDepthDraw good,SDL_GPUTexture *source,SDL_GPUTexture *light) {
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(pass->device);assert(cmd);
  const SDL_GPUBuffer *vertices=pass->vertices;const SDL_GPUTexture *depth=pass->depth;
  for(unsigned test=0;test<6;++test) {
    DioramaDepthDraw draw=good;
    DioramaSceneryLightEffectParams p={.shade={1,1,1,1}};
    ArGpuDioramaDepthMaterial m={.scenery=&p,.illumination=light};
    if(test==0)p.mode=3;
    if(test==1)p.grade.strength=NAN;
    if(test==2)p.mode=2;
    if(test==3)m.illumination=target;
    if(test==4)p.attributes.value=1;
    if(test==5)draw.textured=false;
    assert(!ArGpuDioramaDepthPass_EncodeMaterials(pass,cmd,target,W,H,&draw,&source,&m,1,
        SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
    assert(vertices==pass->vertices && depth==pass->depth);
  }
  assert(SDL_CancelGPUCommandBuffer(cmd));
}

int main(int argc,char **argv) {
  const bool hybrid=argc>1&&strcmp(argv[1],"--hybrid")==0;
  setvbuf(stdout,NULL,_IONBF,0);
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_Window *window=SDL_CreateWindow("Shared-depth material oracle",W,H,SDL_WINDOW_HIDDEN);
  ArRenderDevice device={0};
  if(!window || !ArSdlRenderBackend_CreateForWindow(&device,window,NULL))return 77;
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(&device);assert(gpu);
  ArGpuDioramaDepthPass pass;assert(ArGpuDioramaDepthPass_Init(&pass,gpu));
  ArRenderTexture reference=Texture(&device,true,0,false),target=Texture(&device,true,0,false);
  ArRenderTexture art[2]={Texture(&device,false,0,false),Texture(&device,false,0,true)};
  ArRenderTexture light=Texture(&device,false,1,true),black=Texture(&device,false,2,true);
  ArRenderTexture attributes=Texture(&device,false,3,true),enemy=Texture(&device,false,4,true);
  uint8_t expected[W*H*4],actual[W*H*4],with_background[W*H*4];
  ArRenderVertex2D vertices[4];DioramaDepthVertex projected[4],background[4];
  for(unsigned actor=0;actor<2;++actor)for(unsigned additive=0;additive<2;++additive)
  for(unsigned linear=0;linear<2;++linear)for(unsigned fade=0;fade<2;++fade)
  for(unsigned scenario=0;scenario<(actor?4:8);++scenario) {
    const ArRenderColorF color={.35f,.4f,.45f,fade?.55f:1};
    for(unsigned i=0;i<4;++i) {
      const bool right=i==1||i==2,bottom=i>=2;
      const float x=right?120:8,y=bottom?88:8;
      const float u=scenario==2 || scenario==5 || (actor && scenario==3)
          ? (right?1.125f:-.125f) : right;
      vertices[i]=(ArRenderVertex2D){{x,y},color,{u,bottom}};
      projected[i]=(DioramaDepthVertex){{2*x/W-1,1-2*y/H,.35f,1},color,{u,bottom}};
      background[i]=projected[i];background[i].clip[2]=.8f;
      background[i].color=(ArRenderColorF){.08f,.12f,.04f,1};
    }
    DioramaSceneryLightEffectParams p=Material(scenario,scenario==7?black:light);
    if(actor) {
      p.mode=0;p.attributes=attributes;p.enemy_illumination=enemy;
      p.blur_radius=scenario==1 || scenario==3?1.2f:0;
      p.rim_strength=scenario==2?.4f:0;
      p.lower_content_v_max=scenario==3?.625f:0;
    }
    p.priority.additive=additive;
    /* Match RenderDioramaGeometry, including its explicit clamp. SDL's default
     * AUTO switches to wrapping when any vertex leaves [0,1], which is a
     * different material at real capture-edge/AA coordinates. */
    const ArRenderDrawState state={.flags=kArRenderDrawState_Blend|kArRenderDrawState_Address,
        .blend=additive?kArRenderBlendMode_Add:kArRenderBlendMode_Alpha,
        .address_u=kArRenderTextureAddressMode_Clamp,.address_v=kArRenderTextureAddressMode_Clamp};
    assert(ArRenderDevice_SetRenderTarget(&device,reference));
    assert(ArRenderDevice_Clear(&device,(ArRenderColorF){0}));
    assert(DioramaEffectBackend_BindSceneryLight(&device,&p));
    assert(ArRenderDevice_DrawGeometryWithState(&device,art[linear],vertices,4,indices,6,&state));
    assert(DioramaEffectBackend_Unbind(&device));
    Read(&device,reference,expected);
    assert(ArSdlRenderBackend_SubmitPending(&device));
    DioramaDepthDraw draws[2]={{.vertices=projected,.indices=indices,.vertex_count=4,.index_count=6,
        .depth_mode=additive?kDioramaDepth_Read:kDioramaDepth_Write,
        .blend=state.blend,.textured=true,.nearest=!linear},
        {.vertices=background,.indices=indices,.vertex_count=4,.index_count=6,
          .depth_mode=kDioramaDepth_Write,.blend=kArRenderBlendMode_Alpha}};
    SDL_GPUTexture *sources[]={GpuTexture(art[linear]),NULL};
    ArGpuDioramaDepthMaterial materials[]={{.scenery=&p,
        .illumination=scenario==7?NULL:GpuTexture(light),.actor=actor,
        .attributes=actor?GpuTexture(attributes):NULL,.enemy_illumination=actor?GpuTexture(enemy):NULL},{0}};
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
    assert((hybrid?ArGpuDioramaDepthPass_EncodeHybridMaterials:ArGpuDioramaDepthPass_EncodeMaterials)(&pass,cmd,GpuTexture(target),W,H,
        draws,sources,materials,1,SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
    assert(SDL_SubmitGPUCommandBuffer(cmd));
    Read(&device,target,actual);Compare(actual,expected,scenario);
    if(hybrid||additive)continue;
    assert(ArSdlRenderBackend_SubmitPending(&device));
    cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
    assert(ArGpuDioramaDepthPass_Encode(&pass,cmd,GpuTexture(target),W,H,draws+1,sources+1,1,
        SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false));
    assert(SDL_SubmitGPUCommandBuffer(cmd));
    Read(&device,target,with_background);
    /* Nonzero filtered coverage keeps depth even when the original centre
     * texel is a hole. Empty filtered coverage admits the later far surface.
     * This deliberately tests cutout semantics, not fractional transmission. */
    for(unsigned y=8;y<88;++y)for(unsigned x=8;x<120;++x) {
      const unsigned i=(y*W+x)*4;
      if(actual[i+3])++covered;
      else { ++holes;actual[i]=20;actual[i+1]=31;actual[i+2]=10;actual[i+3]=255; }
    }
    Compare(with_background,actual,scenario);
    if(!actor && scenario==0 && !linear && !fade)Reject(&pass,GpuTexture(target),draws[0],sources[0],GpuTexture(light));
  }
  if(!hybrid)assert(covered>10000 && holes>10000);
  printf("Shared-depth materials%s: %u renders, %u pixels; ordinary scenery/actor lighting, rim/DOF/priority/grade/heat/cloud parity passed%s\n",
      hybrid?" (bounded hybrid)":"",renders,pixels_checked,
      hybrid?"":"; final-alpha cutout depth, failure and reuse also passed");
  DioramaEffectBackend_Reset(&device);ArGpuDioramaDepthPass_Destroy(&pass);
  const ArRenderTexture resources[]={reference,target,art[0],art[1],light,black,attributes,enemy};
  assert(ArRenderDevice_SetRenderTarget(&device,ArRenderTexture_Invalid()));
  for(unsigned i=0;i<sizeof(resources)/sizeof(*resources);++i)ArRenderDevice_DestroyTexture(&device,resources[i]);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();return 0;
}
