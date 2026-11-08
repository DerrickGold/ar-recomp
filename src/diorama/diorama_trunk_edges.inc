/* Connected enclosure: closed native-pixel silhouette edges, never stacked sheets.
 * Contours are cached across stationary animation/orbit frames. The rendered
 * faces share exact endpoints and sort far-to-near before one batch submission. */
static ArRenderTexture s_hollow_bark_material;
static ArRenderTexture s_hollow_side_material;
static uint32_t s_trunk_material_revision;
static void HollowResetMaterial(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device,s_hollow_bark_material);
  ArRenderDevice_DestroyTexture(device,s_hollow_side_material);
  s_hollow_bark_material=ArRenderTexture_Invalid();
  s_hollow_side_material=ArRenderTexture_Invalid();
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
static bool HollowEnsureSideMaterial(ArRenderDevice *device) {
  if(!HollowEnsureMaterial(device))return false;
  if(ArRenderTexture_IsValid(s_hollow_side_material))return true;
  uint32_t pixels[32*64];
  memcpy(pixels,s_trunk_assets->bark,sizeof(s_trunk_assets->bark));
  for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
    pixels[(y+32)*32+x]=s_trunk_assets->side_snow[(y&15)*16+(x&15)];
  const ArRenderTextureDesc desc={.width=32,.height=64,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming,.filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  if(!ArRenderDevice_CreateTexture(device,&desc,&s_hollow_side_material))return false;
  if(ArRenderDevice_UpdateTexture(device,s_hollow_side_material,NULL,pixels,32*4))return true;
  ArRenderDevice_DestroyTexture(device,s_hollow_side_material);
  s_hollow_side_material=ArRenderTexture_Invalid();return false;
}
enum { kHollowMaxEdges=16384 };
typedef struct HollowEdge {int16_t x,y;uint8_t side,snow_riser;} HollowEdge;
typedef struct HollowContour {
  uint32_t revision;
  bool valid;int map,x,y,left,right,height,count;
  HollowEdge edges[kHollowMaxEdges];
} HollowContour;
typedef struct HollowSide {ArRenderVertex2D v[4];float depths[4],distance;uint16_t ground_cover;} HollowSide;
/* Primary and two supplemental strips keep independent camera-stable caches. */
static HollowContour s_hollow_contours[3][3];
static HollowSide s_hollow_sides[kHollowMaxEdges];
static ArRenderVertex2D s_hollow_side_vertices[kHollowMaxEdges*4];
static float s_hollow_side_depths[kHollowMaxEdges*4];
static int32_t s_hollow_side_indices[kHollowMaxEdges*6];
static int HollowSideCompare(const void *a,const void *b) {
  const float da=((const HollowSide *)a)->distance,db=((const HollowSide *)b)->distance;
  return da>db?-1:da<db?1:0;
}
static bool GatherHollowGroundCover(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,const DioramaGroundCoverCap *cap,unsigned kind,int *count) {
  unsigned first,added;
  const DioramaGroundCoverOwner owner={s_trunk_assets,s_trunk_assets->revision,kind+2};
  if(!GroundCoverGather(capture,g,layer,owner,cap,true,&first,&added))return false;
  if(g->depth_sink)return true;
  if(added>(unsigned)(kHollowMaxEdges-*count))return false;
  for(unsigned i=first;i<first+added;++i) {
    HollowSide face={.ground_cover=(uint16_t)(i+1),.distance=s_ground_cover_faces[i].distance};
    GroundCoverCopyFace(i,face.v,face.depths,NULL);s_hollow_sides[(*count)++]=face;
  }
  return true;
}
static bool HollowSnowGroundReceiver(const void *source,int x,int y,int *bottom) {
  const ActionTrunkAssets *assets=source;
  if(!bottom||!ActionTrunk_SideSolid(assets,x,y,kActionTrunkSideSnow)||
      !ActionTrunk_SideSolid(assets,x,y,kActionTrunkSideSolid))return false;
  if(ActionTrunk_SideSolid(assets,x,y-1,kActionTrunkSideSolid))return false;
  const bool high=ActionTrunk_SideSolid(assets,x,y,kActionTrunkSidePriority);
  const int limit=(int)fminf(floorf(y/16.f)*16+16,y+8);int end=y;
  while(end<limit&&ActionTrunk_SideSolid(assets,x,end,kActionTrunkSideSolid)&&
      ActionTrunk_SideSolid(assets,x,end,kActionTrunkSideSnow)&&
      ActionTrunk_SideSolid(assets,x,end,kActionTrunkSidePriority)==high)++end;
  if(end==y)return false;
  *bottom=end;return true;
}
static bool SubmitHollowEdges(ArRenderDevice *device,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,ArRenderTexture texture,const DioramaSceneryLightEffectParams *light,int count) {
  if(!g->depth_sink&&s_ground_cover.face_count&&!GroundCoverShadeBatch())return false;
  qsort(s_hollow_sides,(size_t)count,sizeof(*s_hollow_sides),HollowSideCompare);
  const int tri[]={0,1,2,0,2,3};
  for(int first=0;first<count;) {
    const bool cover=s_hollow_sides[first].ground_cover!=0;int end=first+1;
    while(end<count&&(s_hollow_sides[end].ground_cover!=0)==cover)++end;
    for(int i=first;i<end;++i) {
      const unsigned at=(unsigned)(i-first)*4;
      if(cover)GroundCoverCopyFace(s_hollow_sides[i].ground_cover-1,s_hollow_side_vertices+at,s_hollow_side_depths+at,NULL);
      else {
        memcpy(s_hollow_side_vertices+at,s_hollow_sides[i].v,sizeof(s_hollow_sides[i].v));
        if(g->depth_sink)memcpy(s_hollow_side_depths+at,s_hollow_sides[i].depths,sizeof(s_hollow_sides[i].depths));
      }
      for(unsigned j=0;j<6;++j)s_hollow_side_indices[(i-first)*6+j]=(int32_t)at+tri[j];
    }
    if(!RenderShadedDioramaDepthGeometry(device,cover?ArRenderTexture_Invalid():texture,
        s_hollow_side_vertices,s_hollow_side_depths,(end-first)*4,s_hollow_side_indices,(end-first)*6,
        cover?kArRenderBlendMode_Alpha:layer->blend,NULL,(ArRenderRectF){0,0,1,1},cover?NULL:light,
        g->depth_sink,layer->description->plane,false))return false;
    first=end;
  }
  const bool ok=!g->depth_sink||GroundCoverSubmitDepth(device,g,layer);
  GroundCoverBeginBatch();return ok;
}
/* A native snow ramp is a one- or two-pixel staircase. Its short upright
 * faces belong to the snow cap, not the dark bark below it. Inspect the full
 * silhouette run, including outside the capture, so clipping cannot turn a
 * tall wall into a snow riser. This is cached with the contour. */
static bool HollowSideSnowRiser(int x,int y,int side) {
  if(!(side&1))return false;
  const int outside=x+(side==1?1:-1);
  int first=y,last=y;
  while(first>y-2 && ActionTrunk_SideSolid(s_trunk_assets,x,first-1,kActionTrunkSideSolid) &&
      !ActionTrunk_SideSolid(s_trunk_assets,outside,first-1,kActionTrunkSideSolid))--first;
  while(last<y+2 && ActionTrunk_SideSolid(s_trunk_assets,x,last+1,kActionTrunkSideSolid) &&
      !ActionTrunk_SideSolid(s_trunk_assets,outside,last+1,kActionTrunkSideSolid))++last;
  if(last-first>=2 || ActionTrunk_SideSolid(s_trunk_assets,x,first-1,kActionTrunkSideSolid))return false;
  for(int yy=first;yy<=last;++yy)
    if(!ActionTrunk_SideSolid(s_trunk_assets,x,yy,kActionTrunkSideSnow))return false;
  return true;
}
static bool HollowContourBuild(HollowContour *out,int map,int x,int y,
    int left,int right,int height,int kind) {
  if(out->valid&&out->revision==s_trunk_assets->revision&&out->map==map&&out->x==x&&out->y==y&&out->left==left&&
      out->right==right&&out->height==height)return true;
  out->valid=false;out->count=0;out->revision=s_trunk_assets->revision;
  const int room_height=(int)s_trunk_assets->height;
  const bool passage=kind==2;
  const int mask_left=passage?0:kActionTrunkLeft;
  const int mask_right=passage?(int)s_trunk_assets->side_width:kActionTrunkLeft+kActionTrunkWidth;
  const int mask_pitch=passage?kActionTrunkSideMaskPitch:kActionTrunkMaskPitch;
  const int first_y=y>0?y:0,last_y=y+height<room_height?y+height:room_height;
  const int first_x=x+left>mask_left?x+left:mask_left,last_x=x+right<mask_right?x+right:mask_right;
  for(int yy=first_y;yy<last_y&&first_x<last_x;++yy) {
    const uint8_t *row=passage?ActionTrunk_SideMaskRow(s_trunk_assets,yy,kActionTrunkSideSolid):ActionTrunk_MaskRow(s_trunk_assets,yy,kind);
    const uint8_t *above=passage?ActionTrunk_SideMaskRow(s_trunk_assets,yy-1,kActionTrunkSideSolid):ActionTrunk_MaskRow(s_trunk_assets,yy-1,kind);
    const uint8_t *below=passage?ActionTrunk_SideMaskRow(s_trunk_assets,yy+1,kActionTrunkSideSolid):ActionTrunk_MaskRow(s_trunk_assets,yy+1,kind);
    for(int byte=(first_x-mask_left)>>3;byte<=((last_x-mask_left-1)>>3);++byte) {
      const unsigned filled=row[byte];
      if(!filled)continue;
      const unsigned before=byte?row[byte-1]:0,after=byte+1<mask_pitch?row[byte+1]:0;
      const unsigned flags[4]={filled&~(above?above[byte]:0),
          filled&~((filled>>1)|((after&1)<<7)),filled&~(below?below[byte]:0),
          filled&~((filled<<1)|(before>>7))};
      unsigned exposed=flags[0]|flags[1]|flags[2]|flags[3];
      while(exposed) {
        const unsigned bit=(unsigned)__builtin_ctz(exposed);exposed&=exposed-1;
        const int wx=mask_left+byte*8+(int)bit;
        if(wx<first_x||wx>=last_x)continue;
        for(int side=0;side<4;++side)if(flags[side]&(1u<<bit)) {
          if(out->count==kHollowMaxEdges)return false;
          out->edges[out->count++]=(HollowEdge){(int16_t)(wx-x),(int16_t)(yy-y),(uint8_t)side,
              passage&&HollowSideSnowRiser(wx,yy,side)};
        }
      }
    }
  }
  out->map=map;out->x=x;out->y=y;out->left=left;out->right=right;
  out->height=height;out->valid=true;return true;
}

/* The front remains at the native actor plane. Only the rear endpoint opens
 * into the side passage, so native low/high draw slots remain authoritative. */
static bool DrawHollowPassageEdges(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,int room,bool high,
    const DioramaTrunkGuardStrip *guard,unsigned guard_slot) {
  if(layer->hollow_recess<=0||!s_trunk_assets->side_width||
      (high&&!s_trunk_assets->side_high_pixels))return true;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  const int left=guard?guard->world_x+guard->valid.x-ox:(int)lroundf(g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const int right=guard?left+guard->valid.width:(int)lroundf(g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  const int contour_y=guard?guard->world_y+guard->valid.y:oy;
  HollowContour *contour=&s_hollow_contours[2][guard_slot];
  if(!HollowContourBuild(contour,room,ox,contour_y,left,right,
      guard?guard->valid.height:capture->height,2))return false;
  if(!contour->count)return true;
  if(!HollowEnsureSideMaterial(device))return false;
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  const float front=-layer->hollow_recess;
  const int corners[4][4]={{0,0,1,0},{1,0,1,1},{1,1,0,1},{0,1,0,0}};
  int count=0;
  GroundCoverBeginBatch();
  for(int e=0;e<contour->count;++e) {
    const HollowEdge edge=contour->edges[e];
    const int world_x=ox+edge.x,world_y=contour_y+edge.y;
    if(ActionTrunk_SideSolid(s_trunk_assets,world_x,world_y,kActionTrunkSidePriority)!=high)continue;
    const bool snow=edge.snow_riser || ((edge.side&1)&&
        ActionTrunk_SideSolid(s_trunk_assets,world_x,world_y,kActionTrunkSideSnowFace)) ||
        (edge.side==0&&ActionTrunk_SideSolid(s_trunk_assets,world_x,world_y,kActionTrunkSideSnow));
    const int *c=corners[edge.side];HollowSide next={0};bool ok=true;
    const bool cover_top=s_ground_cover.active_count&&edge.side==0;
    DioramaGroundCoverCap cover={.from={world_x,world_y},.to={world_x+1,world_y},
        .actor_z=-layer->hollow_recess,.contact_radius=6*g->aspect_x/(du*SR_PPU_SURFACE_MAX_WIDTH)};
    if(snow)cover.receiver=HollowSnowGroundReceiver;
    for(int i=0;i<4;++i) {
      const int end=(i==1||i==2)?2:0;
      const float u=(edge.x+c[end])/(float)SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(world_y-oy+c[end+1])/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale;
      const float span=DioramaTrunk_SideDepth(layer->hollow_recess,world_x+c[end]);
      const float z=front-(i>=2?span:0);
      if(cover_top)cover.corners[i]=(DioramaGroundCoverCorner){
          .position={world_x+c[end],world_y+c[end+1],z},.lift={0,-1,0},.depth_fraction=i>=2?1:0};
      Scene3DClipPoint clip;
      ok=ok&&Scene3D_TransformToClip(g->matrix,wx,wy,z,&clip)&&
          ProjectWorldPoint(g->matrix,wx,wy,z,g->width,g->height,&next.v[i].position);
      if(!ok)break;
      if(g->depth_sink)next.depths[i]=clip.z/clip.w*.5f+.5f;
      next.distance+=clip.w*.25f;
      /* Native two-dimensional grain, anchored along the source contour.
       * Neither its world phase nor the selected snow/bark changes on orbit. */
      const float depth_sample=i>=2?span/DioramaTrunk_SideDepth(layer->hollow_recess,0):0;
      next.v[i].tex_coord=(edge.side&1)&&!edge.snow_riser
          ?(ArRenderPointF){(.5f+31*depth_sample)/32.f,
              (snow?.5f:0)+fminf(31.5f,(world_y&31)+c[end+1]+.5f)/64.f}
          :(ArRenderPointF){((world_x&31)+c[end])/32.f,(snow?.5f:0)+(.5f+31*depth_sample)/64.f};
      const ArRenderPointF source_uv={(edge.x+.5f)/SR_PPU_SURFACE_MAX_WIDTH,(world_y-oy+.5f)/SR_PPU_SURFACE_MAX_HEIGHT};
      float shade=(edge.side==0||snow?(i>=2?.86f:1.f):edge.side==2?(i>=2?.5f:.66f):(i>=2?.65f:.84f))*s_trunk_fade;
      if(layer->dimming>0)shade*=1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,source_uv.x,source_uv.y);
      if(layer->exposure&&layer->exposure->sample)shade*=layer->exposure->sample(layer->exposure->userdata,source_uv.x,source_uv.y,source_uv.x,source_uv.y);
      next.v[i].color=(ArRenderColorF){layer->shade.r*shade,layer->shade.g*shade,layer->shade.b*shade,layer->shade.a};
    }
    if(!ok)continue;
    const ArRenderPointF a=next.v[0].position,b=next.v[1].position,cross=next.v[2].position,d=next.v[3].position;
    /* The last side-hall cap narrows to zero at the shaft. Its first
     * triangle can collapse while the second still closes the native pixel. */
    const float area=(b.x-a.x)*(cross.y-a.y)-(b.y-a.y)*(cross.x-a.x)+
        (cross.x-a.x)*(d.y-a.y)-(cross.y-a.y)*(d.x-a.x);
    if(area>=-.00001f) {
      if(cover_top&&!GatherHollowGroundCover(capture,g,layer,&cover,2,&count))return false;
      continue;
    }
    if(count==kHollowMaxEdges)return false;
    s_hollow_sides[count++]=next;
    if(cover_top&&!GatherHollowGroundCover(capture,g,layer,&cover,2,&count))return false;
  }
  DioramaSceneryLightEffectParams light=layer->lighting;
  light.texel_width=1.f/32;light.texel_height=1.f/64;light.mode=0;light.blur_radius=0;
  light.attributes_uv=(ArRenderRectF){0,0,1,1};
  return SubmitHollowEdges(device,g,layer,s_hollow_side_material,&light,count);
}
static bool DrawHollowSolidEdges(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,int room,int kind,
    const DioramaTrunkGuardStrip *guard,unsigned guard_slot) {
  if(layer->hollow_recess<=0)return true;
  if(kind==0&&!HollowEnsureMaterial(device))return false;
  const int origin_x=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int origin_y=capture->camera_y-capture->authentic_y0+1;
  const int left=guard?guard->world_x+guard->valid.x-origin_x:(int)lroundf(g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const int right=guard?left+guard->valid.width:(int)lroundf(g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  const int contour_y=guard?guard->world_y+guard->valid.y:origin_y;
  HollowContour *contour=&s_hollow_contours[kind][guard_slot];
  if(!HollowContourBuild(contour,room,origin_x,contour_y,left,right,
      guard?guard->valid.height:capture->height,kind))return false;
  const float front=layer->authored->z-.5f;
  const float depth=kind==0?layer->hollow_recess:fminf(.065f,layer->hollow_recess*.3f);
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  const int corners[4][4]={{0,0,1,0},{1,0,1,1},{1,1,0,1},{0,1,0,0}};
  int count=0;
  GroundCoverBeginBatch();
  for(int e=0;e<contour->count;++e) {
    const HollowEdge edge=contour->edges[e];const int *c=corners[edge.side];
    const int world_y=contour_y+edge.y;
    HollowSide next={0};
    const bool cover_top=s_ground_cover.active_count&&edge.side==0;
    DioramaGroundCoverCap cover={.from={origin_x+edge.x,world_y},
        .to={origin_x+edge.x+1,world_y},.actor_z=-layer->hollow_recess,
        .contact_radius=6*g->aspect_x/(du*SR_PPU_SURFACE_MAX_WIDTH)};
    const ArRenderColorF bounce=kind==1&&edge.side==2?LifeBranchColor(origin_x+edge.x,world_y):(ArRenderColorF){1,1,1,1};
    bool ok=true;
    for(int i=0;i<4;++i) {
      const int end=(i==1||i==2)?2:0;
      const float u=(edge.x+c[end])/(float)SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(world_y-origin_y+c[end+1])/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale;
      const float z=front-(i>=2?depth:0);
      if(cover_top)cover.corners[i]=(DioramaGroundCoverCorner){
          .position={origin_x+edge.x+c[end],world_y+c[end+1],z},
          .lift={0,-1,0},.depth_fraction=i>=2?1:0};
      Scene3DClipPoint clip;
      ok=ok&&Scene3D_TransformToClip(g->matrix,wx,wy,z,&clip)&&
          ProjectWorldPoint(g->matrix,wx,wy,z,g->width,g->height,&next.v[i].position);
      if(!ok)break;
      if(g->depth_sink)next.depths[i]=clip.z/clip.w*.5f+.5f;
      next.distance+=clip.w*.25f;
      const ArRenderPointF source_uv={(edge.x+.5f)/SR_PPU_SURFACE_MAX_WIDTH,
          (world_y-origin_y+.5f)/SR_PPU_SURFACE_MAX_HEIGHT};
      next.v[i].tex_coord=kind==0?(ArRenderPointF){i>=2?1.f:0.f,
          ((world_y&31)+c[end+1])/32.f}:guard?(ArRenderPointF){
              (origin_x+edge.x-guard->world_x+.5f)/kDioramaTrunkGuardWidth,
              (world_y-guard->world_y+guard->atlas_y+.5f)/kDioramaTrunkGuardHeight}:source_uv;
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
    if(area>=-.00001f) {
      if(cover_top&&!GatherHollowGroundCover(capture,g,layer,&cover,(unsigned)kind,&count))return false;
      continue; /* Exposed outward native face only. */
    }
    if(count==kHollowMaxEdges)return false;
    s_hollow_sides[count++]=next;
    if(cover_top&&!GatherHollowGroundCover(capture,g,layer,&cover,(unsigned)kind,&count))return false;
  }
  DioramaSceneryLightEffectParams light=layer->lighting;
  light.texel_width=kind==0?1.f/32:1.f/SR_PPU_SURFACE_MAX_WIDTH;
  light.texel_height=kind==0?1.f/32:guard?1.f/kDioramaTrunkGuardHeight:1.f/SR_PPU_SURFACE_MAX_HEIGHT;
  light.mode=0;light.blur_radius=0;light.attributes_uv=guard&&kind==1
      ?(ArRenderRectF){(float)(guard->world_x-origin_x)/SR_PPU_SURFACE_MAX_WIDTH,
          (float)(guard->world_y-origin_y-guard->atlas_y)/SR_PPU_SURFACE_MAX_HEIGHT,
          (float)kDioramaTrunkGuardWidth/SR_PPU_SURFACE_MAX_WIDTH,
          (float)kDioramaTrunkGuardHeight/SR_PPU_SURFACE_MAX_HEIGHT}
      :(ArRenderRectF){0,0,1,1};
  return SubmitHollowEdges(device,g,layer,kind==0?s_hollow_bark_material:layer->texture,&light,count);
}
