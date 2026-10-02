#include "action_environment_capture_internal.h"

static bool CaptureGlowSource(ActionSceneEffectFrame *dst,const ActionEnvironmentScene *scene,
    const ActionGlowField *field,uint32_t source,int x,int y,uint32_t identity){
  if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)return true;
  ActionEffectInstance effect={
    .generation=source^identity,.pulse_generation=(source^0x20000000u)^identity,
    .world_x=(int16_t)x,.world_y=(int16_t)y,
    .left_extent=(uint16_t)-field->Geometry[0],.top_extent=(uint16_t)-field->Geometry[1],
    .right_extent=(uint16_t)field->Geometry[2],.bottom_extent=(uint16_t)field->Geometry[3],
    .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
    .kind=kActionEffect_WallTorch,.phase=kActionEffectPhase_WallTorch,.role=kActionEffectRole_Body,
    .flags=kActionEffectFlag_Visible,.render_layer=kActionEffectRenderLayer_Bg1Plane,.projection_plane=kActionEffectProjectionPlane_Bg1,
    .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={field->Geometry[0],field->Geometry[1],field->Geometry[2],field->Geometry[3]}},
  };
  if(field->Receivers[0]<8){effect.tuning.light_receivers_set=1;effect.tuning.light_receivers=(uint8_t)field->Receivers[0];}
  return SceneDecorationAppend(dst,&effect);
}
void ActionGlowField_Capture(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,
    const ActionGlowField *field,uint32_t source){
  if(!scene||!dst||!field||dst->decoration_overflow)return;
  dst->glow_field=*field;dst->glow_field_valid=true;
  if(!(unsigned)field->Components[0])return;
  if(field->Mode[0]){
    for(unsigned i=0;i<(unsigned)field->SourceCount[0];++i)
      if(!CaptureGlowSource(dst,scene,field,source,(int)field->sources[i][0],(int)field->sources[i][1],(unsigned)field->sources[i][2]))return;
    return;
  }
  const ActionBgMapView map=scene->maps[0];SceneBgScanBounds bounds;
  if(!map.map||!SceneBgScanBounds_Init(&bounds,&map,field->MapRule[3]!=0,scene->camera_x[0],scene->camera_y[0]))return;
  for(unsigned y=bounds.y0;y<bounds.y1;y+=kActionBgMetatilePixels)
    for(unsigned x=bounds.x0;x<bounds.x1;x+=kActionBgMetatilePixels){
      uint8_t top,bottom;
      if(!ActionBgMapView_LookupMetatile(&map,(int)x,(int)y,&top)||top!=(unsigned)field->MapRule[0])continue;
      if(field->MapRule[2]&&(!ActionBgMapView_LookupMetatile(&map,(int)x,(int)y+kActionBgMetatilePixels,&bottom)||bottom!=(unsigned)field->MapRule[1]))continue;
      const uint32_t identity=((uint32_t)(y/kActionBgMetatilePixels)<<16)|(uint32_t)(x/kActionBgMetatilePixels);
      if(!CaptureGlowSource(dst,scene,field,source,(int)x+(int)field->Anchor[0],(int)y+(int)field->Anchor[1],identity))return;
    }
}
