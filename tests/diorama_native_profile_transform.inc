/* An affine depth edit must retain the original full ornamental side image,
 * rather than replacing it with per-tile extrusion strips. Also exercise the
 * existing clean side images that omit face-only foam on wet rock profiles. */
static unsigned NativeProfileTransformFixture(unsigned scenario) {
  s_count=0;static NativeAffineProbe probe;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=3;voxel->active_room=5;voxel->profile=kActionVoxelProfile_KassandoraTomb;
  voxel->settings=ActionVoxel_SettingsForRoom(3,5);voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->authored=calloc(4,sizeof(*voxel->authored));
  voxel->authored_at=calloc(16*14,sizeof(*voxel->authored_at));
  voxel->authored_art=calloc(8,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=malloc(4*sizeof(*voxel->authored_cap_art));
  voxel->profile_surfaces=calloc(4,sizeof(*voxel->profile_surfaces));
  assert(voxel->authored&&voxel->authored_at&&voxel->authored_art&&voxel->authored_cap_art&&voxel->profile_surfaces);
  voxel->authored_count=voxel->native_transform_count=voxel->hidden_native_front_count=4;
  voxel->authored_art_count=voxel->authored_art_capacity=8;
  const bool clean=scenario&1;
  voxel->profile_surface_count=voxel->profile_surface_capacity=clean?4:0;
  voxel->authored_cap_art_count=clean?4:0;
  voxel->patch_count=1;voxel->patches[0]=(VoxelPatch){.x=80,.y=96,.width=32,.height=32,
      .kind=kVoxelPatch_SideProfile,.surface_compiled=clean,.surface_count=clean?4:0};
  for(unsigned i=0;i<4;++i) {
    VoxelAuthoredCell *a=&voxel->authored[i];
    *a=(VoxelAuthoredCell){.x=80+(i&1)*16,.y=96+(i>>1)*16,.width=16,
        .faces=kDioramaVoxelFaceMask_All,.native_transform=true,.native_front_hidden=true,.native_front_art=true};
    voxel->authored_at[(a->y/16)*16+a->x/16]=i+1;
    for(unsigned f=0;f<7;++f){a->art_id[f]=i;a->pixels[f]=voxel->authored_art[i];}
    voxel->profile_surfaces[i]=(VoxelProfileSurface){a->x,a->y,i+4};voxel->authored_cap_art[i]=i+4;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      voxel->authored_art[i][y*16+x]=(uint16_t)(0x8000|x|(y<<5)|((i+1)<<10));
      voxel->authored_art[i+4][y*16+x]=(uint16_t)(0x8000|x|(y<<5)|((i+5)<<10));
    }
  }
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.2f,.tilt_y=scenario&2?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=3,.map_number=5,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<2;++frame) {
    const DioramaVoxelTransform transform={.px=96,.py=112,.pz=.24f,
        .dx=frame?12:0,.dy=frame?-8:0,.sx=frame?.8f:1,.sy=frame?1.1f:1,.sz=.6875f};
    for(unsigned i=0;i<4;++i)voxel->authored[i].transform=transform;
    ReviseVoxelFixture(voxel);probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    unsigned panels=0,ends=0;
    for(unsigned i=0;i<probe.count;++i) {
      const DioramaVoxelPickFace *face=&probe.faces[i];if(face->kind!=kDioramaVoxelFace_Side)continue;
      assert(face->authored&&probe.submitted[i]&&!probe.after_actor[i]);++panels;
      const unsigned cell=(face->cell_y-6)*2+face->cell_x-5;assert(cell<4);
      const VoxelAuthoredCell *a=&voxel->authored[cell];const unsigned art=cell+(clean?4:0);
      assert(probe.face_textures[i].value==probe.texture.value);
      for(unsigned j=0;j<4;++j) {
        const float sx=face->world_xy[j*2],sy=face->world_xy[j*2+1];
        const float sample_x=a->x+face->art_uv[j*2],sample_y=a->y+face->art_uv[j*2+1];
        const float inset=clean?0:.5f;
        float raw=(sample_x-80-inset)/(32-2*inset);
        assert(fabsf(sx-80)<.001f||fabsf(sx-112)<.001f);if(sx>96)raw=1-raw;
        raw*=.24f;assert(raw>=-.00001f&&raw<=.24001f);
        const float z=.24f+(raw-.24f)*.6875f;
        const float x=transform.px+(sx-transform.px)*transform.sx+transform.dx;
        const float y=transform.py+(sy-transform.py)*transform.sy+transform.dy;
        assert(fabsf(NativeAffineDepth(&projection,&capture,x,y,face->clip_w[j])-z)<.00001f);
        const ArRenderPointF expected=RootExpectedPoint(&projection,&capture,x,y,z);
        assert(CapPointEqual(expected,(ArRenderPointF){face->screen_xy[j*2],face->screen_xy[j*2+1]}));
        assert(fabsf(sample_y-(96+inset+(32-2*inset)*(sy-96)/32))<.001f);
        const ArRenderPointF uv=probe.vertices[i][j].tex_coord;
        assert(fabsf(uv.x*512-art*16-fmaxf(.5f,fminf(15.5f,face->art_uv[j*2])))<.002f);
        assert(fabsf(uv.y*2048-fmaxf(.5f,fminf(15.5f,face->art_uv[j*2+1])))<.002f);
        ends+=raw<.00001f||raw>.23999f;
      }
    }
    assert(panels==8&&ends);
    /* All pixels of the complete original side image survive the scale. */
    for(unsigned i=0;i<4;++i)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned id=i+(clean?4:0);
      assert(probe.atlas[y*512+id*16+x]==ActionVoxel_AuthoredColor(voxel->authored_art[id][y*16+x],NULL));
    }
    const unsigned creates=r.creates,uploads=r.uploads;
    probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(r.creates==creates&&r.uploads==uploads);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  return s_count;
}
