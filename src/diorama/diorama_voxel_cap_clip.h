#ifndef AR_DIORAMA_VOXEL_CAP_CLIP_H
#define AR_DIORAMA_VOXEL_CAP_CLIP_H
#include <math.h>
#include <stdbool.h>
/* Clamping only a sloping cap's endpoints loses any texture tile crossed
 * entirely between them. Split at every clip-plane crossing first, so each
 * resulting quad is the true clipped polygon, including actor-plane cuts. */
static inline unsigned AuthoredCapCutsShifted(const float near[2],const float far[2],float z0,float z1,
    bool high,const float split[2],float cuts[8]) {
  unsigned n=2;cuts[0]=0;cuts[1]=1;
  const float planes[]={z0,z1};
  for(unsigned edge=0;edge<2;++edge) {
    const float *z=edge?far:near;
    for(unsigned p=0;p<(high?2:3);++p) {
      const float denominator=z[1]-z[0]-(p==2?split[1]-split[0]:0);
      if(fabsf(denominator)<.000001f)continue;
      const float t=((p==2?split[0]:planes[p])-z[0])/denominator;if(t<=0 || t>=1)continue;
      unsigned at=0;while(at<n && cuts[at]<t)++at;
      if(at<n && fabsf(cuts[at]-t)<.000001f)continue;
      for(unsigned i=n;i>at;--i)cuts[i]=cuts[i-1];cuts[at]=t;++n;
    }
  }
  return n;
}
static inline unsigned AuthoredCapCuts(const float near[2],const float far[2],float z0,float z1,
    bool high,float split,float cuts[8]) {
  const float plane[]={split,split};return AuthoredCapCutsShifted(near,far,z0,z1,high,plane,cuts);
}
static inline void AuthoredCapSpan(float *near,float *far,bool high,bool front_phase,float split) {
  if(high)return;
  if(front_phase){*far=fminf(*far,split);*near=fminf(*near,*far);}
  else {*near=fmaxf(*near,split);*far=fmaxf(*near,*far);}
}
#endif
