/* Side artwork may remove face-only ink, but must retain the same projected
 * panel coverage and native actor cut. Camera motion must reuse its atlas. */
static double ProfileSurfaceArea(const CapMesh *mesh,bool after_actor) {
  double area=0;
  for(unsigned i=0;i<mesh->count;i+=4)if(mesh->after_actor[i/4]==after_actor) {
    double twice=0;
    for(unsigned j=0;j<4;++j) {
      ArRenderPointF a=mesh->vertices[i+j].position,b=mesh->vertices[i+(j+1)%4].position;
      twice+=(double)a.x*b.y-(double)b.x*a.y;
    }
    area+=fabs(twice)*.5;
  }
  return area;
}
static unsigned VoxelProfileSurfaceFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=4;voxel->active_room=3;voxel->profile=ActionVoxel_ProfileForRoom(4,3);
  voxel->settings=ActionVoxel_SettingsForRoom(4,3);voxel->width=1024;voxel->height=768;voxel->ready=true;
  const bool high=scenario>=2;
  voxel->patch_count=1;voxel->patches[0]=(VoxelPatch){.x=80,.y=96,.width=32,.height=16,
      .kind=kVoxelPatch_SideProfile,.high_priority=high};
  voxel->profile_surfaces=calloc(2,sizeof(*voxel->profile_surfaces));
  voxel->authored_art=calloc(2,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=calloc(2,sizeof(*voxel->authored_cap_art));
  assert(voxel->profile_surfaces && voxel->authored_art && voxel->authored_cap_art);
  voxel->profile_surface_count=voxel->profile_surface_capacity=2;
  voxel->authored_art_count=voxel->authored_art_capacity=2;
  for(unsigned i=0;i<2;++i) {
    voxel->profile_surfaces[i]=(VoxelProfileSurface){.x=80+i*16,.y=96,.art_id=i};
    voxel->authored_cap_art[i]=i;
    for(unsigned p=0;p<256;++p)voxel->authored_art[i][p]=p==3?0:0x8000;
  }
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=scenario&1?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=4,.map_number=3,.voxel_geometry=voxel};
  CapMesh mesh={0},native={0};r.cap_mesh=&mesh;
  r.cap_texture=textures[high?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far];
  r.actor_texture=textures[kDioramaPlane_Obj2];r.capture=&capture;
  DioramaProjection projection;r.projection=&projection;
  for(unsigned frame=0;frame<4;++frame) {
    if(frame==1) {
      voxel->patches[0].surface_compiled=1;voxel->patches[0].surface_count=2;
      voxel->authored_cap_art_count=2;++voxel->revision;
      r.owned_cap=true;r.owned_cap_pixel=0xff000000;
    }
    if(frame==2)view.camera.tilt_y+=.03f;
    if(frame==3) {
      /* All source cells now belong to authored geometry. An empty compiled
       * profile must not revive the old captured native side panel. */
      voxel->patches[0].surface_count=0;++voxel->revision;
      r.cap_texture=textures[high?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far];
    }
    const unsigned uploads=r.uploads,creates=r.creates;
    mesh.count=0;r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.actor_seen);
    if(frame==3){assert(!mesh.count);continue;}
    assert(mesh.count);
    if(!frame){native=mesh;continue;}
    for(unsigned i=0;i<mesh.count;++i) {
      const ArRenderPointF uv=mesh.vertices[i].tex_coord;
      assert(uv.x*512>=.49f && uv.x*512<=31.51f);
      assert(uv.y*2048>=.49f && uv.y*2048<=15.51f);
    }
    if(frame==1)for(unsigned stage=0;stage<2;++stage) {
      const double expected=ProfileSurfaceArea(&native,stage),actual=ProfileSurfaceArea(&mesh,stage);
      assert(fabs(actual-expected)<fmax(.02,expected*.0001));
    }
    if(frame==2)assert(r.uploads==uploads && r.creates==creates);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
