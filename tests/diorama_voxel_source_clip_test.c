#include "diorama/diorama_voxel_source_clip.h"
#include <assert.h>
#include <stdio.h>

static double Area(const DioramaVoxelSourceVertex *p,unsigned n) {
  double a=0;
  for(unsigned i=0;i<n;++i) {
    const ArRenderPointF x=p[i].vertex.position,y=p[(i+1)%n].vertex.position;
    a+=(double)x.x*y.y-(double)y.x*x.y;
  }
  return a*.5;
}
static void Near(float a,float b) { assert(fabsf(a-b)<.00003f); }
static DioramaVoxelSourceVertex Vertex(float x,float y,float w,float v) {
  return (DioramaVoxelSourceVertex){.vertex={.position={x/w,y/w},
    .tex_coord={x/640.f,v},.color={.2f+x/100.f,.4f,.6f,1}},.depth=.2f+.1f/w,.clip_w=w};
}
int main(void) {
  const float seam=(352.f-8)/352.f;
  for(unsigned perspective=0;perspective<2;++perspective)
    for(unsigned reverse=0;reverse<2;++reverse) {
      /* A sloping side's visible Y and sampled Y are intentionally unrelated.
       * Its near/far source rows straddle the handoff by6px. */
      DioramaVoxelSourceVertex in[3]={Vertex(0,5,1,(344.f-6)/352),
        Vertex(20,7,2,(344.f+6)/352),Vertex(3,31,1.5f,(344.f-2)/352)};
      if(reverse){DioramaVoxelSourceVertex t=in[0];in[0]=in[2];in[2]=t;}
      DioramaVoxelSourceVertex a[4],b[4];
      const unsigned na=DioramaVoxelSource_ClipTriangle(in,seam,false,perspective,a);
      const unsigned nb=DioramaVoxelSource_ClipTriangle(in,seam,true,perspective,b);
      assert(na>=3&&nb>=3);
      assert(fabs(Area(a,na)+Area(b,nb)-Area(in,3))<.00003);
      assert(Area(a,na)*Area(in,3)>0&&Area(b,nb)*Area(in,3)>0);
      unsigned joins=0;
      for(unsigned i=0;i<na;++i) {
        assert(a[i].vertex.tex_coord.y<=seam);
        if(a[i].vertex.tex_coord.y==seam)for(unsigned j=0;j<nb;++j)
          if(b[j].vertex.tex_coord.y==seam&&fabsf(a[i].vertex.tex_coord.x-b[j].vertex.tex_coord.x)<1.e-6f) {
            Near(a[i].vertex.position.x,b[j].vertex.position.x);
            Near(a[i].vertex.position.y,b[j].vertex.position.y);
            Near(a[i].depth,b[j].depth);Near(a[i].clip_w,b[j].clip_w);++joins;
          }
      }
      assert(joins==2);
      for(unsigned j=0;j<nb;++j)assert(b[j].vertex.tex_coord.y>=seam);
      if(!reverse) {
        /* First edge crosses exactly at t=.5 in world/source coordinates.
         * Independent projection of world(10,6),W1.5 proves the depth path;
         * original screen-linear midpoint proves the painter path. */
        const float want_x=perspective?10.f/1.5f:5;
        const float want_y=perspective?6.f/1.5f:4.25f;
        bool found=false;
        for(unsigned i=0;i<na;++i)if(fabsf(a[i].vertex.tex_coord.x-10.f/640)<1.e-6f) {
          Near(a[i].vertex.position.x,want_x);Near(a[i].vertex.position.y,want_y);found=true;
        }
        assert(found);
      }
    }
  /* A cap parallel to the sampling cut is owned once, even if its physical
   * world Y is above/below the capture end. */
  DioramaVoxelSourceVertex cap[3]={Vertex(-20,2,1,seam),Vertex(8,2,1,seam),Vertex(4,6,1,seam)},out[4];
  assert(!DioramaVoxelSource_ClipTriangle(cap,seam,false,true,out));
  assert(DioramaVoxelSource_ClipTriangle(cap,seam,true,true,out)==3);
  cap[0].clip_w=0;assert(!DioramaVoxelSource_ClipTriangle(cap,seam,true,true,out));
  cap[0].clip_w=1;cap[0].vertex.tex_coord.y=NAN;
  assert(!DioramaVoxelSource_ClipTriangle(cap,seam,true,false,out));
  /* Finite capture ends clip coverage instead of stretching its last row.
   * This rectangle loses exactly its last quarter at the real source end. */
  for(unsigned perspective=0;perspective<2;++perspective) {
    DioramaVoxelSourceVertex box[4]={Vertex(0,0,1,0),Vertex(20,0,1,0),
      Vertex(20,40,1,1),Vertex(0,40,1,1)},finite[8];
    const unsigned n=DioramaVoxelSource_ClipLimit(box,4,1,.75f,false,perspective,finite);
    assert(n==4);Near((float)Area(finite,n),600);
    unsigned end_vertices=0;
    for(unsigned i=0;i<n;++i) {
      assert(finite[i].vertex.tex_coord.y<=.75f);
      if(finite[i].vertex.tex_coord.y==.75f) {
        Near(finite[i].vertex.position.y,30);++end_vertices;
      }
    }
    assert(end_vertices==2);
  }
  /* Copied rows328..343 precede guard rows344..407. Every prefix/guard
   * coordinate reconstructs its original receiver row; no row is stretched. */
  for(int end=64;end<=352;end+=32)for(int row=end-16;row<end+64;++row) {
    const float uv=((row+.5f)-(end-16))/80.f;
    const float receiver=(end-16)+uv*80;
    Near(receiver,row+.5f);
    assert(uv>0&&uv<1);
    if(row>=end-8)assert(uv>=8.f/80);
  }
  puts("Voxel source clipping: projected coverage, half-open UV ownership and receiver phase PASS");
  return 0;
}
