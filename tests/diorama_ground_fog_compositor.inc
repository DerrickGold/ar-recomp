/* Physical mist must rise through floor hazards without lifting clear-floor
 * mist to ceiling spikes, and native collision variants share one support. */
static unsigned GroundFogFixture(unsigned scenario) {
  s_count=0;const unsigned kind=scenario/2;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.check_fog=true,
      .fog_top=INFINITY,.fog_bottom=-INFINITY,.fog_near=INFINITY,.fog_far=-INFINITY};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=1;voxel->active_room=2;voxel->profile=kActionVoxelProfile_FillmoreCave;
  voxel->settings=ActionVoxel_SettingsForRoom(1,2);voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->edge_count=voxel->pass_counts[0]=1;voxel->pass_edges[0][0]=0;
  voxel->edges[0]=(VoxelEdge){.x=80,.y=120,.length=80,.side=0,.far_rank=2,.colliding=1};
  if(kind) {
    voxel->patch_count=kind==3?1:2;
    voxel->patches[0]=(VoxelPatch){.x=80,.y=kind==3?40:88,.width=80,.height=16,.kind=kVoxelPatch_Spikes};
    voxel->patches[1]=(VoxelPatch){.x=80,.y=104,.width=80,.height=16,.kind=kVoxelPatch_Spikes};
  }
  ReviseVoxelFixture(voxel);
  static ActionSceneEffectFrame effects;memset(&effects,0,sizeof(effects));
  effects.decoration_count=1;effects.atmosphere_field_valid=true;
  effects.decorations[0]=(ActionEffectInstance){.kind=kActionEffect_TempleGroundMist,
      .flags=kActionEffectFlag_Visible,.world_x=80,.world_y=kind==2?104:120,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={0,-26,80,0}}};
  effects.atmosphere_field.MistBillow[0]=.7f;
  effects.atmosphere_field.MistColor[0]=.4f;effects.atmosphere_field.MistColor[1]=.6f;
  effects.atmosphere_field.MistColor[2]=.8f;effects.atmosphere_field.MistDensity[0]=.3f;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.22f,.tilt_y=scenario&1?.42f:-.42f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=&sink,.map_group=1,.map_number=2,
      .voxel_geometry=voxel,.trunk_effects=&effects};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.voxel_fog_claims&kDioramaFog_Floor);
  assert(r.fog_vertices && r.fog_bottom<120.01f && r.fog_top>60);
  if(kind==1 || kind==2)assert(r.fog_top<88); /* Wisps reach above the32px spike bed. */
  else assert(r.fog_top>92); /* Clear floors and detached ceiling strips keep low mist. */
  assert(r.fog_near>=-.001f && r.fog_far<=voxel->settings.depth+.001f && r.fog_far>r.fog_near);
  Diorama_ResetCompositorResources(&device);ActionVoxel_Release(voxel);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  return s_count;
}
