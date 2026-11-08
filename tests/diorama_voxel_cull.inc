/* Actual native caps in both fixed capture guards remain observable, but
 * only viewport-intersecting geometry is submitted. Camera rotation changes
 * this decision without changing the source extent or native cap vertices. */
static void VoxelCullObserve(void *context,const DioramaVoxelPickFace *face) {
  if(face->kind!=kDioramaVoxelFace_Top)return;
  VoxelCullProbe *probe=context;
  assert(probe->count<sizeof(probe->faces)/sizeof(*probe->faces));
  probe->faces[probe->count++]=*face;
}
static void VoxelCullBoundaryCases(void) {
  ArRenderVertex2D v[4]={0};
  const float boxes[][4]={
    {-20,20,-3,40},{963,20,980,40},{20,-20,40,-3},{20,603,40,620},
    {-20,20,-2,40},{962,20,980,40},{20,-20,40,-2},{20,602,40,620},
    {-20,-20,980,620},{20,20,40,40}};
  for(unsigned b=0;b<sizeof(boxes)/sizeof(*boxes);++b) {
    for(unsigned j=0;j<4;++j)v[j].position=(ArRenderPointF){boxes[b][j==1||j==2?2:0],boxes[b][j>=2?3:1]};
    assert(DioramaVoxelQuadOutsideViewport(v,960,600)==(b<4));
  }
  /* Opposite outside corners enclose the viewport; no individual vertex
   * need be inside it. Invalid/near-plane inputs must fail open. */
  v[0].position=(ArRenderPointF){-10,300};v[1].position=(ArRenderPointF){480,-10};
  v[2].position=(ArRenderPointF){970,300};v[3].position=(ArRenderPointF){480,610};
  assert(!DioramaVoxelQuadOutsideViewport(v,960,600));
  for(unsigned j=0;j<4;++j)v[j].position=(ArRenderPointF){-50,20};
  v[0].position.y=NAN;assert(!DioramaVoxelQuadOutsideViewport(v,960,600));
  v[0].position.y=20;assert(!DioramaVoxelQuadOutsideViewport(v,0,600));
}
static unsigned VoxelCullFixture(unsigned scenario) {
  s_count=0;VoxelCullBoundaryCases();
  VoxelCullProbe probe={0};
  Recorder r={.width=960,.height=600,.bound_effect=-1,.voxel_cull=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=512;voxel->ready=true;
  const unsigned xs[]={208,240,272,496,752,784,808};
  voxel->edge_count=voxel->pass_counts[0]=sizeof(xs)/sizeof(*xs);
  for(unsigned i=0;i<voxel->edge_count;++i) {
    voxel->edges[i]=(VoxelEdge){.x=xs[i],.y=128,.length=8,.side=0,.far_rank=2,.colliding=1};
    voxel->pass_edges[0][i]=i;
  }
  const DioramaCapture capture={.width=496,.height=224,.obj_apron=64,.bg_apron_mask=1,
      .bg1_camera_x=384,.textures=textures,.pixels=pixels};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=VoxelCullObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=scenario>=2?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=496,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=scenario&1?.4f:-.4f}};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<2;++frame) {
    memset(&probe,0,sizeof(probe));
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    unsigned culled=0,drawn=0;assert(probe.count>=voxel->edge_count);
    for(unsigned i=0;i<probe.count;++i) {
      ArRenderVertex2D v[4]={0};
      for(unsigned j=0;j<4;++j)v[j].position=(ArRenderPointF){probe.faces[i].screen_xy[j*2],probe.faces[i].screen_xy[j*2+1]};
      const bool outside=DioramaVoxelQuadOutsideViewport(v,960,600);
      assert(probe.submitted[i]!=outside);
      if(outside)++culled;else ++drawn;
    }
    assert(culled && drawn);
    if(!frame)printf("Voxel guard cull case %u: %u observed, %u submitted, %u offscreen\n",
        scenario,probe.count,drawn,culled);
    assert(capture.width==496 && capture.obj_apron==64 && capture.bg_apron_mask==1);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
