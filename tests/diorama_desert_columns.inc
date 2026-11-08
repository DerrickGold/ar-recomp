typedef struct DesertFrontProbe {
  Recorder *r;const DioramaCapture *capture;const DioramaVoxelGuardView *guard;
  ArRenderTexture far;uint64_t hash;double area;unsigned triangles,guard_draws,curve_faces;
} DesertFrontProbe;
static DesertFrontProbe *s_desert_front_probe;
static bool DesertFrontTexture(void *ctx,ArRenderTexture texture,const ArRenderRectF *src,
    const ArRenderRectF *dst,const ArRenderDrawState *state) {
  Recorder *r=ctx;
  if(r->state.target.value)r->copied_sources[r->state.target.value]=r->copied_sources[texture.value].value?
      r->copied_sources[texture.value]:texture;
  return Texture(ctx,texture,src,dst,state);
}
static void DesertFrontRecord(ArRenderTexture texture,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const float *depths) {
  (void)nv;DesertFrontProbe *p=s_desert_front_probe;Recorder *r=p->r;
  if(r->state.target.value)return;
  const ArRenderTexture source=r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  const bool guard=p->guard&&source.value==p->guard->textures[2].value;
  if(source.value!=p->far.value&&!guard)return;
  if(guard)++p->guard_draws;
  const float width=texture.value==source.value?640:(p->capture->width+2*p->capture->obj_apron);
  const float height=guard?80:texture.value==source.value?352:p->capture->height;
  for(int i=0;i<ni;i+=3) {
    ++p->triangles;
    ArRenderPointF uv[3];
    for(unsigned j=0;j<3;++j) {
      const int at=indices[i+j];const ArRenderVertex2D *q=v+at;
      const float data[]={q->position.x,q->position.y,q->tex_coord.x,q->tex_coord.y,
        q->color.r,q->color.g,q->color.b,q->color.a,depths?depths[at]:0,(float)guard};
      const unsigned char *bytes=(const unsigned char *)data;
      for(unsigned b=0;b<sizeof(data);++b){p->hash^=bytes[b];p->hash*=UINT64_C(1099511628211);}
      uv[j]=(ArRenderPointF){q->tex_coord.x*width,q->tex_coord.y*height};
    }
    p->area+=fabs((double)(uv[1].x-uv[0].x)*(uv[2].y-uv[0].y)-
        (double)(uv[1].y-uv[0].y)*(uv[2].x-uv[0].x))*.5;
  }
}
static bool DesertFrontGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,
    int nv,const int32_t *indices,int ni,const ArRenderDrawState *state) {
  if(!s_depth_fixture)DesertFrontRecord(texture,v,nv,indices,ni,NULL);
  return Geometry(ctx,texture,v,nv,indices,ni,state);
}
static bool DesertFrontDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  DesertFrontRecord(draw->texture,draw->vertices,draw->vertex_count,draw->indices,draw->index_count,draw->depths);
  return DepthGeometry(ctx,device,draw);
}
static void DesertFrontObserve(void *ctx,const DioramaVoxelPickFace *face) {
  DesertFrontProbe *p=ctx;
  if(face->plane==kDioramaPlane_Bg1Far&&face->kind==kDioramaVoxelFace_Front&&
      face->world_xy[0]>=159.9f&&face->world_xy[0]<192&&face->world_xy[1]>=207.9f&&face->world_xy[1]<256)
    ++p->curve_faces;
}
static void DesertFrontFixture(unsigned scenario) {
  s_count=0;s_depth_fixture=!!(scenario&4);
  Recorder r={.width=1280,.height=800,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.draw_texture=DesertFrontTexture;ops.draw_geometry=DesertFrontGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=3;g->active_room=1;g->profile=kActionVoxelProfile_KassandoraDesert;
  g->settings=ActionVoxel_SettingsForRoom(3,1);g->width=1024;g->height=512;g->ready=true;
  g->column_cells=calloc(128,64);assert(g->column_cells);g->column_curves[0]=(VoxelTrunkCurve){.top_y=208,.base_y=216};
  DioramaCapture capture={.width=496,.height=224,.obj_apron=64,.bg_apron_mask=1,
    .bg1_camera_x=184,.camera_y=-1,.textures=textures,.pixels=pixels};
  const ActionRoomSceneFrameState colors={.brightness=15};capture.voxel_frame=&colors;
  DioramaVoxelGuardView guard={.capture_generation=1,.source_revision=ActionVoxel_Revision(g)};
  assert(DioramaVoxelGuard_Resolve(0,0,624,224,(DioramaTrunkGuardBounds){0,0,1024,512},&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=80,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  DioramaProjection projection;DesertFrontProbe probe={.r=&r,.capture=&capture,
    .guard=scenario&2?&guard:NULL,.far=textures[kDioramaPlane_Bg1Far]};
  s_desert_front_probe=&probe;r.capture=&capture;r.projection=&projection;r.actor_texture=textures[kDioramaPlane_Obj2];
  const DioramaView view={.viewport={0,0,1280,800},.visible_width=496,.distance_scale=1,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
    .voxel_observe_face=DesertFrontObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DesertFrontDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.map_group=3,.map_number=1,.voxel_geometry=g,
    .voxel_guard=probe.guard,.depth_sink=s_depth_fixture?&sink:NULL};
  uint64_t hash=0;double area=0;unsigned triangles=0;
  for(unsigned frame=0;frame<3;++frame) {
    g->column_curve_count=frame?1:0;memset(g->column_cells,0,128*64);
    const unsigned x0=frame==2?20:100;
    for(unsigned y=26;y<32;++y)for(unsigned x=x0;x<x0+4;++x)g->column_cells[y*128+x]=1;
    probe.hash=UINT64_C(1469598103934665603);probe.area=0;probe.triangles=probe.guard_draws=probe.curve_faces=0;r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(probe.triangles);if(probe.guard)assert(probe.guard_draws);
    if(!frame){hash=probe.hash;area=probe.area;triangles=probe.triangles;}
    if(frame==1){assert(probe.hash==hash&&probe.triangles==triangles&&probe.area==area);}
    if(frame==2){assert(probe.curve_faces&&probe.hash!=hash);assert(fabs(probe.area-area)<.5);}
  }
  Diorama_ResetCompositorResources(&device);ActionVoxel_Release(g);
  for(unsigned i=0;i<3;++i)Destroy(&r,guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);s_desert_front_probe=NULL;
  printf("Desert local front%u PASS: untouched exact stream; primary/guard finite coverage and curved owner only\n",scenario);
}
