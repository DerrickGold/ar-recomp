/* Reusable source binding and aggregate capture; visual data is never inferred
 * from room identity. Group/room/terrain selection belongs to the recipe loader. */
#include "action_environment_scene.h"
#include "action_environment_capture_internal.h"
#include <math.h>

void ActionRayField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *dst,const ActionRayField *field,uint32_t source) {
  if(!scene||!dst||!field)return;
  for(unsigned bg=0;bg<2;++bg) {
    if(field->dimensions[bg*2]&&scene->maps[bg].world_width!=(unsigned)field->dimensions[bg*2])return;
    if(field->dimensions[bg*2+1]&&scene->maps[bg].world_height!=(unsigned)field->dimensions[bg*2+1])return;
  }
  for(unsigned i=0;i<field->witness_count;++i) {
    const ActionRayWitness *w=&field->witnesses[i];uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[w->bg],w->x,w->y,&tile)||tile!=w->tile)return;
  }
  const int cx=(scene->camera_x[0]+scene->camera_x[1])/2;
  const int cy=(scene->camera_y[0]+scene->camera_y[1])/2;
  const int x=cx+(int)lroundf(field->anchor[0]),y=cy+(int)lroundf(field->anchor[1]);
  if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)return;
  const ActionEffectInstance effect={
    .generation=source,.pulse_generation=field->pulse_seed,
    .world_x=(int16_t)x,.world_y=(int16_t)y,
    .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
    .kind=kActionEffect_ForestCanopyLight,.phase=kActionEffectPhase_ForestCanopyLight,
    .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer=kActionEffectRenderLayer_Bg2Plane,
    .projection_plane=kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={field->window[0],field->window[1],field->window[2],field->window[3]}},
    .clip_rect={field->bounds[0]+cx-scene->camera_x[1]-x,field->bounds[1]+cy-scene->camera_y[1]-y,
                field->bounds[2]+cx-scene->camera_x[1]-x,field->bounds[3]+cy-scene->camera_y[1]-y},
  };
  const unsigned count=dst->decoration_count,visible=dst->decoration_visible_count;
  if(field->components&3) {
    if(!SceneDecorationAppend(dst,&effect))goto overflow;
  }
  if(field->components&4) {
    ActionEffectInstance leaves=effect;leaves.kind=kActionEffect_ForestLeaves;
    leaves.render_layer=kActionEffectRenderLayer_Bg2Alpha;
    if(!SceneDecorationAppend(dst,&leaves))goto overflow;
  }
  if(field->components&8) {
    ActionEffectInstance forward=effect;forward.kind=kActionEffect_ForestForwardLight;
    forward.render_layer=kActionEffectRenderLayer_ForegroundLight;
    if(!SceneDecorationAppend(dst,&forward))goto overflow;
  }
  dst->ray_field=*field;dst->ray_field_valid=1;
  return;
overflow:
  dst->decoration_count=(uint8_t)count;dst->decoration_visible_count=(uint8_t)visible;
}
