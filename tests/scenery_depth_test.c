#include "render/scenery_shading.h"
#include "diorama/diorama_edge_aa.h"
#include "support/test_assert.h"
#include <stdio.h>

typedef struct Check { unsigned draws,vertices,fail_at; double area; } Check;
static float Depth(ArRenderPointF p) { return .2f + p.x*.0007f + p.y*.0003f; }
static float Shade(void *ctx,float u,float v,float x,float y) {
  (void)ctx;(void)u;(void)v;return x<.5f?.4f:y<.5f?.7f:1;
}
static bool Uniform(void *ctx,float u0,float v0,float u1,float v1,float *shade) {
  (void)ctx;(void)u0;(void)v0;(void)u1;(void)v1;*shade=.5f;return true;
}
static bool Submit(void *ctx,const ArRenderVertex2D *v,const float *z,int nv,const int32_t *indices,int ni) {
  Check *c=ctx;assert(z && nv>0 && ni>0 && ni%3==0);++c->draws;
  if(c->fail_at==c->draws)return false;
  c->vertices+=nv;
  for(int i=0;i<nv;++i) {
    assert(fabsf(z[i]-Depth(v[i].position))<.000001f);
    assert(v[i].color.a==1);
  }
  for(int i=0;i<ni;i+=3) {
    for(unsigned j=0;j<3;++j)assert(indices[i+j]>=0 && indices[i+j]<nv);
    const ArRenderPointF a=v[indices[i]].position,b=v[indices[i+1]].position,d=v[indices[i+2]].position;
    c->area+=fabs((double)(b.x-a.x)*(d.y-a.y)-(double)(b.y-a.y)*(d.x-a.x))*.5;
  }
  return true;
}
int main(void) {
  const ArRenderVertex2D vertices[]={{{0,0},{1,1,1,1},{0,0}},{{640,0},{1,1,1,1},{1,0}},
      {{640,352},{1,1,1,1},{1,1}},{{0,352},{1,1,1,1},{0,1}}};
  const int32_t indices[]={0,1,2,0,2,3},reverse[]={3,2,0,2,1,0};
  float depths[4];for(unsigned i=0;i<4;++i)depths[i]=Depth(vertices[i].position);
  float cuts[128];for(unsigned i=0;i<128;++i)cuts[i]=(i+1)/129.f;
  const SceneryShadingField field={.sample=Shade,.x=cuts,.y=cuts,.nx=128,.ny=128};
  unsigned submitted=0;
  for(unsigned mode=0;mode<6;++mode) {
    SceneryShadingField f=field;
    if(mode==0)f.sample=NULL;
    if(mode==1)f.uniform=Uniform;
    if(mode==2)f.nx=f.ny=0;
    Check c={0};
    assert(SceneryShading_DrawDepth(&f,(ArRenderRectF){0,0,1,1},vertices,depths,4,
        mode==4?reverse:indices,6,Submit,&c));
    assert(fabs(c.area-640.*352)<.1);
    assert(c.draws && (mode<3?c.draws==1:c.draws>1));submitted+=c.vertices;
    if(mode>=3) {
      c=(Check){.fail_at=2};
      assert(!SceneryShading_DrawDepth(&f,(ArRenderRectF){0,0,1,1},vertices,depths,4,indices,6,Submit,&c));
      assert(c.draws==2);
    }
  }
  /* Side extrusions can have a collapsed source UV axis while their projected
   * triangle still has area and a depth gradient. */
  ArRenderVertex2D side[4];memcpy(side,vertices,sizeof(side));
  for(unsigned i=0;i<4;++i)side[i].tex_coord.x=.5f;
  Check c={0};assert(SceneryShading_DrawDepth(&field,(ArRenderRectF){.1f,.2f,.8f,.7f},
      side,depths,4,indices,6,Submit,&c));
  /* Many tiny cuts accumulate float endpoint error: bound total area to one
   * part per million, below a .001px strip across the original rectangle. */
  assert(fabs(c.area-640.*352)<640.*352*1e-6);
  /* Coverage fringe must carry its boundary sample's depth without reading
   * through absent sidecars. Legacy and depth-enabled geometry stays equal. */
  ArRenderVertex2D grid[]={vertices[0],vertices[1],vertices[3],vertices[2]},a[8],b[8];
  const float grid_z[]={depths[0],depths[1],depths[3],depths[2]};
  float fringe_z[8];int32_t ai[24],bi[24];int an,ain,bn,bin;
  assert(DioramaEdgeAa_BuildFringe(grid,1,1,1,kDioramaEdgeAa_All,0,1,0,1,1.f/640,1.f/352,a,8,ai,24,&an,&ain));
  assert(DioramaEdgeAa_BuildFringeDepth(grid,grid_z,1,1,1,kDioramaEdgeAa_All,0,1,0,1,
      1.f/640,1.f/352,b,fringe_z,8,bi,24,&bn,&bin));
  assert(an==bn && ain==bin && !memcmp(a,b,sizeof(a)) && !memcmp(ai,bi,sizeof(ai)));
  for(unsigned i=0;i<4;++i) {
    assert(fabsf(fringe_z[i*2]-Depth(b[i*2].position))<.000001f);
    assert(fringe_z[i*2]==fringe_z[i*2+1] && b[i*2+1].color.a==0);
  }
  assert(!DioramaEdgeAa_BuildFringeDepth(grid,NULL,1,1,1,kDioramaEdgeAa_All,0,1,0,1,
      1.f/640,1.f/352,b,fringe_z,8,bi,24,&bn,&bin));assert(!bn && !bin);
  printf("Scenery depth: %u clipped vertices, exact planar depth/area, fast paths, reversed edges, collapsed UVs, streamed failure and fringe parity passed\n",submitted);
}
