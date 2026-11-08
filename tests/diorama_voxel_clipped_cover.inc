/* Capture boundaries can reduce a real contour to a point. Such a contour has
 * no area and must not reach snow support association or abort the frame. */
typedef struct ClippedCoverProbe { unsigned starts, supports, ends, faces; } ClippedCoverProbe;
static void ClippedCoverObserve(void *userdata,const DioramaGroundCoverObservation *o) {
  ClippedCoverProbe *p=userdata;assert(o->complete);
  if(o->event==kDioramaGroundCoverBegin)++p->starts;
  else if(o->event==kDioramaGroundCoverSupport) {
    ++p->supports;assert(o->support_xy[2]>o->support_xy[0]);
    assert(o->support_xy[0]>=80&&o->support_xy[2]<=84);
  } else {++p->ends;assert(o->caps&&o->triangles);}
}
static void ClippedCoverFace(void *userdata,const DioramaVoxelPickFace *f) {
  if(f->kind!=kDioramaVoxelFace_Top&&f->kind!=kDioramaVoxelFace_Side)return;
  ClippedCoverProbe *p=userdata;++p->faces;
  const float dx=f->world_xy[2]-f->world_xy[0],dy=f->world_xy[3]-f->world_xy[1];
  assert(dx*dx+dy*dy>0);
}
static bool ClippedCoverDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  if(!draw->texture.value) {
    assert(!draw->lighting&&draw->blend==kArRenderBlendMode_Alpha&&draw->clip_w);
    for(int i=0;i<draw->vertex_count;++i)assert(isfinite(draw->depths[i])&&draw->clip_w[i]>0);
    return true;
  }
  return DepthGeometry(ctx,device,draw);
}
static unsigned ClippedCoverFixture(unsigned scenario) {
  s_count=0;const bool shared=scenario<2,right=scenario&1;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},textures+i));pixels[i]=&present;
  }
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=6;g->active_room=5;g->profile=kActionVoxelProfile_NorthwallSnow;
  g->settings=ActionVoxel_SettingsForRoom(6,5);g->width=256;g->height=224;g->ready=true;
  g->front=calloc(256*224,1);g->soft_surfaces=calloc(16*14,1);assert(g->front&&g->soft_surfaces);
  memset(g->soft_surfaces,1,16*14);
  for(unsigned x=80;x<84;++x)for(unsigned y=96;y<104;++y)g->front[y*256+x]=kVoxelSolid;
  g->edges[0]=(VoxelEdge){.x=80,.y=96,.length=4,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edges[1]=(VoxelEdge){.x=-1,.y=96,.length=1,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edges[2]=(VoxelEdge){.x=256,.y=96,.length=1,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edges[3]=(VoxelEdge){.x=80,.y=0,.length=1,.side=1,.far_rank=2,.colliding=1};
  g->edges[4]=(VoxelEdge){.x=80,.y=225,.length=1,.side=1,.far_rank=2,.colliding=1};
  g->edge_count=g->pass_counts[0]=5;for(unsigned i=0;i<5;++i)g->pass_edges[0][i]=i;
  ActionSceneEffectFrame effects={.authored_count=1};
  effects.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,.generation=101,
      .flags=kActionEffectFlag_Visible,.world_y=96,.render_layer=kActionEffectRenderLayer_WorldDust,
      .projection_plane=kActionEffectProjectionPlane_Bg1,.tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},
      .particle_style={.color_end=0xfff4df,.seed=621},
      .field_style={.placement=2,.ground_depth=4,.ground_width=8,.point_count=2,.points={{-16,0},{272,0}}}};
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=right?.3f:-.3f}};
  ClippedCoverProbe probe={0};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=ClippedCoverFace,.voxel_observe_ground_cover=ClippedCoverObserve,
      .voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=ClippedCoverDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=shared?&sink:NULL,.map_group=6,.map_number=5,
      .voxel_geometry=g,.trunk_effects=&effects};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid&&probe.starts&&probe.supports&&probe.ends&&probe.faces);
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);return s_count;
}
