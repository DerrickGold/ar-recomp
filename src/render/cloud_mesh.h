#ifndef AR_CLOUD_MESH_H
#define AR_CLOUD_MESH_H

/* Small allocation-free cloud shapes shared by mist, spray, smoke and dust.
 * These produce local geometry only. Callers retain support/collision queries,
 * clipping, draw order and CPU/GPU projection. Inline to preserve batching cost. */
#include "render/render_types.h"
#include <math.h>

enum { kCloudPatchVertices=35,kCloudMistVertices=25,
       kCloudPuffSegments=12,kCloudPuffRings=3,kCloudPuffVertices=37 };
static inline float CloudMesh_Falloff(float x) {
  const float t=fmaxf(0,1-x*x);return t*t;
}
static inline void CloudMesh_Patch(ArRenderVertex2D vertices[kCloudPatchVertices],
    float x,float y,float rx,float ry,ArRenderColorF color,float lean) {
  for(int row=0;row<5;++row) {
    const float v=(row-2)*.5f;
    for(int col=0;col<7;++col) {
      const float u=(col-3)/3.f;
      vertices[row*7+col]=(ArRenderVertex2D){
        {x+u*rx+v*lean,y+v*ry},
        {color.r,color.g,color.b,color.a*CloudMesh_Falloff(u)*CloudMesh_Falloff(v)},{0,0}};
    }
  }
}

/* Shared harmonic silhouette for smoke puffs and their fire/ember parents.
 * The caller supplies the same quantized unit circle as its other effects. */
static inline float CloudMesh_Harmonic(const float circle[32][2],unsigned seed,unsigned ticks,int segment) {
  const int i1=(segment+(int)((ticks/3u+seed)&31))&31;
  const int i2=(segment*3+(int)((ticks/2u+seed*5u)&31))&31;
  const int i3=(segment*5+(int)((ticks+seed*11u)&31))&31;
  return .52f*circle[i1][0]+.31f*circle[i2][0]+.17f*circle[i3][0];
}
typedef struct CloudPuffStyle {
  float scales[3],whitening[2],alpha[3],distortion[2];
} CloudPuffStyle;
static const CloudPuffStyle kCloudSoftPuff={
  {.24f,.70f,1},{.42f,.24f},{1,.84f,.34f},{.035f,.055f}};
static inline void CloudMesh_Puff(float corners[kCloudPuffVertices][4],ArRenderColorF colors[4],
    const CloudPuffStyle *style,const float circle[32][2],ArRenderColorF tint,float opacity,
    unsigned seed,unsigned ticks) {
  static const float circle12[12][2]={
    {1,0},{.866025f,.5f},{.5f,.866025f},{0,1},{-.5f,.866025f},{-.866025f,.5f},
    {-1,0},{-.866025f,-.5f},{-.5f,-.866025f},{0,-1},{.5f,-.866025f},{.866025f,-.5f}};
  for(unsigned r=0;r<4;++r) {
    colors[r]=tint;
    if(r<2) {
      colors[r].r+=(1-tint.r)*style->whitening[r];
      colors[r].g+=(1-tint.g)*style->whitening[r];
      colors[r].b+=(1-tint.b)*style->whitening[r];
    }
    colors[r].a=r<3?opacity*style->alpha[r]:0;
  }
  for(unsigned i=0;i<4;++i)corners[0][i]=0;
  for(int r=0;r<kCloudPuffRings;++r)for(int s=0;s<kCloudPuffSegments;++s) {
    const float shape=1+(style->distortion[0]+style->distortion[1]*r)*
        CloudMesh_Harmonic(circle,seed,ticks,s*32/kCloudPuffSegments);
    float *p=corners[1+r*kCloudPuffSegments+s];
    p[0]=circle12[s][0]*style->scales[r]*shape;p[1]=0;p[2]=0;
    p[3]=circle12[s][1]*style->scales[r]*shape;
  }
}

/* Borrowed parameters keep complete field recipes and small mist nodes on
 * the same kernel without copying a field or changing its serialized shape. */
typedef struct CloudMistStyle {
  const float *volume,*billow,*density,*lighting,*color,*gain;
} CloudMistStyle;
static inline void CloudMesh_MistSlice(ArRenderVertex2D vertices[kCloudMistVertices],
    CloudMistStyle s,float left,float right,float floor,float height,
    float phase,unsigned slice,float illumination) {
  const float half=(right-left)*.5f;
  const float center=(left+right)*.5f+half*s.volume[2]*sinf(phase+slice);
  const float width=half*(s.volume[3]-s.volume[4]*slice);
  for(int col=0;col<5;++col) {
    const float u=(col-2)*.5f;
    const float billow=s.billow[0]+s.billow[1]*sinf(u*s.billow[3]+phase+slice)+
        s.billow[2]*cosf(u*s.billow[4]-phase*s.billow[5]);
    const float top=height*(s.volume[5]+s.volume[6]*billow-s.volume[7]*slice);
    for(int row=0;row<5;++row) {
      const float rise=1-row*.25f,across=1-u*u*u*u,q=fmaxf(0,1-rise*rise);
      const float density=(s.density[0]+s.density[1]*billow)*across*across*(q*q*q);
      const float light=s.lighting[0]+illumination+s.lighting[1]*rise+s.lighting[2]*billow;
      vertices[row*5+col]=(ArRenderVertex2D){
        {center+u*width,floor-top*rise},
        {fminf(1,s.color[0]+s.gain[0]*light),fminf(1,s.color[1]+s.gain[1]*light),
         fminf(1,s.color[2]+s.gain[2]*light),fminf(1,density)},{0,0}};
    }
  }
}
#endif
