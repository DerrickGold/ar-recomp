#include "diorama_ground_cover.h"
#include <math.h>
#include <string.h>

static uint64_t GroundCoverKey(uint64_t key,const void *data,size_t bytes) {
  const unsigned char *p=data;
  for(size_t i=0;i<bytes;++i){key^=p[i];key*=UINT64_C(1099511628211);}
  return key;
}
static bool GroundCoverSupportInsert(ActionGroundCoverInterval *out,unsigned *count,float lo,float hi) {
  if(hi<=lo)return true;
  unsigned at=0;while(at<*count&&out[at].t1<lo-.00001f)++at;
  unsigned end=at;
  while(end<*count&&out[end].t0<=hi+.00001f) {
    lo=fminf(lo,out[end].t0);hi=fmaxf(hi,out[end].t1);++end;
  }
  if(at==end&&*count==kActionGroundCoverMaxIntervals)return false;
  memmove(out+at+1,out+end,(*count-end)*sizeof(*out));
  *count=*count-(end-at)+1;out[at]=(ActionGroundCoverInterval){lo,hi};return true;
}
/* Authored snow traces name a native support tile/run, not every jagged pixel
 * in its outline. Test the complete one-pixel receiver footprint against that
 * run; this retains the native raster allowance on two-pixel collision steps
 * without widening exact matching or attaching to another stacked ledge. */
static bool GroundCoverReceiverSupport(const ActionGroundCoverBank *bank,
    DioramaGroundCoverOwner owner,ActionGroundCoverPoint from,ActionGroundCoverPoint to,
    float tolerance,DioramaGroundCoverReceiverFn receiver,
    ActionGroundCoverInterval *out,unsigned *count) {
  *count=0;
  if(!receiver||!isfinite(from.x)||!isfinite(from.y)||!isfinite(to.x)||!isfinite(to.y)||
      !isfinite(tolerance)||tolerance<0||tolerance>ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE||
      fabsf(to.x-from.x)<.00001f)return false;
  const float dx=to.x-from.x,dy=to.y-from.y;
  const float left=fmaxf(fminf(from.x,to.x),bank->points[0].x);
  const float right=fminf(fmaxf(from.x,to.x),bank->points[bank->point_count-1].x);
  if(right<=left)return true;
  if(right-left>4096)return false; /* Finite native/editor cap budget. */
  for(int x=(int)floorf(left);x<ceilf(right);++x) {
    const float xa=fmaxf(left,x),xb=fminf(right,x+1);
    const float ya=from.y+dy*(xa-from.x)/dx,yb=from.y+dy*(xb-from.x)/dx;
    const int y=(int)floorf((ya+yb)*.5f+.00001f);int bottom;
    if(!receiver(owner.asset,x,y,&bottom)) {
      ActionGroundCoverInterval exact[kActionGroundCoverMaxIntervals];unsigned n=0;
      if(!ActionGroundCover_ClipSupport(bank,(ActionGroundCoverPoint){xa,ya},
          (ActionGroundCoverPoint){xb,yb},tolerance,exact,kActionGroundCoverMaxIntervals,&n))return false;
      for(unsigned i=0;i<n;++i) {
        float a=(xa+(xb-xa)*exact[i].t0-from.x)/dx,b=(xa+(xb-xa)*exact[i].t1-from.x)/dx;
        if(a>b){const float swap=a;a=b;b=swap;}
        if(!GroundCoverSupportInsert(out,count,a,b))return false;
      }
      continue;
    }
    if(bottom<=y||bottom>floorf(y/16.f)*16+16)return false;
    for(unsigned i=1;i<bank->point_count;++i) {
      const ActionGroundCoverPoint a=bank->points[i-1],b=bank->points[i];
      const float lo=fmaxf(xa,a.x),hi=fminf(xb,b.x);if(hi<=lo)continue;
      const float slope=(b.y-a.y)/(b.x-a.x);
      const float pa=a.y+(lo-a.x)*slope,pb=a.y+(hi-a.x)*slope;
      const float ca=from.y+dy*(lo-from.x)/dx,cb=from.y+dy*(hi-from.x)/dx;
      /* The path must touch this continuous ink run somewhere within the
       * pixel footprint. A gap below the top terminates ownership. */
      if(fmaxf(pa-ca,pb-cb)<-tolerance||fminf(pa,pb)>bottom)continue;
      float ta=(lo-from.x)/dx,tb=(hi-from.x)/dx;
      if(ta>tb){const float swap=ta;ta=tb;tb=swap;}
      if(!GroundCoverSupportInsert(out,count,ta,tb))return false;
    }
  }
  return true;
}
static bool DioramaGroundCover_Associate(DioramaGroundCoverCache *cache,
    DioramaGroundCoverOwner owner,const ActionGroundCoverBank *bank,
    ActionGroundCoverPoint from,ActionGroundCoverPoint to,float tolerance,
    DioramaGroundCoverReceiverFn receiver,
    const DioramaGroundCoverAssociation **out) {
  if(out)*out=NULL;
  if(!cache||!bank||!out)return false;
  uint64_t hash=GroundCoverKey(bank->content_key,&owner.asset,sizeof(owner.asset));
  hash=GroundCoverKey(hash,&owner.revision,sizeof(owner.revision));
  hash=GroundCoverKey(hash,&owner.kind,sizeof(owner.kind));
  hash=GroundCoverKey(hash,&from,sizeof(from));hash=GroundCoverKey(hash,&to,sizeof(to));
  hash=GroundCoverKey(hash,&tolerance,sizeof(tolerance));
  hash=GroundCoverKey(hash,&receiver,sizeof(receiver));
  DioramaGroundCoverAssociation *a=&cache->entries[hash%kDioramaGroundCoverCacheSize];
  if(a->valid&&a->owner.asset==owner.asset&&a->owner.revision==owner.revision&&
      a->owner.kind==owner.kind&&a->bank_key==bank->content_key&&
      !memcmp(&a->from,&from,sizeof(from))&&!memcmp(&a->to,&to,sizeof(to))&&a->tolerance==tolerance&&a->receiver==receiver) {
    ++cache->hits;*out=a;return a->success;
  }
  ++cache->misses;
  *a=(DioramaGroundCoverAssociation){.owner=owner,.bank_key=bank->content_key,
      .from=from,.to=to,.tolerance=tolerance,.receiver=receiver,.valid=true};
  a->success=receiver?GroundCoverReceiverSupport(bank,owner,from,to,tolerance,receiver,a->intervals,&a->count):
      ActionGroundCover_ClipSupport(bank,from,to,tolerance,a->intervals,kActionGroundCoverMaxIntervals,&a->count);
  *out=a;return a->success;
}

static float GroundCoverFraction(float value) { return fminf(1,fmaxf(0,value)); }
/* Interpolate the actual 012/023 receiver triangles, never a bilinear patch. */
static DioramaGroundCoverCorner GroundCoverCorner(const DioramaGroundCoverCap *cap,float t,float d) {
  const float weights[4]={d<=t?1-t:1-d,d<=t?t-d:0,d<=t?d:t,d<=t?0:d-t};
  DioramaGroundCoverCorner out={0};
  for(unsigned j=0;j<4;++j) {
    for(unsigned axis=0;axis<3;++axis) {
      out.position[axis]+=weights[j]*cap->corners[j].position[axis];
      out.lift[axis]+=weights[j]*cap->corners[j].lift[axis];
    }
    out.depth_fraction+=weights[j]*cap->corners[j].depth_fraction;
  }
  return out;
}
static bool DioramaGroundCover_PrepareCapPressure(const ActionGroundCoverBank *bank,
    const ActionSceneEffectFrame *frame,const DioramaGroundCoverCap *cap,ActionGroundCoverPressure *out) {
  if(!cap||!out)return false;*out=(ActionGroundCoverPressure){0};
  float xyz[4][3];bool native_xy=true;
  float minz=INFINITY,maxz=-INFINITY;
  for(unsigned i=0;i<4;++i) {
    const bool end=i==1||i==2;
    for(unsigned axis=0;axis<3;++axis) {
      xyz[i][axis]=cap->pressure_quad?cap->pressure_xyz[i*3+axis]:cap->corners[i].position[axis];
      if(!isfinite(xyz[i][axis]))return false;
    }
    native_xy&=fabsf(xyz[i][0]-(end?cap->to.x:cap->from.x))<.00001f&&
        fabsf(xyz[i][1]-(end?cap->to.y:cap->from.y))<.00001f;
    minz=fminf(minz,xyz[i][2]);maxz=fmaxf(maxz,xyz[i][2]);
  }
  if(!isfinite(cap->actor_z))return false;
  if(cap->actor_z<minz-.000001f||cap->actor_z>maxz+.000001f)return true;
  if(maxz-minz<.000001f)return true;
  if(native_xy)return ActionGroundCover_PrepareSupportPressure(bank,frame,cap->from,cap->to,out);
  const unsigned triangles[2][3]={{0,1,2},{0,2,3}};
  const float native_x[4]={cap->from.x,cap->to.x,cap->to.x,cap->from.x};
  ActionGroundCoverContactTrace traces[2];unsigned count=0;
  for(unsigned triangle=0;triangle<2;++triangle) {
    float points[6][3];unsigned n=0;
    float lo=INFINITY,hi=-INFINITY;
    for(unsigned i=0;i<3;++i){const float z=xyz[triangles[triangle][i]][2];lo=fminf(lo,z);hi=fmaxf(hi,z);}
    /* A coplanar/zero-depth triangle has no unique floor trace. Keep its
     * settled cover, with no guessed footprint or division by a tiny span. */
    if(hi-lo<.000001f)continue;
    for(unsigned edge=0;edge<3;++edge) {
      const unsigned a=triangles[triangle][edge],b=triangles[triangle][(edge+1)%3];
      const float za=xyz[a][2]-cap->actor_z,zb=xyz[b][2]-cap->actor_z;
      if(fabsf(za)<.000001f) {
        points[n][0]=xyz[a][0];points[n][1]=xyz[a][1];points[n++][2]=native_x[a];
      }
      if((za<-.000001f&&zb>.000001f)||(za>.000001f&&zb<-.000001f)) {
        const float t=za/(za-zb);
        points[n][0]=xyz[a][0]+(xyz[b][0]-xyz[a][0])*t;
        points[n][1]=xyz[a][1]+(xyz[b][1]-xyz[a][1])*t;
        points[n++][2]=native_x[a]+(native_x[b]-native_x[a])*t;
      }
    }
    float length2=0;unsigned first=0,last=0;
    for(unsigned a=0;a<n;++a)for(unsigned b=a+1;b<n;++b) {
      const float dx=points[b][0]-points[a][0],dy=points[b][1]-points[a][1],d=dx*dx+dy*dy;
      if(d>length2){length2=d;first=a;last=b;}
    }
    if(length2<.000001f||fabsf(points[last][2]-points[first][2])<.000001f)continue;
    traces[count++]=(ActionGroundCoverContactTrace){
        .from={points[first][0],points[first][1]},.to={points[last][0],points[last][1]},
        .source_from_x=points[first][2],.source_to_x=points[last][2]};
  }
  return ActionGroundCover_PrepareMappedPressure(bank,frame,traces,count,out);
}
static bool GroundCoverSample(const ActionGroundCoverBank *bank,
    const ActionGroundCoverPressure *pressure,const DioramaGroundCoverCap *cap,
    float t,float d,DioramaGroundCoverVertex *out) {
  DioramaGroundCoverCorner c=GroundCoverCorner(cap,t,d);
  const float distance=(c.position[2]-cap->actor_z)/cap->contact_radius;
  float weight=fmaxf(0,1-distance*distance);weight*=weight;
  const float x=cap->from.x+(cap->to.x-cap->from.x)*t;
  /* Association endpoints may differ by one floating-point ulp. */
  out->native_x=fminf(bank->points[bank->point_count-1].x,fmaxf(bank->points[0].x,x));
  if(!ActionGroundCover_Sample(bank,pressure,out->native_x,
      GroundCoverFraction(c.depth_fraction),weight,&out->sample))return false;
  out->sample.native_floor_y=cap->from.y+(cap->to.y-cap->from.y)*t;
  for(unsigned axis=0;axis<3;++axis)
    out->position[axis]=c.position[axis]+c.lift[axis]*out->sample.height;
  return true;
}
enum { kGroundCoverDepthCuts = 32 };
static bool GroundCoverInsert(float value,float cuts[kGroundCoverDepthCuts],unsigned *count) {
  if(value<=.000001f||value>=.999999f)return true;
  unsigned i=0;while(i<*count&&cuts[i]<value)++i;
  if((i<*count&&fabsf(cuts[i]-value)<.000001f)||(i&&fabsf(cuts[i-1]-value)<.000001f))return true;
  if(*count==kGroundCoverDepthCuts)return false;
  for(unsigned j=*count;j>i;--j)cuts[j]=cuts[j-1];
  cuts[i]=value;++*count;return true;
}
/* Full-material feather and physical contact roots. Evaluating both halves
 * retains the original nonplanar diagonal, even when a transform tilts Z. */
static bool GroundCoverDepthCuts(const ActionGroundCoverBank *bank,
    const DioramaGroundCoverCap *cap,float t,float cuts[kGroundCoverDepthCuts],unsigned *count) {
  const float middle=1-fminf(.8f,bank->painted_width*.25f)/bank->painted_width;
  const float endpoints[3]={0,t,1};
  for(unsigned part=0;part<2;++part) {
    const float d0=endpoints[part],d1=endpoints[part+1];
    if(d1-d0<.000001f)continue;
    const DioramaGroundCoverCorner a=GroundCoverCorner(cap,t,d0),z=GroundCoverCorner(cap,t,d1);
    const float df=z.depth_fraction-a.depth_fraction;
    if(fabsf(df)>.000001f) {
      const float f=(middle-a.depth_fraction)/df;
      if(f>0&&f<1&&!GroundCoverInsert(d0+(d1-d0)*f,cuts,count))return false;
    }
    const float dz=z.position[2]-a.position[2];
    if(fabsf(dz)>.000001f)for(int offset=-1;offset<=1;++offset) {
      const float f=(cap->actor_z+offset*cap->contact_radius-a.position[2])/dz;
      if(f>0&&f<1&&!GroundCoverInsert(d0+(d1-d0)*f,cuts,count))return false;
    }
  }
  return true;
}
typedef struct GroundCoverParam { float t,d; } GroundCoverParam;
typedef struct GroundCoverInkRect { unsigned char x0,y0,x1,y1; } GroundCoverInkRect;
enum { kGroundCoverInkRects=128,kGroundCoverClipVertices=10 };
static unsigned GroundCoverInkRects(const uint16_t *pixels,GroundCoverInkRect *rects) {
  if(!pixels){rects[0]=(GroundCoverInkRect){0,0,16,16};return 1;}
  unsigned count=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;) {
    if(!(pixels[y*16+x]&0x8000)){++x;continue;}
    const unsigned start=x++;
    while(x<16&&(pixels[y*16+x]&0x8000))++x;
    unsigned prior=0;
    while(prior<count&&(rects[prior].y1!=y||rects[prior].x0!=start||rects[prior].x1!=x))++prior;
    if(prior<count)rects[prior].y1=(unsigned char)(y+1);
    else rects[count++]=(GroundCoverInkRect){(unsigned char)start,(unsigned char)y,
        (unsigned char)x,(unsigned char)(y+1)};
  }
  return count;
}
static float GroundCoverUv(const DioramaGroundCoverCap *cap,GroundCoverParam p,unsigned axis) {
  const float weights[4]={p.d<=p.t?1-p.t:1-p.d,p.d<=p.t?p.t-p.d:0,
      p.d<=p.t?p.d:p.t,p.d<=p.t?0:p.d-p.t};
  float out=0;for(unsigned i=0;i<4;++i)out+=weights[i]*cap->top_uv[i*2+axis];
  return out;
}
static unsigned GroundCoverClipUv(const DioramaGroundCoverCap *cap,
    unsigned axis,float boundary,bool greater,const GroundCoverParam *in,unsigned count,
    GroundCoverParam out[kGroundCoverClipVertices]) {
  if(!count)return 0;
  unsigned n=0;GroundCoverParam a=in[count-1];
  float da=(GroundCoverUv(cap,a,axis)-boundary)*(greater?1:-1);
  for(unsigned i=0;i<count;++i) {
    const GroundCoverParam b=in[i];const float db=(GroundCoverUv(cap,b,axis)-boundary)*(greater?1:-1);
    if((da<0)!=(db<0)) {
      const float f=da/(da-db);
      if(n==kGroundCoverClipVertices)return kGroundCoverClipVertices+1;
      out[n++]=(GroundCoverParam){a.t+(b.t-a.t)*f,a.d+(b.d-a.d)*f};
    }
    if(db>=0) {
      if(n==kGroundCoverClipVertices)return kGroundCoverClipVertices+1;
      out[n++]=b;
    }
    a=b;da=db;
  }
  return n;
}
/* Clip one parametric cell against either native receiver triangle. */
static unsigned GroundCoverTriangleClip(const GroundCoverParam in[4],bool upper,
    GroundCoverParam out[6]) {
  unsigned count=0;GroundCoverParam a=in[3];float da=upper?a.t-a.d:a.d-a.t;
  for(unsigned i=0;i<4;++i) {
    const GroundCoverParam b=in[i];const float db=upper?b.t-b.d:b.d-b.t;
    if((da<0)!=(db<0)) {
      const float f=da/(da-db);
      out[count++]=(GroundCoverParam){a.t+(b.t-a.t)*f,a.d+(b.d-a.d)*f};
    }
    if(db>=0)out[count++]=b;
    a=b;da=db;
  }
  return count;
}
static bool GroundCoverCapValid(const DioramaGroundCoverCap *cap) {
  if(!cap||!isfinite(cap->actor_z)||!isfinite(cap->contact_radius)||cap->contact_radius<=0)return false;
  for(unsigned i=0;i<4;++i) {
    const DioramaGroundCoverCorner *c=&cap->corners[i];
    if(!isfinite(c->depth_fraction)||c->depth_fraction<0||c->depth_fraction>1)return false;
    for(unsigned axis=0;axis<3;++axis)
      if(!isfinite(c->position[axis])||!isfinite(c->lift[axis]))return false;
    if(cap->top_pixels)for(unsigned axis=0;axis<2;++axis)
      if(!isfinite(cap->top_uv[i*2+axis])||cap->top_uv[i*2+axis]<0||cap->top_uv[i*2+axis]>=16)return false;
  }
  return true;
}
static bool DioramaGroundCover_BuildCap(DioramaGroundCoverCache *cache,
    DioramaGroundCoverOwner owner,const ActionGroundCoverBank *bank,
    const ActionGroundCoverPressure *pressure,const DioramaGroundCoverCap *cap,float tolerance,
    DioramaGroundCoverVertex *out,unsigned capacity,unsigned *count) {
  if(!count)return false;
  *count=0;
  if(!GroundCoverCapValid(cap))return false;
  const DioramaGroundCoverAssociation *a;
  if(!DioramaGroundCover_Associate(cache,owner,bank,cap->from,cap->to,tolerance,cap->receiver,&a))return false;
  GroundCoverInkRect ink[kGroundCoverInkRects];
  const unsigned ink_count=GroundCoverInkRects(cap->top_pixels,ink);
  if(!ink_count)return true;
  const bool full_ink=ink_count==1&&ink[0].x0==0&&ink[0].y0==0&&ink[0].x1==16&&ink[0].y1==16;
  unsigned written=0;
  for(unsigned span=0;span<a->count;++span) {
    float xs[kActionGroundCoverMaxContactCuts];unsigned nx=0;
    if(!ActionGroundCover_ContactCuts(pressure,cap->from.x,cap->to.x,a->intervals[span],
        xs,kActionGroundCoverMaxContactCuts,&nx))return false;
    for(unsigned x=0;x+1<nx;++x) {
      float ds[kGroundCoverDepthCuts]={0,1};unsigned nd=2;
      if(!GroundCoverDepthCuts(bank,cap,xs[x],ds,&nd)||
          !GroundCoverDepthCuts(bank,cap,xs[x+1],ds,&nd))return false;
      for(unsigned d=0;d+1<nd;++d) {
        const GroundCoverParam rect[4]={{xs[x],ds[d]},{xs[x+1],ds[d]},
            {xs[x+1],ds[d+1]},{xs[x],ds[d+1]}};
        for(unsigned half=0;half<2;++half) {
          GroundCoverParam triangle_polygon[6];
          const unsigned triangle_count=GroundCoverTriangleClip(rect,half==0,triangle_polygon);
          for(unsigned region=0;region<(full_ink?1:ink_count);++region) {
            GroundCoverParam work[2][kGroundCoverClipVertices];unsigned n=triangle_count;
            memcpy(work[0],triangle_polygon,n*sizeof(*triangle_polygon));
            if(!full_ink) {
              const GroundCoverInkRect bounds=ink[region];
              const unsigned axis[4]={0,0,1,1};
              const float value[4]={bounds.x0,bounds.x1,bounds.y0,bounds.y1};
              for(unsigned plane=0;plane<4;++plane) {
                n=GroundCoverClipUv(cap,axis[plane],value[plane],!(plane&1),work[plane&1],n,work[1-(plane&1)]);
                if(n>kGroundCoverClipVertices)return false;
                if(!n)break;
              }
            }
            const GroundCoverParam *polygon=work[0];
            for(unsigned triangle=1;triangle+1<n;++triangle) {
              const GroundCoverParam p[3]={polygon[0],polygon[triangle],polygon[triangle+1]};
              const float area=(p[1].t-p[0].t)*(p[2].d-p[0].d)-(p[1].d-p[0].d)*(p[2].t-p[0].t);
              if(fabsf(area)<.0000001f)continue;
              if(!out||written>capacity||capacity-written<3)return false;
              for(unsigned j=0;j<3;++j)
                if(!GroundCoverSample(bank,pressure,cap,p[j].t,p[j].d,&out[written+j]))return false;
              written+=3;
            }
          }
        }
      }
    }
  }
  *count=written;return true;
}

enum { kGroundCoverLipCuts=kActionGroundCoverMaxContactCuts+30 };
static bool GroundCoverLipInsert(float value,float cuts[kGroundCoverLipCuts],unsigned *count) {
  unsigned at=0;while(at<*count&&cuts[at]<value)++at;
  if((at<*count&&fabsf(cuts[at]-value)<.000001f)||
      (at&&fabsf(cuts[at-1]-value)<.000001f))return true;
  if(*count==kGroundCoverLipCuts)return false;
  memmove(cuts+at+1,cuts+at,(*count-at)*sizeof(*cuts));
  cuts[at]=value;++*count;return true;
}
static bool DioramaGroundCover_BuildLip(DioramaGroundCoverCache *cache,
    DioramaGroundCoverOwner owner,const ActionGroundCoverBank *bank,
    const ActionGroundCoverPressure *pressure,const DioramaGroundCoverCap *cap,float tolerance,
    DioramaGroundCoverVertex *out,unsigned capacity,unsigned *count) {
  if(!count)return false;
  *count=0;
  if(!GroundCoverCapValid(cap))return false;
  /* A depth slice can expose a draw boundary, but only the actual original
   * front contour owns a material lip. In particular the actor plane is not
   * a new front edge of a snow-covered core rim. */
  if(cap->corners[0].depth_fraction>.000001f||cap->corners[1].depth_fraction>.000001f||
      (cap->corners[2].depth_fraction<=.000001f&&cap->corners[3].depth_fraction<=.000001f))return true;
  const DioramaGroundCoverAssociation *association;
  if(!DioramaGroundCover_Associate(cache,owner,bank,cap->from,cap->to,tolerance,
      cap->receiver,&association))return false;
  unsigned written=0;
  for(unsigned span=0;span<association->count;++span) {
    float cuts[kGroundCoverLipCuts];unsigned n=0;
    const ActionGroundCoverInterval interval=association->intervals[span];
    if(!ActionGroundCover_ContactCuts(pressure,cap->from.x,cap->to.x,interval,
        cuts,kGroundCoverLipCuts,&n))return false;
    /* The near-edge UV is linear even on a nonplanar012/023 receiver. Split
     * at its real nearest-texel boundaries, then test each open interval once:
     * overlapping closed row rectangles would duplicate a lip exactly on a
     * painted row boundary. */
    if(cap->top_pixels)for(unsigned axis=0;axis<2;++axis) {
      const float a=cap->top_uv[axis],b=cap->top_uv[2+axis],delta=b-a;
      if(fabsf(delta)<.000001f)continue;
      for(unsigned boundary=1;boundary<16;++boundary) {
        const float t=(boundary-a)/delta;
        if(t>interval.t0&&t<interval.t1&&!GroundCoverLipInsert(t,cuts,&n))return false;
      }
    }
    for(unsigned part=0;part+1<n;++part) {
      if(cuts[part+1]-cuts[part]<.000001f)continue;
      if(cap->top_pixels) {
        const GroundCoverParam middle={(cuts[part]+cuts[part+1])*.5f,0};
        const unsigned u=(unsigned)fminf(15,fmaxf(0,GroundCoverUv(cap,middle,0)));
        const unsigned v=(unsigned)fminf(15,fmaxf(0,GroundCoverUv(cap,middle,1)));
        if(!(cap->top_pixels[v*16+u]&0x8000))continue;
      }
      if(!out||written>capacity||capacity-written<12)return false;
      DioramaGroundCoverVertex edge[6];
      for(unsigned end=0;end<2;++end) {
        const float t=cuts[part+end];
        DioramaGroundCoverVertex top;
        if(!GroundCoverSample(bank,pressure,cap,t,0,&top))return false;
        const DioramaGroundCoverCorner corner=GroundCoverCorner(cap,t,0);
        const float alpha=fminf(.98f,bank->intensity*.75f)*top.sample.edge;
        for(unsigned row=0;row<3;++row) {
          DioramaGroundCoverVertex *v=&edge[row*2+end];*v=top;
          const float lift=row==0?top.sample.height:row==1?0:-1.5f;
          for(unsigned axis=0;axis<3;++axis)v->position[axis]=corner.position[axis]+corner.lift[axis]*lift;
          v->sample.height=row==0?top.sample.height:0;
          v->sample.crest_mix=row==0?1:0;
          v->sample.alpha=row==2?0:alpha;
        }
      }
      static const unsigned triangles[12]={1,0,2,1,2,3,3,2,4,3,4,5};
      for(unsigned i=0;i<12;++i)out[written+i]=edge[triangles[i]];
      written+=12;
    }
  }
  *count=written;return true;
}

/* Close only a compile-proven native Snow step. Sampling both existing
 * receiving endpoints retains their bank phase, depth feather and physical
 * pressure; no horizontal cap or native front geometry is replaced. */
static bool GroundCoverAssociationEnd(const DioramaGroundCoverAssociation *a,unsigned end) {
  for(unsigned i=0;i<a->count;++i)
    if(end?a->intervals[i].t1>=1-.000001f:a->intervals[i].t0<=.000001f)return true;
  return false;
}
static bool DioramaGroundCover_BuildClosingRisers(DioramaGroundCoverCache *cache,
    DioramaGroundCoverOwner owner,const ActionGroundCoverBank *bank,
    const ActionSceneEffectFrame *frame,const ActionGroundCoverPressure *pressure,
    const DioramaGroundCoverCap *cap,float tolerance,
    DioramaGroundCoverVertex *out,unsigned capacity,unsigned *count) {
  if(!count)return false;*count=0;
  if(!cap->closing_riser[0]&&!cap->closing_riser[1])return true;
  if(!GroundCoverCapValid(cap)||!cap->receiver||cap->from.x>=cap->to.x||
      cap->from.y!=cap->to.y)return false;
  const DioramaGroundCoverAssociation *a;
  if(!DioramaGroundCover_Associate(cache,owner,bank,cap->from,cap->to,tolerance,cap->receiver,&a))return false;
  const bool supported[2]={GroundCoverAssociationEnd(a,0),GroundCoverAssociationEnd(a,1)};
  unsigned written=0;
  for(unsigned end=0;end<2;++end) {
    const unsigned drop=cap->closing_riser[end];
    if(!drop||drop>2||!supported[end])continue;
    const float x=end?cap->to.x:cap->from.x;
    DioramaGroundCoverCap lower=*cap;
    lower.closing_riser[0]=lower.closing_riser[1]=0;
    lower.from=(ActionGroundCoverPoint){end?x:x-1,cap->from.y+drop};
    lower.to=(ActionGroundCoverPoint){end?x+1:x,cap->from.y+drop};
    /* Only an untransformed native one-pixel support trace is admitted. The
     * native producer has already matched complete depth intervals. */
    lower.top_pixels=NULL;
    for(unsigned j=0;j<4;++j) {
      const bool last=j==1||j==2;
      lower.corners[j].position[0]=last?lower.to.x:lower.from.x;
      lower.corners[j].position[1]=lower.from.y;
      if(lower.pressure_quad) {
        lower.pressure_xyz[j*3]=lower.corners[j].position[0];
        lower.pressure_xyz[j*3+1]=lower.from.y;
      }
    }
    const DioramaGroundCoverAssociation *b;
    if(!DioramaGroundCover_Associate(cache,owner,bank,lower.from,lower.to,tolerance,lower.receiver,&b))return false;
    if(!GroundCoverAssociationEnd(b,!end))continue;
    ActionGroundCoverPressure lower_pressure;
    if(!DioramaGroundCover_PrepareCapPressure(bank,frame,&lower,&lower_pressure))return false;
    float ds[kGroundCoverDepthCuts]={0,1};unsigned nd=2;
    if(!GroundCoverDepthCuts(bank,cap,(float)end,ds,&nd)||
        !GroundCoverDepthCuts(bank,&lower,(float)!end,ds,&nd))return false;
    for(unsigned d=0;d+1<nd;++d) {
      if(cap->top_pixels) {
        const GroundCoverParam p={(float)end,(ds[d]+ds[d+1])*.5f};
        const unsigned u=(unsigned)fminf(15,fmaxf(0,GroundCoverUv(cap,p,0)));
        const unsigned v=(unsigned)fminf(15,fmaxf(0,GroundCoverUv(cap,p,1)));
        if(!(cap->top_pixels[v*16+u]&0x8000))continue;
      }
      if(!out||written>capacity||capacity-written<6)return false;
      DioramaGroundCoverVertex q[4];
      if(!GroundCoverSample(bank,pressure,cap,(float)end,ds[d],q)||
          !GroundCoverSample(bank,&lower_pressure,&lower,(float)!end,ds[d],q+1)||
          !GroundCoverSample(bank,&lower_pressure,&lower,(float)!end,ds[d+1],q+2)||
          !GroundCoverSample(bank,pressure,cap,(float)end,ds[d+1],q+3))return false;
      static const unsigned order[2][6]={{1,0,3,1,3,2},{0,1,2,0,2,3}};
      for(unsigned i=0;i<6;++i)out[written+i]=q[order[end][i]];
      written+=6;
    }
  }
  *count=written;return true;
}
