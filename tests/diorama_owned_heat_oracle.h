#ifndef AR_TEST_OWNED_HEAT_ORACLE_H
#define AR_TEST_OWNED_HEAT_ORACLE_H
#include "diorama/diorama_heat.h"
#include <math.h>
/* Test oracle uses native receiver coordinates, not a material's atlas UV.
 * Compute in double precision independently of the production helper/GLSL. */
typedef struct OwnedHeatOraclePoint { double x,y; } OwnedHeatOraclePoint;
static inline OwnedHeatOraclePoint OwnedHeatOracleOffset(const DioramaHeat *heat,double x,double y) {
  double amplitude=0,phase=0;
  for(unsigned r=0;r<kDioramaHeatRegions;++r) {
    const double left=heat->bounds[r][0],top=heat->bounds[r][1],right=heat->bounds[r][2],bottom=heat->bounds[r][3];
    if(heat->motion[r][0]<=0||right<=left||bottom<=top||x<=left||x>=right||y<=top||y>=bottom)continue;
    double envelope=fmin(fmin((x-left)/(right-left),(right-x)/(right-left)),
        fmin((y-top)/(bottom-top),(bottom-y)/(bottom-top)))*8;
    if(envelope>1)envelope=1;
    const double strength=heat->motion[r][0]*(3*envelope*envelope-2*envelope*envelope*envelope);
    if(strength>amplitude){amplitude=strength;phase=heat->motion[r][1];}
  }
  return (OwnedHeatOraclePoint){amplitude*(sin(.12*y+3*phase)+.4*sin(.08*x-.05*y+2*phase)),
      amplitude*.18*sin(.09*x+.07*y+2*phase)};
}
static inline OwnedHeatOraclePoint OwnedHeatOracleUv(const DioramaHeat *heat,
    DioramaHeatOwned policy,OwnedHeatOraclePoint world,OwnedHeatOraclePoint uv) {
  if(policy.mode<.5||policy.texel_u<=0||policy.texel_v<=0||policy.scale<=0)return uv;
  const OwnedHeatOraclePoint d=OwnedHeatOracleOffset(heat,world.x,world.y);
  if(!d.x&&!d.y)return uv;
  double *out[2]={&uv.x,&uv.y};const double step[2]={policy.texel_u,policy.texel_v},delta[2]={d.x,d.y};
  for(unsigned a=0;a<2;++a) {
    const double cell=floor(*out[a]/step[a]/16),inset=.5/policy.scale;
    double local=*out[a]/step[a]-cell*16+delta[a];
    if(policy.mode<1.5){local=fmod(local,16);if(local<0)local+=16;}
    if(local<inset)local=inset;if(local>16-inset)local=16-inset;
    *out[a]=(cell*16+local)*step[a];
  }
  return uv;
}
#endif
