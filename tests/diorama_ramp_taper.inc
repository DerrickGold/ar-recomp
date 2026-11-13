/* Recover submitted physical depths independently of the taper helpers.
 * Adjacent two-pixel fronts and their narrow seams must meet across camera
 * signs and native actor-depth partitions. */
static void RampTaperObserve(void *context,const DioramaVoxelPickFace *face) {
  NativeAffineProbe *probe=context;
  if(face->plane!=kDioramaPlane_Bg1Far ||
      (face->kind!=kDioramaVoxelFace_Front && face->kind!=kDioramaVoxelFace_Top &&
       face->kind!=kDioramaVoxelFace_Side && face->kind!=kDioramaVoxelFace_Bottom))return;
  float x=0,y=0;for(unsigned j=0;j<4;++j){x+=face->world_xy[j*2]*.25f;y+=face->world_xy[j*2+1]*.25f;}
  if(x<80 || x>96 || y<96 || y>128)return;
  assert(probe->count<512);probe->faces[probe->count++]=*face;
}
static float RampTaperExpected(float y,int contact) {
  return .24f*fmaxf(0,fminf(1,(y-contact-4)/12));
}
static unsigned RampTaperFixture(unsigned scenario) {
  s_count=0;s_depth_fixture=!!(scenario&2);
  static NativeAffineProbe probe;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=4;g->active_room=1;g->profile=kActionVoxelProfile_AitosMountains;
  g->settings=(ActionVoxelSettings){.depth=.24f,.play_recess=.08f};
  g->width=1024;g->height=512;g->ready=true;
  g->column_cells=calloc(128,64);assert(g->column_cells);
  g->column_curve_count=1;
  VoxelTrunkCurve *curve=&g->column_curves[0];
  *curve=(VoxelTrunkCurve){.top_y=100,.base_y=126,.contact_x=80,.contact_count=8,.linear=1};
  const int grade=scenario&4?2:1;
  for(unsigned i=0;i<8;++i)curve->contact[i]=(short)(96+i*grade);
  for(unsigned y=12;y<16;++y)for(unsigned x=10;x<12;++x)g->column_cells[y*128+x]=1;
  for(unsigned i=0;i<8;++i) {
    const unsigned at=g->edge_count++;g->pass_edges[0][g->pass_counts[0]++]=at;
    g->edges[at]=(VoxelEdge){.x=(short)(80+i*2),.y=curve->contact[i],.length=2,
      .side=0,.far_rank=2,.colliding=true,.column_curve=1};
  }
  for(unsigned i=0;i<7;++i) {
    const unsigned at=g->edge_count++;g->pass_edges[0][g->pass_counts[0]++]=at;
    g->edges[at]=(VoxelEdge){.x=(short)(82+i*2),.y=(short)(curve->contact[i]+4),
      .length=(short)(12+grade),.side=3,.near_rank=2,.far_rank=2,.colliding=true,
      .column_curve=1,.neighbor_column_curve=1};
  }
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.2f,.tilt_y=scenario&1?-.35f:.35f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
    .voxel_observe_face=RampTaperObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
    .map_group=4,.map_number=1,.voxel_geometry=g};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid&&probe.count&&r.actor_seen);
  unsigned fronts=0,tops=0,seams=0,lips=0,rear=0,front_band=0,rear_band=0;
  for(unsigned f=0;f<probe.count;++f) {
    const DioramaVoxelPickFace *face=&probe.faces[f];assert(probe.submitted[f]);
    float middle_x=0;for(unsigned j=0;j<4;++j)middle_x+=face->world_xy[j*2]*.25f;
    const bool front=face->kind==kDioramaVoxelFace_Front;
    const bool top=face->kind==kDioramaVoxelFace_Top;
    fronts+=front;tops+=top;seams+=!front&&!top;
    const int column=(int)floorf((middle_x-80)*.5f);
    if(front)assert(column>=0&&column<8&&fabsf(face->world_xy[2]-face->world_xy[0])<=2.001f);
    assert(!probe.after_actor[f]);
    float depths[4];
    for(unsigned j=0;j<4;++j) {
      const float x=roundf(face->world_xy[j*2]),y=face->world_xy[j*2+1];
      const float z=NativeAffineDepth(&projection,&capture,x,y,face->clip_w[j]);
      depths[j]=z;
      assert(z>=-.0001f&&z<=.2401f);
      if(front) {
        const int contact=96+column*grade;
        assert(fabsf(z-RampTaperExpected(y,contact))<.0001f);
        if(y<=contact+4){assert(fabsf(z)<.0001f);++lips;}
        if(y>=contact+16){assert(fabsf(z-.24f)<.0001f);++rear;}
      } else if(top) {
        assert(fabsf(z)<.0001f || fabsf(z-.07f)<.0001f || fabsf(z-.24f)<.0001f);
      } else {
        /* Internal seams continue the front ink and lighting, rather than
         * introducing a darker horizontal-floor donor every two pixels. */
        const ArRenderVertex2D *vertex=&probe.vertices[f][j];
        assert(ArRenderTexture_Equals(probe.face_textures[f],probe.native_texture));
        assert(fabsf(vertex->tex_coord.x-(x+.5f)/SR_PPU_SURFACE_MAX_WIDTH)<.00001f);
        assert(fabsf(vertex->tex_coord.y-(y-1)/SR_PPU_SURFACE_MAX_HEIGHT)<.00001f);
        assert(fabsf(vertex->color.r-1)<.00001f && fabsf(vertex->color.g-1)<.00001f &&
            fabsf(vertex->color.b-1)<.00001f);
        const int right=(int)roundf((x-80)*.5f),left=right-1;
        assert(left>=0&&right<8);
        /* Noninteger actor knots are checked at their true Y coordinate. */
        const float actual_a=RampTaperExpected(y,96+right*grade);
        const float actual_b=RampTaperExpected(y,96+left*grade);
        const bool on_boundary=fabsf(z-actual_a)<.0001f || fabsf(z-actual_b)<.0001f;
        if(!(on_boundary || fabsf(z-.07f)<.0001f))
          fprintf(stderr,"ramp endpoint scenario%u face%u vertex%u xy%.5f,%.5f z%.6f expected%.6f..%.6f after%u\n",
              scenario,f,j,x,y,z,actual_a,actual_b,probe.after_actor[f]);
        assert(on_boundary || fabsf(z-.07f)<.0001f);
        if(z<actual_a-.0001f || z>actual_b+.0001f) {
          /* The clipped half can end at one collapsed actor-plane point;
           * it must not leave a finite strip across the hidden interval. */
          assert(fabsf(z-.07f)<.0001f);
          assert(CapPointEqual(probe.vertices[f][j].position,probe.vertices[f][3-j].position));
        }
      }
    }
    if(!front&&!top && (fabsf(depths[0]-depths[3])>.0001f || fabsf(depths[1]-depths[2])>.0001f)) {
      const float near=fminf(fminf(depths[0],depths[1]),fminf(depths[2],depths[3]));
      const float far=fmaxf(fmaxf(depths[0],depths[1]),fmaxf(depths[2],depths[3]));
      assert(far<=.0701f || near>=.0699f);
      if(far<=.0701f)++front_band;else ++rear_band;
    }
  }
  /* These left-facing internal seams are culled from the opposite yaw. */
  const bool visible_seams=!!(scenario&1);
  if(!(fronts&&tops&&lips&&rear&&(!visible_seams||(seams&&front_band&&rear_band))))
    fprintf(stderr,"ramp taper scenario%u fronts%u tops%u seams%u lips%u rear%u bands%u/%u\n",
        scenario,fronts,tops,seams,lips,rear,front_band,rear_band);
  assert(fronts&&tops&&lips&&rear);
  if(visible_seams)assert(seams&&front_band&&rear_band);
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
