#include "action_field_motion.h"
#include <math.h>

/* A shared, smooth wind envelope. Scrubbing and camera movement cannot
 * restart or teleport a gust. */
float ActionFieldMotion_Wind(const ActionEffectInstance *e) {
  const float phase=(e->pulse_ticks%4096)*6.2831853f/4096;
  const float gust=.5f+.3f*sinf(phase*5)+.2f*sinf(phase*11+.7f);
  return 1-e->field_style.gust*(1-gust);
}
float ActionFieldMotion_GroundHeight(const ActionEffectFieldStyle *s, float x) {
  unsigned i=1;
  while(i+1<s->point_count && x>s->points[i].x)++i;
  const float t=fmaxf(0,fminf(1,(x-s->points[i-1].x)/(s->points[i].x-s->points[i-1].x)));
  return s->points[i-1].y+t*(s->points[i].y-s->points[i-1].y);
}
/* One player and the strongest nearby enemy, with no renderer history. */
ActionFieldWakes ActionFieldMotion_Gather(const ActionSceneEffectFrame *frame,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,bool ground) {
  ActionFieldWakes out={0};
  if(!frame||e->field_style.wake<=0||frame->actor_count>kActionEffectActorMax)return out;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  for(unsigned i=0;i<frame->actor_count;++i) {
    const ActionEffectActor *a=&frame->actors[i];const ActionActorWake *wake=&a->wake;
    if(!a->visible||!a->body_radius||wake->strength<=0||wake->age>=kActionActorWakeTicks)continue;
    const float x=wake->x-e->world_x,y=wake->y-e->world_y;
    if(x<clip->x0-72||x>clip->x1+72||y<clip->y0-72||y>clip->y1+72)continue;
    if(ground) {
      const ActionEffectFieldStyle *s=&e->field_style;
      if(x<s->points[0].x||x>s->points[s->point_count-1].x||fabsf(y-ActionFieldMotion_GroundHeight(s,x))>12)continue;
    } else if(x<r->x0-32||x>r->x1+32||y<r->y0||y>r->y1+32)continue;
    const float age=wake->age/(float)kActionActorWakeTicks;
    const float strength=e->field_style.wake*wake->strength*(1-age)*(1-age);
    const unsigned slot=a->player?0:1;
    if(strength>out.bodies[slot].strength)
      out.bodies[slot]=(ActionFieldWake){x,y,wake->dx,a->body_radius+28,strength,wake->age,a->generation};
  }
  return out;
}
