/* Aitos native continuation and procedural effects share finite coverage.
 * Publication alone changes; the primary image projection must remain exact. */
static unsigned WaterfallEffectBoundsFixture(unsigned scenario) {
  assert(scenario<8);s_count=0;s_depth_fixture=scenario>=4;
  const unsigned room=2+((scenario>>1)&1);
  Recorder r={.width=1280,.height=800,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=4;voxel->active_room=room;voxel->profile=kActionVoxelProfile_AitosMountains;
  voxel->settings=ActionVoxel_SettingsForRoom(4,room);voxel->width=1024;voxel->height=512;voxel->ready=true;
  const DioramaCapture capture={.width=496,.height=352,.authentic_y0=64,
    .obj_apron=64,.bg_apron_mask=1,.bg1_camera_x=72,.camera_y=16,.textures=textures,.pixels=pixels};
  const DioramaCapture original_capture=capture;
  DioramaVoxelGuardView guard={.capture_generation=1,.source_revision=ActionVoxel_Revision(voxel)};
  assert(DioramaVoxelGuard_Resolve(-112,-47,624,352,
      (DioramaTrunkGuardBounds){-192,-80,896,512},&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=80,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  const DioramaVoxelGuardView original_guard=guard;
  const DioramaView view={.viewport={0,2,1280,796},.visible_width=360,.visible_height=224,
    .distance_scale=1,.camera_framing_weight=1,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  DioramaScene scene={.render=&options,.map_group=4,.map_number=room,
    .voxel_geometry=voxel,.depth_sink=s_depth_fixture?&sink:NULL};
  DioramaProjection reference,projection;
  r.capture=&capture;r.projection=&projection;
  for(unsigned frame=0;frame<14;++frame) {
    guard=original_guard;scene.voxel_geometry=voxel;scene.map_number=room;
    voxel->active_room=room;voxel->ready=true;
    voxel->settings=ActionVoxel_SettingsForRoom(4,room);
    unsigned rows=frame==0?64:frame==1?47:0;
    if(frame==1)assert(DioramaVoxelGuard_Resolve(-112,-47,624,352,
      (DioramaTrunkGuardBounds){-192,-80,896,432},&guard.layout));
    if(frame==2)assert(DioramaVoxelGuard_Resolve(-112,-47,624,352,
      (DioramaTrunkGuardBounds){-192,-80,896,385},&guard.layout)); /* finite empty EU */
    if(frame==4)--guard.source_revision;
    if(frame==5)++guard.layout.lower.world_x;
    if(frame==6)guard.capture_generation=0;
    if(frame==7)guard.textures[1]=ArRenderTexture_Invalid();
    if(frame==8)assert(DioramaVoxelGuard_Resolve(-112,-47,624,352,
      (DioramaTrunkGuardBounds){-111,-80,895,512},&guard.layout)); /* narrower lower rectangle */
    if(frame==9)assert(DioramaVoxelGuard_Resolve(-112,-47,624,352,
      (DioramaTrunkGuardBounds){-192,309,896,123},&guard.layout)); /* disconnected lower strip */
    if(frame==10)scene.voxel_geometry=NULL; /* ordinary Diorama */
    if(frame==11)voxel->ready=false;
    if(frame==12)scene.map_number=voxel->active_room=1; /* other Aitos profile */
    if(frame==13)voxel->settings.depth=0; /* native guard drawing is disabled */
    scene.voxel_guard=NULL;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    reference=projection;
    scene.voxel_guard=frame==3?NULL:&guard;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(!memcmp(&capture,&original_capture,sizeof(capture)));
    assert(projection.valid&&projection.bg1_plane.valid);
    float x0,y0,x1,y1;
    assert(Diorama_PlaneCaptureBounds(&projection,&projection.bg1_plane,&x0,&y0,&x1,&y1));
    assert(y0==0&&fabsf(y1-(352+rows))<.0001f);
    assert(x0==-64&&x1==560); /* ordinary window plus actual captured apron */
    assert(projection.bg1_plane.u0==reference.bg1_plane.u0&&
        projection.bg1_plane.v0==reference.bg1_plane.v0&&
        projection.bg1_plane.u1==reference.bg1_plane.u1&&
        projection.bg1_plane.v1==reference.bg1_plane.v1);
    /* Mist, spray and particles all consume this published plane. Every
     * point keeps its native transform, including admitted continuation. */
    const float points[][2]={{40,100},{248,351},{72,370},{176,380},{420,398}};
    for(unsigned i=0;i<sizeof(points)/sizeof(points[0]);++i) {
      ArRenderPointF a,b;
      assert(Diorama_ProjectCapturedBg1Point(&reference,points[i][0],points[i][1],&a,NULL,NULL));
      assert(Diorama_ProjectCapturedBg1Point(&projection,points[i][0],points[i][1],&b,NULL,NULL));
      assert(a.x==b.x&&a.y==b.y);
      assert((points[i][1]<=y1)==(points[i][1]<=352+rows));
    }
    /* Native planes, actor ordering, camera and all other effect bounds are
     * byte-identical. Only the explicit effect-clip endpoint may differ. */
    DioramaProjection same=projection;
    same.bg1_plane.source_v1=reference.bg1_plane.source_v1;
    assert(!memcmp(&same,&reference,sizeof(same)));
    Record((float)rows);Record(y1);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<3;++i)Destroy(&r,original_guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);
  return s_count;
}
