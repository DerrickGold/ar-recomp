#include "action_effect_receivers.h"
#include "action_environment_exposure.h"
#include "action_exposure.h"
#include "action_light_kinds.h"
#include "render/scenery_dimming.h"
#include <math.h>
#include <string.h>

bool ActionEffectReceivers_IsLight(unsigned kind) {
  return ActionLightKind_Supported(kind);
}
uint8_t ActionEffectReceivers_Mask(const ActionEffectInstance *e) {
  /* The castle's sky fan is behind the opaque back wall. Its field shares
   * receiver settings with the window spill, but promoting the sky itself
   * into the material/actor maps bypasses that wall's painter-order mask.
   * CastleLight owns the light entering through the validated openings. */
  if (e->kind == kActionEffect_CastleSky) return 0;
  return e->tuning.light_receivers_set ? e->tuning.light_receivers :
      ActionLightKind_DefaultReceivers(e->kind);
}
uint8_t ActionEffectReceivers_FamilyMask(const ActionEffectInstance *e,
    const ActionNativeMembers *members) {
  uint8_t mask = ActionEffectReceivers_Mask(e);
  if (e->kind == kActionEffect_CastleSky) return mask;
  if (members && members->count <= kActionNativeMemberMax)
    for (unsigned i = 0; i < members->count; ++i) {
      const ActionNativeMember *m = &members->records[i];
      if (m->kind == e->kind && m->enabled && m->light_receivers_set)
        mask |= m->light_receivers;
    }
  return mask;
}
bool ActionEffectReceivers_Member(const ActionEffectInstance *e,
    const ActionNativeMember *member, unsigned receiver) {
  return !receiver || ((member && member->light_receivers_set
      ? member->light_receivers : ActionEffectReceivers_Mask(e)) & receiver);
}
uint8_t ActionEffectReceivers_Layer(const ActionEffectInstance *e) {
  if(ActionEffectReceivers_IsLight(e->kind)) {
    /* Multiplicative light belongs to receiving artwork, never the final
     * image (which already contains lit sprites and HUD). Flat mode uses the
     * BG1 winner mask; Diorama consumes it in the material map. */
    if(e->render_layer==kActionEffectRenderLayer_ForegroundLight)
      return kActionEffectRenderLayer_Bg1Light;
    if(e->tuning.light_receivers_set && e->render_layer==kActionEffectRenderLayer_WorldOverlay)
      return kActionEffectRenderLayer_Bg1Plane;
  }
  return e->render_layer;
}
uint8_t ActionEffectReceivers_SurfaceLayer(const ActionEffectInstance *e) {
  if (!ActionEffectReceivers_IsLight(e->kind) ||
      !(ActionEffectReceivers_Mask(e) & kActionReceiver_Scenery))
    return kActionEffectRenderLayer_Count;
  const unsigned layer = ActionEffectReceivers_Layer(e);
  if (layer == kActionEffectRenderLayer_Bg1Light || layer == kActionEffectRenderLayer_Bg1Plane ||
      layer == kActionEffectRenderLayer_Bg1HighPlane) return layer;
  /* BG2 atmosphere keeps its original depth. Only sources with configured
   * receivers also illuminate foreground materials; a sky color wash does not. */
  if ((e->tuning.light_receivers_set || ActionLightKind_DefaultReceivers(e->kind) != kActionReceiver_Scenery) &&
      (layer == kActionEffectRenderLayer_Bg2Plane || layer == kActionEffectRenderLayer_Bg2HighPlane ||
       layer == kActionEffectRenderLayer_Bg2Alpha)) return kActionEffectRenderLayer_Bg1Plane;
  return kActionEffectRenderLayer_Count;
}
bool ActionEffectReceivers_SurfaceLight(const ActionEffectInstance *e) {
  return ActionEffectReceivers_SurfaceLayer(e) != kActionEffectRenderLayer_Count;
}
bool ActionEffectReceivers_HasSurfaceLights(const ActionSceneEffectFrame *frame) {
  if (!frame || frame->decoration_count > kActionSceneDecorationMaxInstances ||
      frame->authored_count > kActionAuthoredMaxInstances || frame->effect_count > kActionSceneEffectMaxInstances)
    return false;
  for (unsigned list = 0; list < 3; ++list) {
    const ActionEffectInstance *entries = list == 2 ? frame->effects : list ? frame->authored : frame->decorations;
    const unsigned count = list == 2 ? frame->effect_count : list ? frame->authored_count : frame->decoration_count;
    for (unsigned i = 0; i < count; ++i) {
      ActionEffectInstance effective = entries[i];
      effective.tuning.light_receivers = ActionEffectReceivers_FamilyMask(&entries[i], &frame->members);
      effective.tuning.light_receivers_set |= effective.tuning.light_receivers != ActionEffectReceivers_Mask(&entries[i]);
      const ActionEffectInstance *e = &effective;
      if ((e->flags & kActionEffectFlag_Visible) && !(e->flags & kActionEffectFlag_LightingOff) &&
          (ActionEffectReceivers_Mask(e) & kActionReceiver_Scenery) &&
          ActionEffectReceivers_SurfaceLight(e)) return true;
    }
  }
  return false;
}
bool ActionEffectReceivers_HasActors(const ActionSceneEffectFrame *frame, bool lighting_enabled) {
  if (!frame || frame->decoration_count > kActionSceneDecorationMaxInstances ||
      frame->authored_count > kActionAuthoredMaxInstances || frame->effect_count > kActionSceneEffectMaxInstances)
    return false;
  const unsigned actors = kActionReceiver_Player | kActionReceiver_Enemies;
  for (unsigned list = 0; list < 3; ++list) {
    const ActionEffectInstance *entries = list == 2 ? frame->effects : list ? frame->authored : frame->decorations;
    const unsigned count = list == 2 ? frame->effect_count : list ? frame->authored_count : frame->decoration_count;
    for (unsigned i = 0; i < count; ++i) {
      const ActionEffectInstance *e = &entries[i];
      if (!(e->flags & kActionEffectFlag_Visible)) continue;
      if (e->tuning.dim_receivers_set && (e->tuning.dim_receivers & actors) &&
          (e->kind == kActionEffect_AuthoredExposure || e->kind == kActionEffect_CaveAmbientLight ||
           e->kind == kActionEffect_CastleLight)) return true;
      if (lighting_enabled && !(e->flags & kActionEffectFlag_LightingOff) &&
          ActionEffectReceivers_IsLight(e->kind) &&
          (ActionEffectReceivers_FamilyMask(e, &frame->members) & actors)) return true;
    }
  }
  return false;
}
bool ActionEffectReceivers_MapsMatch(const ActionSceneEffectFrame *frame, unsigned a, unsigned b) {
  if (!frame || frame->decoration_count > kActionSceneDecorationMaxInstances ||
      frame->authored_count > kActionAuthoredMaxInstances || frame->effect_count > kActionSceneEffectMaxInstances ||
      frame->members.count > kActionNativeMemberMax) return false;
  for(unsigned list=0;list<3;++list) {
    const ActionEffectInstance *entries=list==2?frame->effects:list?frame->authored:frame->decorations;
    const unsigned count=list==2?frame->effect_count:list?frame->authored_count:frame->decoration_count;
    for(unsigned i=0;i<count;++i) {
      const ActionEffectInstance *e=&entries[i];
      if(!(e->flags&kActionEffectFlag_Visible)||(e->flags&kActionEffectFlag_LightingOff)||
          !ActionEffectReceivers_IsLight(e->kind))continue;
      const unsigned mask=ActionEffectReceivers_Mask(e);
      if (!!(mask&a)!=!!(mask&b))return false;
      if ((a==kActionReceiver_Scenery||b==kActionReceiver_Scenery) &&
          (mask&(a|b)) && !ActionEffectReceivers_SurfaceLight(e))return false;
      for(unsigned j=0;j<frame->members.count;++j) {
        const ActionNativeMember *m=&frame->members.records[j];
        if(m->enabled&&m->kind==e->kind&&m->light_receivers_set &&
            !!(m->light_receivers&a)!=!!(m->light_receivers&b))return false;
      }
    }
  }
  return true;
}
typedef struct ReceiverProjection {int x,y,bg2_x,bg2_y;} ReceiverProjection;
bool ActionEffectReceivers_ProjectPoint(void *context,const ActionEffectInstance *e,float x,float y,ArRenderPointF *point) {
  const ReceiverProjection *p=context;
  const bool bg2=e->projection_plane==kActionEffectProjectionPlane_Bg2 || e->projection_plane==kActionEffectProjectionPlane_Bg2High;
  const bool between=e->projection_plane==kActionEffectProjectionPlane_BetweenBackgrounds;
  const int camera_x=between?(p->x+p->bg2_x)/2:bg2?p->bg2_x:p->x;
  const int camera_y=between?(p->y+p->bg2_y)/2:bg2?p->bg2_y:p->y;
  *point=(ArRenderPointF){e->world_x+x+p->x-camera_x,e->world_y+y+p->y-camera_y};
  return isfinite(point->x)&&isfinite(point->y);
}
static bool Clip(void *context,const ActionEffectInstance *e,ActionEffectLocalRect *clip) {
  const ReceiverProjection *p=context;ArRenderPointF origin;ActionEffectReceivers_ProjectPoint(context,e,0,0,&origin);
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
  bool same_receivers=true;
  for(unsigned list=0;list<3;++list){
    const ActionEffectInstance *entries=list==2?frame->effects:list?frame->authored:frame->decorations;
    const unsigned count=list==2?frame->effect_count:list?frame->authored_count:frame->decoration_count;
    for(unsigned i=0;i<count;++i){const ActionEffectInstance *e=&entries[i];
      if(!(e->flags&kActionEffectFlag_Visible))continue;
      if(e->tuning.dim_receivers_set&&(e->kind==kActionEffect_CaveAmbientLight||e->kind==kActionEffect_CastleLight))out->dim_receivers|=e->tuning.dim_receivers;
      if(ActionEffectReceivers_IsLight(e->kind)) {
        const unsigned mask=ActionEffectReceivers_FamilyMask(e,&frame->members);
        light_receivers|=mask;
        if(!!(mask&kActionReceiver_Player)!=!!(mask&kActionReceiver_Enemies))same_receivers=false;
      }
    }
  }
  for (unsigned i = 0; i < frame->members.count && i < kActionNativeMemberMax; ++i) {
    const ActionNativeMember *m = &frame->members.records[i];
    if (m->enabled && m->light_receivers_set &&
        !!(m->light_receivers & kActionReceiver_Player) != !!(m->light_receivers & kActionReceiver_Enemies))
      same_receivers = false;
  }
  for(unsigned role=0;role<2;++role) {
    ActionSceneEffectRenderBatch *batch=role?&out->enemies:&out->player;
    batch->vertex_count=batch->index_count=0;
    const unsigned receiver=role?kActionReceiver_Enemies:kActionReceiver_Player;
    if(!lighting_enabled||!(light_receivers&receiver))continue;
    if(role&&same_receivers) {
      batch->vertex_count=out->player.vertex_count;batch->index_count=out->player.index_count;
      memcpy(batch->vertices,out->player.vertices,batch->vertex_count*sizeof(batch->vertices[0]));
      memcpy(batch->indices,out->player.indices,batch->index_count*sizeof(batch->indices[0]));
      continue;
    }
    /* Frame profiles are immutable; this bounded copy filters instances only
     * when this role receives light. Scenery-only scenes skip both the copy
     * and all object-receiver geometry work. */
    ActionSceneEffectFrame selected=*frame;
    selected.light_receiver = receiver;
    for(unsigned list=0;list<3;++list) {
      ActionEffectInstance *entries=list==2?selected.effects:list?selected.authored:selected.decorations;
      const unsigned count=list==2?selected.effect_count:list?selected.authored_count:selected.decoration_count;
      for(unsigned i=0;i<count;++i) {
        ActionEffectInstance *e=&entries[i];
        if(!ActionEffectReceivers_IsLight(e->kind)||!(ActionEffectReceivers_FamilyMask(e,&frame->members)&receiver))
          e->flags&=(uint8_t)~kActionEffectFlag_Visible;
      }
    }
    /* Each supported source owns one pass. Both retained results borrow this
     * scratch sequentially; no geometry or allocations escape into the PPU. */
    static ActionSceneEffectRenderBatch scratch;
    for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer) {
      if(!ActionSceneDecorationRender_Build(&selected,layer,lighting_enabled,false,ActionEffectReceivers_ProjectPoint,Clip,&projection,&scratch))return false;
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
static void Sample(const ActionSceneEffectRenderBatch *batch,float x,float y,float light[3]) {
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
    light[0]+=(a->color.r*t+b->color.r*u+c->color.r*v)*alpha;
    light[1]+=(a->color.g*t+b->color.g*u+c->color.g*v)*alpha;
    light[2]+=(a->color.b*t+b->color.b*u+c->color.b*v)*alpha;
  }
}

float ActionEffectReceivers_Ambient(const ActionReceiverLighting *lighting, unsigned receiver, float x, float y) {
  float factor=1;
  if(lighting&&lighting->frame) {
    if(lighting->dim_receivers&receiver)factor*=1-SceneryDimming_Amount(lighting->dimming,lighting->ramp,x,y);
    const ActionSceneEffectFrame *frame=lighting->frame;
    for(unsigned i=0;i<frame->authored_count;++i) {
      const ActionEffectInstance *e=&frame->authored[i];
      if(!ActionExposure_Matches(e,e,receiver))continue;
      bool previous=false;
      for(unsigned j=0;j<i;++j)if(ActionExposure_Matches(e,&frame->authored[j],receiver))previous=true;
      if(previous)continue;
      const ActionEffectInstance *boxes[kActionAuthoredMaxInstances];unsigned count=0;
      for(unsigned j=i;j<frame->authored_count;++j)if(ActionExposure_Matches(e,&frame->authored[j],receiver))boxes[count++]=&frame->authored[j];
      factor*=ActionExposure_Union(boxes,count,x,y);
    }
  }
  return factor;
}
uint32_t ActionEffectReceivers_Attribute(const ActionReceiverLighting *lighting,
    unsigned receiver, float x, float y) {
  const float ambient = receiver ? ActionEffectReceivers_Ambient(lighting,receiver,x,y) : 1;
  const unsigned code = (unsigned)lroundf(fmaxf(0,fminf(255,ambient*64)));
  return 0xff000000u | (code<<16) |
      (receiver==kActionReceiver_Player ? 0xff00u : receiver==kActionReceiver_Enemies ? 0xffu : 0);
}
void ActionEffectReceivers_Sample(const ActionReceiverLighting *lighting,unsigned receiver,
    float x,float y,float multiply[3],float add[3]) {
  const float factor = ActionEffectReceivers_Ambient(lighting,receiver,x,y);
  float light[3]={0};
  add[0]=add[1]=add[2]=0;
  if(lighting&&lighting->frame)
    Sample(receiver==kActionReceiver_Player?&lighting->player:&lighting->enemies,x,y,light);
  /* Room exposure attenuates ambient light, not the source palette or the
   * incoming local light. A 25% room plus 75% white light restores the native
   * colors exactly. Colored sources restore brightness with their own tint;
   * unlike an additive color wash, black stays black and detail keeps its
   * contrast. Both scenery blend passes describe incoming light here. */
  for(unsigned i=0;i<3;++i)multiply[i]=fmaxf(0,fminf(kActionReceiverMaxIllumination,factor+light[i]));
}
