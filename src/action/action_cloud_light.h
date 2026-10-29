#ifndef AR_ACTION_CLOUD_LIGHT_H
#define AR_ACTION_CLOUD_LIGHT_H
/* Convert authored cloud-opening recipes to a bounded, world-space material field. */
#include "action_effects.h"
#include "diorama/diorama_cloud_light.h"
_Static_assert(kDioramaCloudLights==4,"rainbow packets store four incident sunlight weights");

static inline DioramaCloudLight ActionCloudLight_PrepareWithSun(const ActionSceneEffectFrame *frame,
    float x,float y,float width,float height,float directions[3][4]) {
  DioramaCloudLight out={.source={x,y,width,height}};
  if(directions)for(unsigned axis=0;axis<3;++axis)
    for(unsigned i=0;i<4;++i)directions[axis][i]=0;
  if(!frame||frame->authored_count>kActionAuthoredMaxInstances)return out;
  unsigned count=0;
  for(unsigned i=0;i<frame->authored_count&&count<kDioramaCloudLights;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    if(e->kind!=kActionEffect_AuthoredCloudLight||!(e->flags&kActionEffectFlag_Visible)||
        e->tuning.intensity<=0||!e->particle_lifetime)continue;
    const ActionEffectLocalRect *b=&e->geometry.data.rect;
    const ActionEffectFieldStyle *s=&e->field_style;
    if(b->x1<=b->x0||b->y1<=b->y0)continue;
    if(directions) {
      const float angle=s->angle*.01745329252f,depth=s->cloud.sun_depth*.01745329252f;
      /* World Y points up and the camera looks toward negative Z. Positive
       * depth puts the sun behind the viewer, shining into the scene. */
      directions[0][count]=sinf(angle)*cosf(depth);
      directions[1][count]=-cosf(angle)*cosf(depth);
      directions[2][count]=-sinf(depth);
    }
    out.background_dim[count]=1-s->cloud.background_brightness;
    out.meta[1]=fmaxf(out.meta[1],out.background_dim[count]);
    DioramaCloudLightRegion *r=&out.regions[count++];
    *r=(DioramaCloudLightRegion){
      .bounds={e->world_x+b->x0,e->world_y+b->y0,b->x1-b->x0,b->y1-b->y0},
      .core={s->cloud.edge,s->cloud.halo,s->cloud.streak,s->cloud.echo},
      .shape={s->cloud.falloff,s->cloud.cover,s->cloud.edge_position,s->cloud.echo_position},
      .motion={tanf(s->angle*.01745329252f),
          (e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime,
          DioramaCloudLight_Seed(e->particle_style.seed),s->strands},
      .color={((e->tuning.color>>16)&255)/255.f,((e->tuning.color>>8)&255)/255.f,
          (e->tuning.color&255)/255.f,e->tuning.intensity},
      .spill={((e->particle_style.color_end>>16)&255)/255.f,
          ((e->particle_style.color_end>>8)&255)/255.f,
          (e->particle_style.color_end&255)/255.f,s->softness}};
  }
  out.meta[0]=(float)count;
  return out;
}
static inline DioramaCloudLight ActionCloudLight_Prepare(const ActionSceneEffectFrame *frame,
    float x,float y,float width,float height) {
  return ActionCloudLight_PrepareWithSun(frame,x,y,width,height,NULL);
}
#endif
