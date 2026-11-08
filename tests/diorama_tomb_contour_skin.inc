/* Include after diorama_surface_texture.inc. Register
 * scenarios0..7 with both s_depth_fixture=false/true (16cases). The numbered
 * native atlas is deliberately nonuniform; assertions read actual submissions. */
static void TombContourObserve(void *context,const DioramaVoxelPickFace *face) {
  NativeAffineProbe *p=context;
  if(face->authored || (face->kind!=kDioramaVoxelFace_Top&&
      face->kind!=kDioramaVoxelFace_Side&&face->kind!=kDioramaVoxelFace_Bottom))return;
  assert(p->count<512);p->faces[p->count++]=*face;
}
static unsigned TombContourSkinRenderFixture(unsigned scenario) {
  const bool lower=scenario&1,left=scenario&2,negative=scenario&4;
  const int base=negative?-16:80,y=lower?111:96;
  const unsigned side=left?3:1,skin=lower?kVoxelContourSkinBottom:kVoxelContourSkinTop;
  s_count=0;static NativeAffineProbe probe,plain,skinned;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=3;g->active_room=4;g->profile=kActionVoxelProfile_KassandoraTomb;
  g->settings=ActionVoxel_SettingsForRoom(3,4);g->origin_x=-32;g->width=1024;g->height=768;g->ready=true;
  g->authored=calloc(1,sizeof(*g->authored));g->authored_at=calloc((size_t)g->width/16,g->height/16*sizeof(*g->authored_at));
  g->authored_art=calloc(1,sizeof(*g->authored_art));g->authored_cap_art=malloc(sizeof(*g->authored_cap_art));
  assert(g->authored&&g->authored_at&&g->authored_art&&g->authored_cap_art);
  g->authored_art_count=g->authored_art_capacity=g->authored_cap_art_count=1;g->authored_cap_art[0]=0;
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)g->authored_art[0][py*16+px]=NativeAffineColor(px,py);
  DioramaCapture capture={.width=256,.height=224,.bg1_camera_x=base-80,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=lower?-.2f:.2f,.tilt_y=left?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=TombContourObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=3,.map_number=4,.voxel_geometry=g};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  const float depth=ActionVoxel_Depth(g);
  for(unsigned frame=0;frame<4;++frame) {
    const int shift=frame==3?256:0;
    const DioramaVoxelTransform transform={.px=base+8+shift,.py=104,.pz=.09f,
        .dx=frame==3?-256:0,.dy=frame==3?-5:0,.sx=frame==3?.75f:1,.sy=1,.sz=1};
    if(frame!=2)++g->revision;
    capture.bg1_camera_x=base-80+(frame==2?3:0);
    g->authored_count=g->native_transform_count=frame==3;
    memset(g->authored_at,0,(size_t)(g->width/16)*(g->height/16)*sizeof(*g->authored_at));
    if(frame==3) {
      VoxelAuthoredCell *a=g->authored;
      *a=(VoxelAuthoredCell){.x=base+shift,.y=96,.width=16,.native_transform=true,.transform=transform,
          .faces=kDioramaVoxelFaceMask_Top|kDioramaVoxelFaceMask_Bottom|kDioramaVoxelFaceMask_Left|kDioramaVoxelFaceMask_Right};
      for(unsigned f=0;f<7;++f)a->pixels[f]=g->authored_art[0];
      g->authored_at[(size_t)(a->y/16)*(g->width/16)+(a->x-g->origin_x)/16]=1;
    }
    const VoxelEdge outline[]={
      {.x=base+shift,.y=y,.length=8,.side=lower?2:0,.far_rank=2,.colliding=1},
      {.x=base+8+shift,.y=y,.length=8,.side=lower?2:0,.far_rank=2,.colliding=1},
      {.x=base+(left?0:15)+shift,.y=y,.length=1,.side=side,.far_rank=2,.colliding=1},
      /* An original16px hard wall trimmed to a2px source band must not be
       * shaded or reoriented as a closing riser. It stays at a separate Y. */
      {.x=base+(left?0:15),.y=140,.length=2,.side=side,.far_rank=2,.colliding=1,.fold_origin_y=128,.fold_length=16},
    };
    g->edge_count=g->pass_counts[0]=4;
    for(unsigned i=0;i<4;++i) {
      g->pass_edges[0][i]=i;g->edges[i]=outline[i];VoxelEdge *e=&g->edges[i];
      e->surface_texture=1;e->top_sample_size=16;e->top_sample_x=900;e->top_sample_y=500;e->top_owned_art=1;
      e->contour_skin=frame&&i<3?skin:0;
    }
    probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    const unsigned creates=r.creates,uploads=r.uploads;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.count&&r.actor_seen&&probe.uploaded);
    for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!frame){plain=probe;continue;}
    assert(probe.count==plain.count);if(frame==2)assert(r.creates==creates&&r.uploads==uploads);
    unsigned cap_riser_joins=0,depth_joins=0,half_joins=0,before=0,after=0,long_wall=0;
    for(unsigned f=0;f<probe.count;++f) {
      const DioramaVoxelPickFace *face=&probe.faces[f];
      const bool vertical=fabsf(face->world_xy[0]-face->world_xy[2])<.001f;
      const bool ordinary=face->world_xy[1]>=128;
      assert(face->kind==(vertical?kDioramaVoxelFace_Side:
          lower?kDioramaVoxelFace_Bottom:kDioramaVoxelFace_Top));
      assert(probe.face_textures[f].value==probe.texture.value);
      if(probe.after_actor[f])++after;else ++before;
      assert(probe.after_actor[f]==plain.after_actor[f]);
      for(unsigned j=0;j<4;++j) {
        const ArRenderVertex2D *v=&probe.vertices[f][j];
        const float x=roundf(face->world_xy[j*2]),yy=roundf(face->world_xy[j*2+1]);
        if(frame==1) {
          assert(CapPointEqual(v->position,plain.vertices[f][j].position));
          assert(fabsf(face->clip_w[j]-plain.faces[f].clip_w[j])<.000001f);
          if(!vertical||ordinary)assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));
        }
        if(ordinary) {
          assert(CapUvEqual(v->tex_coord,plain.vertices[f][j].tex_coord));
          assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));++long_wall;continue;
        }
        const float px=transform.px+(x-transform.px)*transform.sx+transform.dx,py=yy+transform.dy;
        const float z=NativeAffineDepth(&projection,&capture,px,py,face->clip_w[j]);
        const float expected_u=.5f+15*(x-(base+shift))/16;
        const float expected_v=.5f+15*(lower?1-z/depth:z/depth);
        assert(fabsf(v->tex_coord.x*512-expected_u)<.001f);
        assert(fabsf(v->tex_coord.y*2048-expected_v)<.001f);
        if(frame>=2)assert(CapUvEqual(v->tex_coord,skinned.vertices[f][j].tex_coord));
        for(unsigned other=0;other<f;++other) {
          const DioramaVoxelPickFace *of=&probe.faces[other];
          const bool ov=fabsf(of->world_xy[0]-of->world_xy[2])<.001f;
          if(of->world_xy[1]>=128)continue;
          for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[other][k].position)) {
            assert(CapUvEqual(v->tex_coord,probe.vertices[other][k].tex_coord));
            if(vertical!=ov) {
              assert(fabsf(v->color.r-probe.vertices[other][k].color.r)<.000001f);
              assert(fabsf(v->color.g-probe.vertices[other][k].color.g)<.000001f);
              assert(fabsf(v->color.b-probe.vertices[other][k].color.b)<.000001f);++cap_riser_joins;
            }
            if(probe.after_actor[f]!=probe.after_actor[other])++depth_joins;
            if(!vertical&&!ov&&fabsf(x-(base+shift+8))<.001f)++half_joins;
          }
        }
      }
    }
    assert(before&&after&&cap_riser_joins&&depth_joins&&half_joins&&long_wall);
    if(frame==1)skinned=probe;
  }
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);return s_count;
}
