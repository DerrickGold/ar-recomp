/* Keep ordinary Desert faces on their original capture-anchored triangles.
 * Only the compiled grounded shaft cells leave that plane. Rectangles are
 * source ownership, not extra walls or synthesized texture. */
enum { kDesertColumnCells=(SR_PPU_SURFACE_MAX_WIDTH/8+1)*(SR_PPU_SURFACE_MAX_HEIGHT/8+1) };
struct DioramaDesertColumnMask {
  ArRenderRectF rect[kDesertColumnCells];
  unsigned count;
};
static bool BuildDesertColumnMask(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,DioramaDesertColumnMask *mask) {
  mask->count=0;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  const bool bounded=layer->voxel_source_rect.w>0;
  const float l=bounded?layer->voxel_source_rect.x:g->u0*SR_PPU_SURFACE_MAX_WIDTH;
  const float r=bounded?l+layer->voxel_source_rect.w:g->u1*SR_PPU_SURFACE_MAX_WIDTH;
  const float t=bounded?layer->voxel_source_rect.y:g->v0*SR_PPU_SURFACE_MAX_HEIGHT;
  const float b=bounded?t+layer->voxel_source_rect.h:g->v1*SR_PPU_SURFACE_MAX_HEIGHT;
  const int x0=(int)floorf((l+ox)/8),y0=(int)floorf((t+oy)/8);
  const int nx=(int)ceilf((r+ox)/8)-x0,ny=(int)ceilf((b+oy)/8)-y0;
  if(nx<=0||ny<=0||nx>(int)SR_PPU_SURFACE_MAX_WIDTH/8+1||ny>(int)SR_PPU_SURFACE_MAX_HEIGHT/8+1)return false;
  uint8_t cells[kDesertColumnCells]={0};
  for(int y=0;y<ny;++y)for(int x=0;x<nx;++x)
    cells[y*nx+x]=ActionVoxel_ColumnCurveForCell(layer->voxel_geometry,x0+x,y0+y)!=NULL;
  for(int y=0;y<ny;++y)for(int x=0;x<nx;++x) {
    if(!cells[y*nx+x])continue;
    int ex=x+1,ey=y+1;while(ex<nx&&cells[y*nx+ex])++ex;
    for(;ey<ny;++ey) {
      bool same=true;for(int xx=x;xx<ex;++xx)if(!cells[ey*nx+xx]){same=false;break;}
      if(!same)break;
    }
    for(int yy=y;yy<ey;++yy)for(int xx=x;xx<ex;++xx)cells[yy*nx+xx]=0;
    const float left=fmaxf(l,(x0+x)*8-ox),right=fminf(r,(x0+ex)*8-ox);
    const float top=fmaxf(t,(y0+y)*8-oy),bottom=fminf(b,(y0+ey)*8-oy);
    if(right>left&&bottom>top)mask->rect[mask->count++]=(ArRenderRectF){
      left/SR_PPU_SURFACE_MAX_WIDTH,top/SR_PPU_SURFACE_MAX_HEIGHT,
      (right-left)/SR_PPU_SURFACE_MAX_WIDTH,(bottom-top)/SR_PPU_SURFACE_MAX_HEIGHT};
  }
  return true;
}

typedef struct DesertColumnSubmit {
  const DioramaLayerDraw *layer;
  ArRenderRectF source_uv;
  ArRenderVertex2D *vertices;
  float *depths;
  int32_t *indices;
  unsigned count,capacity;
} DesertColumnSubmit;
static DesertColumnSubmit s_desert_column_scratch;
static void ResetDesertColumnScratch(void) {
  free(s_desert_column_scratch.vertices);free(s_desert_column_scratch.depths);
  free(s_desert_column_scratch.indices);memset(&s_desert_column_scratch,0,sizeof(s_desert_column_scratch));
}
static bool ReserveDesertColumnScratch(unsigned count) {
  if(count<=s_desert_column_scratch.capacity)return true;
  ArRenderVertex2D *v=malloc((size_t)count*sizeof(*v));
  float *d=malloc((size_t)count*sizeof(*d));int32_t *i=malloc((size_t)count*sizeof(*i));
  if(!v||!d||!i){free(v);free(d);free(i);return false;}
  ResetDesertColumnScratch();s_desert_column_scratch=(DesertColumnSubmit){
    .vertices=v,.depths=d,.indices=i,.capacity=count};return true;
}
static bool SubmitDesertColumnPolygon(DesertColumnSubmit *s,
    const DioramaVoxelSourceVertex *p,unsigned n) {
  for(unsigned fan=1;fan+1<n;++fan) {
    if(s->count>INT_MAX-3)return false;
    if(s->vertices) {
      if(s->count+3>s->capacity)return false;
      const unsigned corners[]={0,fan,fan+1};
      for(unsigned j=0;j<3;++j) {
        s->vertices[s->count+j]=p[corners[j]].vertex;
        s->depths[s->count+j]=p[corners[j]].depth;
        s->indices[s->count+j]=(int32_t)(s->count+j);
      }
    }
    s->count+=3;
  }
  return true;
}
static double DesertColumnPolygonArea(const DioramaVoxelSourceVertex *p,unsigned n) {
  double area=0;for(unsigned i=0;i<n;++i) {
    const ArRenderPointF a=p[i].vertex.tex_coord,b=p[(i+1)%n].vertex.tex_coord;
    area+=(double)a.x*b.y-(double)a.y*b.x;
  }
  return fabs(area);
}
/* Subtract each rectangle as four disjoint half-plane pieces. Intersections
 * interpolate the existing screen-linear native triangle, including shade
 * and depth. No new projection or diagonal is introduced outside ownership. */
static bool ClipDesertColumnSource(DesertColumnSubmit *s,
    const DioramaVoxelSourceVertex *p,unsigned n,unsigned first) {
  const DioramaDesertColumnMask *mask=s->layer->desert_columns;
  for(unsigned at=first;at<mask->count;++at) {
    const ArRenderRectF r=mask->rect[at];
    const float limits[]={ (r.x-s->source_uv.x)/s->source_uv.w,
      (r.x+r.w-s->source_uv.x)/s->source_uv.w,
      (r.y-s->source_uv.y)/s->source_uv.h,
      (r.y+r.h-s->source_uv.y)/s->source_uv.h };
    const unsigned axes[]={0,0,1,1};const bool greater[]={true,false,true,false};
    DioramaVoxelSourceVertex inside[8],scratch[8];unsigned m=n;
    memcpy(inside,p,n*sizeof(*p));
    for(unsigned side=0;side<4&&m;++side) {
      m=DioramaVoxelSource_ClipLimit(inside,m,axes[side],limits[side],greater[side],false,scratch);
      memcpy(inside,scratch,m*sizeof(*inside));
    }
    if(m<3||DesertColumnPolygonArea(inside,m)==0)continue;
    m=n;memcpy(inside,p,n*sizeof(*p));
    for(unsigned side=0;side<4&&m;++side) {
      unsigned outside=DioramaVoxelSource_ClipLimit(inside,m,axes[side],limits[side],!greater[side],false,scratch);
      if(outside>=3&&DesertColumnPolygonArea(scratch,outside)>0&&
          !ClipDesertColumnSource(s,scratch,outside,at+1))return false;
      m=DioramaVoxelSource_ClipLimit(inside,m,axes[side],limits[side],greater[side],false,scratch);
      memcpy(inside,scratch,m*sizeof(*inside));
    }
    return true;
  }
  return SubmitDesertColumnPolygon(s,p,n);
}
static bool RenderDesertColumnSource(ArRenderDevice *device,const DioramaLayerDraw *layer,
    ArRenderTexture texture,const ArRenderVertex2D *vertices,const float *depths,
    int nv,const int32_t *indices,int ni,ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *lighting,const DioramaDepthSink *sink,bool group_sources) {
  if(!layer->desert_columns||!layer->desert_columns->count)
    return RenderVoxelNativeSources(device,layer,texture,vertices,depths,NULL,nv,indices,ni,
        layer->blend,layer->exposure,source_uv,lighting,sink,true,false,group_sources);
  if(ni%3||source_uv.w<=0||source_uv.h<=0)return false;
  DesertColumnSubmit submit={.layer=layer,.source_uv=source_uv};bool ok=true;
  /* Count then pack one bounded batch. The guard splitter still sees the
   * entire original ordered stream, retaining its first-target flush prefix
   * and body/fringe grouping. No per-triangle draw or new texture is used. */
  for(unsigned pass=0;pass<2&&ok;++pass) {
    submit.count=0;
    for(int i=0;i<ni&&ok;i+=3) {
      DioramaVoxelSourceVertex triangle[3];
      for(unsigned j=0;j<3;++j) {
        const int v=indices[i+j];if(v<0||v>=nv){ok=false;break;}
        triangle[j]=(DioramaVoxelSourceVertex){vertices[v],depths?depths[v]:0,1};
      }
      if(ok)ok=ClipDesertColumnSource(&submit,triangle,3,0);
    }
    if(!pass&&ok) {
      if(!submit.count)return true;
      ok=ReserveDesertColumnScratch(submit.count);
      if(ok) {
        submit.capacity=s_desert_column_scratch.capacity;
        submit.vertices=s_desert_column_scratch.vertices;submit.depths=s_desert_column_scratch.depths;
        submit.indices=s_desert_column_scratch.indices;
      }
    }
  }
  if(ok)ok=RenderVoxelNativeSources(device,layer,texture,submit.vertices,depths?submit.depths:NULL,
      NULL,(int)submit.count,submit.indices,(int)submit.count,layer->blend,layer->exposure,
      source_uv,lighting,sink,true,false,group_sources);
  return ok;
}
