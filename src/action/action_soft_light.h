#ifndef AR_ACTION_SOFT_LIGHT_H
#define AR_ACTION_SOFT_LIGHT_H
#include "action_light_kinds.h"
#include "action_water_wave.h"
#include "action_effect_masks.h"

/* Source geometry and airborne samples share the same light shapes. Visible
 * diffusion is independent of incident energy; optical glare can emit none. */
static const float kActionSoftLightRadius[4]={0,.16f,.58f,1};
static const float kActionSoftLightAlpha[4]={.20f,.16f,.05f,0};
enum {kActionWaterRippleLayers=2,kActionWaterRippleColumns=3,kActionWaterRippleRows=3,
  kActionWaterRippleVertices=18,kActionWaterRippleIndices=48,
  kActionWaterRippleVertexBudget=16*7}; /* A clipped triangle has at most seven vertices. */

static inline bool ActionSoftLight_Valid(const ActionEffectInstance *e) {
  if(!e->field_style.ripple)return true;
  const float amplitude=e->field_style.amplitude,drift=e->field_style.drift,softness=e->field_style.softness;
  if(e->field_style.ripple!=1||!e->particle_lifetime||!isfinite(amplitude)||amplitude<0||amplitude>32||
      !isfinite(drift)||fabsf(drift)>128||!isfinite(softness)||softness<0||softness>1)return false;
  return true;
}

/* Two independently deforming caustic patterns. The immutable mask contains
 * curved, broken cell boundaries; UV movement bends them without long lines,
 * respawning particles, camera-relative motion or per-frame texture uploads. */
static inline ArRenderVertex2D ActionWaterRipple_Vertex(const ActionEffectInstance *e,
    float phase,unsigned layer,unsigned column,unsigned row) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float u=column*.5f,v=row*.5f,width=r->x1-r->x0,height=r->y1-r->y0;
  const float clock=phase+layer*2.3f+e->world_x*.013f+e->world_y*.009f;
  const float dx=fminf(.085f,e->field_style.amplitude/width)*sinf(u*3.14159265f)*
      (.65f*sinf(clock+v*4.1f)+.35f*cosf(clock*2-u*3.2f));
  const float dy=fminf(.085f,e->field_style.amplitude/height)*sinf(v*3.14159265f)*
      (.65f*cosf(clock-u*3.7f)+.35f*sinf(clock*2+v*2.6f));
  const float weight=layer?.42f-.12f*sinf(phase):.58f+.12f*sinf(phase);
  const unsigned mask=kActionMaskCausticFirst+(unsigned)floorf(e->field_style.softness*(kActionMaskCausticCount-1)+.5f);
  const uint32_t color=e->tuning.color;
  float tu=layer?1-u-dx:u+dx,tv=layer?1-v-dy:v+dy;
  /* Source identity varies the orientation without another atlas or seed
   * control. Adjacent authored regions do not repeat the same arrangement. */
  const unsigned rotation=e->generation&3;
  for(unsigned i=0;i<rotation;++i){const float previous=tu;tu=1-tv;tv=previous;}
  return (ArRenderVertex2D){.position={r->x0+u*width,r->y0+v*height},
    .tex_coord=ActionEffectMasks_Uv(mask,tu,tv),
    .color={((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,
      .28f*e->tuning.intensity*weight*(.55f+.45f*v)}};
}
static inline float ActionWaterRipple_Sample(const ActionEffectInstance *e,float x,float y) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  if(x<=r->x0||x>=r->x1||y<=r->y0||y>=r->y1)return 0;
  const float phase=ActionWaterWave_Phase(e)+e->field_style.drift*.01f;
  const float u=(x-r->x0)/(r->x1-r->x0)*2,v=(y-r->y0)/(r->y1-r->y0)*2;
  const unsigned col=(unsigned)fminf(1,u),row=(unsigned)fminf(1,v);
  const float tx=u-col,ty=v-row;
  const bool upper=tx+ty<=1;
  const float wa=upper?1-tx-ty:tx+ty-1,wb=upper?tx:1-ty,wc=upper?ty:1-tx;
  float out=0;
  for(unsigned layer=0;layer<kActionWaterRippleLayers;++layer) {
    const ArRenderVertex2D a=ActionWaterRipple_Vertex(e,phase,layer,col+(upper?0:1),row+(upper?0:1)),
      b=ActionWaterRipple_Vertex(e,phase,layer,col+1,row),c=ActionWaterRipple_Vertex(e,phase,layer,col,row+1);
    out+=(a.color.a*wa+b.color.a*wb+c.color.a*wc)*ActionEffectMasks_Sample(
        a.tex_coord.x*wa+b.tex_coord.x*wb+c.tex_coord.x*wc,
        a.tex_coord.y*wa+b.tex_coord.y*wb+c.tex_coord.y*wc);
  }
  return out;
}
static inline ArRenderColorF ActionSoftLight_Sample(const ActionEffectInstance *e,float x,float y) {
  ArRenderColorF out={0};
  const unsigned receivers=e->tuning.light_receivers_set?e->tuning.light_receivers:
      ActionLightKind_DefaultReceivers(e->kind);
  if(e->kind!=kActionEffect_AuthoredLight || !(e->flags&kActionEffectFlag_Visible) ||
      (e->flags&kActionEffectFlag_LightingOff) || !(receivers&kActionReceiver_Scenery))return out;
  const float illumination=e->tuning.illumination_set?e->tuning.illumination:
      ActionLightKind_Illumination(e->kind);
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float rx=(r->x1-r->x0)*.5f,ry=(r->y1-r->y0)*.5f;
  if(!ActionSoftLight_Valid(e)||rx<=0||ry<=0||illumination<=0)return out;
  if(e->field_style.ripple) {
    out.a=ActionWaterRipple_Sample(e,x-e->world_x,y-e->world_y)*illumination;
  } else {
    const float u=(x-e->world_x)/rx,v=(y-e->world_y)/ry;
    if(fabsf(u)>=1||fabsf(v)>=1)return out;
    const float distance=hypotf(u,v);
    if(distance>=1)return out;
    unsigned ring=1;while(ring<3&&distance>kActionSoftLightRadius[ring])++ring;
    const float t=(distance-kActionSoftLightRadius[ring-1])/
        (kActionSoftLightRadius[ring]-kActionSoftLightRadius[ring-1]);
    out.a=(kActionSoftLightAlpha[ring-1]+(kActionSoftLightAlpha[ring]-kActionSoftLightAlpha[ring-1])*t)*
        e->tuning.intensity*illumination;
  }
  out.r=((e->tuning.color>>16)&255)/255.f*out.a;
  out.g=((e->tuning.color>>8)&255)/255.f*out.a;
  out.b=(e->tuning.color&255)/255.f*out.a;
  return out;
}
#endif
