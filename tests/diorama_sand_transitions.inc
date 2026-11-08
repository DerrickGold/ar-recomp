static void SandTransitionRenderFixture(unsigned scenario) {
  const unsigned lengths[]={1,2,3,6};const unsigned extent=lengths[scenario/4];scenario%=4;
  const bool left=scenario&1;s_depth_fixture=!!(scenario&2);s_count=0;
  static NativeAffineProbe probe,plain;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;}
  probe.native_texture=textures[kDioramaPlane_Bg1Far];probe.rear_texture=textures[kDioramaPlane_Bg1Hi];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);g->active_group=3;g->active_room=2;
  g->profile=kActionVoxelProfile_KassandoraDesert;g->settings=ActionVoxel_SettingsForRoom(3,2);
  g->width=1024;g->height=768;g->ready=true;
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.2f,.tilt_y=left?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.voxel_observe_face=TombContourObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,.map_group=3,.map_number=2,.voxel_geometry=g};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  unsigned joints=0,endpoints=0;
  for(unsigned frame=0;frame<4;++frame) {
    const bool mode=frame&1,unavailable=frame>=2;const int y=unavailable?226-(int)extent:96;
    ++g->revision;g->edge_count=g->pass_counts[1]=3;g->pass_counts[0]=0;
    g->high_deck_count=g->pass_counts[3]=2;g->pass_edges[3][0]=0;g->pass_edges[3][1]=1;
    for(unsigned i=0;i<3;++i) {
      g->pass_edges[1][i]=i;g->edges[i]=(VoxelEdge){.x=i==2?(left?80:95):80+8*i,.y=y,.length=i==2?1:8,
        .side=i==2?(left?3:1):0,.far_rank=2,.high_priority=1,.colliding=1,.soft_surface=1,.textured_top=1};
      if(mode) {VoxelEdge *e=g->edges+i;e->surface_texture=kVoxelSurfaceTextureLiveInward;
        e->top_sample_size=extent;e->top_sample_x=e->x;e->top_sample_y=e->y;e->surface_derived=1;e->surface_top_material=i==2;}
    }
    probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.count);for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!mode){plain=probe;continue;}assert(probe.count==plain.count);
    for(unsigned f=0;f<probe.count;++f)for(unsigned j=0;j<4;++j) {
      const DioramaVoxelPickFace *face=&probe.faces[f];const ArRenderVertex2D *v=&probe.vertices[f][j];
      assert(CapPointEqual(v->position,plain.vertices[f][j].position));
      assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));
      assert(probe.after_actor[f]==plain.after_actor[f]);
      if(unavailable){assert(CapUvEqual(v->tex_coord,plain.vertices[f][j].tex_coord));continue;}
      const float x=face->world_xy[j*2],yy=face->world_xy[j*2+1];
      const float z=NativeAffineDepth(&projection,&capture,x,yy,face->clip_w[j]);
      const float expected=95.5f+(extent-1)*z/ActionVoxel_Depth(g); /* native worldY96 -> captureY95 */
      if(fabsf(v->tex_coord.y*352-expected)>.002f)fprintf(stderr,"SAND_UV case%u face%u vertex%u z%f actual%f expected%f\\n",scenario,f,j,z,v->tex_coord.y*352,expected);
      assert(fabsf(v->tex_coord.y*352-expected)<.002f);
      if(z<.0001f||fabsf(z-ActionVoxel_Depth(g))<.0001f)++endpoints;
      for(unsigned o=0;o<f;++o)for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[o][k].position)) {
        if(face->kind!=probe.faces[o].kind)assert(CapUvEqual(v->tex_coord,probe.vertices[o][k].tex_coord));
        if(probe.after_actor[f]!=probe.after_actor[o] &&
            face->world_xy[0]==probe.faces[o].world_xy[0] &&
            face->world_xy[2]==probe.faces[o].world_xy[2])assert(CapUvEqual(v->tex_coord,probe.vertices[o][k].tex_coord));++joints;
      }
    }
  }
  assert(endpoints&&joints);ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
}
static void SandTransitionBoundsFixture(void) {
  const float guard[]={0,224,256,64},gap[]={0,225,256,63},narrow[]={90,224,32,64};
  for(unsigned rows=1;rows<=16;++rows) {
    assert(DioramaSandSurface_Covered(80,224-(float)rows,8,256,224,NULL,rows));
    assert(!DioramaSandSurface_Covered(80,225-(float)rows,8,256,224,NULL,rows));
    assert(DioramaSandSurface_Covered(80,225-(float)rows,8,256,224,guard,rows));
    assert(!DioramaSandSurface_Covered(80,225-(float)rows,8,256,224,gap,rows));
    assert(!DioramaSandSurface_Covered(80,225-(float)rows,8,256,224,narrow,rows));
    assert(DioramaSandSurface_SourceY(12,.06f,.06f,.2f,rows)==12.5f);
    assert(DioramaSandSurface_SourceY(12,.2f,.06f,.2f,rows)==12.5f+rows-1);
    for(unsigned j=0;j<=16;++j) {
      const float z=.06f+j*.14f/16,span=.2f-.06f;
      const float f=fmaxf(0,fminf(1,(z-.06f)/span));
      const float old=12+.5f+15*f,now=DioramaSandSurface_SourceY(12,z,.06f,.2f,16);
      assert(!memcmp(&old,&now,sizeof(old)));
    }
  }
  assert(!DioramaSandSurface_Covered(0,0,8,256,224,NULL,0));
  assert(!DioramaSandSurface_Covered(0,0,8,256,224,NULL,17));
}
