/* Exercise the exact WASM sink boundary without starting a GPU or game. */
#include "diorama/diorama_depth_pass.h"
#include "diorama/diorama_effect_backend.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static ArRenderDevice s_device;
static ArRenderTextureDesc s_textures[128];
static bool s_depth_active, s_depth_failed;
static bool begin_ok=true, clear_ok=true, draw_ok=true, finish_ok=true, bind_ok=true;
static unsigned begins, clears, draws, finishes, binds, unbinds;
static int last_mode, last_finish, last_filter, last_blend;
static int current_effect=-1, submitted_effect=-1;
static const void *last_vertices, *last_indices;
static const float *last_depths, *last_w;
static int WebDepthBegin(void) { ++begins; return begin_ok; }
static int WebDepthClear(void) { ++clears; return clear_ok; }
static int WebDepthFinish(int success) { ++finishes; last_finish=success; return finish_ok; }
static int WebDepthGeometry(int texture,const void *vertices,int nv,const void *indices,
    int ni,int blend,int u,int v,int filter,const float *depths,const float *w,int mode) {
  assert(texture==7 && nv==3 && ni==3 && u==0 && v==0);
  ++draws; last_mode=mode; last_filter=filter; last_blend=blend;
  submitted_effect=current_effect;
  last_vertices=vertices; last_indices=indices; last_depths=depths; last_w=w;
  return draw_ok;
}
bool DioramaEffectBackend_BindSceneryLight(ArRenderDevice *device,
    const DioramaSceneryLightEffectParams *params) {
  assert(device==&s_device && params && params->illumination.value==4);
  ++binds; if (bind_ok) current_effect=4; return bind_ok;
}
bool DioramaEffectBackend_BindRimLight(ArRenderDevice *device,
    const DioramaRimLightEffectParams *params) {
  assert(device==&s_device && params->strength==.33f);
  assert(params->texel_width==1.f/SR_PPU_SURFACE_MAX_WIDTH);
  current_effect=1; return true;
}
bool DioramaEffectBackend_BindDofEdge(ArRenderDevice *device,
    const DioramaDofEdgeEffectParams *params) {
  assert(device==&s_device && params->blur_radius==2 && !params->edge_feather);
  current_effect=2; return true;
}
bool DioramaEffectBackend_BindPrioritySurface(ArRenderDevice *device,
    const DioramaPrioritySurfaceEffectParams *params) {
  assert(device==&s_device && params->width==320 && params->high_band);
  current_effect=3; return true;
}
bool DioramaEffectBackend_Unbind(ArRenderDevice *device) {
  assert(device==&s_device); ++unbinds; current_effect=-1; return true;
}
#include "../tools/action_editor/compositor_preview_depth.inc"

int main(void) {
  const ArRenderVertex2D vertices[3]={
    {{5,10},{1,1,1,1},{0,0}}, {{51,16},{1,1,1,1},{1,0}}, {{22,44},{1,1,1,1},{0,1}}};
  const int32_t indices[]={0,1,2};
  const float depths[]={.2f,.4f,.3f}, w[]={1,2,3};
  s_textures[7].width=32;
  DioramaDepthSubmission draw={.texture={7},.vertices=vertices,.depths=depths,
      .indices=indices,.vertex_count=3,.index_count=3,.blend=kArRenderBlendMode_Alpha,.clip_w=w};
  /* Inactive modes cannot accidentally emit a shared-depth draw. The room
   * compositor installs this sink only when the Voxel mode requires it. */
  assert(!DepthSubmit(NULL,&s_device,&draw) && !draws);
  begin_ok=false; assert(!DioramaPreview_DepthBegin() && !s_depth_active);
  begin_ok=true;
  const DioramaDepthSink *sink=DioramaPreview_DepthBegin(); assert(sink);
  const unsigned started=begins;
  assert(!DioramaPreview_DepthBegin() && begins==started);
  assert(sink->submit(sink->userdata,&s_device,&draw));
  assert(last_mode==kDioramaDepth_Write && last_filter==-1 && last_blend==kArRenderBlendMode_Alpha);
  assert(last_vertices==vertices && last_indices==indices && last_depths==depths && last_w==w);
  assert(!binds && !unbinds && current_effect==-1 && submitted_effect==-1);
  /* Native meshes can enter with a caller-bound rim/DOF material. Depth
   * visibility must preserve that material during and after the draw. */
  for (int effect=1; effect<=2; ++effect) {
    current_effect=effect;
    assert(sink->submit(sink->userdata,&s_device,&draw));
    assert(submitted_effect==effect && current_effect==effect && !binds && !unbinds);
  }
  current_effect=-1;
  draw.clip_w=NULL; draw.native_actor=true;
  assert(sink->submit(sink->userdata,&s_device,&draw) && last_mode==kDioramaDepth_NativeActor);
  draw.blend=kArRenderBlendMode_Add;
  assert(sink->submit(sink->userdata,&s_device,&draw) && last_mode==kDioramaDepth_NativeActor);
  draw.native_actor=false;
  assert(sink->submit(sink->userdata,&s_device,&draw) && last_mode==kDioramaDepth_Read);
  DioramaSceneryLightEffectParams unlit={.rim_strength=.33f};
  draw.lighting=&unlit; draw.native_actor=true;
  assert(sink->submit(sink->userdata,&s_device,&draw));
  assert(submitted_effect==1 && current_effect==-1 && unbinds==1);
  unlit.mode=1; unlit.rim_strength=0; unlit.blur_radius=2;
  assert(sink->submit(sink->userdata,&s_device,&draw));
  assert(submitted_effect==2 && current_effect==-1 && unbinds==2);
  unlit.mode=2; unlit.priority.width=320; unlit.priority.high_band=true;
  draw.native_actor=false;
  assert(sink->submit(sink->userdata,&s_device,&draw));
  assert(submitted_effect==3 && current_effect==-1 && unbinds==3);
  const DioramaSceneryLightEffectParams lighting={.illumination={4}};
  draw.lighting=&lighting;
  assert(sink->submit(sink->userdata,&s_device,&draw) && binds==1);
  assert(submitted_effect==4 && current_effect==-1 && unbinds==4);
  assert(sink->clear(sink->userdata,&s_device) && clears==1);
  assert(sink->finish(sink->userdata,&s_device,true) && last_finish==1 && !s_depth_active);

  sink=DioramaPreview_DepthBegin(); assert(sink);
  draw_ok=false;
  assert(!sink->submit(sink->userdata,&s_device,&draw));
  const unsigned failed_draws=draws;
  assert(!sink->submit(sink->userdata,&s_device,&draw) && draws==failed_draws);
  assert(sink->finish(sink->userdata,&s_device,false) && last_finish==0 && !s_depth_active);
  draw_ok=true;
  sink=DioramaPreview_DepthBegin(); assert(sink);
  draw.native_actor=true; draw.clip_w=w;
  assert(!sink->submit(sink->userdata,&s_device,&draw) && draws==failed_draws);
  assert(!sink->finish(sink->userdata,&s_device,true) && last_finish==0 && !s_depth_active);
  draw.native_actor=false; draw.clip_w=NULL;
  sink=DioramaPreview_DepthBegin(); assert(sink);
  clear_ok=false;
  assert(!sink->clear(sink->userdata,&s_device));
  assert(sink->finish(sink->userdata,&s_device,false) && !s_depth_active);
  clear_ok=true;
  sink=DioramaPreview_DepthBegin(); assert(sink);
  finish_ok=false;
  assert(!sink->finish(sink->userdata,&s_device,true) && !s_depth_active);
  finish_ok=true;
  sink=DioramaPreview_DepthBegin(); assert(sink);
  bind_ok=false;
  assert(!sink->submit(sink->userdata,&s_device,&draw));
  assert(sink->finish(sink->userdata,&s_device,false) && !s_depth_active);
  assert(finishes==6);
  puts("action editor depth bridge: camera depth/W, native actor priority and abort scopes passed");
  return 0;
}
