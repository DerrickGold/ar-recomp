/* Validate actual submitted vertices: a mixed rock/sand source must not
 * move its sand with the column, including through the depth submission. */
static unsigned MaskedColumnFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=3;voxel->active_room=1;voxel->profile=kActionVoxelProfile_KassandoraDesert;
  voxel->settings=ActionVoxel_SettingsForRoom(3,1);voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->column_cells=calloc(32,28);voxel->pixel_recess_cells=calloc(32,28);
  assert(voxel->column_cells && voxel->pixel_recess_cells);
  voxel->column_curve_count=1;voxel->column_curves[0]=(VoxelTrunkCurve){.top_y=64,.base_y=72};
  voxel->column_cells[12*32+10]=voxel->pixel_recess_cells[12*32+10]=1;
  voxel->pixel_recess_mask_count=voxel->pixel_recess_cell_count=1;
  for(unsigned y=0;y<8;++y)voxel->pixel_recess_rows[0][y]=0xf0;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.distance=3.25f,.tilt_x=.15f,.tilt_y=scenario?-.4f:.4f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=scenario?&sink:NULL,
      .map_group=3,.map_number=1,.voxel_geometry=voxel};
  r.root_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];r.capture=&capture;
  for(unsigned frame=0;frame<2;++frame) {
    RootMesh mesh={0};r.root_mesh=&mesh;r.actor_seen=false;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    unsigned sand=0,rock=0;
    for(unsigned i=0;i<mesh.count;i+=4) {
      const ArRenderVertex2D *v=mesh.vertices+i;
      float x=0,y=0;
      for(unsigned j=0;j<4;++j){x+=v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH*.25f;y+=v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT*.25f+1*.25f;}
      if(x<80 || x>=88 || y<96 || y>=104)continue;
      const float depth=x<84?.08f:0;
      if(depth)++rock;else ++sand;
      for(unsigned j=0;j<4;++j) {
        const ArRenderPointF p=RootExpectedPoint(&projection,&capture,
            v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH,v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+1,depth);
        assert(CapPointEqual(p,v[j].position));
      }
    }
    assert(sand==1 && rock==1);
  }
  if(scenario)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
