/* Native-low terrain and decorative beam volumes must not erase a passing actor. Replay
 * the actual submitted triangles against a small opaque OBJ source: ordering
 * alone is insufficient unless a foreground control really hits those pixels. */
typedef struct NativeActorDraw {
  ArRenderVertex2D vertices[3];
  bool actor;
  unsigned source_width,source_height;
} NativeActorDraw;
typedef struct NativeActorRecorder {
  Recorder recorder; /* Existing validating backend callbacks own this prefix. */
  NativeActorDraw draws[512];
  unsigned count;
} NativeActorRecorder;
static bool NativeActorGeometry(void *context,ArRenderTexture texture,
    const ArRenderVertex2D *vertices,int vertex_count,const int32_t *indices,
    int index_count,const ArRenderDrawState *state) {
  NativeActorRecorder *test=context;Recorder *r=&test->recorder;
  if(!Geometry(r,texture,vertices,vertex_count,indices,index_count,state))return false;
  const ArRenderTexture source=r->copied_sources[texture.value].value?
      r->copied_sources[texture.value]:texture;
  const bool actor=source.value==r->actor_texture.value;
  for(int triangle=0;triangle+2<index_count;triangle+=3) {
    ArRenderVertex2D v[3];
    for(unsigned j=0;j<3;++j)v[j]=vertices[indices[triangle+j]];
    bool observed=false;
    for(unsigned f=0;f<r->native_affine->count && !observed;++f) {
      observed=true;
      for(unsigned j=0;j<3;++j) {
        bool corner=false;
        for(unsigned k=0;k<4;++k)corner|=
            fabsf(v[j].position.x-r->native_affine->faces[f].screen_xy[k*2])<.001f &&
            fabsf(v[j].position.y-r->native_affine->faces[f].screen_xy[k*2+1])<.001f;
        observed&=corner;
      }
    }
    if(!actor && !observed)continue;
    assert(test->count<sizeof(test->draws)/sizeof(*test->draws));
    NativeActorDraw *draw=test->draws+test->count++;
    *draw=(NativeActorDraw){.actor=actor,
        .source_width=source.value==texture.value?r->textures[texture.value].width:r->capture->width,
        .source_height=source.value==texture.value?r->textures[texture.value].height:r->capture->height};
    memcpy(draw->vertices,v,sizeof(draw->vertices));
  }
  return true;
}
static float NativeActorCross(ArRenderPointF a,ArRenderPointF b,ArRenderPointF p) {
  return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
}
static bool NativeActorTriangle(const ArRenderVertex2D *a,const ArRenderVertex2D *b,
    const ArRenderVertex2D *c,ArRenderPointF point,ArRenderPointF *uv) {
  const float area=NativeActorCross(a->position,b->position,c->position);
  if(fabsf(area)<.00001f)return false;
  const float wa=NativeActorCross(b->position,c->position,point)/area,
      wb=NativeActorCross(c->position,a->position,point)/area,
      wc=NativeActorCross(a->position,b->position,point)/area;
  if(wa<-.00001f || wb<-.00001f || wc<-.00001f)return false;
  *uv=(ArRenderPointF){wa*a->tex_coord.x+wb*b->tex_coord.x+wc*c->tex_coord.x,
      wa*a->tex_coord.y+wb*b->tex_coord.y+wc*c->tex_coord.y};
  return true;
}
static uint32_t NativeActorPixel(const NativeActorDraw *draw,ArRenderPointF point) {
  const ArRenderVertex2D *v=draw->vertices;ArRenderPointF uv;
  if(!NativeActorTriangle(v,v+1,v+2,point,&uv))return 0;
  if(!draw->actor)return UINT32_C(0xff9b6231); /* Fixture map artwork is fully opaque. */
  const int x=(int)floorf(uv.x*draw->source_width),y=(int)floorf(uv.y*draw->source_height);
  /* A checker-pattern lower-body sprite makes any overwritten pixel visible.
   * Only these source pixels are opaque; the rest of the OBJ plane is clear. */
  if(x<72 || x>=104 || y<80 || y>=120)return 0;
  return ((x^y)&1)?UINT32_C(0xff195be1):UINT32_C(0xfff6e85a);
}
static void NativeActorPixels(const NativeActorRecorder *test,const DioramaProjection *projection,
    unsigned policy) {
  unsigned samples=0,overlap=0,overwritten=0;
  for(unsigned y=80;y<120;++y)for(unsigned x=72;x<104;++x) {
    ArRenderPointF point;
    assert(Diorama_ProjectCapturedPoint(projection,x+.37f,y+.43f,2,&point,NULL,NULL));
    uint32_t expected=0,actual=0;bool map=false;
    for(unsigned d=0;d<test->count;++d) {
      const NativeActorDraw *draw=test->draws+d;
      const uint32_t pixel=NativeActorPixel(draw,point);if(!pixel)continue;
      if(draw->actor)expected=pixel;else map=true;
      actual=pixel;
    }
    if(!expected)continue;
    ++samples;overlap+=map;overwritten+=actual!=expected;
  }
  if(samples<=200 || overlap<=20) {
    fprintf(stderr,"Native actor pixels: policy%u samples%u overlap%u overwritten%u commands%u output%d,%d\n",policy,samples,overlap,overwritten,test->count,projection->output_x,projection->output_y);
  }
  assert(samples>200 && overlap>20);
  if(policy!=1)assert(!overwritten);
  else assert(overwritten>5); /* Native-high caps prove the oracle can catch cutoff legs. */
}
static unsigned NativeActorPriorityFixture(unsigned scenario) {
  s_count=0;const unsigned path=scenario%3;const bool shared_depth=scenario>=3;
  static NativeAffineProbe probe;static NativeActorRecorder test;
  memset(&probe,0,sizeof(probe));memset(&test,0,sizeof(test));
  Recorder *r=&test.recorder;
  *r=(Recorder){.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};
  ArRenderBackendOps ops=kOps;ops.draw_geometry=NativeActorGeometry;ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&ops,&test,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},textures+i));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r->actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=2;voxel->active_room=1;voxel->profile=kActionVoxelProfile_BloodpoolMarsh;
  voxel->settings=ActionVoxel_SettingsForRoom(2,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=malloc(sizeof(*voxel->authored_cap_art));
  voxel->authored=calloc(1,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)voxel->width/16,voxel->height/16*sizeof(*voxel->authored_at));
  voxel->authored_contours=calloc(4,sizeof(*voxel->authored_contours));
  assert(voxel->authored_art&&voxel->authored_cap_art&&voxel->authored&&voxel->authored_at&&voxel->authored_contours);
  voxel->authored_art_count=voxel->authored_art_capacity=voxel->authored_cap_art_count=1;
  voxel->authored_cap_art[0]=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)voxel->authored_art[0][y*16+x]=NativeAffineColor(x,y);
  const VoxelEdge outline[]={
    {.x=80,.y=96,.length=16,.side=0,.far_rank=2,.colliding=1},
    {.x=95,.y=96,.length=8,.side=1,.far_rank=2,.colliding=1},
    {.x=80,.y=103,.length=16,.side=2,.far_rank=2,.colliding=1},
    {.x=80,.y=96,.length=8,.side=3,.far_rank=2,.colliding=1},
  };
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=r};
  DioramaScene scene={.render=&options,.depth_sink=shared_depth?&sink:NULL,
      .map_group=2,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r->projection=&projection;r->capture=&capture;
  for(unsigned angle=0;angle<2;++angle)for(unsigned policy=0;policy<4;++policy) {
    const bool high=policy==1,native=policy!=2;
    /* Marahna puts both BG1 and native sprites on the same additive source.
     * Priority must hold inside that target as well as the main output. */
    scene.additive_plane_mask=angle?
        (1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Far)|
        (1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Obj2):0;
    view.camera.tilt_y=angle?-.4f:.4f;
    memset(voxel->pass_counts,0,sizeof(voxel->pass_counts));
    const bool mixed=policy==3;
    voxel->edge_count=path==2?0:4+mixed;voxel->pass_counts[high]=voxel->edge_count;
    if(high&&path!=2){voxel->pass_counts[3]=1;voxel->pass_edges[3][0]=0;}
    for(unsigned i=0;i<4;++i) {
      VoxelEdge *e=voxel->edges+i;*e=outline[i];e->high_priority=high;e->native_actor_priority=native;
      e->surface_texture=kVoxelSurfaceTexturePatch;e->top_owned_art=1;e->top_sample_size=16;
      voxel->pass_edges[high][i]=i;
    }
    if(mixed && path!=2) {
      /* Both halves of the same walking cap retain their native-low order
       * when only one of the adjacent materials carries the old override. */
      voxel->edges[0].length=8;
      voxel->edges[4]=voxel->edges[0];
      voxel->edges[4].x+=8;
      voxel->edges[4].native_actor_priority=false;
      voxel->pass_edges[0][4]=4;
    }
    voxel->authored_count=path?1:0;voxel->native_transform_count=path==1;
    VoxelAuthoredCell *a=voxel->authored;
    *a=(VoxelAuthoredCell){.x=80,.y=96,.width=path==2?12:16,.native_transform=path==1,.surface=path==2,
        .faces=kDioramaVoxelFaceMask_All,.contour_count=path==2?4:0,
        .transform=path==1?(DioramaVoxelTransform){.px=88,.py=100,.pz=.12f,.dx=2,.dy=2,.sx=.85f,.sy=1,.sz=1}:(DioramaVoxelTransform){0}};
    for(unsigned q=0;q<4;++q) {
      a->back[q]=ActionVoxel_Depth(voxel);a->priority[q]=high;a->native_actor_priority[q]=native&&(!mixed||!(q&1));
    }
    for(unsigned art=0;art<7;++art)a->pixels[art]=voxel->authored_art[0];
    voxel->authored_at[6*(voxel->width/16)+5]=path?1:0;
    voxel->authored_contour_count=voxel->authored_contour_capacity=path==2?4:0;
    if(path==2)for(unsigned i=0;i<4;++i) {
      voxel->authored_contours[i]=(VoxelAuthoredContour){.x=outline[i].x-80,.y=outline[i].y-96,
          .length=outline[i].length,.side=outline[i].side};
      a->surface_sample_size[(unsigned[]){1,5,6,4}[i]]=16;
    }
    if(mixed && path==2) {
      voxel->authored_contours[0].length=8;
      voxel->authored_contours[2]=(VoxelAuthoredContour){.x=8,.y=0,.length=8,.side=0};
    }
    ++voxel->revision;probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));
    r->actor_seen=false;test.count=0;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&r->actor_seen&&probe.count);
    unsigned before=0,after=0,top=0,side=0;
    for(unsigned f=0;f<probe.count;++f) {
      assert(probe.submitted[f]);
      if(probe.after_actor[f])++after;else ++before;
      const int kind=probe.faces[f].kind;
      top+=kind==kDioramaVoxelFace_Top;
      side+=kind==kDioramaVoxelFace_Side||kind==kDioramaVoxelFace_Left||kind==kDioramaVoxelFace_Right;
    }
    /* The orbit looks down on the walking cap; normal back-face culling
     * hides its underside. Every face that is actually submitted is checked. */
    assert(top&&side);
    if(high)assert(after);
    else assert(before&&!after);
    NativeActorPixels(&test,&projection,policy);
    if(shared_depth)assert(r->depth_draws&&r->depth_actor_vertices);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(r,textures[i]);
  assert(!r->live);return s_count;
}
