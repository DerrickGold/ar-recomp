#ifndef AR_CLOUD_MOTION_H
#define AR_CLOUD_MOTION_H

/* Deterministic cloud placement. Phase is supplied by the scene's own paused
 * clock; never substitute host time, camera position or per-frame RNG. */
#include "render/cloud_noise.h"

typedef struct CloudBankMotion {
  float x,y,width,height,drift,amplitude,rolling,intensity,phase,wind_phase;
  unsigned strands;
  bool volume;
} CloudBankMotion;
typedef struct CloudBankPatch {
  float x,y,rx,ry,alpha;
} CloudBankPatch;

/* One body and one crest. The two volumetric layers have exactly the same
 * footprint; the wispy profile intentionally has a separate moving curl. */
static inline void CloudMotion_Bank(const CloudBankMotion *s,unsigned i,uint32_t seed,
    CloudBankPatch patches[2]) {
  const float width=s->width,height=s->height,spacing=width/s->strands,phase=s->phase;
  if(s->volume) {
    const float depth=CloudNoise_Seed(seed^91);
    const float rx=fminf(width*.45f,fminf(spacing*(.85f+.25f*CloudNoise_Seed(seed^11)),
        height*(.75f+.35f*depth)));
    const float ry=fminf(height*.40f,rx*(.38f+.10f*CloudNoise_Seed(seed^3)));
    const float drift=fminf(fabsf(s->drift),fmaxf(0,(width-2*rx)*.4f));
    const float inset=rx+drift;
    const float wind=s->wind_phase*(.75f+.17f*depth)+
        sinf(phase*2+CloudNoise_Seed(seed)*6.2831853f)*.08f*s->rolling;
    const float x=s->x+inset+(width-2*inset)*
        (i+.25f+.5f*CloudNoise_Seed(seed^43))/s->strands+wind*drift*(s->drift<0?-1:1);
    const float lift=fminf(s->amplitude,(height-2*ry)*.3f)*s->rolling;
    const float y=s->y+ry+lift+(height-2*ry-2*lift)*(.2f+.6f*depth)+
        sinf(phase*2+CloudNoise_Seed(seed^73)*6.2831853f)*lift;
    for(unsigned tier=0;tier<2;++tier)
      patches[tier]=(CloudBankPatch){x,y,rx,ry,fminf(.94f,s->intensity*(tier?.90f:.82f)*(.75f+.25f*depth))};
    return;
  }
  const float cycle=phase+CloudNoise_Seed(seed)*6.2831853f;
  const float rx=fminf(width*.45f,spacing*(.72f+.18f*CloudNoise_Seed(seed^11)));
  const float drift=fminf(fabsf(s->drift),fmaxf(0,(width-2*rx)*.15f))*(s->drift<0?-1:1);
  const float inset=rx+fabsf(drift);
  const float x=s->x+inset+(width-2*inset)*(i+.35f+.3f*CloudNoise_Seed(seed^43))/s->strands+sinf(cycle)*drift;
  const float y=s->y+height*(.55f+.14f*CloudNoise_Seed(seed^91))+sinf(cycle*2)*s->amplitude;
  const float roll=s->rolling*sinf(cycle),ry=height*(.30f+.08f*CloudNoise_Seed(seed^3));
  for(unsigned tier=0;tier<2;++tier) {
    patches[tier]=(CloudBankPatch){
      x+(tier?rx*(.22f*roll+.14f*cosf(cycle*2)):0),y-(tier?ry*(.70f+.40f*roll):0),
      rx*(tier?1.15f+.16f*roll:1),ry*(tier?.42f+.12f*roll:1),
      fminf(.85f,s->intensity*(tier?.52f:.74f))};
  }
}

typedef struct CloudOrbitMotion {
  float x,dx,x_phase,y,dy,y_rate,y_phase,alpha,alpha_range,alpha_phase;
} CloudOrbitMotion;
typedef struct CloudOrbitSample { float x,y,alpha; } CloudOrbitSample;
static inline CloudOrbitSample CloudMotion_Orbit(CloudOrbitMotion s,float phase) {
  return (CloudOrbitSample){s.x+s.dx*sinf(phase+s.x_phase),
    s.y+s.dy*cosf(phase*s.y_rate+s.y_phase),
    s.alpha+s.alpha_range*(.5f+.5f*sinf(phase+s.alpha_phase))};
}
#endif
