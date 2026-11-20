/* Surface finishes stay on their receiving native cap and obey the ordinary
 * lighting/particle toggles without a copied screen-space sparkle layer. */
typedef struct IceFinishProbe { Recorder *recorder;unsigned quads;uint64_t hash; } IceFinishProbe;
static bool IceFinishDepth(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  IceFinishProbe *p=context;
  if(draw->read_only_depth) {
    assert(!draw->native_actor && draw->blend==kArRenderBlendMode_Alpha && draw->vertex_count%4==0);
    p->quads+=draw->vertex_count/4;assert(p->quads<=4096);
    assert(draw->plane==kDioramaPlane_Bg1Far && !p->recorder->actor_seen);
    for(int i=0;i<draw->vertex_count;++i) {
      const ArRenderVertex2D *v=draw->vertices+i;float xyz[3];
      GroundCoverNativePoint(p->recorder,v,draw->depths[i],draw->clip_w[i],xyz);
      assert(xyz[0]>=79.99f && xyz[0]<=208.01f && xyz[2]>=-.0001f && xyz[2]<=.2201f);
      assert(fabsf(xyz[1]-175.75f)<.01f);
      assert(v->color.a>=0 && v->color.a<=1);
      const unsigned char *bytes=(const unsigned char *)v;
      for(unsigned b=0;b<sizeof(*v);++b)p->hash=(p->hash^bytes[b])*UINT64_C(1099511628211);
    }
    ++p->recorder->depth_draws;p->recorder->depth_vertices+=draw->vertex_count;
    return true;
  }
  return DepthGeometry(p->recorder,device,draw);
}
static unsigned IceFinishFixture(unsigned angle) {
  s_count=0;Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=6;g->active_room=3;g->profile=kActionVoxelProfile_NorthwallIce;
  g->settings=(ActionVoxelSettings){.depth=.22f,.play_recess=.075f};g->width=g->height=256;g->ready=true;
  g->edge_count=2;g->pass_counts[0]=2;
  for(unsigned i=0;i<2;++i) {
    g->edges[i]=(VoxelEdge){.x=80+64*i,.y=176,.length=64,.side=0,.far_rank=2,.colliding=1,
      .surface_finish=kVoxelSurfaceFinishIce};g->pass_edges[0][i]=i;
  }
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.5f,.tilt_y=angle?-.4f:.4f,.distance=3.25f}};
  static ActionSceneEffectFrame frame;frame=(ActionSceneEffectFrame){.game_frame=320};
  IceFinishProbe probe={.recorder=&r};const DioramaDepthSink sink={.submit=IceFinishDepth,.userdata=&probe};
  DioramaScene scene={.render=&options,.map_group=6,.map_number=3,.voxel_geometry=g,
    .depth_sink=&sink,.trunk_effects=&frame,.trunk_lighting=true,.trunk_particles=true};
  uint64_t paused=0,animated=0;
  for(unsigned state=0;state<8;++state) {
    frame.game_frame=state==2?400:320;
    scene.trunk_lighting=state!=4;scene.trunk_particles=state!=3;
    for(unsigned i=0;i<2;++i)g->edges[i].surface_finish=state==5?kVoxelSurfaceFinishNone:kVoxelSurfaceFinishIce;
    options.voxel_inspection=state==6?kDioramaVoxelInspect_Backing:0;
    view.camera.tilt_x=state==7?-.5f:.5f;
    if(!state || state>=5)ReviseVoxelFixture(g);
    r.actor_seen=false;probe.quads=0;probe.hash=UINT64_C(14695981039346656037);
    DioramaProjection projection;r.projection=&projection;r.capture=&capture;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if(state>=4)assert(!probe.quads);
    else {
      assert(probe.quads>=6);
      if(!state)paused=probe.hash;
      else if(state==1)assert(probe.hash==paused);
      else if(state==2){animated=probe.hash;assert(animated!=paused);}
    }
  }
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
