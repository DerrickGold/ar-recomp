/* Verified material skins change only the source artwork on an existing
 * contour. Exercise real submitted UVs and the production actor-band cuts. */
static unsigned VoxelMaterialSkinFixture(unsigned scenario) {
  const bool riser_skin=scenario>=16;
  const unsigned sample_size=scenario>=8?16:8;scenario%=8;
  const unsigned riser_side=(scenario&1)?3:1;
  s_count=0;static NativeAffineProbe probe,plain,skinned;
  memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  const unsigned groups[]={1,2,3,6},rooms[]={1,2,1,1},profile=scenario/2;
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=groups[profile];voxel->active_room=rooms[profile];
  voxel->profile=ActionVoxel_ProfileForRoom(voxel->active_group,voxel->active_room);
  voxel->settings=ActionVoxel_SettingsForRoom(voxel->active_group,voxel->active_room);
  voxel->width=1024;voxel->height=768;voxel->ready=true;
  const VoxelEdge outline[]={
    {.x=80,.y=96,.length=8,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=80+sample_size-1,.y=96,.length=riser_skin&&riser_side==1?1:2,.side=1,.far_rank=2,.colliding=1,.fold_origin_y=96,.fold_length=riser_skin&&riser_side==1?1:8},
    {.x=80,.y=103,.length=8,.side=2,.far_rank=2,.colliding=1},
    {.x=80,.y=96,.length=riser_skin&&riser_side==3?1:8,.side=3,.far_rank=2,.colliding=1},
    /* A sixteen-pixel motif spans two compiler edges. Its second half
     * must continue the first half's phase, including at actor cuts. */
    {.x=88,.y=96,.length=8,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=88,.y=103,.length=8,.side=2,.far_rank=2,.colliding=1},
  };
  const unsigned edge_count=sample_size==16?6:4;
  voxel->edge_count=voxel->pass_counts[0]=edge_count;
  for(unsigned i=0;i<edge_count;++i)voxel->pass_edges[0][i]=i;
  voxel->authored=calloc(1,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)voxel->width/16,voxel->height/16*sizeof(*voxel->authored_at));
  voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=malloc(sizeof(*voxel->authored_cap_art));
  assert(voxel->authored && voxel->authored_at && voxel->authored_art && voxel->authored_cap_art);
  voxel->authored_art_count=voxel->authored_art_capacity=1;voxel->authored_cap_art[0]=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)voxel->authored_art[0][y*16+x]=NativeAffineColor(x,y);
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=riser_skin?.2f:(scenario&1)?-.2f:.2f,.tilt_y=(scenario&1)?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=groups[profile],.map_number=rooms[profile],.voxel_geometry=voxel,
      .bg1_dimming=.4f,.bg1_dimming_ramp={0,0,.8f,.9f}};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<5;++frame) {
    const int shift=frame>=3?256:0;
    const DioramaVoxelTransform transform={.px=84+shift,.py=100,.pz=.09f,
        .dx=frame>=3?-256:0,.dy=frame>=3?-5:0,.sx=frame>=3?.75f:1,.sy=1,.sz=1};
    if(frame!=2 && frame!=4)++voxel->revision;
    capture.bg1_camera_x=frame==2?3:0;
    voxel->authored_cap_art_count=frame?1:0;
    voxel->authored_count=voxel->native_transform_count=frame>=3?1:0;
    if(frame>=3) {
      VoxelAuthoredCell *a=voxel->authored;
      *a=(VoxelAuthoredCell){.x=80+shift,.y=96,.width=16,.native_transform=true,.transform=transform,
          .faces=kDioramaVoxelFaceMask_Top|kDioramaVoxelFaceMask_Right|kDioramaVoxelFaceMask_Bottom|kDioramaVoxelFaceMask_Left};
      for(unsigned art=0;art<7;++art)a->pixels[art]=voxel->authored_art[0];
      voxel->authored_at[(size_t)(a->y/16)*(voxel->width/16)+a->x/16]=1;
    }
    for(unsigned i=0;i<edge_count;++i) {
      VoxelEdge e=outline[i];e.x+=shift;
      /* A two-pixel material remainder of a long desert cliff is still a
       * vertical wall, not a newly classified soft stair riser. */
      e.soft_surface=(riser_skin && e.side==riser_side) || (frame && profile==2 && e.side==1);
      e.surface_top_material=frame && riser_skin && e.side==riser_side;
      if(frame) {e.surface_texture=1;e.top_sample_x=900;e.top_sample_y=500;e.top_sample_size=sample_size;e.top_owned_art=1;}
      voxel->edges[i]=e;
    }
    probe.count=0;probe.source_shift=shift;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    const unsigned creates=r.creates,uploads=r.uploads;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && probe.count && r.actor_seen);
    for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!frame){plain=probe;continue;}
    assert(probe.uploaded && probe.count==plain.count);
    if(frame==2 || frame==4)assert(r.creates==creates && r.uploads==uploads);
    unsigned before=0,after=0,joins=0,cap_joins=0;
    for(unsigned f=0;f<probe.count;++f) {
      const DioramaVoxelPickFace *face=&probe.faces[f];
      assert(face->art_source_x==900 && face->art_source_y==500 && face->art_source_size==sample_size);
      assert(probe.face_textures[f].value==probe.texture.value);
      assert(probe.after_actor[f]==plain.after_actor[f]);
      if(probe.after_actor[f])++after;else ++before;
      const bool vertical=fabsf(face->world_xy[0]-face->world_xy[2])<.001f;
      const bool right=roundf(face->world_xy[0])==80+sample_size+shift;
      const bool top_riser=riser_skin && vertical && (right==(riser_side==1));
      const bool uv_vertical=vertical && !top_riser;
      const bool reverse=top_riser?false:vertical?right:roundf(face->world_xy[1])==104;
      for(unsigned j=0;j<4;++j) {
        const ArRenderVertex2D *v=&probe.vertices[f][j];
        const float x=roundf(face->world_xy[j*2]),y=roundf(face->world_xy[j*2+1]);
        const float px=transform.px+(x-transform.px)*transform.sx+transform.dx,py=y+transform.dy;
        const float z=NativeAffineDepth(&projection,&capture,px,py,face->clip_w[j]);
        const float along=(uv_vertical?y-96:x-80-shift)/sample_size;
        const float depth=z/ActionVoxel_Depth(voxel),across=reverse?1-depth:depth;
        assert(fabsf(v->tex_coord.x*512-(.5f+(uv_vertical?15*across:(sample_size-1)*along)))<.001f);
        assert(fabsf(v->tex_coord.y*2048-(.5f+(uv_vertical?(sample_size-1)*along:15*across)))<.001f);
        if(frame==1) {
          assert(CapPointEqual(v->position,plain.vertices[f][j].position));
          assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));
        } else assert(CapUvEqual(v->tex_coord,skinned.vertices[f][j].tex_coord));
        for(unsigned other=0;other<f;++other)if(fabsf(z-(ActionVoxel_PlayRecess(voxel)-.01f))<.001f &&
            (fabsf(probe.faces[other].world_xy[0]-probe.faces[other].world_xy[2])<.001f)==vertical)
          for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[other][k].position)) {
            assert(CapUvEqual(v->tex_coord,probe.vertices[other][k].tex_coord));++joins;
          }
        if(riser_skin)for(unsigned other=0;other<f;++other) {
          const DioramaVoxelPickFace *of=&probe.faces[other];
          const bool other_vertical=fabsf(of->world_xy[0]-of->world_xy[2])<.001f;
          const bool other_riser=other_vertical &&
              ((roundf(of->world_xy[0])==80+sample_size+shift)==(riser_side==1));
          if(!((top_riser && of->kind==kDioramaVoxelFace_Top) ||
               (other_riser && face->kind==kDioramaVoxelFace_Top)))continue;
          for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[other][k].position)) {
            assert(CapUvEqual(v->tex_coord,probe.vertices[other][k].tex_coord));++cap_joins;
          }
        }
      }
    }
    assert(before && !after && joins);
    if(riser_skin)assert(cap_joins);
    if(frame==1)skinned=probe;
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}

/* Adjacent native grass/dirt decks share one physical walking span. The
 * complete cap remains flat at its native actor contact, including the rear
 * endpoint; fallback art stays on the untouched front source. */
static unsigned VoxelFlatRearJoinFixture(unsigned scenario) {
  s_count=0;static NativeAffineProbe probe,original;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];probe.rear_texture=textures[SR_PPU_OVERLAY_BG1];
  r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  const VoxelEdge edges[]={
    {.x=80,.y=128,.length=16,.side=0,.far_rank=2,.colliding=1,.textured_top=1,.rear_fold_top=7},
    {.x=96,.y=128,.length=16,.side=0,.far_rank=2,.colliding=1,.rear_fold_top=7},
    {.x=80,.y=128,.length=16,.side=3,.far_rank=2,.colliding=1,.rear_fold_top=7},
    {.x=111,.y=128,.length=16,.side=1,.far_rank=2,.colliding=1,.rear_fold_top=7},
  };
  voxel->edge_count=voxel->pass_counts[0]=4;
  memcpy(voxel->edges,edges,sizeof(edges));for(unsigned i=0;i<4;++i)voxel->pass_edges[0][i]=i;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.5f,.tilt_y=scenario?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  const float depth=ActionVoxel_Depth(voxel),contact=ActionVoxel_PlayRecess(voxel)-.01f;
  const float knots[]={0,contact,depth};
  for(unsigned frame=0;frame<2;++frame) {
    for(unsigned i=0;i<4;++i)voxel->edges[i].flat_rear_join=frame;
    ++voxel->revision;probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && probe.count && r.actor_seen);
    unsigned seam[3]={0},brown_phase=0,retained=0;
    for(unsigned f=0;f<probe.count;++f) {
      assert(probe.submitted[f]);const DioramaVoxelPickFace *face=&probe.faces[f];
      const bool top=face->kind==kDioramaVoxelFace_Top;
      const bool brown=top && (face->world_xy[0]+face->world_xy[2])*.5f>96;
      assert(!probe.after_actor[f]);
      bool near=false;
      for(unsigned j=0;j<4;++j) {
        const float x=roundf(face->world_xy[j*2]),y=roundf(face->world_xy[j*2+1]);
        near|=CapPointEqual(RootExpectedPoint(&projection,&capture,x,y,0),probe.vertices[f][j].position);
      }
      for(unsigned j=0;j<4;++j) {
        const ArRenderVertex2D *v=&probe.vertices[f][j];
        const float x=roundf(face->world_xy[j*2]),y=roundf(face->world_xy[j*2+1]);
        unsigned matched=3;
        for(unsigned k=0;k<3 && matched==3;++k) {
          float rise=frame?0:7*fmaxf(0,fminf(1,(knots[k]-contact)/(depth-contact)));
          if(!top)rise*=(144-y)/16;
          const ArRenderPointF expected=RootExpectedPoint(&projection,&capture,x,y-rise,knots[k]);
          if(CapPointEqual(expected,v->position))matched=k;
        }
        assert(matched<3);
        if(top && x==96)++seam[matched];
        if(brown && !near) {
          const float expected=frame?128.5f-knots[matched]/depth:
              127.5f-7*fmaxf(0,fminf(1,(knots[matched]-contact)/(depth-contact)));
          assert(fabsf(v->tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-expected)<.0001f);
          if(matched==2)++brown_phase;
        }
        if(frame && matched!=2) {
          bool same=false;
          for(unsigned before=0;before<original.count && !same;++before)
            if(original.faces[before].kind==face->kind)
              for(unsigned p=0;p<4 && !same;++p)
                same=fabsf(original.faces[before].world_xy[p*2]-face->world_xy[j*2])<.001f &&
                    fabsf(original.faces[before].world_xy[p*2+1]-face->world_xy[j*2+1])<.001f &&
                    CapPointEqual(original.vertices[before][p].position,v->position) &&
                    (brown || CapUvEqual(original.vertices[before][p].tex_coord,v->tex_coord));
          assert(same);++retained;
        }
      }
    }
    assert(seam[0]>=2 && seam[1]>=4 && seam[2]>=2);
    if(frame)assert(brown_phase>=2 && retained && probe.count==original.count);
    else original=probe;
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
