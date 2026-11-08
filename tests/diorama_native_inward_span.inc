/* Actual submitted atlas UVs, independently recovered from projected depth.
 * Twelve cases: Temple foreground / partial-depth Cavern shelf / curved
 * column, both camera signs, painter and shared-depth paths. */
static unsigned NativeInwardSpanFixture(unsigned scenario) {
  const unsigned family=(scenario%6)/2;
  const bool depth_sink=scenario>=6,negative=scenario&1;
  s_count=0;static NativeAffineProbe probe,plain;
  memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[family==0?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far];
  probe.rear_texture=textures[SR_PPU_OVERLAY_BG1];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=family==0?5:family==1?4:2;
  voxel->active_room=family==2?2:4;
  voxel->profile=ActionVoxel_ProfileForRoom(voxel->active_group,voxel->active_room);
  voxel->settings=(ActionVoxelSettings){.depth=.24f,.play_recess=.08f};
  voxel->width=1024;voxel->height=768;voxel->ready=true;
  if(family==2) {
    voxel->column_curve_count=1;
    voxel->column_curves[0]=(VoxelTrunkCurve){.top_y=64,.base_y=160};
  }
  voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=calloc(1,sizeof(*voxel->authored_cap_art));
  assert(voxel->authored_art&&voxel->authored_cap_art);
  voxel->authored_art_count=voxel->authored_art_capacity=1;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    voxel->authored_art[0][y*16+x]=NativeAffineColor(x,y);
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.2f,.tilt_y=negative?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=depth_sink?&sink:NULL,
      .map_group=voxel->active_group,.map_number=voxel->active_room,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  unsigned all_cut_joins=0,curved_y_samples=0,collapsed_endpoints=0;
  for(unsigned frame=0;frame<(family==2?7u:5u);++frame) {
    const bool conventional=frame==3&&family!=0;
    const unsigned mode=(!frame||frame==5)?0:conventional?1:kVoxelSurfaceTextureNativeInward;
    voxel->settings.play_recess=frame==4?.215f:.08f;
    /* At y112 the original curve interval is exactly empty. Its vertical
     * face still has a real triangle nearer y96; mode2 must keep it. Compare
     * a fresh raw-source frame before the optional-art frame, not old poses. */
    if(family==2)voxel->column_curves[0].base_y=frame>=5?112:160;
    if(frame!=2)++voxel->revision;
    voxel->authored_cap_art_count=mode?1:0;
    memset(voxel->pass_counts,0,sizeof(voxel->pass_counts));voxel->edge_count=0;
    for(unsigned side=family==0?1:0;side<4;++side) {
      const unsigned i=voxel->edge_count++,high=family==0;
      voxel->pass_edges[high][voxel->pass_counts[high]++]=i;
      voxel->edges[i]=(VoxelEdge){.x=side==1?95:80,.y=side==2?111:96,.length=16,
          .side=side,.far_rank=family==0?1:2,.near_rank=family==1?1:0,
          .high_priority=high,.colliding=family!=0,.column_curve=family==2?1:0,
          .surface_texture=mode,.surface_derived=mode==kVoxelSurfaceTextureNativeInward,
          .top_sample_x=80,.top_sample_y=96,.top_sample_size=mode?16:0,.top_owned_art=mode?1:0};
    }
    probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    const unsigned creates=r.creates,uploads=r.uploads;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.count&&r.actor_seen);
    for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!mode){plain=probe;continue;}
    assert(probe.uploaded);
    if(frame==2)assert(r.creates==creates&&r.uploads==uploads);
    if(frame<4||frame==6)assert(probe.count==plain.count);
    unsigned near_endpoints=0,far_endpoints=0;
    for(unsigned f=0;f<probe.count;++f) {
      const DioramaVoxelPickFace *face=&probe.faces[f];
      assert(probe.face_textures[f].value==probe.texture.value);
      const bool vertical=fabsf(face->world_xy[0]-face->world_xy[2])<.001f;
      const bool reverse=vertical?roundf(face->world_xy[0])==96:roundf(face->world_xy[1])==112;
      for(unsigned j=0;j<4;++j) {
        const ArRenderVertex2D *v=&probe.vertices[f][j];
        const float x=roundf(face->world_xy[j*2]),y=roundf(face->world_xy[j*2+1]);
        const float z=NativeAffineDepth(&projection,&capture,x,y,face->clip_w[j]);
        /* Literal physical contracts, not the renderer's span helper. */
        float near=family==0?.18f:.06f;
        if(family==2) {
          const float t=fmaxf(0,fminf(1,(y-64)/(frame>=5?48:96)));
          near=.24f*t*t*(3-2*t);++curved_y_samples;
        }
        assert(z>=-.0001f&&z<=.2401f);
        if(z<near-.0001f) {
          /* Actor clipping can collapse one tapered endpoint at the literal
           * actor boundary while its other endpoint still has area. That
           * endpoint is preserved in the raw geometry and uses front ink. */
          assert(family==2&&fabsf(z-(frame==4?.205f:.07f))<.0001f);
          const unsigned partner=3-j;
          assert(CapPointEqual(v->position,probe.vertices[f][partner].position));
        }
        const bool collapsed=near==.24f;
        const float fraction=conventional?z/.24f:collapsed?0:
            fmaxf(0,fminf(1,(z-near)/(.24f-near)));
        if(collapsed)++collapsed_endpoints;
        const float across=reverse?1-fraction:fraction;
        const float along=(vertical?y-96:x-80)/16;
        assert(fabsf(v->tex_coord.x*512-(.5f+15*(vertical?across:along)))<.003f);
        assert(fabsf(v->tex_coord.y*2048-(.5f+15*(vertical?along:across)))<.003f);
        if(fabsf(z-near)<.0001f)++near_endpoints;
        if(fabsf(z-.24f)<.0001f)++far_endpoints;
        if(frame<4||frame==6) {
          assert(CapPointEqual(v->position,plain.vertices[f][j].position));
          assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));
          assert(probe.after_actor[f]==plain.after_actor[f]);
        }
        for(unsigned other=0;other<f;++other)if(probe.after_actor[other]!=probe.after_actor[f]&&
            probe.faces[other].kind==face->kind)
          for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[other][k].position)) {
            assert(CapUvEqual(v->tex_coord,probe.vertices[other][k].tex_coord));++all_cut_joins;
          }
      }
    }
    assert(near_endpoints&&far_endpoints);
  }
  if(family==1)assert(all_cut_joins);
  if(family==2)assert(curved_y_samples&&collapsed_endpoints);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
