/* Connected enclosure: closed native-pixel silhouette edges, never stacked sheets.
 * Contours are cached across stationary animation/orbit frames. The rendered
 * faces share exact endpoints and sort far-to-near before one batch submission. */
static ArRenderTexture s_hollow_bark_material;
static uint32_t s_trunk_material_revision;
static void HollowResetMaterial(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device,s_hollow_bark_material);
  s_hollow_bark_material=ArRenderTexture_Invalid();
}
static bool HollowEnsureMaterial(ArRenderDevice *device) {
  if(s_trunk_material_revision!=s_trunk_assets->revision){HollowResetMaterial(device);s_trunk_material_revision=s_trunk_assets->revision;}
  if(ArRenderTexture_IsValid(s_hollow_bark_material))return true;
  const ArRenderTextureDesc desc={.width=32,.height=32,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming,.filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  if(!ArRenderDevice_CreateTexture(device,&desc,&s_hollow_bark_material))return false;
  if(ArRenderDevice_UpdateTexture(device,s_hollow_bark_material,NULL,s_trunk_assets->bark,32*4))return true;
  HollowResetMaterial(device);return false;
}
enum { kHollowMaxEdges=16384 };
typedef struct HollowEdge {int16_t x,y;uint8_t side;} HollowEdge;
typedef struct HollowContour {
  uint32_t revision;
  bool valid;int map,x,y,left,right,height,count;
  HollowEdge edges[kHollowMaxEdges];
} HollowContour;
typedef struct HollowSide {ArRenderVertex2D v[4];float depths[4],distance;} HollowSide;
static HollowContour s_hollow_contours[2];
static HollowSide s_hollow_sides[kHollowMaxEdges];
static ArRenderVertex2D s_hollow_side_vertices[kHollowMaxEdges*4];
static float s_hollow_side_depths[kHollowMaxEdges*4];
static int32_t s_hollow_side_indices[kHollowMaxEdges*6];
static int HollowSideCompare(const void *a,const void *b) {
  const float da=((const HollowSide *)a)->distance,db=((const HollowSide *)b)->distance;
  return da>db?-1:da<db?1:0;
}
static bool HollowContourBuild(HollowContour *out,int map,int x,int y,
    int left,int right,int height,int kind) {
  if(out->valid&&out->revision==s_trunk_assets->revision&&out->map==map&&out->x==x&&out->y==y&&out->left==left&&
      out->right==right&&out->height==height)return true;
  out->valid=false;out->count=0;out->revision=s_trunk_assets->revision;
  const int room_height=(int)s_trunk_assets->height;
  const int first_y=y>0?y:0,last_y=y+height<room_height?y+height:room_height;
  const int first_x=x+left>320?x+left:320,last_x=x+right<704?x+right:704;
  for(int yy=first_y;yy<last_y&&first_x<last_x;++yy) {
    const uint8_t *row=ActionTrunk_MaskRow(s_trunk_assets,yy,kind);
    const uint8_t *above=ActionTrunk_MaskRow(s_trunk_assets,yy-1,kind);
    const uint8_t *below=ActionTrunk_MaskRow(s_trunk_assets,yy+1,kind);
    for(int byte=(first_x-320)>>3;byte<=((last_x-321)>>3);++byte) {
      const unsigned filled=row[byte];
      if(!filled)continue;
      const unsigned before=byte?row[byte-1]:0,after=byte<47?row[byte+1]:0;
      const unsigned flags[4]={filled&~(above?above[byte]:0),
          filled&~((filled>>1)|((after&1)<<7)),filled&~(below?below[byte]:0),
          filled&~((filled<<1)|(before>>7))};
      unsigned exposed=flags[0]|flags[1]|flags[2]|flags[3];
      while(exposed) {
        const unsigned bit=(unsigned)__builtin_ctz(exposed);exposed&=exposed-1;
        const int wx=320+byte*8+(int)bit;
        if(wx<first_x||wx>=last_x)continue;
        for(int side=0;side<4;++side)if(flags[side]&(1u<<bit)) {
          if(out->count==kHollowMaxEdges)return false;
          out->edges[out->count++]=(HollowEdge){(int16_t)(wx-x),(int16_t)(yy-y),(uint8_t)side};
        }
      }
    }
  }
  out->map=map;out->x=x;out->y=y;out->left=left;out->right=right;
  out->height=height;out->valid=true;return true;
}
static bool DrawHollowSolidEdges(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,int room,int kind) {
  if(layer->hollow_recess<=0)return true;
  if(kind==0&&!HollowEnsureMaterial(device))return false;
  const int origin_x=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int origin_y=capture->camera_y-capture->authentic_y0+1;
  const int left=(int)lroundf(g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const int right=(int)lroundf(g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  HollowContour *contour=&s_hollow_contours[kind];
  if(!HollowContourBuild(contour,room,origin_x,origin_y,left,right,capture->height,kind))return false;
  const float front=layer->authored->z-.5f;
  const float depth=kind==0?layer->hollow_recess:fminf(.065f,layer->hollow_recess*.3f);
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  const int corners[4][4]={{0,0,1,0},{1,0,1,1},{1,1,0,1},{0,1,0,0}};
  int count=0;
  for(int e=0;e<contour->count;++e) {
    const HollowEdge edge=contour->edges[e];const int *c=corners[edge.side];
    HollowSide next={0};
    const ArRenderColorF bounce=kind==1&&edge.side==2?LifeBranchColor(origin_x+edge.x,origin_y+edge.y):(ArRenderColorF){1,1,1,1};
    bool ok=true;
    for(int i=0;i<4;++i) {
      const int end=(i==1||i==2)?2:0;
      const float u=(edge.x+c[end])/(float)SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(edge.y+c[end+1])/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale;
      const float z=front-(i>=2?depth:0);
      Scene3DClipPoint clip;
      ok=ok&&Scene3D_TransformToClip(g->matrix,wx,wy,z,&clip)&&
          ProjectWorldPoint(g->matrix,wx,wy,z,g->width,g->height,&next.v[i].position);
      if(!ok)break;
      if(g->depth_sink)next.depths[i]=clip.z/clip.w*.5f+.5f;
      next.distance+=clip.w*.25f;
      const ArRenderPointF source_uv={(edge.x+.5f)/SR_PPU_SURFACE_MAX_WIDTH,
          (edge.y+.5f)/SR_PPU_SURFACE_MAX_HEIGHT};
      next.v[i].tex_coord=kind==0?(ArRenderPointF){i>=2?1.f:0.f,
          (((origin_y+edge.y)&31)+c[end+1])/32.f}:source_uv;
      float shade=kind==0?(i>=2?.48f:.73f):(edge.side==0?(i>=2?.72f:.94f):(i>=2?.45f:.64f));
      if(kind==0)shade*=s_trunk_fade;
      if(layer->dimming>0)shade*=1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,source_uv.x,source_uv.y);
      if(layer->exposure&&layer->exposure->sample)shade*=layer->exposure->sample(
          layer->exposure->userdata,source_uv.x,source_uv.y,source_uv.x,source_uv.y);
      next.v[i].color=(ArRenderColorF){layer->shade.r*shade*bounce.r,layer->shade.g*shade*bounce.g,
          layer->shade.b*shade*bounce.b,layer->shade.a};
    }
    if(!ok)continue;
    const ArRenderPointF a=next.v[0].position,b=next.v[1].position,cross=next.v[2].position;
    const float area=(b.x-a.x)*(cross.y-a.y)-(b.y-a.y)*(cross.x-a.x);
    if(area>=-.00001f)continue; /* Exposed outward face only. */
    s_hollow_sides[count++]=next;
  }
  if(!count)return true;
  qsort(s_hollow_sides,(size_t)count,sizeof(s_hollow_sides[0]),HollowSideCompare);
  const int tri[6]={0,1,2,0,2,3};
  for(int i=0;i<count;++i) {
    memcpy(s_hollow_side_vertices+i*4,s_hollow_sides[i].v,sizeof(s_hollow_sides[i].v));
    if(g->depth_sink)memcpy(s_hollow_side_depths+i*4,s_hollow_sides[i].depths,sizeof(s_hollow_sides[i].depths));
    for(int j=0;j<6;++j)s_hollow_side_indices[i*6+j]=i*4+tri[j];
  }
  DioramaSceneryLightEffectParams light=layer->lighting;
  light.texel_width=kind==0?1.f/32:1.f/SR_PPU_SURFACE_MAX_WIDTH;
  light.texel_height=kind==0?1.f/32:1.f/SR_PPU_SURFACE_MAX_HEIGHT;
  light.mode=0;light.blur_radius=0;light.attributes_uv=(ArRenderRectF){0,0,1,1};
  return RenderShadedDioramaDepthGeometry(device,kind==0?s_hollow_bark_material:layer->texture,
      s_hollow_side_vertices,s_hollow_side_depths,count*4,
      s_hollow_side_indices,count*6,layer->blend,NULL,(ArRenderRectF){0,0,1,1},&light,
      g->depth_sink,layer->description->plane,false);
}
