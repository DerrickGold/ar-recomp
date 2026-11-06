#include "platform/sdl/diorama_depth_alpha_util.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include <float.h>

enum { W=127,H=83 };
static int indices[]={0,1,2};
static uint32_t random_state=17;
static float Random(float lo,float hi) {
  random_state=random_state*1664525u+1013904223u;
  return lo+(hi-lo)*(float)(random_state>>8)/16777215.f;
}
static bool Equal(ArRenderRectI a,ArRenderRectI b) {
  return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;
}
static double Cross(double ax,double ay,double bx,double by,double x,double y) {
  return (bx-ax)*(y-ay)-(by-ay)*(x-ax);
}
int main(void) {
  for(unsigned order=0;order<2;++order)for(unsigned key=0;key<2;++key) {
    const DioramaDepthAlphaTargetFormats f=DioramaDepthAlpha_SelectTargetFormats(order,key);
    assert(f.order==(order?SDL_GPU_TEXTUREFORMAT_R32_FLOAT:SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT));
    assert(f.key==(key?SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT:SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT));
  }
  assert(!DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_LOAD,false));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_LOAD,true));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_CLEAR,false));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_CLEAR,true));
  DioramaDepthVertex v[4]={{.clip={-.5f,-.5f,.2f,1}},{.clip={.5f,-.5f,.2f,1}},
      {.clip={0,.5f,.2f,1}},{.clip={100,100,0,1}}};
  DioramaDepthDraw d={.vertices=v,.vertex_count=4,.indices=indices,.index_count=3};
  const ArRenderRectI full={0,0,W,H};
  ArRenderRectI r=DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H);
  assert(r.x>0 && r.y>0 && r.w<W && r.h<H); /* Unused vertex is irrelevant. */
  d.clip_enabled=true;d.clip=(ArRenderRectI){50,30,10,10};
  assert(Equal(DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H),d.clip));
  d.clip_enabled=false;
  for(unsigned i=0;i<3;++i)v[i].clip[0]=-10;
  assert(!DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H).w);
  v[0].clip[3]=0;assert(Equal(DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H),full));
  v[0].clip[3]=-1;assert(Equal(DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H),full));
  v[0].clip[3]=1;v[0].clip[0]=NAN;assert(Equal(DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H),full));
  v[0].clip[0]=FLT_MAX;v[0].clip[3]=FLT_TRUE_MIN;
  r=DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H);assert(r.x>=0 && r.y>=0 && r.x+r.w<=W && r.y+r.h<=H);
  unsigned sampled=0;
  /* Independent pixel-center raster oracle, including triangles crossing
   * target edges, disjoint draw clips, skew, winding and highly varying W. */
  for(unsigned test=0;test<4000;++test) {
    double x[3],y[3];
    for(unsigned i=0;i<3;++i) {
      const float w=Random(.01f,8);
      v[i]=(DioramaDepthVertex){.clip={Random(-3,3)*w,Random(-3,3)*w,Random(-2,2)*w,w}};
      x[i]=(1+(double)v[i].clip[0]/w)*W*.5;
      y[i]=(1-(double)v[i].clip[1]/w)*H*.5;
    }
    d.clip_enabled=test&1;
    d.clip=(ArRenderRectI){test%40,test%20,30+(int)(test%50),20+(int)(test%40)};
    r=DioramaDepthAlpha_ResolveBounds(&d,0,1,W,H);
    const ArRenderRectI clip=d.clip_enabled?d.clip:full;
    for(int py=clip.y;py<clip.y+clip.h;++py)for(int px=clip.x;px<clip.x+clip.w;++px) {
      const double a=Cross(x[0],y[0],x[1],y[1],px+.5,py+.5);
      const double b=Cross(x[1],y[1],x[2],y[2],px+.5,py+.5);
      const double c=Cross(x[2],y[2],x[0],y[0],px+.5,py+.5);
      if((a>=0 && b>=0 && c>=0)||(a<=0 && b<=0 && c<=0)) {
        assert(px>=r.x && py>=r.y && px<r.x+r.w && py<r.y+r.h);++sampled;
      }
    }
    DioramaDepthDraw pair[]={d,d};pair[1].clip_enabled=true;pair[1].clip=(ArRenderRectI){0,0,12,12};
    ArRenderRectI both=DioramaDepthAlpha_ResolveBounds(pair,0,2,W,H);
    if(r.w && r.h)assert(both.x<=r.x && both.y<=r.y && both.x+both.w>=r.x+r.w && both.y+both.h>=r.y+r.h);
  }
  printf("PASS: 4000 projected triangles, %u covered pixel centers; clip W, scissor, union and full-fallback cases\n",sampled);
}
