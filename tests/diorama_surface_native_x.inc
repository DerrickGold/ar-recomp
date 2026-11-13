/* A native-width beam cap must not pull a transparent neighbouring column
 * across the sole opaque pixel of a short contour. Exercise uploaded art and
 * submitted UVs, including the two actor bands and a clipped larger span. */
static unsigned VoxelNativeXSurfaceFixture(unsigned scenario) {
  s_count=0;s_depth_fixture=scenario>=2;
  static NativeAffineProbe probe,generic;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},textures+i));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=2;voxel->active_room=1;voxel->profile=kActionVoxelProfile_BloodpoolMarsh;
  voxel->settings=ActionVoxel_SettingsForRoom(2,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  const VoxelEdge outlines[]={
    {.x=80,.y=96,.length=1,.side=0,.far_rank=2,.colliding=1},
    {.x=82,.y=96,.length=14,.side=0,.far_rank=2,.colliding=1},
    {.x=96,.y=96,.length=1,.side=0,.far_rank=2,.colliding=1},
  };
  voxel->edge_count=voxel->pass_counts[0]=3;
  voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=malloc(sizeof(*voxel->authored_cap_art));
  voxel->authored=calloc(1,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)voxel->width/16,voxel->height/16*sizeof(*voxel->authored_at));
  voxel->authored_contours=calloc(2,sizeof(*voxel->authored_contours));
  assert(voxel->authored_art && voxel->authored_cap_art && voxel->authored &&
      voxel->authored_at && voxel->authored_contours);
  voxel->authored_art_count=voxel->authored_art_capacity=voxel->authored_cap_art_count=1;
  voxel->authored_cap_art[0]=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    voxel->authored_art[0][y*16+x]=x==1?0:NativeAffineColor(x,y);
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.2f,.tilt_y=scenario&1?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=2,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<6;++frame) {
    const bool native_x=frame!=0 && frame!=5,authored=frame>=4;
    capture.bg1_camera_x=frame==2?85:frame==3?3:0;
    for(unsigned i=0;i<3;++i) {
      voxel->pass_edges[0][i]=i;voxel->edges[i]=outlines[i];
      VoxelEdge *e=voxel->edges+i;e->surface_texture=kVoxelSurfaceTexturePatch;
      e->surface_derived=1;e->surface_native_x=native_x;
      e->top_sample_x=900;e->top_sample_y=500;e->top_sample_size=16;e->top_owned_art=1;
    }
    if(authored) {
      /* Width editing moves physical vertices while retaining the native
       * source-column footprint. The unflagged control keeps old material UVs. */
      voxel->edge_count=voxel->pass_counts[0]=0;voxel->authored_count=1;
      VoxelAuthoredCell *a=voxel->authored;
      *a=(VoxelAuthoredCell){.x=80,.y=96,.width=8,.surface=true,
          .faces=kDioramaVoxelFaceMask_Top,.contour_count=2};
      for(unsigned q=0;q<4;++q)a->back[q]=ActionVoxel_Depth(voxel);
      for(unsigned art=0;art<7;++art)a->pixels[art]=voxel->authored_art[0];
      a->surface_sample_x[1]=900;a->surface_sample_y[1]=500;a->surface_sample_size[1]=16;
      a->surface_native_x[1]=native_x;
      voxel->authored_at[6*(voxel->width/16)+5]=1;
      voxel->authored_contour_count=voxel->authored_contour_capacity=2;
      voxel->authored_contours[0]=(VoxelAuthoredContour){.x=0,.y=0,.length=1,.side=0};
      voxel->authored_contours[1]=(VoxelAuthoredContour){.x=2,.y=0,.length=14,.side=0};
    }
    ++voxel->revision;probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && probe.count && probe.uploaded && r.actor_seen);
    unsigned before=0,after=0,narrow_samples=0,generic_holes=0,clipped=0,joins=0;
    for(unsigned f=0;f<probe.count;++f) {
      assert(probe.submitted[f] && probe.face_textures[f].value==probe.texture.value);
      const DioramaVoxelPickFace *face=probe.faces+f;const ArRenderVertex2D *v=probe.vertices[f];
      assert(face->kind==kDioramaVoxelFace_Top);
      if(probe.after_actor[f])++after;else ++before;
      const float x0=face->world_xy[0],x1=face->world_xy[2],period=x0<96?80:96;
      assert(x1>x0);const bool narrow=x1-x0<1.01f;
      if(frame==2 && fabsf(x0-85)<.001f)++clipped;
      for(unsigned j=0;j<4;++j) {
        const float x=face->world_xy[j*2],column=x-period;
        const float expected=native_x?fmaxf(.0001f,fminf(15.9999f,column)):.5f+15*column/16;
        assert(fabsf(v[j].tex_coord.x*512-expected)<.001f);
        if(frame==1) {
          assert(CapPointEqual(v[j].position,generic.vertices[f][j].position));
          assert(!memcmp(&v[j].color,&generic.vertices[f][j].color,sizeof(v[j].color)));
          assert(v[j].tex_coord.y==generic.vertices[f][j].tex_coord.y);
        }
        /* Only the same contour's depth split shares atlas phase. Adjacent
         *16px native cells legitimately wrap15.9999 back to.0001 at a join. */
        for(unsigned other=0;other<f;++other)if(fabsf(probe.faces[other].world_xy[0]-x0)<.001f &&
            fabsf(probe.faces[other].world_xy[2]-x1)<.001f)
          for(unsigned k=0;k<4;++k)if(CapPointEqual(v[j].position,probe.vertices[other][k].position)) {
            assert(CapUvEqual(v[j].tex_coord,probe.vertices[other][k].tex_coord));++joins;
          }
      }
      /* UVs are affine along a physical cap row; perspective-correct draw
       * interpolation preserves these values after screen projection. Check
       * interior points (never a shared raster edge) against nearest texels. */
      for(unsigned row=0;row<2;++row)for(unsigned n=0;n<20;++n) {
        const unsigned left=row?3:0,right=row?2:1;const float t=(n+.37f)/20.f;
        const float wx=x0+(x1-x0)*t;
        const unsigned sx=(unsigned)floorf((v[left].tex_coord.x*(1-t)+v[right].tex_coord.x*t)*512);
        const unsigned sy=(unsigned)floorf((v[left].tex_coord.y*(1-t)+v[right].tex_coord.y*t)*2048);
        assert(sx<16 && sy<16);
        if(native_x) {
          assert(sx==(unsigned)floorf(wx-period));
          assert(probe.atlas[sy*512+sx]>>24);
        }
        if(narrow) {
          ++narrow_samples;
          if(native_x)assert(sx==0);
          else generic_holes+=!(probe.atlas[sy*512+sx]>>24);
        }
      }
    }
    assert(before && !after && joins && narrow_samples);
    if(frame==2)assert(clipped && probe.count<generic.count);
    if(!native_x)assert(generic_holes);
    if(!frame)generic=probe;
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
