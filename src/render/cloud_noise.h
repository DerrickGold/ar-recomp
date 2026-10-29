#ifndef AR_CLOUD_NOISE_H
#define AR_CLOUD_NOISE_H

/* Portable cloud material noise. These kernels deliberately retain their
 * interpolation order: replacing one with another changes existing artwork.
 * No clock, renderer, allocation or mutable global state. */
#include "deterministic_hash.h"
#include <math.h>
#include <stdbool.h>

static inline float CloudNoise_Smooth(float t) { return t*t*(3-2*t); }
static inline float CloudNoise_Ramp(float a, float b, float x) {
  return CloudNoise_Smooth(fmaxf(0, fminf(1, (x-a)/(b-a))));
}
static inline float CloudNoise_Seed(uint32_t seed) {
  return (DeterministicHash_Mix32(seed)&65535)/65535.f;
}
static inline float CloudNoise_Seeded2(float x, float y, uint32_t seed) {
  const int ix=(int)floorf(x),iy=(int)floorf(y);
  const float u=CloudNoise_Ramp(0,1,x-ix),v=CloudNoise_Ramp(0,1,y-iy);
  float n[4];
  for(unsigned j=0;j<4;++j)
    n[j]=CloudNoise_Seed(seed^(uint32_t)(ix+(j&1))*0x9e3779b9u^
        (uint32_t)(iy+(j>>1))*0x85ebca6bu);
  return (n[0]+(n[1]-n[0])*u)*(1-v)+(n[2]+(n[3]-n[2])*u)*v;
}
static inline float CloudNoise_Seeded3(float x, float y, float z, uint32_t seed) {
  const int iz=(int)floorf(z);
  const float t=CloudNoise_Ramp(0,1,z-iz);
  const float a=CloudNoise_Seeded2(x,y,seed^(uint32_t)iz*0xc2b2ae35u);
  const float b=CloudNoise_Seeded2(x,y,seed^(uint32_t)(iz+1)*0xc2b2ae35u);
  return a+(b-a)*t;
}
static inline float CloudNoise_LatticeHash(int x, int y, int z) {
  uint32_t h=(uint32_t)x*374761393u+(uint32_t)y*668265263u+(uint32_t)z*2246822519u;
  h=(h^(h>>13))*1274126177u;
  return (float)((h^(h>>16))&65535u)/65535.f;
}
static inline float CloudNoise_Periodic2(float x, float y, int period) {
  const int x0=(int)floorf(x),y0=(int)floorf(y);
  const float fx=CloudNoise_Smooth(x-(float)x0),fy=CloudNoise_Smooth(y-(float)y0);
  const int xa=((x0%period)+period)%period,ya=((y0%period)+period)%period;
  const int xb=(xa+1)%period,yb=(ya+1)%period;
  const float n00=CloudNoise_LatticeHash(xa,ya,0),n10=CloudNoise_LatticeHash(xb,ya,0);
  const float n01=CloudNoise_LatticeHash(xa,yb,0),n11=CloudNoise_LatticeHash(xb,yb,0);
  const float top=n00+(n10-n00)*fx,bottom=n01+(n11-n01)*fx;
  return top+(bottom-top)*fy;
}
static inline float CloudNoise_Weighted3(float x, float y, float z) {
  const int ix=(int)floorf(x),iy=(int)floorf(y),iz=(int)floorf(z);
  const float fx=CloudNoise_Ramp(0,1,x-ix),fy=CloudNoise_Ramp(0,1,y-iy),fz=CloudNoise_Ramp(0,1,z-iz);
  float result=0;
  for(int dz=0;dz<2;dz++)for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++)
    result+=CloudNoise_LatticeHash(ix+dx,iy+dy,iz+dz)*
        (dx?fx:1-fx)*(dy?fy:1-fy)*(dz?fz:1-fz);
  return result;
}

/* Coherent scanlines reuse a cell's hashes, without lowering noise resolution.
 * Reset the cell when lattice Y or its interpolation weight changes. */
typedef struct CloudNoiseCell {
  int x,z;
  bool valid;
  float start[2][2],delta[2][2];
} CloudNoiseCell;
static inline float CloudNoise_Scan3(float x,float z,int iy,float fy,CloudNoiseCell *cell) {
  const int ix=(int)floorf(x),iz=(int)floorf(z);
  const float fx=CloudNoise_Smooth(x-ix),fz=CloudNoise_Smooth(z-iz);
  if(!cell->valid||cell->x!=ix||cell->z!=iz) {
    for(int dz=0;dz<2;dz++)for(int dy=0;dy<2;dy++) {
      const float a=CloudNoise_LatticeHash(ix,iy+dy,iz+dz);
      cell->start[dz][dy]=a;
      cell->delta[dz][dy]=CloudNoise_LatticeHash(ix+1,iy+dy,iz+dz)-a;
    }
    cell->x=ix;cell->z=iz;cell->valid=true;
  }
  float plane[2];
  for(int dz=0;dz<2;dz++) {
    float row[2];
    for(int dy=0;dy<2;dy++)row[dy]=cell->start[dz][dy]+cell->delta[dz][dy]*fx;
    plane[dz]=row[0]+(row[1]-row[0])*fy;
  }
  return plane[0]+(plane[1]-plane[0])*fz;
}

/* Shared cool-white shroud material for planar and spherical cloud sheets. */
static inline uint32_t CloudNoise_SheetColor(float value) {
  float density=(value-.42f)/.38f;
  density=CloudNoise_Smooth(fminf(1,fmaxf(0,density)));
  const unsigned alpha=(unsigned)(density*255+.5f),tint=236+(unsigned)(density*19);
  return (alpha<<24)|(tint<<16)|(tint<<8)|255u;
}
#endif
