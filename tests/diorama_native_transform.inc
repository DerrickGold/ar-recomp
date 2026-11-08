static uint16_t NativeAffineColor(int x,int y) {
  return 0x8000|((x&3)*5+2)|(((y&3)*6+3)<<5)|((((x+y)&3)*7+1)<<10);
}
static void NativeAffineObserve(void *context,const DioramaVoxelPickFace *face) {
  NativeAffineProbe *probe=context;
  if(!face->authored && face->kind!=kDioramaVoxelFace_Top && face->kind!=kDioramaVoxelFace_Side)return;
  float x=0,y=0;for(unsigned i=0;i<4;++i){x+=face->world_xy[i*2]*.25f;y+=face->world_xy[i*2+1]*.25f;}
  if(x<79+probe->source_shift || x>113+probe->source_shift || y<95 || y>161)return;
  assert(probe->count<512);probe->faces[probe->count++]=*face;
}
/* Recover physical recession from the independently published clip W. Native
 * pixel anchoring makes the world position affine in recession before divide. */
static float NativeAffineDepth(const DioramaProjection *p,const DioramaCapture *capture,
    float x,float y,float clip_w) {
  const float wx=((x-capture->bg1_camera_x)/capture->width-.5f)*p->aspect_x;
  const float wy=(.5f-(y-capture->camera_y-1)/capture->height)*p->height_scale;
  const float base=p->matrix[3]*wx+p->matrix[7]*wy+p->matrix[15];
  const float slope=(p->matrix[3]*wx+p->matrix[7]*wy)/p->camera_distance-p->matrix[11];
  assert(fabsf(slope)>.01f);return (clip_w-base)/slope;
}
static void NativeAffineCheckCrisp(const NativeAffineProbe *probe,const Recorder *r,
    unsigned index,const VoxelAuthoredCell *cell) {
  const DioramaVoxelPickFace *face=&probe->faces[index];
  const ArRenderVertex2D *v=probe->vertices[index];
  assert(probe->crisp_uploaded && probe->face_textures[index].value==probe->crisp_texture.value);
  const ArRenderTextureDesc *desc=&r->textures[probe->crisp_texture.value];
  assert(desc->width==2048 && desc->height==(int)probe->crisp_height && desc->filter==kArRenderFilter_Linear);
  float middle_x=0,middle_y=0;
  for(unsigned j=0;j<4;++j){middle_x+=v[j].tex_coord.x*2048*.25f;middle_y+=v[j].tex_coord.y*probe->crisp_height*.25f;}
  const int ox=(int)floorf(middle_x/64)*64,oy=(int)floorf(middle_y/64)*64;
  assert(ox>=0 && ox+64<=2048 && oy>=0 && oy+64<=(int)probe->crisp_height);
  for(unsigned j=0;j<4;++j) {
    /* Half an expanded texel guards a cell's atlas edge. Interior native
     * coordinates retain their original sampling phase. */
    const float x=4*fmaxf(.125f,fminf(15.875f,face->art_uv[j*2]));
    const float y=4*fmaxf(.125f,fminf(15.875f,face->art_uv[j*2+1]));
    assert(fabsf(v[j].tex_coord.x*2048-ox-x)<.002f);
    assert(fabsf(v[j].tex_coord.y*probe->crisp_height-oy-y)<.002f);
  }
  /* Native presentation replicates each source pixel four times, then uses
   * linear filtering. The owned source must preserve that exact footprint. */
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
    assert(probe->crisp_pixels[(oy+y)*2048+ox+x]==
        ActionVoxel_AuthoredColor(cell->pixels[0][(y/4)*16+x/4],NULL));
}
static unsigned NativeAffineFixture(unsigned scenario) {
  s_count=0;static NativeAffineProbe probe,baseline;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  const VoxelEdge outline[]={
    {.x=84,.y=96,.length=24,.side=0,.far_rank=2,.colliding=1,.textured_top=1,
      .top_sample_x=89,.top_sample_y=103,.top_sample_size=4},
    {.x=84,.y=96,.length=8,.side=3,.far_rank=2,.colliding=1},
    {.x=107,.y=96,.length=8,.side=1,.far_rank=2,.colliding=1},
    {.x=80,.y=104,.length=4,.side=0,.far_rank=2,.colliding=1},
    {.x=108,.y=104,.length=4,.side=0,.far_rank=2,.colliding=1},
    {.x=80,.y=104,.length=48,.side=3,.far_rank=2,.colliding=1},
    {.x=111,.y=104,.length=48,.side=1,.far_rank=2,.colliding=1},
    {.x=80,.y=151,.length=32,.side=2,.far_rank=2,.colliding=1},
  };
  VoxelEdge original[32];unsigned edges=0;
  for(unsigned i=0;i<sizeof(outline)/sizeof(*outline);++i)for(int at=0;at<outline[i].length;) {
    VoxelEdge e=outline[i];const int coordinate=(e.side&1)?e.y:e.x;
    const int length=(coordinate+at+8)/8*8-(coordinate+at);
    e.length=outline[i].length-at<length?outline[i].length-at:length;
    e.x+=(e.side&1)?0:at;e.y+=(e.side&1)?at:0;assert(edges<32);original[edges++]=e;at+=e.length;
  }
  voxel->edge_count=voxel->pass_counts[0]=edges;
  for(unsigned i=0;i<edges;++i)voxel->pass_edges[0][i]=i;
  voxel->authored=calloc(8,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)voxel->width/16,voxel->height/16*sizeof(*voxel->authored_at));
  voxel->authored_art=calloc(2,sizeof(*voxel->authored_art));
  voxel->authored_cap_art=malloc(2*sizeof(*voxel->authored_cap_art));
  assert(voxel->authored && voxel->authored_at && voxel->authored_art && voxel->authored_cap_art);
  voxel->authored_art_count=voxel->authored_art_capacity=2;
  voxel->authored_cap_art[0]=0;voxel->authored_cap_art[1]=1;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    voxel->authored_art[0][y*16+x]=NativeAffineColor(89+x,103+y);
    voxel->authored_art[1][y*16+x]=NativeAffineColor(x,y);
  }
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<5;++frame) {
    const int shift=frame>=3?256:0;
    const DioramaVoxelTransform transform={.px=96+shift,.py=124,.pz=.09f,
        .dx=frame>=3?-256:frame==2?16:0,.dy=frame>=2?-10:0,
        .dz=frame==4?-.05f:0,.sx=frame>=2?.5f:1,.sy=1,.sz=frame==4?1.2f:1};
    probe.count=0;probe.source_shift=shift;memset(probe.submitted,0,sizeof(probe.submitted));
    r.actor_seen=false;++voxel->revision;
    voxel->authored_count=frame?8:0;
    voxel->native_transform_count=voxel->hidden_native_front_count=frame>1?8:0;
    voxel->transformed_rear_count=frame>1?8:0;
    voxel->authored_cap_art_count=frame>1?2:0;
    memset(voxel->authored_at,0,(size_t)(voxel->width/16)*(voxel->height/16)*sizeof(*voxel->authored_at));
    for(unsigned i=0;i<8;++i) {
      VoxelAuthoredCell *a=&voxel->authored[i];
      *a=(VoxelAuthoredCell){.x=80+shift+(i&1)*16,.y=96+(i>>1)*16,.width=16,
          .faces=kDioramaVoxelFaceMask_All,.native_transform=frame>1,.native_front_hidden=frame>1,
          .native_transform_requested=frame>0,
          .native_front_art=true,.transform=transform};
      for(unsigned q=0;q<4;++q)a->back[q]=ActionVoxel_Depth(voxel);
      if(frame>1)for(unsigned row=0;row<16;++row)a->native_rear_rows[row]=0xffff;
      for(unsigned face=0;face<7;++face){a->art_id[face]=1;a->pixels[face]=voxel->authored_art[1];}
      if(frame)voxel->authored_at[(size_t)(a->y/16)*(voxel->width/16)+a->x/16]=i+1;
    }
    for(unsigned i=0;i<edges;++i) {
      VoxelEdge e=original[i];e.x+=shift;if(e.top_sample_size)e.top_sample_x+=shift;
      if(frame>1) {
        if(e.top_sample_size)e.top_owned_art=1;
        for(unsigned source=0;source<2;++source) {
          e.source_owned_art[source]=2;e.source_x[source]=(e.x/16)*16;e.source_y[source]=(e.y/16)*16;
        }
      }
      voxel->edges[i]=e;
    }
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && probe.count && r.actor_seen);
    for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!frame){baseline=probe;continue;}
    if(frame==1) {
      /* A pivot-only identity remains on the original native path, including
       * source filtering/shading. It must not allocate any owned-art page. */
      assert(probe.count==baseline.count && !probe.uploaded && !probe.crisp_uploaded);
      for(unsigned f=0;f<probe.count;++f) {
        assert(!probe.faces[f].authored && !memcmp(&probe.faces[f],&baseline.faces[f],sizeof(probe.faces[f])));
        assert(!memcmp(probe.vertices[f],baseline.vertices[f],sizeof(probe.vertices[f])));
        assert(probe.after_actor[f]==baseline.after_actor[f]);
      }
      continue;
    }
    assert(probe.uploaded);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      assert(probe.atlas[y*512+x]==ActionVoxel_AuthoredColor(voxel->authored_art[0][y*16+x],NULL));
      assert(probe.atlas[y*512+16+x]==ActionVoxel_AuthoredColor(voxel->authored_art[1][y*16+x],NULL));
    }
    unsigned caps=0,fronts=0,backs=0,before=0,after=0,shared=0;
    const float split=fmaxf(0,ActionVoxel_PlayRecess(voxel)-.01f);
    for(unsigned f=0;f<probe.count;++f) {
      const DioramaVoxelPickFace *face=&probe.faces[f];
      const bool backing=face->kind==kDioramaVoxelFace_Backing;
      const bool front=face->kind==kDioramaVoxelFace_Front;
      if(backing)++backs;else if(front)++fronts;else{++caps;if(probe.after_actor[f])++after;else ++before;}
      float physical[4];
      for(unsigned j=0;j<4;++j) {
        const float sx=roundf(face->world_xy[j*2]),sy=roundf(face->world_xy[j*2+1]);
        const float px=transform.px+(sx-transform.px)*transform.sx+transform.dx,py=sy+transform.dy;
        physical[j]=NativeAffineDepth(&projection,&capture,px,py,face->clip_w[j]);
        const float raw=(physical[j]-transform.pz-transform.dz)/transform.sz+transform.pz;
        assert(raw>=-.00001f && raw<=ActionVoxel_Depth(voxel)+.00001f);
        if(front)assert(fabsf(raw)<.00001f);
        if(backing)assert(fabsf(raw-ActionVoxel_Depth(voxel))<.00001f);
        const ArRenderPointF expected=RootExpectedPoint(&projection,&capture,px,py,physical[j]);
        assert(CapPointEqual(expected,(ArRenderPointF){face->screen_xy[j*2],face->screen_xy[j*2+1]}));
        if(!front && !backing) {
          if(probe.after_actor[f])assert(physical[j]<=split+.00001f);
          else assert(physical[j]>=split-.00001f);
        }
      }
      if(backing)continue;
      if(front) {
        const VoxelAuthoredCell *cell=ActionVoxel_AuthoredCellAt(voxel,face->cell_x,face->cell_y);
        assert(cell);NativeAffineCheckCrisp(&probe,&r,f,cell);
        for(unsigned other=0;other<f;++other)if(probe.faces[other].kind==kDioramaVoxelFace_Front &&
            probe.faces[other].cell_x!=face->cell_x)
          for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
            if(fabsf(face->world_xy[j*2]-probe.faces[other].world_xy[k*2])<.001f &&
                fabsf(face->world_xy[j*2+1]-probe.faces[other].world_xy[k*2+1])<.001f) {
              assert(CapPointEqual((ArRenderPointF){face->screen_xy[j*2],face->screen_xy[j*2+1]},
                  (ArRenderPointF){probe.faces[other].screen_xy[k*2],probe.faces[other].screen_xy[k*2+1]}));++shared;
            }
        continue;
      }
      if(frame==4)continue; /* Z clipping introduces new source parameters. */
      unsigned match=baseline.count;
      for(unsigned b=0;b<baseline.count && match==baseline.count;++b) {
        bool same=baseline.faces[b].kind==face->kind;
        for(unsigned j=0;j<4;++j) {
          const float bx=roundf(baseline.faces[b].world_xy[j*2]),by=roundf(baseline.faces[b].world_xy[j*2+1]);
          const float bz=NativeAffineDepth(&projection,&capture,bx,by,baseline.faces[b].clip_w[j]);
          same&=fabsf(bx+shift-roundf(face->world_xy[j*2]))<.001f &&
              fabsf(by-roundf(face->world_xy[j*2+1]))<.001f && fabsf(bz-physical[j])<.00001f;
        }
        if(same)match=b;
      }
      assert(match<baseline.count);assert(probe.after_actor[f]==baseline.after_actor[match]);
      for(unsigned j=0;j<4;++j) {
        const ArRenderVertex2D *v=&probe.vertices[f][j],*b=&baseline.vertices[match][j];
        const float u=v->tex_coord.x*512,vv=v->tex_coord.y*2048;
        assert(u>=0 && u<32 && vv>=0 && vv<16);
        float du=u+(u<16?89:0)-b->tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
        float dv=vv+(u<16?103:0)-(b->tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+1);
        du-=roundf(du/4)*4;dv-=roundf(dv/4)*4;
        assert(fabsf(du)<.0002f && fabsf(dv)<.0002f);
        assert(!memcmp(&v->color,&b->color,sizeof(v->color)));
      }
    }
    assert(caps && fronts && backs && shared && before && after);
  }
  /* Moving high-priority owned art into view must not depend on any original
   * high-band pixels or texture having been captured at the destination. */
  const ArRenderTexture original_high=textures[kDioramaPlane_Bg1Hi];
  pixels[kDioramaPlane_Bg1Hi]=NULL;textures[kDioramaPlane_Bg1Hi]=ArRenderTexture_Invalid();
  voxel->pass_counts[0]=0;voxel->pass_counts[1]=edges;voxel->pass_counts[3]=0;
  for(unsigned i=0;i<edges;++i) {
    voxel->edges[i].high_priority=true;voxel->pass_edges[1][i]=i;
    if(voxel->edges[i].colliding && voxel->edges[i].side==0 && voxel->edges[i].far_rank>=2)
      voxel->pass_edges[3][voxel->pass_counts[3]++]=i;
  }
  for(unsigned i=0;i<8;++i)for(unsigned q=0;q<4;++q)voxel->authored[i].priority[q]=1;
  ++voxel->revision;probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid && projection.bg1_high_plane.valid && r.actor_seen);
  unsigned high_fronts=0;
  for(unsigned i=0;i<probe.count;++i)if(probe.faces[i].kind==kDioramaVoxelFace_Front) {
    assert(probe.faces[i].authored && probe.faces[i].priority==1 && probe.faces[i].plane==kDioramaPlane_Bg1Hi);
    assert(probe.submitted[i] && probe.after_actor[i]);++high_fronts;
  }
  assert(high_fronts);
  const unsigned reused_creates=r.creates,reused_uploads=r.uploads;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(r.creates==reused_creates && r.uploads==reused_uploads); /* Resident source, no per-frame upload. */
  /* A top-art edit promotes one member to an authored surface, but its Auto
   * native front still needs the same filtering as the rest of its group. */
  VoxelAuthoredCell *mixed=&voxel->authored[0];mixed->surface=true;mixed->native_transform=false;
  mixed->native_front_hidden=false;--voxel->native_transform_count;++voxel->revision;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  unsigned mixed_fronts=0;
  for(unsigned i=0;i<probe.count;++i)if(probe.faces[i].kind==kDioramaVoxelFace_Front) {
    const VoxelAuthoredCell *cell=ActionVoxel_AuthoredCellAt(voxel,probe.faces[i].cell_x,probe.faces[i].cell_y);
    assert(cell);NativeAffineCheckCrisp(&probe,&r,i,cell);mixed_fronts+=cell==mixed;
  }
  assert(mixed_fronts);
  /* Source-space lighting follows the original artwork, including when the
   * source is offscreen and the affine transform brings it into view. */
  const float cuts[]={0,1};
  const SceneryShadingField exposure={.x=cuts,.y=cuts,.nx=2,.ny=2,.sample=Exposure,.userdata=&r};
  DioramaScene shaded=scene;shaded.bg1_dimming=.5f;shaded.bg1_dimming_ramp=(ArRenderRectF){0,0,.8f,.9f};
  shaded.scenery_exposure=&exposure;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&shaded,&projection)));
  unsigned shaded_fronts=0;
  for(unsigned i=0;i<probe.count;++i)if(probe.faces[i].kind==kDioramaVoxelFace_Front) {
    ++shaded_fronts;
    for(unsigned j=0;j<4;++j) {
      const float u=probe.faces[i].world_xy[j*2]/SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(probe.faces[i].world_xy[j*2+1]-1)/SR_PPU_SURFACE_MAX_HEIGHT;
      const float light=(1-.5f*fminf(1,fmaxf(0,u/.8f))*fminf(1,fmaxf(0,v/.9f)))*
          (.6f+.2f*fmaxf(0,fminf(1,u+v)));
      const ArRenderColorF color=probe.vertices[i][j].color;
      assert(fabsf(color.r-light)<.00001f && fabsf(color.g-light)<.00001f && fabsf(color.b-light)<.00001f);
    }
  }
  assert(shaded_fronts && r.exposure_samples);
  const unsigned shaded_creates=r.creates,shaded_uploads=r.uploads;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&shaded,&projection)));
  assert(r.creates==shaded_creates && r.uploads==shaded_uploads);
  /* An explicit front donor keeps the ordinary authored-art filter. */
  mixed->native_front_art=false;++voxel->revision;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  unsigned custom_fronts=0;
  for(unsigned i=0;i<probe.count;++i)if(probe.faces[i].kind==kDioramaVoxelFace_Front &&
      probe.faces[i].cell_x==mixed->x/16 && probe.faces[i].cell_y==mixed->y/16) {
    assert(probe.face_textures[i].value==probe.texture.value &&
        r.textures[probe.texture.value].filter==kArRenderFilter_Nearest);++custom_fronts;
  }
  assert(custom_fronts);
  DioramaScene inactive=scene;inactive.voxel_geometry=NULL;
  probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&inactive,&projection)));
  assert(!projection.voxel_scenery && !projection.bg1_high_plane.valid && !probe.count);
  textures[kDioramaPlane_Bg1Hi]=original_high;pixels[kDioramaPlane_Bg1Hi]=&present;
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
