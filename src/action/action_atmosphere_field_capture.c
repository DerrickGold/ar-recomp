/* Complete world-space atmosphere, shared by native capture and editor reconstruction. */
#include "action_environment_capture_internal.h"
#include "action_atmosphere_field.h"
#include "action_floor_support.h"
#include <math.h>
_Static_assert(kActionLandingDustMaxPuffs + 7 + kActionTempleMistMaxSpans <=
                   kActionSceneDecorationMaxInstances,
               "temple floor mist must leave room for landing dust and ambient fields");

static int TempleMistFloor(const ActionEnvironmentScene *scene, const ActionAtmosphereField *field, int x) {
  /* Search only the lower hall using the same exposed-floor predicate as
   * authored mist and the editor collision overlay. */
  const int top = (int)field->FloorArea[1];
  const int bottom = (int)field->FloorArea[3];
  for (int y=top;y<=bottom;y+=16)
    if (ActionFloorSupport_Cell(scene,x,y)&16) return y;
  return 0;
}

static void CaptureTempleMist(ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene, const ActionAtmosphereField *field) {
  const unsigned room=scene->room;
  const uint16_t clock=scene->clock;
  const uint8_t count_before = dst->decoration_count;
  const uint8_t visible_before = dst->decoration_visible_count;
  const int start = (int)field->FloorArea[0];
  const int end = (int)field->FloorArea[2];
  int left = start, floor = 0;
  unsigned spans = 0;
  for (int x = start; x <= end; x += 16) {
    const int next_floor = x < end ? TempleMistFloor(scene,field,x) : 0;
    if (next_floor == floor) continue;
    if (floor) {
      /* A changed/fragmented map may exceed the cosmetic budget. Omit only
       * this family instead of consuming actor or landing-particle records. */
      if (++spans > kActionTempleMistMaxSpans ||
          dst->decoration_count >= kActionSceneDecorationMaxInstances) {
        dst->decoration_count = count_before;
        dst->decoration_visible_count = visible_before;
        return;
      }
      const ActionEffectInstance effect = {
        .generation = ((uint32_t)field->FloorSource[0] << 16) | (unsigned)left,
        .pulse_generation = ((uint32_t)field->FloorSource[1] << 16) | (unsigned)left,
        .world_x = (int16_t)left, .world_y = (int16_t)floor, .environment_room = (uint16_t)room,
        .age_ticks = clock, .phase_ticks = clock, .pulse_ticks = clock,
        .kind = kActionEffect_TempleGroundMist, .phase = kActionEffectPhase_CaveEnvironment,
        .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
        .render_layer = kActionEffectRenderLayer_Bg1Mist,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,-field->FloorStyle[0],x-left,0}},
        .clip_rect = {0,-field->FloorStyle[0],x-left,0},
      };
      (void)SceneDecorationAppend(dst, &effect);
    }
    left = x;
    floor = next_floor;
  }
}

void ActionAtmosphereField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *dst,const ActionAtmosphereField *field,uint32_t source) {
  if(!scene||!dst||!field)return;
  for(unsigned bg=0;bg<2;++bg) {
    if(field->Dimensions[bg*2]&&scene->maps[bg].world_width!=(unsigned)field->Dimensions[bg*2])return;
    if(field->Dimensions[bg*2+1]&&scene->maps[bg].world_height!=(unsigned)field->Dimensions[bg*2+1])return;
  }
  for(unsigned i=0;i<(unsigned)field->Counts[4];++i) {
    const float *w=ActionAtmosphereField_Witness(field,i);uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[(unsigned)w[0]],(int)w[1],(int)w[2],&tile)||tile!=(unsigned)w[3])return;
  }
  static const struct { unsigned kind,component,layer; } kernels[]={
    {kActionEffect_TempleDust,1,kActionEffectRenderLayer_WorldOverlay},
    {kActionEffect_TowerWindowLight,2,kActionEffectRenderLayer_ForegroundLight},
    {kActionEffect_CaveAmbientLight,4,kActionEffectRenderLayer_ForegroundLight},
    {kActionEffect_TempleGrit,8,kActionEffectRenderLayer_WorldDust},
  };
  const unsigned count=dst->decoration_count,visible=dst->decoration_visible_count;
  const int x=scene->camera_x[0]+(int)lroundf(field->Anchor[0]);
  const int y=scene->camera_y[0]+(int)lroundf(field->Anchor[1]);
  if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)return;
  for(unsigned i=0;i<sizeof(kernels)/sizeof(kernels[0]);++i) {
    if(!((unsigned)field->Components[0]&kernels[i].component))continue;
    ActionEffectInstance effect={
      .generation=source|kernels[i].kind,.pulse_generation=(source^0x10000000u)|kernels[i].kind,
      .world_x=(int16_t)x,.world_y=(int16_t)y,.environment_room=scene->room,
      .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=kernels[i].kind,.phase=kActionEffectPhase_CaveEnvironment,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=kernels[i].layer,.projection_plane=kActionEffectProjectionPlane_Bg1,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={field->Window[0],field->Window[1],field->Window[2],field->Window[3]}},
      .clip_rect={field->Bounds[0]-x,field->Bounds[1]-y,field->Bounds[2]-x,field->Bounds[3]-y},
    };
    if(effect.kind==kActionEffect_CaveAmbientLight||effect.kind==kActionEffect_TowerWindowLight){
      effect.tuning.dim_receivers_set=1;effect.tuning.dim_receivers=(uint8_t)field->Receivers[0];
      effect.tuning.light_receivers_set=field->Receivers[1]<8;effect.tuning.light_receivers=(uint8_t)field->Receivers[1];
    }
    if(!SceneDecorationAppend(dst,&effect))goto overflow;
  }
  if((unsigned)field->Components[0]&16)CaptureTempleMist(dst,scene,field);
  dst->atmosphere_field=*field;dst->atmosphere_field_valid=1;return;
overflow:
  dst->decoration_count=(uint8_t)count;dst->decoration_visible_count=(uint8_t)visible;
}
