#include "action_woodland.h"
#include "deterministic_hash.h"
#include <math.h>
#include <string.h>

static float Unit(uint32_t seed) {return DeterministicHash_Unit32(seed);}
static float Smooth(float t) {t=fmaxf(0,fminf(1,t));return t*t*(3-2*t);}

ActionFlowerMotion ActionWoodland_FlowerMotion(const ActionSceneEffectFrame *frame,const ActionEffectInstance *e) {
  if(!e->particle_lifetime)return (ActionFlowerMotion){0};
  return (ActionFlowerMotion){
    .phase=(e->pulse_ticks&65535u)*(65536u/e->particle_lifetime)*(6.2831853f/65536.f),
    .wind=ActionFieldMotion_Wind(e),
    .wakes=ActionFieldMotion_Gather(frame,e,&e->geometry.data.rect,false)};
}
bool ActionWoodland_Flower(const ActionEffectInstance *e,const ActionEffectFloorField *floor,
    unsigned plant,const ActionFlowerMotion *motion,ActionFlowerPose *out) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!floor||floor->count>kActionAuthoredFloorMaxSpans||!s->strands||s->strands>kActionFlowerMaxPlants||
      plant>=s->strands||s->flower_style>3)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const uint32_t seed=e->particle_style.seed^(plant+1)*0x9e3779b9u;
  const float u=(plant+.15f+.7f*Unit(seed))/s->strands,spread=2*u-1;
  const float x=(r->x0+r->x1)*.5f+(r->x1-r->x0)*.5f*spread*(.65f+.35f*fabsf(spread));
  const ActionEffectFloorSpan *support=NULL;
  for(unsigned i=0;i<floor->count;++i)
    if(x+e->world_x>=floor->spans[i].x0&&x+e->world_x<floor->spans[i].x1){support=&floor->spans[i];break;}
  if(!support||support->height<6)return false;
  const float back=fminf(s->ground_width,support->height*.25f)*Unit(seed^157);
  const float root=support->y-e->world_y-back,variety=Unit(seed^113);
  const unsigned species=s->flower_style?s->flower_style-1:variety<.56f?0:variety<.85f?1:2;
  const float height=fminf(species==1?26:22,support->height-back-3)*
      (.56f+.24f*Unit(seed^17))*(species==1?1.08f:species==2?.82f:1);
  const float wind=(.7f*sinf(motion->phase+(e->world_x+x)*.019f)+
      .3f*sinf(motion->phase*2+Unit(seed^91)*6.2831853f))*motion->wind;
  float sway=wind*fminf(s->amplitude,height*.25f);
  for(unsigned i=0;i<kActionFieldWakeCount;++i) {
    const ActionFieldWake *wake=&motion->wakes.bodies[i];
    if(wake->strength<=0||fabsf(wake->y-(support->y-e->world_y))>12)continue;
    const float distance=(x-wake->x)/wake->radius,t=fmaxf(0,1-distance*distance);
    sway+=(wake->dx<0?-1:1)*height*.32f*wake->strength*t*t;
  }
  *out=(ActionFlowerPose){.seed=seed,.species=species,.x=x,.root=root,.back=back,.height=height,
    .tip_x=x+sway,.tip_y=root-height+fabsf(sway)*.13f,
    .floor_x0=support->x0,.floor_x1=support->x1,
    .color=species==1||Unit(seed^53)>.7f?e->particle_style.color_end:e->tuning.color};
  return true;
}
static bool Matches(const ActionWoodlandEvent *event,const ActionEffectInstance *e,unsigned index) {
  return event->valid&&event->source==e->generation&&event->kind==e->kind&&event->index==index&&
      event->definition_seed==e->particle_style.seed&&event->owner_x==e->world_x&&event->owner_y==e->world_y;
}
const ActionWoodlandEvent *ActionWoodland_Event(const ActionSceneEffectFrame *frame,
    const ActionEffectInstance *e,unsigned index) {
  if(frame)for(unsigned i=0;i<kActionWoodlandMaxEvents;++i)
    if(Matches(&frame->woodland[i],e,index))return &frame->woodland[i];
  return NULL;
}
ActionButterflyPose ActionWoodland_Butterfly(const ActionEffectInstance *e,unsigned index,
    const ActionSceneEffectFrame *frame) {
  const ActionEffectFieldStyle *s=&e->field_style;
  ActionButterflyPose p={.seed=DeterministicHash_Mix32(e->particle_style.seed^(index+1)*71u)};
  if(s->point_count<2||s->point_count>kActionContourMaxPoints||!e->particle_lifetime||s->activity<=0)return p;
  /* Whole routes divide the native clock, including routes with three perches. */
  const unsigned legs=(65536u/(e->particle_lifetime*s->point_count))*s->point_count;
  const float route=((e->pulse_ticks+p.seed)&65535u)*(legs/65536.f);
  const unsigned visit=(unsigned)route,at=visit%s->point_count,next=(at+1)%s->point_count;
  const float phase=route-visit,t=fmaxf(0,(phase-(1-s->activity))/s->activity),ease=Smooth(t);
  const float lift=sinf(t*3.1415927f),wiggle=lift*lift*e->particle_style.wander;
  p.x=s->points[at].x+(s->points[next].x-s->points[at].x)*ease+wiggle*.35f*sinf(t*15+Unit(p.seed^37)*6.28f);
  p.y=s->points[at].y+(s->points[next].y-s->points[at].y)*ease-wiggle*(.8f+.2f*sinf(t*11+Unit(p.seed^19)*6.28f));
  p.flight=fminf(1,lift*8);
  const ActionWoodlandEvent *event=s->wake>0?ActionWoodland_Event(frame,e,index):NULL;
  const unsigned age=event?(uint16_t)(e->pulse_ticks-event->born):kActionWoodlandFlightTicks;
  if(age<kActionWoodlandFlightTicks) {
    const float u=age/(float)kActionWoodlandFlightTicks,returning=Smooth((u-.45f)/.55f);
    const float arc=sinf(u*3.1415927f),lift=arc*arc;
    /* Begin at the captured perch; return continuously to the ordinary route.
     * The renderer never stores a pose or advances a private flight clock. */
    p.x=(event->x-e->world_x)*(1-returning)+p.x*returning+
        event->direction*lift*(22+3*sinf(u*12))*s->wake;
    p.y=(event->y-e->world_y)*(1-returning)+p.y*returning-lift*34*s->wake;
    p.flight=fmaxf(p.flight,fminf(1,arc*10));
  }
  return p;
}
static bool Eligible(const ActionEffectInstance *e) {
  return (e->flags&kActionEffectFlag_Visible)&&e->tuning.intensity>0&&e->field_style.wake>0&&
      ((e->kind==kActionEffect_AuthoredFoliage&&e->field_style.flowers&&e->field_style.petals)||
       (e->kind==kActionEffect_AuthoredParticlePath&&e->field_style.pattern==kActionParticle_Butterflies));
}
static bool Moving(const ActionEffectActor *a) {
  return a->visible&&a->body_radius&&a->wake.age<4&&a->wake.strength>.1f&&fabsf(a->wake.dx)>.25f;
}
static bool Available(const ActionWoodlandObserver *s,const ActionEffectInstance *e,unsigned index) {
  for(unsigned i=0;i<kActionWoodlandMaxEvents;++i)if(Matches(&s->events[i],e,index))return false;
  return true;
}
static void Emit(ActionWoodlandObserver *s,const ActionEffectInstance *e,unsigned index,
    float x,float y,float end_x,float ground_y,float direction,uint32_t color,unsigned clock) {
  for(unsigned i=0;i<kActionWoodlandMaxEvents;++i)if(!s->events[i].valid) {
    s->events[i]=(ActionWoodlandEvent){.source=e->generation,.definition_seed=e->particle_style.seed,
      .seed=DeterministicHash_Mix32(e->generation^(clock*71u)^index),.color=color,
      .x=x,.y=y,.end_x=end_x,.ground_y=ground_y,.direction=direction,
      .owner_x=e->world_x,.owner_y=e->world_y,.born=(uint16_t)clock,.kind=e->kind,.index=(uint8_t)index,.valid=true};
    return;
  }
}
void ActionWoodland_Capture(ActionWoodlandObserver *state,ActionSceneEffectFrame *frame,unsigned group,
    unsigned room,unsigned terrain,unsigned clock,bool enabled) {
  memset(frame->woodland,0,sizeof(frame->woodland));
  if(!enabled||frame->authored_count>kActionAuthoredMaxInstances||frame->actor_count>kActionEffectActorMax) {
    memset(state,0,sizeof(*state));return;
  }
  const unsigned delta=(uint16_t)(clock-state->clock);
  const bool reset=!state->valid||state->group!=group||state->room!=room||state->terrain!=terrain||delta>4;
  if(reset)memset(state,0,sizeof(*state));
  state->valid=true;state->clock=(uint16_t)clock;state->group=(uint8_t)group;state->room=(uint8_t)room;state->terrain=(uint8_t)terrain;
  /* Retire removed/disabled definitions even on a paused frame. */
  for(unsigned i=0;i<kActionWoodlandMaxEvents;++i) {
    ActionWoodlandEvent *event=&state->events[i];if(!event->valid)continue;
    bool found=false;
    for(unsigned j=0;j<frame->authored_count&&!found;++j) {
      const ActionEffectInstance *e=&frame->authored[j];
      found=Eligible(e)&&Matches(event,e,event->index)&&
          (e->kind!=kActionEffect_AuthoredParticlePath||event->index<e->particle_count);
    }
    if(!found||(uint16_t)(clock-event->born)>=kActionWoodlandCooldownTicks)event->valid=false;
  }
  if(delta&&!reset)for(unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectInstance *e=&frame->authored[i];if(!Eligible(e))continue;
    if(e->kind==kActionEffect_AuthoredParticlePath) {
      for(unsigned butterfly=0;butterfly<e->particle_count&&butterfly<4;++butterfly) {
        if(!Available(state,e,butterfly))continue;
        const ActionButterflyPose p=ActionWoodland_Butterfly(e,butterfly,NULL);
        if(p.flight>.01f)continue;
        const float x=e->world_x+p.x,y=e->world_y+p.y;
        for(unsigned j=0;j<frame->actor_count;++j) {
          const ActionEffectActor *a=&frame->actors[j];if(!Moving(a))continue;
          const float dx=x-a->x;
          if(fabsf(dx)>a->body_radius+26*e->field_style.wake||fabsf(y-(a->feet_y-16))>26||
              (dx*a->wake.dx<=0&&fabsf(dx)>a->body_radius))continue;
          Emit(state,e,butterfly,x,y,x,y,dx<0?-1:1,0,clock);break;
        }
      }
    } else if(Available(state,e,0)) {
      const ActionEffectLocalRect *r=&e->geometry.data.rect;bool nearby=false;
      for(unsigned j=0;j<frame->actor_count&&!nearby;++j) {
        const ActionEffectActor *a=&frame->actors[j];
        nearby=Moving(a)&&a->x>=e->world_x+r->x0-a->body_radius-4&&
            a->x<=e->world_x+r->x1+a->body_radius+4&&
            a->feet_y>=e->world_y+r->y0&&a->feet_y<=e->world_y+r->y1+12;
      }
      if(!nearby)continue;
      const ActionFlowerMotion motion=ActionWoodland_FlowerMotion(frame,e);
      float nearest=1e9f;ActionFlowerPose hit={0};const ActionEffectActor *body=NULL;
      for(unsigned plant=0;plant<e->field_style.strands;++plant) {
        ActionFlowerPose p;if(!ActionWoodland_Flower(e,&frame->authored_floor[i],plant,&motion,&p)||p.species==2)continue;
        for(unsigned j=0;j<frame->actor_count;++j) {
          const ActionEffectActor *a=&frame->actors[j];if(!Moving(a))continue;
          const float dx=fabsf(a->x-e->world_x-p.x);
          if(dx>a->body_radius+4||dx>=nearest||fabsf(a->feet_y-e->world_y-p.root-p.back)>12)continue;
          nearest=dx;hit=p;body=a;
        }
      }
      if(body) {
        const float side=body->wake.dx<0?-1:1,x=e->world_x+hit.tip_x;
        const float end=fmaxf(hit.floor_x0+3,fminf(hit.floor_x1-3,x+side*12));
        Emit(state,e,0,x,e->world_y+hit.tip_y,end,e->world_y+hit.root,side,hit.color,clock);
      }
    }
  }
  memcpy(frame->woodland,state->events,sizeof(frame->woodland));
}
