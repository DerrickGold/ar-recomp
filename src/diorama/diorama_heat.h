#ifndef AR_DIORAMA_HEAT_H
#define AR_DIORAMA_HEAT_H
#include "render/render_device.h"
#include <math.h>

/* Background-only texture displacement, shared by the portable mesh and the
 * resident skybox shader. std140 vec4 layout; no intermediate scene capture.
 * Overlaps take the strongest envelope rather than stacking distortion. */
enum { kDioramaHeatRegions=4, kDioramaHeatColumns=32, kDioramaHeatRows=16 };
typedef struct DioramaHeat {
  float source[4]; /* capture's world origin XY, texture width/height */
  float bounds[kDioramaHeatRegions][4]; /* world x0,y0,x1,y1 */
  float motion[kDioramaHeatRegions][4]; /* amplitude, phase, reserved */
} DioramaHeat;
static inline bool DioramaHeat_Active(const DioramaHeat *h) {
  for(unsigned i=0;i<kDioramaHeatRegions;++i)if(h->motion[i][0]>0)return true;
  return false;
}
static inline ArRenderPointF DioramaHeat_Uv(const DioramaHeat *h,ArRenderPointF uv) {
  if(h->source[2]<=0||h->source[3]<=0)return uv;
  const float x=uv.x*h->source[2]+h->source[0],y=uv.y*h->source[3]+h->source[1];
  float strength=0,phase=0;
  for(unsigned i=0;i<kDioramaHeatRegions;++i) {
    const float *r=h->bounds[i];
    if(h->motion[i][0]<=0||r[2]<=r[0]||r[3]<=r[1])continue;
    const float u=(x-r[0])/(r[2]-r[0]),v=(y-r[1])/(r[3]-r[1]);
    if(u<=0||u>=1||v<=0||v>=1)continue;
    const float edge=fminf(1,8*fminf(fminf(u,1-u),fminf(v,1-v)));
    const float a=h->motion[i][0]*edge*edge*(3-2*edge);
    if(a>strength){strength=a;phase=h->motion[i][1];}
  }
  uv.x+=strength*(sinf(y*.12f+phase*3)+.4f*sinf(x*.08f-y*.05f+phase*2))/h->source[2];
  uv.y+=strength*.18f*sinf(x*.09f+y*.07f+phase*2)/h->source[3];
  return uv;
}
#endif
