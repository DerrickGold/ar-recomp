#include "action_environment_capture_internal.h"

void ActionMoonField_Capture(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,
    const ActionMoonField *field,uint32_t source) {
  if(!scene||!dst||!field||dst->decoration_overflow)return;
  for(unsigned bg=0;bg<2;++bg) {
    if((field->Dimensions[bg*2]&&scene->maps[bg].world_width!=field->Dimensions[bg*2])||
       (field->Dimensions[bg*2+1]&&scene->maps[bg].world_height!=field->Dimensions[bg*2+1]))return;
  }
  const float *witness[]={field->Witness1,field->Witness2};
  for(unsigned i=0;i<(unsigned)field->WitnessCount[0];++i) {
    const float *w=witness[i];uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[(unsigned)w[0]],(int)w[1],(int)w[2],&tile)||tile!=(unsigned)w[3])return;
  }
  if(!(unsigned)field->Components[0]) {
    dst->moon_field=*field;dst->moon_field_valid=true;return;
  }
  if(dst->decoration_count>kActionSceneDecorationMaxInstances-3)return;
  /* Keep the original batch order: water, mist, rays, reflections, timber,
   * insects, cloud. A recipe replacement must not reorder alpha compositing. */
  unsigned insert=dst->decoration_count;uint16_t sources=0;
  for(unsigned i=0;i<dst->decoration_count;++i) {
    if(dst->decorations[i].kind==kActionEffect_BloodpoolWater)sources=dst->decorations[i].source_mask;
    if(dst->decorations[i].kind==kActionEffect_BloodpoolTimber&&insert==dst->decoration_count)insert=i;
  }
  ActionEffectInstance effects[3];
  for(unsigned i=0;i<3;++i) {
    const float *rect=i==2?field->CloudWindow:field->Window;
    effects[i]=(ActionEffectInstance){
      .generation=source+(i==2?4:i),.pulse_generation=source+(i==2?4:i),
      .world_x=(int16_t)field->Anchor[0],.world_y=(int16_t)field->Anchor[1],
      .environment_room=scene->room,.age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=i==0?kActionEffect_BloodpoolMoonlight:i==1?kActionEffect_BloodpoolMoonReflection:kActionEffect_BloodpoolCloud,
      .phase=kActionEffectPhase_BloodpoolEnvironment,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect|(i==1?0:kActionEffectFlag_StaticAnchor),
      .source_mask=i==2?sources:0,
      .render_layer=i==2?kActionEffectRenderLayer_Bg2Alpha:kActionEffectRenderLayer_Bg2Plane,
      .projection_plane=kActionEffectProjectionPlane_Bg2,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={rect[0],rect[1],rect[2],rect[3]}},
      .clip_rect={rect[0],rect[1],rect[2],rect[3]},
    };
  }
  memmove(dst->decorations+insert+2,dst->decorations+insert,(dst->decoration_count-insert)*sizeof(effects[0]));
  memcpy(dst->decorations+insert,effects,2*sizeof(effects[0]));
  dst->decoration_count+=2;dst->decoration_visible_count+=2;
  (void)SceneDecorationAppend(dst,&effects[2]);
  dst->moon_field=*field;dst->moon_field_valid=true;
  dst->bloodpool.water_scroll_valid=scene->water_scroll_valid;
  if(scene->water_scroll_valid)memcpy(dst->bloodpool.water_scroll,scene->water_scroll,sizeof(dst->bloodpool.water_scroll));
}
