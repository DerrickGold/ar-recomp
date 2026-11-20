/* Connected enclosure silhouette. World-space contours survive scrolling;
 * each draw projects and sorts the edges inside its current capture. */
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
enum {
  kHollowEdgeSnowRiser=1, kHollowEdgeFrontStep=2, kHollowEdgeSnow=4, kHollowEdgeHigh=8,
};
typedef struct HollowEdge {int16_t x,y;uint8_t side,flags;} HollowEdge;
typedef struct HollowEdgeDepth {float front[2],back[2];} HollowEdgeDepth;
static const int kHollowEdgeCorners[4][2][2]={
  {{0,0},{1,0}},{{1,0},{1,1}},{{1,1},{0,1}},{{0,1},{0,0}},
};
enum { kHollowRoomEdgeLimit=131072 };
typedef struct HollowContour {
  HollowEdge *edges;
  HollowEdgeDepth *depths; /* Only recessed passage edges require a depth profile. */
  unsigned count, capacity;
} HollowContour;
typedef struct HollowRoomContour {
  HollowContour contour;
  unsigned rows[kActionTrunkMaxHeight+1];
  uint32_t revision;
  unsigned room;
  bool attempted, ready, column, clearance;
} HollowRoomContour;
typedef struct HollowContourView {
  const HollowEdge *edges;
  const HollowEdgeDepth *depths;
  unsigned count;
  int left, right;
} HollowContourView;
typedef struct HollowSide {ArRenderVertex2D v[4];float depths[4],distance;uint16_t ground_cover;} HollowSide;
static HollowRoomContour s_hollow_contours[3];
static HollowEdge s_hollow_window_edges[kHollowMaxEdges];
static void HollowResetContours(void) {
  for(unsigned i=0;i<3;++i) {
    free(s_hollow_contours[i].contour.edges);
    free(s_hollow_contours[i].contour.depths);
  }
  memset(s_hollow_contours,0,sizeof(s_hollow_contours));
}
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
  if(ActionTrunk_SideSolid(assets,x,y-1,kActionTrunkSideSolid)&&!ActionTrunk_SideContact(assets,x,y))return false;
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
static unsigned HollowCoreMaskByte(unsigned kind,int y,int byte) {
  const uint8_t *row=ActionTrunk_MaskRow(s_trunk_assets,y,kind);
  if(!row||byte<0||byte>=kActionTrunkMaskPitch)return 0;
  return row[byte]|(s_trunk_column&&kind==1&&byte>=20&&byte<28
      ?s_trunk_assets->column_caps[y*8+byte-20]:0);
}
static unsigned HollowPassageMaskByte(int y,int byte,bool clearance) {
  if(clearance)return ActionTrunk_SideShellByte(s_trunk_assets,y,byte);
  const uint8_t *row=ActionTrunk_SideMaskRow(s_trunk_assets,y,kActionTrunkSideSolid);
  return row&&byte>=0&&byte<kActionTrunkSideMaskPitch?row[byte]:0;
}
static bool AppendHollowEdge(HollowContour *out,HollowEdge edge,const HollowEdgeDepth *depth,bool grow) {
  if(out->count==out->capacity) {
    if(!grow||out->capacity==kHollowRoomEdgeLimit)return false;
    const unsigned capacity=out->capacity?out->capacity*2:1024;
    HollowEdge *edges=realloc(out->edges,capacity*sizeof(*edges));
    if(!edges)return false;
    out->edges=edges;
    if(depth) {
      HollowEdgeDepth *depths=realloc(out->depths,capacity*sizeof(*depths));
      if(!depths)return false;
      out->depths=depths;
    }
    out->capacity=capacity;
  }
  if(depth) {
    if(!out->depths) {
      out->depths=malloc(out->capacity*sizeof(*out->depths));
      if(!out->depths)return false;
    }
    out->depths[out->count]=*depth;
  }
  out->edges[out->count++]=edge;
  return true;
}

static void SampleHollowEdgeDepth(int x,int y,unsigned side,bool neighbor,float fractions[2]) {
  static const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
  const int owner_x=x+(neighbor?dx[side]:0),owner_y=y+(neighbor?dy[side]:0);
  for(unsigned end=0;end<2;++end) {
    const int *corner=kHollowEdgeCorners[side][end];
    fractions[end]=ActionTrunk_SideFrontFraction(s_trunk_assets,owner_x,owner_y,x+corner[0],y+corner[1]);
  }
}

static unsigned HollowPassageEdgeFlags(int x,int y,int side,bool step) {
  const bool riser=HollowSideSnowRiser(x,y,side);
  const bool snow=riser || ((side&1)&&ActionTrunk_SideSolid(s_trunk_assets,x,y,kActionTrunkSideSnowFace)) ||
      (side==0&&(ActionTrunk_SideSolid(s_trunk_assets,x,y,kActionTrunkSideSnow) ||
          (ActionTrunk_SideContact(s_trunk_assets,x,y)&&ActionTrunk_SideSolid(s_trunk_assets,x,y+1,kActionTrunkSideSnow))));
  const bool high=ActionTrunk_SideSolid(s_trunk_assets,x,y,kActionTrunkSidePriority);
  return (riser?kHollowEdgeSnowRiser:0)|(step?kHollowEdgeFrontStep:0)|
      (snow?kHollowEdgeSnow:0)|(high?kHollowEdgeHigh:0);
}

static bool ScanHollowContour(HollowContour *out,unsigned *rows,int first_x,int last_x,
    int first_y,int last_y,int kind,bool clearance) {
  const int room_height=(int)s_trunk_assets->height;
  const bool passage=kind==2;
  const int mask_left=passage?0:kActionTrunkLeft;
  const int mask_right=passage?(int)s_trunk_assets->side_width:kActionTrunkLeft+kActionTrunkWidth;
  const int mask_pitch=passage?kActionTrunkSideMaskPitch:kActionTrunkMaskPitch;
  if(first_x<mask_left)first_x=mask_left;
  if(last_x>mask_right)last_x=mask_right;
  for(int yy=first_y;yy<last_y;++yy) {
    if(rows)rows[yy]=out->count;
    if(first_x>=last_x)continue;
    for(int byte=(first_x-mask_left)>>3;byte<=((last_x-mask_left-1)>>3);++byte) {
      const unsigned filled=passage?HollowPassageMaskByte(yy,byte,clearance):HollowCoreMaskByte((unsigned)kind,yy,byte);
      if(!filled)continue;
      const unsigned before=passage?HollowPassageMaskByte(yy,byte-1,clearance):HollowCoreMaskByte((unsigned)kind,yy,byte-1);
      const unsigned after=passage?HollowPassageMaskByte(yy,byte+1,clearance):HollowCoreMaskByte((unsigned)kind,yy,byte+1);
      const unsigned upper=passage?HollowPassageMaskByte(yy-1,byte,clearance):HollowCoreMaskByte((unsigned)kind,yy-1,byte);
      const unsigned lower=passage?HollowPassageMaskByte(yy+1,byte,clearance):HollowCoreMaskByte((unsigned)kind,yy+1,byte);
      const unsigned flags[4]={filled&~upper,
          filled&~((filled>>1)|((after&1)<<7)),filled&~lower,
          filled&~((filled<<1)|(before>>7))};
      unsigned exposed=flags[0]|flags[1]|flags[2]|flags[3];
      bool can_step=false;
      if(clearance) {
        /* Adjacent pixels share the same clearance field at their border.
         * A zero-clearance cell cannot contain an internal clearance step. */
        const unsigned stride=kActionTrunkSideWidth/8+1;
        const uint8_t *grid=s_trunk_assets->side_clearance_grid+(yy/8)*stride+byte;
        can_step=grid[0]||grid[1]||grid[stride]||grid[stride+1];
      }
      if(can_step) {
        unsigned nearby=0;
        for(int sy=yy-8;sy<=yy+1;++sy)if(sy>=0&&sy<room_height) {
          const uint8_t *contacts=s_trunk_assets->side_contacts+sy*mask_pitch;
          const unsigned center=contacts[byte],a=byte?contacts[byte-1]:0,b=byte+1<mask_pitch?contacts[byte+1]:0;
          nearby|=center|((center<<1)|(a>>7))|((center>>1)|((b&1)<<7));
        }
        const uint8_t *high=ActionTrunk_SideMaskRow(s_trunk_assets,yy,kActionTrunkSidePriority);
        const uint8_t *ha=ActionTrunk_SideMaskRow(s_trunk_assets,yy-1,kActionTrunkSidePriority);
        const uint8_t *hb=ActionTrunk_SideMaskRow(s_trunk_assets,yy+1,kActionTrunkSidePriority);
        const unsigned h=high[byte],hl=byte?high[byte-1]:0,hr=byte+1<mask_pitch?high[byte+1]:0;
        nearby|=h^((h<<1)|(hl>>7));nearby|=h^((h>>1)|((hr&1)<<7));
        nearby|=h^(ha?ha[byte]:0);nearby|=h^(hb?hb[byte]:0);
        exposed|=filled&nearby;
      }
      while(exposed) {
        const unsigned bit=(unsigned)__builtin_ctz(exposed);exposed&=exposed-1;
        const int wx=mask_left+byte*8+(int)bit;
        if(wx<first_x||wx>=last_x)continue;
        for(int side=0;side<4;++side) {
          const bool boundary=!!(flags[side]&(1u<<bit));bool step=false;
          HollowEdgeDepth depth={.back={1,1}};
          if(!boundary&&can_step) {
            const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
            const int nx=wx+dx[side],ny=yy+dy[side];
            if(!ActionTrunk_SideCrown(s_trunk_assets,nx,ny))continue;
            SampleHollowEdgeDepth(wx,yy,side,false,depth.front);
            SampleHollowEdgeDepth(wx,yy,side,true,depth.back);
            step=depth.front[0]<=depth.back[0]&&depth.front[1]<=depth.back[1]&&
                (depth.front[0]<depth.back[0]||depth.front[1]<depth.back[1]);
          } else if(boundary&&can_step&&rows) {
            SampleHollowEdgeDepth(wx,yy,side,false,depth.front);
          }
          if(!boundary&&!step)continue;
          const unsigned material=passage?HollowPassageEdgeFlags(wx,yy,side,step):0;
          if(!AppendHollowEdge(out,(HollowEdge){(int16_t)wx,(int16_t)yy,(uint8_t)side,(uint8_t)material},
              rows&&clearance?&depth:NULL,rows!=NULL))return false;
        }
      }
    }
  }
  if(rows)rows[last_y]=out->count;
  return true;
}

static bool ResolveHollowContour(int left,int right,int top,int height,int kind,bool clearance,
    HollowContourView *view) {
  *view=(HollowContourView){.left=left,.right=right};
  const int room_height=(int)s_trunk_assets->height;
  const int first_y=top>0?top:0, last_y=top+height<room_height?top+height:room_height;
  const int mask_left=kind==2?0:kActionTrunkLeft;
  const int mask_right=kind==2?(int)s_trunk_assets->side_width:kActionTrunkLeft+kActionTrunkWidth;
  if(first_y>=last_y||left>=right||left>=mask_right||right<=mask_left)return true;
  HollowRoomContour *cache=&s_hollow_contours[kind];
  const bool column=kind==1&&s_trunk_column;
  if(!cache->attempted||cache->revision!=s_trunk_assets->revision||cache->room!=s_trunk_assets->room||
      cache->column!=column||cache->clearance!=clearance) {
    cache->contour.count=0;
    cache->attempted=true;
    cache->revision=s_trunk_assets->revision;
    cache->room=s_trunk_assets->room;
    cache->column=column;
    cache->clearance=clearance;
    if(!clearance) {
      free(cache->contour.depths);
      cache->contour.depths=NULL;
    }
    cache->ready=ScanHollowContour(&cache->contour,cache->rows,mask_left,mask_right,
        0,room_height,kind,clearance);
    if(!cache->ready) {
      free(cache->contour.edges);
      free(cache->contour.depths);
      cache->contour=(HollowContour){0};
    }
  }
  if(cache->ready) {
    const unsigned start=cache->rows[first_y];
    view->count=cache->rows[last_y]-start;
    if(view->count)view->edges=cache->contour.edges+start;
    if(view->count&&cache->contour.depths)view->depths=cache->contour.depths+start;
    return true;
  }

  /* A complex user edit or allocation failure still gets the same bounded
   * per-window extraction the renderer used before the room cache. */
  HollowContour window={.edges=s_hollow_window_edges,.capacity=kHollowMaxEdges};
  if(!ScanHollowContour(&window,NULL,left,right,first_y,last_y,kind,clearance))return false;
  view->edges=window.edges;
  view->count=window.count;
  return true;
}

/* Native standing lips retain actor contact; recessed bodies and internal
 * steps share their endpoints with the source sheets and silhouette caps. */
static bool DrawHollowPassageEdges(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,bool high,
    const DioramaTrunkGuardStrip *guard) {
  if(layer->hollow_recess<=0||!s_trunk_assets->side_width||
      (high&&!s_trunk_assets->side_high_pixels))return true;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  const int left=guard?guard->world_x+guard->valid.x-ox:(int)lroundf(g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const int right=guard?left+guard->valid.width:(int)lroundf(g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  const int contour_y=guard?guard->world_y+guard->valid.y:oy;
  HollowContourView contour;
  const bool clearance=layer->hollow_voxel&&s_trunk_assets->side_clearance;
  if(!ResolveHollowContour(ox+left,ox+right,contour_y,
      guard?guard->valid.height:capture->height,2,clearance,&contour))return false;
  if(!contour.count)return true;
  if(!HollowEnsureSideMaterial(device))return false;
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  const float front=-layer->hollow_recess;
  const float max_span=DioramaTrunk_SideDepth(layer->hollow_recess,0,layer->hollow_voxel);
  const float contact_radius=6*g->aspect_x/(du*SR_PPU_SURFACE_MAX_WIDTH);
  int count=0;
  GroundCoverBeginBatch();
  for(unsigned e=0;e<contour.count;++e) {
    const HollowEdge edge=contour.edges[e];
    const int world_x=edge.x,world_y=edge.y;
    if(world_x<contour.left||world_x>=contour.right)continue;
    const int local_x=world_x-ox;
    if(!!(edge.flags&kHollowEdgeHigh)!=high)continue;
    const bool snow=edge.flags&kHollowEdgeSnow;
    HollowEdgeDepth fallback={.back={1,1}};
    const HollowEdgeDepth *depth=contour.depths?&contour.depths[e]:&fallback;
    if(!contour.depths&&clearance) {
      SampleHollowEdgeDepth(world_x,world_y,edge.side,false,fallback.front);
      if(edge.flags&kHollowEdgeFrontStep)SampleHollowEdgeDepth(world_x,world_y,edge.side,true,fallback.back);
    }
    const int (*corners)[2]=kHollowEdgeCorners[edge.side];
    const float spans[2]={DioramaTrunk_SideDepth(layer->hollow_recess,world_x+corners[0][0],layer->hollow_voxel),
        DioramaTrunk_SideDepth(layer->hollow_recess,world_x+corners[1][0],layer->hollow_voxel)};
    HollowSide next={0};bool ok=true;
    const bool cover_top=s_ground_cover.active_count&&edge.side==0;
    DioramaGroundCoverCap cover={.from={world_x,world_y},.to={world_x+1,world_y},
        .actor_z=-layer->hollow_recess,.contact_radius=contact_radius};
    if(snow)cover.receiver=HollowSnowGroundReceiver;
    for(int i=0;i<4;++i) {
      const int end=i==1||i==2;
      const int *c=corners[end];
      const float u=(local_x+c[0])/(float)SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(world_y-oy+c[1])/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale;
      const float fraction=i>=2?depth->back[end]:depth->front[end];
      const float z=front-spans[end]*fraction;
      if(cover_top)cover.corners[i]=(DioramaGroundCoverCorner){
          .position={world_x+c[0],world_y+c[1],z},.lift={0,-1,0},.depth_fraction=fraction};
      Scene3DClipPoint clip;
      ok=ok&&Scene3D_TransformToClip(g->matrix,wx,wy,z,&clip)&&
          ProjectHollowClip(g,&clip,&next.v[i].position,g->depth_sink?next.depths+i:NULL);
      if(!ok)break;
      next.distance+=clip.w*.25f;
      /* Native two-dimensional grain, anchored along the source contour.
       * Neither its world phase nor the selected snow/bark changes on orbit. */
      const float depth_sample=spans[end]*fraction/max_span;
      next.v[i].tex_coord=(edge.side&1)&&!(edge.flags&kHollowEdgeSnowRiser)
          ?(ArRenderPointF){(.5f+31*depth_sample)/32.f,
              (snow?.5f:0)+fminf(31.5f,(world_y&31)+c[1]+.5f)/64.f}
          :(ArRenderPointF){((world_x&31)+c[0])/32.f,(snow?.5f:0)+(.5f+31*depth_sample)/64.f};
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
    const ArRenderPointF uv={(local_x+.5f)/SR_PPU_SURFACE_MAX_WIDTH,(world_y-oy+.5f)/SR_PPU_SURFACE_MAX_HEIGHT};
    const float dimming=layer->dimming>0?1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,uv.x,uv.y):1;
    const float exposure=layer->exposure&&layer->exposure->sample
        ?layer->exposure->sample(layer->exposure->userdata,uv.x,uv.y,uv.x,uv.y):1;
    for(int i=0;i<4;++i) {
      const float shade=(edge.side==0||snow?(i>=2?.86f:1.f):edge.side==2?(i>=2?.5f:.66f):(i>=2?.65f:.84f))
          *s_trunk_fade*dimming*exposure;
      next.v[i].color=(ArRenderColorF){layer->shade.r*shade,layer->shade.g*shade,layer->shade.b*shade,layer->shade.a};
    }
    s_hollow_sides[count++]=next;
    if(cover_top&&!GatherHollowGroundCover(capture,g,layer,&cover,2,&count))return false;
  }
  DioramaSceneryLightEffectParams light=layer->lighting;
  light.texel_width=1.f/32;light.texel_height=1.f/64;light.mode=0;light.blur_radius=0;
  light.attributes_uv=(ArRenderRectF){0,0,1,1};
  return SubmitHollowEdges(device,g,layer,s_hollow_side_material,&light,count);
}
static bool DrawHollowSolidEdges(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,int kind,
    const DioramaTrunkGuardStrip *guard) {
  if(layer->hollow_recess<=0)return true;
  if(kind==0&&!HollowEnsureMaterial(device))return false;
  const int origin_x=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int origin_y=capture->camera_y-capture->authentic_y0+1;
  const int left=guard?guard->world_x+guard->valid.x-origin_x:(int)lroundf(g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const int right=guard?left+guard->valid.width:(int)lroundf(g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  const int contour_y=guard?guard->world_y+guard->valid.y:origin_y;
  HollowContourView contour;
  if(!ResolveHollowContour(origin_x+left,origin_x+right,contour_y,
      guard?guard->valid.height:capture->height,kind,false,&contour))return false;
  const float front=layer->authored->z-.5f;
  const float depth=kind==0?DioramaTrunk_RimDepth(layer->hollow_recess,layer->hollow_voxel):
      DioramaTrunk_BranchDepth(layer->hollow_recess);
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  const int corners[4][4]={{0,0,1,0},{1,0,1,1},{1,1,0,1},{0,1,0,0}};
  int count=0;
  GroundCoverBeginBatch();
  for(unsigned e=0;e<contour.count;++e) {
    const HollowEdge edge=contour.edges[e];
    if(edge.x<contour.left||edge.x>=contour.right)continue;
    const int *c=corners[edge.side];
    const int world_y=edge.y,local_x=edge.x-origin_x;
    HollowSide next={0};
    const bool cover_top=s_ground_cover.active_count&&edge.side==0;
    DioramaGroundCoverCap cover={.from={edge.x,world_y},
        .to={edge.x+1,world_y},.actor_z=-layer->hollow_recess,
        .contact_radius=6*g->aspect_x/(du*SR_PPU_SURFACE_MAX_WIDTH)};
    const ArRenderColorF bounce=kind==1&&edge.side==2?LifeBranchColor(edge.x,world_y):(ArRenderColorF){1,1,1,1};
    bool ok=true;
    for(int i=0;i<4;++i) {
      const int end=(i==1||i==2)?2:0;
      const float u=(local_x+c[end])/(float)SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(world_y-origin_y+c[end+1])/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale;
      const float z=kind==0&&layer->hollow_voxel
          ?(i>=2?-layer->hollow_recess:DioramaTrunk_RimFront(layer->hollow_recess,edge.x+c[end],true))
          :front-(i>=2?depth:0);
      if(cover_top)cover.corners[i]=(DioramaGroundCoverCorner){
          .position={edge.x+c[end],world_y+c[end+1],z},
          .lift={0,-1,0},.depth_fraction=i>=2?1:0};
      Scene3DClipPoint clip;
      ok=ok&&Scene3D_TransformToClip(g->matrix,wx,wy,z,&clip)&&
          ProjectHollowClip(g,&clip,&next.v[i].position,g->depth_sink?next.depths+i:NULL);
      if(!ok)break;
      next.distance+=clip.w*.25f;
      const ArRenderPointF source_uv={(local_x+.5f)/SR_PPU_SURFACE_MAX_WIDTH,
          (world_y-origin_y+.5f)/SR_PPU_SURFACE_MAX_HEIGHT};
      next.v[i].tex_coord=kind==0?(ArRenderPointF){i>=2?1.f:0.f,
          ((world_y&31)+c[end+1])/32.f}:guard?(ArRenderPointF){
              (edge.x-guard->world_x+.5f)/kDioramaTrunkGuardWidth,
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
