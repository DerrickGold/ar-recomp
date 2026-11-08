/* Sparse authored geometry shares the native priority/depth pass. Its native
 * artwork is room-owned; an offscreen donor never samples viewport guards. */
enum { kAuthoredAtlasWidth=512,kAuthoredAtlasHeight=2048,kAuthoredVisibleCells=8192 };
static ArRenderTexture s_voxel_authored_atlas;
static ArRenderTexture s_voxel_empty_source;
static uint32_t s_voxel_empty_pixels[SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT];
static uint64_t s_voxel_owned_high_revision;
static bool s_voxel_owned_high;
static uint64_t s_voxel_authored_revision;
static uint32_t s_voxel_authored_color_key;
static int16_t s_voxel_authored_slot[kVoxelAuthoredArtMax];
static uint32_t s_voxel_authored_owner[kVoxelAuthoredVisibleArtMax];
static unsigned s_voxel_authored_next_slot;
enum { kVoxelCrispPageTiles=1024,kVoxelCrispPages=kVoxelAuthoredVisibleArtMax/kVoxelCrispPageTiles };
static ArRenderTexture s_voxel_crisp_pages[kVoxelCrispPages];
static unsigned s_voxel_crisp_rows[kVoxelCrispPages];
static uint64_t s_voxel_art_upload_generation,s_voxel_crisp_generation[kVoxelCrispPages];
static uint32_t s_voxel_crisp_pixels[2048*64];
static const VoxelAuthoredCell *s_voxel_authored_visible[kAuthoredVisibleCells];
static uint64_t s_voxel_authored_bounds_revision;
static float s_voxel_authored_motion[3];
static bool s_voxel_authored_has_transform;
typedef struct VoxelAuthoredBounds { float x0,y0,x1,y1,z; } VoxelAuthoredBounds;
static VoxelAuthoredBounds s_voxel_authored_bounds[kVoxelAuthoredCellMax];
static void ResetVoxelAuthoredAtlas(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device,s_voxel_authored_atlas);
  s_voxel_authored_atlas=ArRenderTexture_Invalid();s_voxel_authored_revision=0;
  s_voxel_authored_bounds_revision=0;
  ArRenderDevice_DestroyTexture(device,s_voxel_empty_source);s_voxel_empty_source=ArRenderTexture_Invalid();
  s_voxel_owned_high_revision=0;s_voxel_owned_high=false;
  for(unsigned i=0;i<kVoxelCrispPages;++i) {
    ArRenderDevice_DestroyTexture(device,s_voxel_crisp_pages[i]);s_voxel_crisp_pages[i]=ArRenderTexture_Invalid();
    s_voxel_crisp_rows[i]=0;s_voxel_crisp_generation[i]=0;
  }
  s_voxel_art_upload_generation=0;
}
static bool VoxelOwnsHighArt(const ActionVoxelGeometry *geometry) {
  const uint64_t revision=ActionVoxel_Revision(geometry);
  if(s_voxel_owned_high_revision!=revision) {
    unsigned count;const VoxelAuthoredCell *cells=ActionVoxel_AuthoredCells(geometry,&count);
    s_voxel_owned_high=false;
    for(unsigned i=0;i<count && !s_voxel_owned_high;++i)if(cells[i].surface||cells[i].native_transform)
      for(unsigned q=0;q<4;++q)s_voxel_owned_high|=cells[i].priority[q]!=0;
    s_voxel_owned_high_revision=revision;
  }
  return s_voxel_owned_high;
}
static bool EnsureVoxelEmptySource(ArRenderDevice *device) {
  if(ArRenderTexture_IsValid(s_voxel_empty_source))return true;
  const ArRenderTextureDesc desc={SR_PPU_SURFACE_MAX_WIDTH,SR_PPU_SURFACE_MAX_HEIGHT,
      kArRenderPixelFormat_Argb8888,kArRenderTextureUsage_Static,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
  if(!ArRenderDevice_CreateTexture(device,&desc,&s_voxel_empty_source) ||
      !ArRenderDevice_UpdateTexture(device,s_voxel_empty_source,NULL,s_voxel_empty_pixels,
          SR_PPU_SURFACE_MAX_WIDTH*sizeof(*s_voxel_empty_pixels))) {
    ArRenderDevice_DestroyTexture(device,s_voxel_empty_source);s_voxel_empty_source=ArRenderTexture_Invalid();return false;
  }
  return true;
}
static bool CollectVoxelAuthored(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const ActionVoxelGeometry *geometry,unsigned *count) {
  *count=0;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  /* Search the existing O(1) cell grid with a displacement guard. Z motion
   * can enter the rotated view even without a native XY translation. */
  const float depth_pixels=2*fmaxf((g->u1-g->u0)*SR_PPU_SURFACE_MAX_WIDTH/fmaxf(.01f,g->aspect_x),
      (g->v1-g->v0)*SR_PPU_SURFACE_MAX_HEIGHT/fmaxf(.01f,g->height_scale));
  if(s_voxel_authored_bounds_revision!=ActionVoxel_Revision(geometry)) {
    memset(s_voxel_authored_motion,0,sizeof(s_voxel_authored_motion));
    s_voxel_authored_has_transform=false;
    unsigned cells;const VoxelAuthoredCell *compiled=ActionVoxel_AuthoredCells(geometry,&cells);
    for(unsigned i=0;i<cells;++i) {
      s_voxel_authored_has_transform|=ActionVoxel_TransformValid(&compiled[i].transform);
      if(!compiled[i].bend.axis)continue;
      s_voxel_authored_motion[0]=fmaxf(s_voxel_authored_motion[0],fabsf(compiled[i].bend.dx));
      s_voxel_authored_motion[1]=fmaxf(s_voxel_authored_motion[1],fabsf(compiled[i].bend.dy));
      s_voxel_authored_motion[2]=fmaxf(s_voxel_authored_motion[2],fabsf(compiled[i].bend.dz));
    }
    if(cells>kVoxelAuthoredCellMax)return false;
    if(s_voxel_authored_has_transform)for(unsigned i=0;i<cells;++i) {
      const VoxelAuthoredCell *a=&compiled[i];
      VoxelAuthoredBounds box={INFINITY,INFINITY,-INFINITY,-INFINITY,0};
      for(unsigned q=0;q<4;++q)for(unsigned end=0;end<4;++end) {
        const float depths[]={a->front[q],a->back[q],a->front_end[q],a->back_end[q]};
        const float x=a->x+8+a->offset_x+((q&1)?1:-1)*a->width*.5f;
        const float y=a->y+((q&2)?16:0);
        const float z=a->native_transform?(end&1?ActionVoxel_Depth(geometry)+ActionVoxel_TrimDepth(geometry):0):depths[end];
        for(unsigned bend=0;bend<2;++bend) {
          float tx,ty,tz;ActionVoxel_TransformPoint(&a->transform,x+bend*a->bend.dx,
              y+bend*a->bend.dy,z+bend*a->bend.dz,&tx,&ty,&tz);
          box.x0=fminf(box.x0,tx);box.y0=fminf(box.y0,ty);box.x1=fmaxf(box.x1,tx);box.y1=fmaxf(box.y1,ty);
          box.z=fmaxf(box.z,fabsf(tz));
        }
      }
      s_voxel_authored_bounds[i]=box;
    }
    s_voxel_authored_bounds_revision=ActionVoxel_Revision(geometry);
  }
  /* An affine pivot can bring a distant source into view. Walk only the
   * bounded authored list in that case, never a giant expanded source grid. */
  if(s_voxel_authored_has_transform) {
    unsigned cells;const VoxelAuthoredCell *compiled=ActionVoxel_AuthoredCells(geometry,&cells);
    for(unsigned i=0;i<cells;++i) {
      const VoxelAuthoredCell *a=&compiled[i];if(!a->surface&&!a->native_transform)continue;
      const VoxelAuthoredBounds *box=&s_voxel_authored_bounds[i];
      const float guard=box->z*depth_pixels+16;
      if(box->x1+guard<ox || box->x0-guard>ox+capture->width ||
          box->y1+guard<oy || box->y0-guard>oy+capture->height)continue;
      if(*count==kAuthoredVisibleCells)return false;
      s_voxel_authored_visible[(*count)++]=a;
    }
    return true;
  }
  const float guard_x=s_voxel_authored_motion[0]+s_voxel_authored_motion[2]*depth_pixels;
  const float guard_y=s_voxel_authored_motion[1]+s_voxel_authored_motion[2]*depth_pixels;
  const int x0=(int)floorf((ox-64-guard_x)/16.f),x1=(int)ceilf((ox+capture->width+64+guard_x)/16.f);
  const int y0=(int)floorf((oy-16-guard_y)/16.f),y1=(int)ceilf((oy+capture->height+guard_y)/16.f);
  for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x) {
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(geometry,x,y);
    if(!a || !a->surface)continue;
    const float zguard=fabsf(a->bend.dz)*depth_pixels;
    if(a->x+8+a->offset_x+a->width*.5f+fmaxf(0,a->bend.dx)+zguard<ox ||
        a->x+8+a->offset_x-a->width*.5f+fminf(0,a->bend.dx)-zguard>ox+capture->width ||
        a->y+16+fmaxf(0,a->bend.dy)+zguard<oy || a->y+fminf(0,a->bend.dy)-zguard>oy+capture->height)continue;
    if(*count==kAuthoredVisibleCells)return false;
    s_voxel_authored_visible[(*count)++]=a;
  }
  return true;
}
static bool EnsureVoxelAuthoredAtlas(ArRenderDevice *device,const ActionVoxelGeometry *geometry,
    const ActionRoomSceneFrameState *frame,unsigned visible) {
  const unsigned total=ActionVoxel_AuthoredArtCount(geometry);
  if(total>kVoxelAuthoredArtMax)return false;
  bool needed[kVoxelAuthoredArtMax]={0},pinned[kVoxelAuthoredVisibleArtMax]={0};
  bool dirty[kAuthoredAtlasHeight/16]={0};unsigned wanted=0;
  const unsigned faces[]={kDioramaVoxelFaceMask_Front,kDioramaVoxelFaceMask_Top,0,0,
      kDioramaVoxelFaceMask_Left,kDioramaVoxelFaceMask_Right,kDioramaVoxelFaceMask_Bottom};
  for(unsigned i=0;i<visible;++i)for(unsigned face=0;face<7;++face) {
    const VoxelAuthoredCell *a=s_voxel_authored_visible[i];
    if(!(a->faces&faces[face]) && !(a->native_transform&&face==0) &&
        !(face==3 && ActionVoxel_TransformValid(&a->transform) && (a->faces&kDioramaVoxelFaceMask_Back)))continue;
    const unsigned id=a->art_id[face];if(id>=total)return false;
    if(!needed[id]){needed[id]=true;++wanted;}
  }
  unsigned cap_count;const uint16_t *caps=ActionVoxel_AuthoredCapArt(geometry,&cap_count);
  for(unsigned i=0;i<cap_count;++i) {
    const unsigned id=caps[i];if(id>=total)return false;
    if(!needed[id]){needed[id]=true;++wanted;}
  }
  /* Reserve the complete view before drawing. There is no partial geometry
   * fallback when a malformed/oversized capture exceeds the fixed budget. */
  if(wanted>kVoxelAuthoredVisibleArtMax)return false;
  if(!wanted)return true;
  const uint32_t color_key=frame?frame->brightness|((uint32_t)frame->fixed_color<<4)|
      ((uint32_t)frame->cgadsub<<19)|((uint32_t)(frame->cgwsel&3)<<27):15;
  const uint64_t revision=ActionVoxel_Revision(geometry);
  if(revision!=s_voxel_authored_revision || color_key!=s_voxel_authored_color_key ||
      !ArRenderTexture_IsValid(s_voxel_authored_atlas)) {
    memset(s_voxel_authored_slot,0xff,sizeof(s_voxel_authored_slot));
    memset(s_voxel_authored_owner,0xff,sizeof(s_voxel_authored_owner));s_voxel_authored_next_slot=0;
    s_voxel_authored_revision=revision;s_voxel_authored_color_key=color_key;
  }
  if(!ArRenderTexture_IsValid(s_voxel_authored_atlas)) {
    const ArRenderTextureDesc desc={kAuthoredAtlasWidth,kAuthoredAtlasHeight,kArRenderPixelFormat_Argb8888,
        kArRenderTextureUsage_Static,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
    if(!ArRenderDevice_CreateTexture(device,&desc,&s_voxel_authored_atlas))return false;
  }
  for(unsigned id=0;id<total;++id)if(needed[id] && s_voxel_authored_slot[id]>=0)pinned[s_voxel_authored_slot[id]]=true;
  for(unsigned id=0;id<total;++id)if(needed[id] && s_voxel_authored_slot[id]<0) {
    unsigned searched=0;
    while(pinned[s_voxel_authored_next_slot] && searched++<kVoxelAuthoredVisibleArtMax)
      s_voxel_authored_next_slot=(s_voxel_authored_next_slot+1)%kVoxelAuthoredVisibleArtMax;
    if(searched>=kVoxelAuthoredVisibleArtMax)return false;
    const unsigned slot=s_voxel_authored_next_slot;
    if(s_voxel_authored_owner[slot]!=UINT32_MAX)s_voxel_authored_slot[s_voxel_authored_owner[slot]]=-1;
    s_voxel_authored_owner[slot]=id;s_voxel_authored_slot[id]=slot;pinned[slot]=true;
    dirty[slot/32]=true;s_voxel_authored_next_slot=(slot+1)%kVoxelAuthoredVisibleArtMax;
  }
  uint32_t pixels[kAuthoredAtlasWidth*16];
  for(unsigned row=0;row<kAuthoredAtlasHeight/16;++row)if(dirty[row]) {
    for(unsigned tile=0;tile<32;++tile) {
      const uint16_t *source=ActionVoxel_AuthoredArt(geometry,s_voxel_authored_owner[row*32+tile]);
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        pixels[y*kAuthoredAtlasWidth+tile*16+x]=source?ActionVoxel_AuthoredColor(source[y*16+x],frame):0;
    }
    const ArRenderRectI rect={0,(int)row*16,kAuthoredAtlasWidth,16};
    if(!ArRenderDevice_UpdateTexture(device,s_voxel_authored_atlas,&rect,pixels,kAuthoredAtlasWidth*sizeof(*pixels))) {
      s_voxel_authored_revision=0;return false;
    }
    ++s_voxel_art_upload_generation;
  }
  return true;
}
static bool PrepareVoxelAuthoredAtlas(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const ActionVoxelGeometry *geometry) {
  unsigned visible;return CollectVoxelAuthored(capture,g,geometry,&visible) &&
      EnsureVoxelAuthoredAtlas(device,geometry,capture->voxel_frame,visible);
}
static ArRenderTexture VoxelAuthoredAtlas(void) { return s_voxel_authored_atlas; }
static bool VoxelOwnedArtReady(unsigned id) { return id<kVoxelAuthoredArtMax && s_voxel_authored_slot[id]>=0; }
static ArRenderPointF VoxelOwnedArtUv(unsigned id,float u,float v) {
  const unsigned slot=s_voxel_authored_slot[id];
  return (ArRenderPointF){((slot%32)*16+u)/kAuthoredAtlasWidth,((slot/32)*16+v)/kAuthoredAtlasHeight};
}
/* The native front uses integer 4x nearest expansion followed by linear
 * sampling. Cache that same source once per native-art upload. Pages remain
 * within 2048x2048 and grow only to their occupied rows, rather than creating
 * an 8192px-high copy of the entire shared cap atlas. */
static bool PrepareVoxelCrispPage(ArRenderDevice *device,const ActionVoxelGeometry *geometry,
    const ActionRoomSceneFrameState *frame,unsigned page,unsigned needed_rows) {
  if(page>=kVoxelCrispPages || !needed_rows || needed_rows>32)return false;
  if(s_voxel_crisp_rows[page]<needed_rows || !ArRenderTexture_IsValid(s_voxel_crisp_pages[page])) {
    unsigned rows=1;while(rows<needed_rows)rows*=2;
    ArRenderDevice_DestroyTexture(device,s_voxel_crisp_pages[page]);s_voxel_crisp_pages[page]=ArRenderTexture_Invalid();
    const ArRenderTextureDesc desc={2048,(int)rows*64,kArRenderPixelFormat_Argb8888,
        kArRenderTextureUsage_Static,kArRenderFilter_Linear,kArRenderBlendMode_Alpha};
    if(!ArRenderDevice_CreateTexture(device,&desc,&s_voxel_crisp_pages[page]))return false;
    s_voxel_crisp_rows[page]=rows;s_voxel_crisp_generation[page]=0;
  }
  if(s_voxel_crisp_generation[page]==s_voxel_art_upload_generation)return true;
  for(unsigned row=0;row<s_voxel_crisp_rows[page];++row) {
    for(unsigned y=0;y<16;++y) {
      uint32_t *out=s_voxel_crisp_pixels+y*4*2048;
      for(unsigned tile=0;tile<32;++tile) {
        const unsigned id=s_voxel_authored_owner[page*kVoxelCrispPageTiles+row*32+tile];
        const uint16_t *source=ActionVoxel_AuthoredArt(geometry,id);
        for(unsigned x=0;x<16;++x) {
          const uint32_t color=source?ActionVoxel_AuthoredColor(source[y*16+x],frame):0;
          const unsigned at=tile*64+x*4;out[at]=out[at+1]=out[at+2]=out[at+3]=color;
        }
      }
      for(unsigned copy=1;copy<4;++copy)memcpy(out+copy*2048,out,2048*sizeof(*out));
    }
    const ArRenderRectI rect={0,(int)row*64,2048,64};
    if(!ArRenderDevice_UpdateTexture(device,s_voxel_crisp_pages[page],&rect,s_voxel_crisp_pixels,2048*sizeof(uint32_t)))return false;
  }
  s_voxel_crisp_generation[page]=s_voxel_art_upload_generation;return true;
}
static bool PrepareVoxelCrispFronts(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaLayerDraw *layer,unsigned visible) {
  unsigned needed[kVoxelCrispPages]={0};
  for(unsigned i=0;i<visible;++i) {
    const VoxelAuthoredCell *a=s_voxel_authored_visible[i];
    if(!(a->faces&kDioramaVoxelFaceMask_Front) || (!a->native_transform&&!a->native_front_art))continue;
    const int slot=s_voxel_authored_slot[a->art_id[0]];if(slot<0)return false;
    const unsigned page=(unsigned)slot/kVoxelCrispPageTiles,rows=((unsigned)slot%kVoxelCrispPageTiles)/32+1;
    if(rows>needed[page])needed[page]=rows;
  }
  /* Resolve every page extent before submitting any faces. A later source
   * tile must never resize a texture already referenced by queued draws. */
  for(unsigned page=0;page<kVoxelCrispPages;++page)if(needed[page] &&
      !PrepareVoxelCrispPage(device,layer->voxel_geometry,capture->voxel_frame,page,needed[page]))return false;
  return true;
}
/* Match the piecewise surface used by front triangles. New actor/depth cuts
 * inside a segment interpolate its endpoints rather than rebending the cut
 * into a different curve from its adjacent front. */
static void AuthoredBendOffsets(const VoxelAuthoredCell *a,float sx,float sy,
    float *dx,float *dy,float *dz) {
  if(!a->bend.axis){*dx=*dy=*dz=0;return;}
  const float value=a->bend.axis==kDioramaVoxelBend_X?sx:sy;
  float lo=floorf(value/4)*4,hi=lo+4;
  const float knots[]={a->bend.anchor,a->bend.tip};
  for(unsigned i=0;i<2;++i)if(knots[i]>lo && knots[i]<hi) {
    if(knots[i]<=value)lo=knots[i];else hi=knots[i];
  }
  float ax,ay,az,bx,by,bz;
  ActionVoxel_AuthoredBend(a,a->bend.axis==kDioramaVoxelBend_X?lo:sx,
      a->bend.axis==kDioramaVoxelBend_Y?lo:sy,&ax,&ay,&az);
  ActionVoxel_AuthoredBend(a,a->bend.axis==kDioramaVoxelBend_X?hi:sx,
      a->bend.axis==kDioramaVoxelBend_Y?hi:sy,&bx,&by,&bz);
  const float t=(value-lo)/(hi-lo);*dx=ax+(bx-ax)*t;*dy=ay+(by-ay)*t;*dz=az+(bz-az)*t;
}
static bool SubmitVoxelAuthored(ArRenderDevice *device,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,unsigned count) {
  if(!count)return true;
  SortVoxelSides(count);
  for(unsigned first=0;first<count;) {
    const unsigned crisp=s_voxel_sides[s_voxel_order[first]].crisp_page;
    unsigned end=first+1;while(end<count && s_voxel_sides[s_voxel_order[end]].crisp_page==crisp)++end;
    for(unsigned i=first;i<end;++i) {
      const VoxelSide *s=&s_voxel_sides[s_voxel_order[i]];const unsigned at=(i-first)*4;
      memcpy(s_voxel_vertices+at,s->v,sizeof(s->v));
      memcpy(s_voxel_depths+at,s->depths,sizeof(s->depths));
      memcpy(s_voxel_clip_w+at,s->clip_w,sizeof(s->clip_w));
      if(crisp)for(unsigned j=0;j<4;++j)
        s_voxel_vertices[at+j].tex_coord.y=(s_voxel_vertices[at+j].tex_coord.y*kAuthoredAtlasHeight-(crisp-1)*512)/
            (s_voxel_crisp_rows[crisp-1]*16);
    }
    DioramaSceneryLightEffectParams lighting=layer->lighting;
    if(crisp){lighting.texel_width=1.f/2048;lighting.texel_height=1.f/(s_voxel_crisp_rows[crisp-1]*64);}
    if(!RenderShadedDioramaDepthGeometryWithW(device,crisp?s_voxel_crisp_pages[crisp-1]:s_voxel_authored_atlas,
        s_voxel_vertices,s_voxel_depths,s_voxel_clip_w,(end-first)*4,VoxelQuadIndices(),(end-first)*6,
        layer->blend,NULL,(ArRenderRectF){0,0,1,1},&lighting,g->depth_sink,
        layer->description->plane,false))return false;
    first=end;
  }
  return true;
}
static bool AddVoxelAuthoredFace(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,const VoxelAuthoredCell *cell,
    unsigned art,int kind,unsigned priority,const float xy[8],
    const float depths[4],const float uv[8],unsigned *count) {
  if(*count==kVoxelMaxVisibleFaces) {
    if(!SubmitVoxelAuthored(device,g,layer,*count))return false;*count=0;
  }
  const int slot=s_voxel_authored_slot[cell->art_id[art]];if(slot<0)return false;
  VoxelSide face={0};
  const bool native_front=kind==kDioramaVoxelFace_Front && (cell->native_transform||cell->native_front_art);
  if(native_front) {
    const unsigned page=(unsigned)slot/kVoxelCrispPageTiles;
    if(!ArRenderTexture_IsValid(s_voxel_crisp_pages[page]))return false;
    face.crisp_page=page+1;
  }
  DioramaVoxelPickFace pick={.authored=true,.kind=kind,.priority=priority,.plane=layer->description->plane,
      .cell_x=cell->x/16,.cell_y=cell->y/16,.art_index=art};
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  for(unsigned j=0;j<4;++j) {
    const float sx=cell->x+(xy[j*2]-cell->x-8-cell->offset_x)*16/cell->width+8;
    const float sy=xy[j*2+1];float dx,dy,dz;AuthoredBendOffsets(cell,sx,sy,&dx,&dy,&dz);
    float x,y,z;ActionVoxel_TransformPoint(&cell->transform,xy[j*2]+dx,sy+dy,depths[j]+dz,&x,&y,&z);
    const float u=(x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
    const float scale=1+z/fmaxf(1,g->camera.distance);
    if(!VoxelProjectWithW(g,((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x*scale,
        (.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale*scale,-z,
        &face.v[j].position,&face.distance,&face.depths[j],&face.clip_w[j]))return true;
    const float inset=native_front?.125f:.0001f;
    face.v[j].tex_coord=(ArRenderPointF){((slot%32)*16+fmaxf(inset,fminf(16-inset,uv[j*2])))/kAuthoredAtlasWidth,
        ((slot/32)*16+fmaxf(inset,fminf(16-inset,uv[j*2+1])))/kAuthoredAtlasHeight};
    pick.art_uv[j*2]=uv[j*2];pick.art_uv[j*2+1]=uv[j*2+1];
    face.v[j].color=layer->shade;
    const float shade=(kind==kDioramaVoxelFace_Side || kind==kDioramaVoxelFace_Left ||
        kind==kDioramaVoxelFace_Right || kind==kDioramaVoxelFace_Bottom)?.82f:1;
    face.v[j].color.r*=shade;face.v[j].color.g*=shade;face.v[j].color.b*=shade;
    if(native_front || kind==kDioramaVoxelFace_Backing) {
      const float source_u=(sx-ox)/SR_PPU_SURFACE_MAX_WIDTH,source_v=(sy-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
      float light=1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,source_u,source_v);
      if(layer->exposure && layer->exposure->sample)
        light*=layer->exposure->sample(layer->exposure->userdata,source_u,source_v,source_u,source_v);
      face.v[j].color.r*=light;face.v[j].color.g*=light;face.v[j].color.b*=light;
    }
    pick.screen_xy[j*2]=face.v[j].position.x;pick.screen_xy[j*2+1]=face.v[j].position.y;
    pick.world_xy[j*2]=sx;pick.world_xy[j*2+1]=sy;pick.depth[j]=face.depths[j];pick.clip_w[j]=face.clip_w[j];
  }
  if(g->options->voxel_observe_face)g->options->voxel_observe_face(g->options->voxel_observer_userdata,&pick);
  s_voxel_sides[(*count)++]=face;return true;
}
/* World-fixed subdivision plus profile endpoints keeps every adjacent face
 * on the same piecewise curve. Clipping and actor splits reuse these knots. */
static unsigned AuthoredAxisCuts(const VoxelAuthoredCell *a,unsigned axis,float start,float end,float cuts[16]) {
  unsigned n=0;cuts[n++]=start;
  if((axis==kDioramaVoxelBend_Y && a->shape==kDioramaVoxelShape_Curve) || a->bend.axis==axis)
    for(float value=(floorf(start/4)+1)*4;value<end;value+=4)cuts[n++]=value;
  cuts[n++]=end;
  float knots[4];unsigned count=0;
  if(axis==kDioramaVoxelBend_Y && a->shape!=kDioramaVoxelShape_Box) {
    knots[count++]=a->shape_y;knots[count++]=a->shape_y+a->shape_h;
  }
  if(a->bend.axis==axis){knots[count++]=a->bend.anchor;knots[count++]=a->bend.tip;}
  for(unsigned k=0;k<count;++k) {
    const float value=knots[k];if(value<=start || value>=end)continue;
    unsigned at=0;while(at<n && cuts[at]<value)++at;if(at<n && cuts[at]==value)continue;
    for(unsigned i=n;i>at;--i)cuts[i]=cuts[i-1];cuts[at]=value;++n;
  }
  return n;
}
#include "diorama_voxel_cap_clip.h"
static bool DrawVoxelNativeTransformedBacking(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer) {
  if(!ActionVoxel_HasTransformedBacks(layer->voxel_geometry))return true;
  unsigned visible;if(!CollectVoxelAuthored(capture,g,layer->voxel_geometry,&visible) ||
      !EnsureVoxelAuthoredAtlas(device,layer->voxel_geometry,capture->voxel_frame,visible))return false;
  unsigned count=0;
  for(unsigned i=0;i<visible;++i) {
    const VoxelAuthoredCell *a=s_voxel_authored_visible[i];
    if(!ActionVoxel_TransformValid(&a->transform) || !(a->faces&kDioramaVoxelFaceMask_Back))continue;
    bool ink=false;for(unsigned row=0;row<16;++row)ink|=a->native_rear_rows[row]!=0;
    if(!ink)continue;
    uint16_t used[16]={0};
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      if(!(a->native_rear_rows[y]&(1u<<(15-x))) || (used[y]&(1u<<(15-x))))continue;
      unsigned x1=x+1,y1=y+1;
      while(x1<16 && (a->native_rear_rows[y]&(1u<<(15-x1))) && !(used[y]&(1u<<(15-x1))))++x1;
      const unsigned bits=((1u<<(x1-x))-1)<<(16-x1);
      while(y1<16 && (a->native_rear_rows[y1]&bits)==bits && !(used[y1]&bits))++y1;
      for(unsigned row=y;row<y1;++row)used[row]|=bits;
      const float xy[]={a->x+x,a->y+y,a->x+x1,a->y+y,a->x+x1,a->y+y1,a->x+x,a->y+y1};
      const float uv[]={x,y,x1,y,x1,y1,x,y1};float z[4];
      for(unsigned j=0;j<4;++j)z[j]=ActionVoxel_Depth(layer->voxel_geometry)+
          ActionVoxel_RearRelief(layer->voxel_geometry,xy[j*2],xy[j*2+1]);
      if(!AddVoxelAuthoredFace(device,capture,g,layer,a,3,kDioramaVoxelFace_Backing,0,xy,z,uv,&count))return false;
    }
  }
  return SubmitVoxelAuthored(device,g,layer,count);
}
/* Transform-only records retain the semantic native front: its eight-pixel
 * material grid, trunk/column knots and mixed post pixel-depth masks. */
static bool DrawVoxelNativeTransformedFront(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,const VoxelAuthoredCell *a,
    bool high,unsigned *count) {
  if(!(a->faces&kDioramaVoxelFaceMask_Front) || g->options->voxel_inspection==kDioramaVoxelInspect_Cutaway)return true;
  const ActionVoxelGeometry *geometry=layer->voxel_geometry;
  for(unsigned q=0;q<4;++q)if(a->priority[q]==high) {
    const int tx=a->x/8+(q&1),ty=a->y/8+(q>>1),x=(q&1)*8,y=(q>>1)*8;
    const uint8_t *mask=!high?ActionVoxel_PixelRecessRows(geometry,tx,ty):NULL;
    const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(geometry,tx,ty);
    const bool column=!curve && ActionVoxel_ColumnCurveForCell(geometry,tx,ty);
    if(column)curve=ActionVoxel_ColumnCurveForCell(geometry,tx,ty);
    float ys[16];unsigned ny=0;
    if(curve)ny=VoxelTrunkCuts(curve,a->y+y,a->y+y+8,0,true,ys);
    else if(mask){for(unsigned row=0;row<=8;++row)ys[ny++]=a->y+y+row;}
    else {ys[ny++]=a->y+y;ys[ny++]=a->y+y+8;}
    const unsigned columns=mask?8:1;
    for(unsigned row=0;row+1<ny;++row)for(unsigned col=0;col<columns;++col) {
      const float x0=a->x+x+col*8/columns,x1=a->x+x+(col+1)*8/columns;
      float depths[2];
      for(unsigned end=0;end<2;++end)depths[end]=curve?
          (column?ActionVoxel_ColumnFrontDepth(geometry,curve,ys[row+end]):ActionVoxel_TrunkFrontDepth(geometry,curve,ys[row+end])):
          mask?((mask[row]&(1u<<(7-col)))?ActionVoxel_SceneryRecess(geometry):0):ActionVoxel_FrontRecess(geometry,tx,ty);
      const float xy[]={x0,ys[row],x1,ys[row],x1,ys[row+1],x0,ys[row+1]};
      const float z[]={depths[0],depths[0],depths[1],depths[1]};
      const float uv[]={x0-a->x,ys[row]-a->y,x1-a->x,ys[row]-a->y,
          x1-a->x,ys[row+1]-a->y,x0-a->x,ys[row+1]-a->y};
      if(!AddVoxelAuthoredFace(device,capture,g,layer,a,0,kDioramaVoxelFace_Front,high,xy,z,uv,count))return false;
    }
  }
  return true;
}
static bool DrawVoxelAuthored(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,bool front_phase) {
  if(g->options->voxel_inspection==kDioramaVoxelInspect_Backing)return true;
  unsigned total=0;if(!ActionVoxel_AuthoredCells(layer->voxel_geometry,&total) || !total)return true;
  unsigned visible;if(!CollectVoxelAuthored(capture,g,layer->voxel_geometry,&visible) ||
      !EnsureVoxelAuthoredAtlas(device,layer->voxel_geometry,capture->voxel_frame,visible))return false;
  if(!front_phase && g->options->voxel_inspection!=kDioramaVoxelInspect_Cutaway &&
      !PrepareVoxelCrispFronts(device,capture,layer,visible))return false;
  const bool high=layer->description->plane==kDioramaPlane_Bg1Hi;
  const float split=fmaxf(0,layer->voxel_recess-.01f);
  const float unit=16*g->aspect_x/((g->u1-g->u0)*SR_PPU_SURFACE_MAX_WIDTH);
  if(!(unit>0))return false;
  unsigned count=0;
  for(unsigned i=0;i<visible;++i) {
    const VoxelAuthoredCell *a=s_voxel_authored_visible[i];
    if(a->native_transform) {
      if(!front_phase && !DrawVoxelNativeTransformedFront(device,capture,g,layer,a,high,&count))return false;
      continue;
    }
    const float left=a->x+8-a->width*.5f+a->offset_x;
    if(!front_phase && (a->faces&kDioramaVoxelFaceMask_Front) &&
        g->options->voxel_inspection!=kDioramaVoxelInspect_Cutaway)
      for(unsigned q=0;q<4;++q)if(a->priority[q]==high) {
      const int x=(q&1)*8,y=(q>>1)*8;bool ink=false;
      for(int yy=y;yy<y+8 && !ink;++yy)for(int xx=x;xx<x+8;++xx)ink|=(a->pixels[0][yy*16+xx]&0x8000)!=0;
      if(!ink)continue;
      float xc[16],yc[16];
      const unsigned nx=AuthoredAxisCuts(a,kDioramaVoxelBend_X,a->x+x,a->x+x+8,xc);
      const unsigned ny=AuthoredAxisCuts(a,kDioramaVoxelBend_Y,a->y+y,a->y+y+8,yc);
      for(unsigned yy=0;yy+1<ny;++yy)for(unsigned xx=0;xx+1<nx;++xx) {
        float f0,b0,f1,b1;ActionVoxel_AuthoredDepths(a,q,yc[yy],&f0,&b0);
        ActionVoxel_AuthoredDepths(a,q,yc[yy+1],&f1,&b1);
        const float px0=left+(xc[xx]-a->x)*a->width/16,px1=left+(xc[xx+1]-a->x)*a->width/16;
        const float xy[]={px0,yc[yy],px1,yc[yy],px1,yc[yy+1],px0,yc[yy+1]};
        const float z[]={f0,f0,f1,f1},uv[]={xc[xx]-a->x,yc[yy]-a->y,xc[xx+1]-a->x,yc[yy]-a->y,
            xc[xx+1]-a->x,yc[yy+1]-a->y,xc[xx]-a->x,yc[yy+1]-a->y};
        if(!AddVoxelAuthoredFace(device,capture,g,layer,a,0,kDioramaVoxelFace_Front,high,xy,z,uv,&count))return false;
      }
    }
    unsigned contours;const VoxelAuthoredContour *edges=ActionVoxel_AuthoredContours(layer->voxel_geometry,a,&contours);
    const unsigned masks[]={kDioramaVoxelFaceMask_Top,kDioramaVoxelFaceMask_Right,kDioramaVoxelFaceMask_Bottom,kDioramaVoxelFaceMask_Left};
    const unsigned arts[]={1,5,6,4};
    const int kinds[]={kDioramaVoxelFace_Top,kDioramaVoxelFace_Right,kDioramaVoxelFace_Bottom,kDioramaVoxelFace_Left};
    for(unsigned e=0;e<contours;++e) {
      const VoxelAuthoredContour edge=edges[e];const unsigned side=edge.side,q=(edge.y/8)*2+edge.x/8;
      if(!(a->faces&masks[side]) || a->priority[q]!=high)continue;
      const bool automatic=(a->automatic_art&(1u<<arts[side]))!=0;
      const float sx=a->x+edge.x+(side==1),sy=a->y+edge.y+(side==2);
      const unsigned axis=side&1?kDioramaVoxelBend_Y:kDioramaVoxelBend_X;
      const float start=side&1?sy:sx;
      float cuts[16];const unsigned n=AuthoredAxisCuts(a,axis,start,start+edge.length,cuts);
      for(unsigned segment=0;segment+1<n;++segment) {
        float raw_near[2],raw_far[2],local_split[2],base_x[2],base_y[2];
        for(unsigned end=0;end<2;++end) {
          const float source_x=side&1?sx:cuts[segment+end];
          const float source_y=side&1?cuts[segment+end]:sy;
          base_x[end]=left+(source_x-a->x)*a->width/16;base_y[end]=source_y;
          ActionVoxel_AuthoredDepths(a,q,source_y,&raw_near[end],&raw_far[end]);
          float dx,dy,dz;AuthoredBendOffsets(a,source_x,source_y,&dx,&dy,&dz);
          const DioramaVoxelTransform *t=&a->transform;
          local_split[end]=(ActionVoxel_TransformValid(t)?t->pz+(split-t->pz-t->dz)/t->sz:split)-dz;
        }
        /* Bound by the undeformed material interval. A tilted actor plane
         * can reveal an interior strip even when both endpoints collapse. */
        const float min_depth=fminf(raw_near[0],raw_near[1]),max_depth=fmaxf(raw_far[0],raw_far[1]);
        const int first=automatic?0:(int)floorf(min_depth/unit);
        const int last=automatic?1:(int)ceilf(max_depth/unit);
        for(int tile=first;tile<last;++tile) {
          const float z0=automatic?min_depth:tile*unit,z1=automatic?max_depth:(tile+1)*unit;
          float tc[8];const unsigned nt=AuthoredCapCutsShifted(raw_near,raw_far,z0,z1,high,local_split,tc);
          for(unsigned part=0;part+1<nt;++part) {
            float zn[2],zf[2],px[2],py[2],u[2];
            for(unsigned end=0;end<2;++end) {
              const float t=tc[part+end];zn[end]=raw_near[0]+(raw_near[1]-raw_near[0])*t;
              zf[end]=raw_far[0]+(raw_far[1]-raw_far[0])*t;
              AuthoredCapSpan(&zn[end],&zf[end],high,front_phase,
                  local_split[0]+(local_split[1]-local_split[0])*t);
              zn[end]=fmaxf(z0,fminf(z1,zn[end]));zf[end]=fmaxf(z0,fminf(z1,zf[end]));
              px[end]=base_x[0]+(base_x[1]-base_x[0])*t;py[end]=base_y[0]+(base_y[1]-base_y[0])*t;
              u[end]=cuts[segment]+(cuts[segment+1]-cuts[segment])*t-(side&1?a->y:a->x);
            }
            if(zf[0]<=zn[0]+.000001f && zf[1]<=zn[1]+.000001f)continue;
            const float xy[]={px[0],py[0],px[1],py[1],px[1],py[1],px[0],py[0]};
            const float z[]={zn[0],zn[1],zf[1],zf[0]};
            float uv[]={u[0],(zn[0]-z0)*16/unit,u[1],(zn[1]-z0)*16/unit,
                u[1],(zf[1]-z0)*16/unit,u[0],(zf[0]-z0)*16/unit};
            if(automatic)for(unsigned vertex=0;vertex<4;++vertex) {
              /* The front alpha defines the volume outline. Its automatic
               * skin extends opaque contour ink; explicit art keeps alpha. */
              const unsigned end=vertex==1 || vertex==2;
              const float along=fmaxf(.5f,fminf(edge.length-.5f,u[end]-(side&1?edge.y:edge.x)));
              uv[vertex*2]=edge.x+(side&1?.5f:along);
              uv[vertex*2+1]=edge.y+(side&1?along:.5f);
            }
            if(!AddVoxelAuthoredFace(device,capture,g,layer,a,arts[side],kinds[side],high,xy,z,uv,&count))return false;
          }
        }
      }
    }
  }
  return SubmitVoxelAuthored(device,g,layer,count);
}
