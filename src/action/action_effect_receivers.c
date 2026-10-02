#include "action_effect_receivers.h"
#include "action_environment_exposure.h"
#include "action_light_kinds.h"
#include "render/scenery_dimming.h"
#include <math.h>
#include <string.h>

bool ActionEffectReceivers_IsLight(unsigned kind) {
  return ActionLightKind_Supported(kind);
}
uint8_t ActionEffectReceivers_Layer(const ActionEffectInstance *e) {
  if(e->tuning.light_receivers_set && ActionEffectReceivers_IsLight(e->kind) &&
      (e->render_layer==kActionEffectRenderLayer_WorldOverlay || e->render_layer==kActionEffectRenderLayer_ForegroundLight))
    return e->render_layer==kActionEffectRenderLayer_ForegroundLight ?
        kActionEffectRenderLayer_Bg1Light : kActionEffectRenderLayer_Bg1Plane;
  return e->render_layer;
}
typedef struct ReceiverProjection {int x,y,bg2_x,bg2_y;} ReceiverProjection;
static bool Project(void *context,const ActionEffectInstance *e,float x,float y,ArRenderPointF *point) {
  const ReceiverProjection *p=context;
  const bool bg2=e->projection_plane==kActionEffectProjectionPlane_Bg2 || e->projection_plane==kActionEffectProjectionPlane_Bg2High;
  *point=(ArRenderPointF){e->world_x+x+(bg2?p->x-p->bg2_x:0),e->world_y+y+(bg2?p->y-p->bg2_y:0)};
  return isfinite(point->x)&&isfinite(point->y);
}
static bool Clip(void *context,const ActionEffectInstance *e,ActionEffectLocalRect *clip) {
  const ReceiverProjection *p=context;ArRenderPointF origin;Project(context,e,0,0,&origin);
  *clip=e->geometry.data.rect;
  clip->x0=fmaxf(clip->x0,p->x-384-origin.x);clip->x1=fminf(clip->x1,p->x+640-origin.x);
  clip->y0=fmaxf(clip->y0,p->y-128-origin.y);clip->y1=fminf(clip->y1,p->y+352-origin.y);
  return clip->x1>clip->x0&&clip->y1>clip->y0;
}
bool ActionEffectReceivers_Prepare(const ActionSceneEffectFrame *frame,
    unsigned group,unsigned room,int camera_x,int camera_y,int bg2_x,int bg2_y,bool lighting_enabled,ActionReceiverLighting *out) {
  if(!frame||!out||frame->effect_count>kActionSceneEffectMaxInstances||frame->authored_count>kActionAuthoredMaxInstances||frame->decoration_count>kActionSceneDecorationMaxInstances)return false;
  out->frame=frame;out->dim_receivers=0;
  out->dimming=ActionEnvironment_NativeBg1Dimming(frame,group,room);
  out->ramp=ActionEnvironment_Bg1DimmingRamp(frame,group,room);
  ReceiverProjection projection={camera_x,camera_y,bg2_x,bg2_y};
  unsigned light_receivers=0;
  for(unsigned list=0;list<3;++list){
    const ActionEffectInstance *entries=list==2?frame->effects:list?frame->authored:frame->decorations;
    const unsigned count=list==2?frame->effect_count:list?frame->authored_count:frame->decoration_count;
    for(unsigned i=0;i<count;++i){const ActionEffectInstance *e=&entries[i];
      if(!(e->flags&kActionEffectFlag_Visible))continue;
      if(e->tuning.dim_receivers_set&&(e->kind==kActionEffect_CaveAmbientLight||e->kind==kActionEffect_CastleLight))out->dim_receivers|=e->tuning.dim_receivers;
      if(ActionEffectReceivers_IsLight(e->kind)&&e->tuning.light_receivers_set)light_receivers|=e->tuning.light_receivers;
    }
  }
  for(unsigned role=0;role<2;++role) {
    ActionSceneEffectRenderBatch *batch=role?&out->enemies:&out->player;
    batch->vertex_count=batch->index_count=0;out->light_start[role]=0;
    const unsigned receiver=role?kActionReceiver_Enemies:kActionReceiver_Player;
    if(!lighting_enabled||!(light_receivers&receiver))continue;
    /* Frame profiles are immutable; this bounded copy filters instances only
     * when this role actually receives authored light. Default scenes skip
     * both the copy and all receiver geometry work. */
    ActionSceneEffectFrame selected=*frame;
    for(unsigned list=0;list<3;++list) {
      ActionEffectInstance *entries=list==2?selected.effects:list?selected.authored:selected.decorations;
      const unsigned count=list==2?selected.effect_count:list?selected.authored_count:selected.decoration_count;
      for(unsigned i=0;i<count;++i) {
        ActionEffectInstance *e=&entries[i];
        if(!ActionEffectReceivers_IsLight(e->kind)||!e->tuning.light_receivers_set||!(e->tuning.light_receivers&receiver))
          e->flags&=(uint8_t)~kActionEffectFlag_Visible;
        else {e->tuning.light_receivers=7;}
      }
    }
    /* Each supported source owns one pass. Both retained results borrow this
     * scratch sequentially; no geometry or allocations escape into the PPU. */
    static ActionSceneEffectRenderBatch scratch;
    for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer) {
      if(layer==kActionEffectRenderLayer_Bg1Light)out->light_start[role]=batch->index_count;
      if(!ActionSceneDecorationRender_Build(&selected,layer,lighting_enabled,false,Project,Clip,&projection,&scratch))return false;
      if(batch->vertex_count+scratch.vertex_count>kActionSceneEffectRenderMaxVertices ||
          batch->index_count+scratch.index_count>kActionSceneEffectRenderMaxIndices)return false;
      const int base=batch->vertex_count;
      memcpy(batch->vertices+base,scratch.vertices,scratch.vertex_count*sizeof(scratch.vertices[0]));
      for(int i=0;i<scratch.index_count;++i)batch->indices[batch->index_count++]=base+scratch.indices[i];
      batch->vertex_count+=scratch.vertex_count;
    }
  }
  return true;
}
static bool TopLeft(ArRenderPointF a,ArRenderPointF b) {
  return b.y>a.y || (b.y==a.y && b.x<a.x);
}
static void Sample(const ActionSceneEffectRenderBatch *batch,int light_start,float x,float y,float add[3],float light[3]) {
  for(int i=0;i<batch->index_count;i+=3) {
    const ArRenderVertex2D *a=&batch->vertices[batch->indices[i]],*b=&batch->vertices[batch->indices[i+1]],*c=&batch->vertices[batch->indices[i+2]];
    if(x<fminf(a->position.x,fminf(b->position.x,c->position.x)) ||
        x>fmaxf(a->position.x,fmaxf(b->position.x,c->position.x)) ||
        y<fminf(a->position.y,fminf(b->position.y,c->position.y)) ||
        y>fmaxf(a->position.y,fmaxf(b->position.y,c->position.y)))continue;
    float area=(b->position.x-a->position.x)*(c->position.y-a->position.y)-
        (b->position.y-a->position.y)*(c->position.x-a->position.x);
    if(fabsf(area)<.00001f)continue;
    if(area<0){const ArRenderVertex2D *swap=b;b=c;c=swap;area=-area;}
    float w0=(c->position.x-b->position.x)*(y-b->position.y)-(c->position.y-b->position.y)*(x-b->position.x);
    float w1=(a->position.x-c->position.x)*(y-c->position.y)-(a->position.y-c->position.y)*(x-c->position.x);
    float w2=(b->position.x-a->position.x)*(y-a->position.y)-(b->position.y-a->position.y)*(x-a->position.x);
    /* World-space cancellation at shared vertices can leave tiny positive
     * edge values. Snap relative to triangle area before applying ownership. */
    const float epsilon=area*.00001f;
    if(fabsf(w0)<=epsilon)w0=0;if(fabsf(w1)<=epsilon)w1=0;if(fabsf(w2)<=epsilon)w2=0;
    if(w0<0||w1<0||w2<0 || (w0==0&&!TopLeft(b->position,c->position)) ||
        (w1==0&&!TopLeft(c->position,a->position)) || (w2==0&&!TopLeft(a->position,b->position)))continue;
    const float t=w0/area,u=w1/area,v=w2/area,alpha=a->color.a*t+b->color.a*u+c->color.a*v;
    float *result=i>=light_start?light:add;
    result[0]+=(a->color.r*t+b->color.r*u+c->color.r*v)*alpha;
    result[1]+=(a->color.g*t+b->color.g*u+c->color.g*v)*alpha;
    result[2]+=(a->color.b*t+b->color.b*u+c->color.b*v)*alpha;
  }
}

void ActionEffectReceivers_Sample(const ActionReceiverLighting *lighting,unsigned receiver,
    float x,float y,float multiply[3],float add[3]) {
  float factor=1,light[3]={1,1,1};
  add[0]=add[1]=add[2]=0;
  if(lighting&&lighting->frame) {
    if(lighting->dim_receivers&receiver)factor*=1-SceneryDimming_Amount(lighting->dimming,lighting->ramp,x,y);
    const ActionSceneEffectFrame *frame=lighting->frame;
    for(unsigned i=0;i<frame->authored_count;++i) {
      const ActionEffectInstance *e=&frame->authored[i];const ActionEffectLocalRect *r=&e->geometry.data.rect;
      if(e->kind==kActionEffect_AuthoredExposure&&(e->flags&kActionEffectFlag_Visible)&&(e->tuning.dim_receivers&receiver)&&
          x>=e->world_x+r->x0&&x<e->world_x+r->x1&&y>=e->world_y+r->y0&&y<e->world_y+r->y1)
        factor*=1-e->tuning.intensity;
    }
    const unsigned role=receiver==kActionReceiver_Player?0:1;
    Sample(role?&lighting->enemies:&lighting->player,lighting->light_start[role],x,y,add,light);
  }
  for(unsigned i=0;i<3;++i){multiply[i]=fmaxf(0,fminf(2,factor*light[i]));add[i]=fmaxf(0,fminf(1,add[i]));}
}
