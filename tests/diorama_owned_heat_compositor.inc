/* Check the actual producer boundary independently of texture-space phase.
 * Native observer coordinates name the original source; receiver coordinates
 * must name its transformed geometry, including each actor-band cut. */
typedef struct OwnedHeatTransportProbe {
  Recorder *recorder;
  NativeAffineProbe *faces;
  DioramaVoxelTransform transform;
  bool active;
  unsigned skin,finite,crisp,native,before,after;
  unsigned seen[512];
} OwnedHeatTransportProbe;
static bool OwnedHeatDepth(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  OwnedHeatTransportProbe *p=context;const DioramaHeatOwned material=draw->lighting?draw->lighting->heat_owned:(DioramaHeatOwned){0};
  if(material.mode>0) {
    assert(p->active&&draw->heat_world_xy&&draw->clip_w&&!draw->native_actor&&draw->lighting);
    assert(draw->vertex_count%4==0);
    for(int first=0;first<draw->vertex_count;first+=4) {
      unsigned found=p->faces->count;
      for(unsigned f=0;f<p->faces->count&&found==p->faces->count;++f) {
        bool same=true;for(unsigned j=0;j<4;++j)same&=CapPointEqual(draw->vertices[first+j].position,
            (ArRenderPointF){p->faces->faces[f].screen_xy[j*2],p->faces->faces[f].screen_xy[j*2+1]});
        if(same)found=f;
      }
      assert(found<p->faces->count);const DioramaVoxelPickFace *face=&p->faces->faces[found];++p->seen[found];
      const bool finite=face->kind==kDioramaVoxelFace_Front||face->kind==kDioramaVoxelFace_Backing;
      assert(material.mode==(finite?2:1));
      const bool crisp=draw->texture.value==p->faces->crisp_texture.value;
      assert(material.scale==(crisp?4:1));
      const ArRenderTextureDesc *desc=&p->recorder->textures[draw->texture.value];
      assert(fabsf(material.texel_u*desc->width-material.scale)<.00001f);
      assert(fabsf(material.texel_v*desc->height-material.scale)<.00001f);
      if(crisp)++p->crisp;else if(finite)++p->finite;else ++p->skin;
      if(p->recorder->actor_seen)++p->after;else ++p->before;
      for(unsigned j=0;j<4;++j) {
        /* Native edge picks are nudged .01 into their owning source cell;
         * this fixture's integer contour endpoints remain on the real edge. */
        const float sx=face->authored?face->world_xy[j*2]:roundf(face->world_xy[j*2]);
        const float sy=face->authored?face->world_xy[j*2+1]:roundf(face->world_xy[j*2+1]);
        const ArRenderPointF expected={p->transform.px+(sx-p->transform.px)*p->transform.sx+p->transform.dx,
            p->transform.py+(sy-p->transform.py)*p->transform.sy+p->transform.dy};
        if(!CapPointEqual(draw->heat_world_xy[first+j],expected))fprintf(stderr,
            "heat receiver kind%d authored%d source%.6f,%.6f actual%.6f,%.6f expected%.6f,%.6f transform%.3f,%.3f scale%.3f,%.3f\n",
            face->kind,face->authored,sx,sy,draw->heat_world_xy[first+j].x,draw->heat_world_xy[first+j].y,
            expected.x,expected.y,p->transform.dx,p->transform.dy,p->transform.sx,p->transform.sy);
        assert(CapPointEqual(draw->heat_world_xy[first+j],expected));
        /* Offscreen source/donor coordinates and atlas indices are not
         * receiver positions; the visible transformed root remains nearby. */
        assert(expected.x>=79&&expected.x<=113&&expected.y>=85&&expected.y<=113);
        Record(draw->heat_world_xy[first+j].x);Record(draw->heat_world_xy[first+j].y);
      }
    }
  } else {assert(!draw->heat_world_xy);++p->native;}
  return DepthGeometry(p->recorder,device,draw);
}
static unsigned OwnedHeatTransportFixture(unsigned scenario) {
  s_count=0;const bool authored=scenario>=2;static NativeAffineProbe probe,unheated;
  memset(&probe,0,sizeof(probe));Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true,.native_affine=&probe};
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(int i=0;i<kDioramaPlane_Count;++i){assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
      .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;}
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);voxel->active_group=voxel->active_room=1;
  voxel->profile=kActionVoxelProfile_FillmoreForest;voxel->settings=ActionVoxel_SettingsForRoom(1,1);
  voxel->width=1024;voxel->height=768;voxel->ready=true;
  voxel->authored=calloc(1,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)(voxel->width/16)*(voxel->height/16),sizeof(*voxel->authored_at));
  voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));voxel->authored_cap_art=calloc(1,sizeof(*voxel->authored_cap_art));
  voxel->authored_contours=calloc(4,sizeof(*voxel->authored_contours));
  assert(voxel->authored&&voxel->authored_at&&voxel->authored_art&&voxel->authored_cap_art&&voxel->authored_contours);
  voxel->authored_art_count=voxel->authored_art_capacity=voxel->authored_cap_art_count=1;
  voxel->authored_contour_count=voxel->authored_contour_capacity=4;
  const VoxelAuthoredContour outline[]={{0,0,8,0},{8,0,8,0},{0,0,16,3},{15,0,16,1}};
  memcpy(voxel->authored_contours,outline,sizeof(outline));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)voxel->authored_art[0][y*16+x]=NativeAffineColor(x,y);
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.2f,.tilt_y=(scenario&1)?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=NativeAffineObserve,.voxel_observer_userdata=&probe};
  OwnedHeatTransportProbe transport={.recorder=&r,.faces=&probe};
  const DioramaDepthSink sink={.submit=OwnedHeatDepth,.userdata=&transport};
  DioramaScene scene={.render=&options,.depth_sink=&sink,.map_group=1,.map_number=1,.voxel_geometry=voxel,
      .scenery_heat={.source={0,0,SR_PPU_SURFACE_MAX_WIDTH,SR_PPU_SURFACE_MAX_HEIGHT},
          .bounds={{40,60,160,180}},.motion={{0,.71f,0,0}}}};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<5;++frame) {
    const int shift=frame==4?256:0;
    transport.transform=(DioramaVoxelTransform){.px=88+shift,.py=104,.pz=.09f,
        .dx=frame==4?-256:0,.dy=frame==4?-8:0,.sx=frame==4?.75f:1,.sy=1,.sz=1};
    transport.active=frame!=0;transport.skin=transport.finite=transport.crisp=transport.native=transport.before=transport.after=0;
    memset(transport.seen,0,sizeof(transport.seen));scene.scenery_heat.motion[0][0]=frame?3:0;
    capture.bg1_camera_x=frame==3?3:0;view.camera.tilt_y=((scenario&1)?-1:1)*(frame==2?.3f:.4f);
    if(frame==0||frame==4)++voxel->revision;
    voxel->authored_count=authored||frame==4;voxel->native_transform_count=!authored&&frame==4;
    voxel->transformed_rear_count=authored&&frame==4;
    memset(voxel->authored_at,0,(size_t)(voxel->width/16)*(voxel->height/16)*sizeof(*voxel->authored_at));
    VoxelAuthoredCell *a=voxel->authored;*a=(VoxelAuthoredCell){.x=80+shift,.y=96,.width=16,
        .surface=authored,.native_front_art=true,.native_transform=!authored&&frame==4,
        .faces=authored?kDioramaVoxelFaceMask_All:kDioramaVoxelFaceMask_Top|kDioramaVoxelFaceMask_Right|
            kDioramaVoxelFaceMask_Bottom|kDioramaVoxelFaceMask_Left,.transform=transport.transform,.contour_count=4};
    for(unsigned q=0;q<4;++q){a->front[q]=0;a->back[q]=ActionVoxel_Depth(voxel);}
    if(voxel->transformed_rear_count)for(unsigned row=0;row<16;++row)a->native_rear_rows[row]=0xffff;
    for(unsigned f=0;f<7;++f)a->pixels[f]=voxel->authored_art[0];
    if(voxel->authored_count)voxel->authored_at[6*(voxel->width/16)+(80+shift)/16]=1;
    voxel->edge_count=voxel->pass_counts[0]=authored?0:2;
    for(unsigned i=0;i<voxel->edge_count;++i){voxel->pass_edges[0][i]=i;
      voxel->edges[i]=(VoxelEdge){.x=80+shift+i*8,.y=96,.length=8,.side=0,.far_rank=2,.colliding=1,
          .textured_top=1,.surface_texture=1,.top_sample_x=900,.top_sample_y=500,.top_sample_size=16,.top_owned_art=1};}
    probe.count=0;probe.source_shift=shift;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    const unsigned creates=r.creates,uploads=r.uploads;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.count&&r.actor_seen&&transport.native);
    if(frame==2||frame==3)assert(r.creates==creates&&r.uploads==uploads);
    if(!frame){assert(!transport.skin&&!transport.finite&&!transport.crisp);unheated=probe;continue;}
    assert(transport.skin&&transport.before&&!transport.after);
    if(authored){assert(transport.crisp);if(frame==4)assert(transport.finite);}
    for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]&&transport.seen[f]==1);
    if(frame==1){assert(probe.count==unheated.count);for(unsigned f=0;f<probe.count;++f)
      assert(!memcmp(probe.vertices[f],unheated.vertices[f],sizeof(probe.vertices[f])));}
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);return s_count;
}
