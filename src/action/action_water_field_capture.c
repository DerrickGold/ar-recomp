/* Read-only material/source bindings. Recipe selection owns room identity;
 * the reusable field itself has no stage-specific visual dispatch. */
#include "action_environment_capture_internal.h"
#include "action_water_field.h"
#include <math.h>

void ActionWaterField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *dst,const ActionWaterField *field,uint32_t source) {
  if(!scene||!dst||!field)return;
  for(unsigned bg=0;bg<2;++bg) {
    if(field->Dimensions[bg*2]&&scene->maps[bg].world_width!=(unsigned)field->Dimensions[bg*2])return;
    if(field->Dimensions[bg*2+1]&&scene->maps[bg].world_height!=(unsigned)field->Dimensions[bg*2+1])return;
  }
  for(unsigned i=0;i<(unsigned)field->Counts[3];++i) {
    const float *w=ActionWaterField_Witness(field,i);uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[(unsigned)w[0]],(int)w[1],(int)w[2],&tile)||tile!=(unsigned)w[3])return;
  }
  uint16_t wet=0;
  for(unsigned i=0;i<(unsigned)field->Counts[2];++i) {
    const ActionCaveWetSource s=ActionWaterField_Wet(field,i);uint8_t tip,landing;
    if(ActionBgMapView_LookupMetatile(&scene->maps[0],s.x,s.ceiling_y-1,&tip)&&
       ActionBgMapView_LookupMetatile(&scene->maps[0],s.x,s.landing_y,&landing)&&
       tip==s.ceiling_tile&&landing==s.landing_tile)wet|=(uint16_t)(1u<<i);
  }
  static const struct { unsigned kind,component,layer,bg; } kernels[]={
    {kActionEffect_CaveWater,1,kActionEffectRenderLayer_Bg2HighPlane,1},
    {kActionEffect_CaveDrips,2,kActionEffectRenderLayer_WorldOverlay,0},
    {kActionEffect_CaveMist,4,kActionEffectRenderLayer_WorldDust,1},
    {kActionEffect_CaveSheen,8,kActionEffectRenderLayer_Bg1Plane,0},
  };
  const unsigned count=dst->decoration_count,visible=dst->decoration_visible_count;
  for(unsigned i=0;i<sizeof(kernels)/sizeof(kernels[0]);++i) {
    if(!((unsigned)field->Components[0]&kernels[i].component))continue;
    const unsigned bg=kernels[i].bg;
    const int x=scene->camera_x[bg]+(int)lroundf(field->Anchor[0]);
    const int y=scene->camera_y[bg]+(int)lroundf(field->Anchor[1]);
    if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)goto overflow;
    const ActionEffectInstance effect={
      .generation=source|kernels[i].kind,.pulse_generation=(source^0x10000000u)|kernels[i].kind,
      .world_x=(int16_t)x,.world_y=(int16_t)y,.environment_room=scene->room,
      .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=kernels[i].kind,.phase=kActionEffectPhase_CaveEnvironment,
      .source_mask=(kernels[i].component&10)?wet:0,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=kernels[i].layer,
      .projection_plane=bg?kActionEffectProjectionPlane_Bg2High:kActionEffectProjectionPlane_Bg1,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={field->Window[0],field->Window[1],field->Window[2],field->Window[3]}},
      .clip_rect={field->Bounds[0]-x,field->Bounds[1]-y,field->Bounds[2]-x,field->Bounds[3]-y},
    };
    if(!SceneDecorationAppend(dst,&effect))goto overflow;
  }
  dst->water_field=*field;dst->water_field_valid=1;return;
overflow:
  dst->decoration_count=(uint8_t)count;dst->decoration_visible_count=(uint8_t)visible;
}
