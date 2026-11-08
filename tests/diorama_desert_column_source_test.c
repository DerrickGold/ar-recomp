#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "render/render_device.h"
#include "diorama/diorama_voxel_source_clip.h"
#define SR_PPU_SURFACE_MAX_WIDTH 640
#define SR_PPU_SURFACE_MAX_HEIGHT 352
/* Minimal recorder surrounds the actual private source-submission helper. */
typedef struct {int width,height,bg1_camera_x,camera_y,authentic_y0,obj_apron;} DioramaCapture;
typedef struct {float u0,u1,v0,v1;} DioramaViewGeometry;
typedef struct {int unused;} DioramaDepthSink;
typedef struct {int unused;} DioramaSceneryLightEffectParams;
typedef struct DioramaDesertColumnMask DioramaDesertColumnMask;
typedef struct {int plane;} Plane;
typedef struct {const DioramaDesertColumnMask *desert_columns;const void *voxel_geometry;
  ArRenderRectF voxel_source_rect;ArRenderBlendMode blend;const void *exposure;const Plane *description;} DioramaLayerDraw;
static int mask_x0,mask_y0,mask_x1,mask_y1;
static const void *ActionVoxel_ColumnCurveForCell(const void *g,int x,int y) {
  return g&&x>=mask_x0&&x<mask_x1&&y>=mask_y0&&y<mask_y1?g:NULL;
}
static ArRenderVertex2D recorded[20000];static float recorded_depth[20000];
static unsigned count,calls;static const ArRenderVertex2D *first_pointer;
static bool RenderShadedDioramaDepthGeometry(ArRenderDevice *d,ArRenderTexture texture,
    const ArRenderVertex2D *v,const float *depths,int nv,const int32_t *index,int ni,
    ArRenderBlendMode blend,const void *exposure,ArRenderRectF source,
    const DioramaSceneryLightEffectParams *light,const DioramaDepthSink *sink,int plane,bool actor) {
  (void)d;(void)texture;(void)nv;(void)blend;(void)exposure;(void)source;(void)light;(void)sink;(void)plane;(void)actor;
  if(!calls)first_pointer=v;++calls;assert(count+(unsigned)ni<20000);
  for(int i=0;i<ni;++i) {recorded[count]=v[index[i]];recorded_depth[count++]=depths?depths[index[i]]:0;}
  return true;
}
static bool RenderVoxelNativeSources(ArRenderDevice *d,const DioramaLayerDraw *layer,ArRenderTexture texture,
    const ArRenderVertex2D *v,const float *depths,const float *w,int nv,const int32_t *index,int ni,
    ArRenderBlendMode blend,const void *exposure,ArRenderRectF source,
    const DioramaSceneryLightEffectParams *light,const DioramaDepthSink *sink,bool body,bool rear,bool group) {
  assert(!w&&body&&!rear);(void)group;
  return RenderShadedDioramaDepthGeometry(d,texture,v,depths,nv,index,ni,blend,exposure,source,light,sink,layer->description->plane,false);
}
#include "../src/diorama/diorama_desert_columns_render.inc"
static double area(const ArRenderVertex2D *v) {
  return fabs((double)(v[1].tex_coord.x-v[0].tex_coord.x)*(v[2].tex_coord.y-v[0].tex_coord.y)-
      (double)(v[1].tex_coord.y-v[0].tex_coord.y)*(v[2].tex_coord.x-v[0].tex_coord.x))*.5;
}
static ArRenderVertex2D vertex(float u,float v) {
  return (ArRenderVertex2D){.position={3*u+5*v,7*u-2*v},.tex_coord={u,v},.color={u,v,u+v,1}};
}
int main(void) {
  static DioramaDesertColumnMask mask;const Plane plane={6};
  DioramaLayerDraw layer={.desert_columns=&mask,.voxel_geometry=&mask,.description=&plane};
  const ArRenderVertex2D vertices[]={vertex(0,0),vertex(1,0),vertex(1,1),vertex(0,1)};
  const float depths[]={0,2,5,3};const int32_t indices[]={0,1,2,0,2,3};
  const ArRenderRectF source={.125f,.25f,.5f,.5f};
  assert(RenderDesertColumnSource(NULL,&layer,ArRenderTexture_Invalid(),vertices,depths,4,indices,6,source,NULL,NULL,true));
  assert(calls==1&&count==6&&first_pointer==vertices);
  for(unsigned i=0;i<6;++i)assert(!memcmp(recorded+i,vertices+indices[i],sizeof(*vertices)));
  for(unsigned compact=0;compact<2;++compact) {
    count=calls=0;mask.count=2;
    const ArRenderRectF domain=compact?source:(ArRenderRectF){0,0,1,1};
    mask.rect[0]=(ArRenderRectF){domain.x+.25f*domain.w,domain.y+.25f*domain.h,.25f*domain.w,.5f*domain.h};
    mask.rect[1]=(ArRenderRectF){domain.x+.75f*domain.w,domain.y,domain.w*.25f,domain.h*.25f};
    assert(RenderDesertColumnSource(NULL,&layer,ArRenderTexture_Invalid(),vertices,depths,4,indices,6,domain,NULL,NULL,false));
    double covered=0;
    for(unsigned i=0;i<count;i+=3) {
      covered+=area(recorded+i);float u=0,v=0;
      for(unsigned j=0;j<3;++j) {
        const ArRenderVertex2D *p=recorded+i+j;const float x=p->tex_coord.x,y=p->tex_coord.y;
        assert(fabsf(p->position.x-(3*x+5*y))<1e-6f&&fabsf(p->position.y-(7*x-2*y))<1e-6f);
        assert(fabsf(p->color.r-x)<1e-6f&&fabsf(p->color.g-y)<1e-6f&&fabsf(p->color.b-x-y)<1e-6f);
        assert(fabsf(recorded_depth[i+j]-(2*x+3*y))<1e-6f);u+=x/3;v+=y/3;
      }
      assert(!(u>.25f&&u<.5f&&v>.25f&&v<.75f));
      assert(!(u>.75f&&u<1&&v>0&&v<.25f));
    }
    assert(fabs(covered-(1-.125-.0625))<1e-7);
  }
  /* A native cell exactly outside the capture cannot switch the mesh path;
   * negative origins and apron use the same primary-source conversion. */
  for(unsigned negative=0;negative<2;++negative) {
    DioramaCapture capture={.width=624,.height=352,.bg1_camera_x=negative?-80:400,
      .camera_y=negative?-32:64,.authentic_y0=64,.obj_apron=0};
    DioramaViewGeometry view={.u0=0,.u1=624.f/640,.v0=0,.v1=1};
    int ox=capture.bg1_camera_x-184,oy=capture.camera_y-63;
    mask_x0=(int)floorf((ox+120)/8.f);mask_x1=mask_x0+4;
    mask_y0=(int)floorf((oy+80)/8.f);mask_y1=mask_y0+6;
    assert(BuildDesertColumnMask(&capture,&view,&layer,&mask));assert(mask.count==1);
    assert(fabsf(mask.rect[0].x-(mask_x0*8-ox)/640.f)<1e-7f);
    assert(fabsf(mask.rect[0].y-(mask_y0*8-oy)/352.f)<1e-7f);
    assert(fabsf(mask.rect[0].w-32.f/640)<1e-7f&&fabsf(mask.rect[0].h-48.f/352)<1e-7f);
    mask_x0=10000;mask_x1=10004;assert(BuildDesertColumnMask(&capture,&view,&layer,&mask));assert(!mask.count);
    view.u1=2;assert(!BuildDesertColumnMask(&capture,&view,&layer,&mask));
  }
  ResetDesertColumnScratch();assert(!s_desert_column_scratch.capacity);
  puts("PASS actual source submission: empty exact triangles, two disjoint cutouts, compact/native UV, original affine shade/depth, finite/negative source ownership");
}
