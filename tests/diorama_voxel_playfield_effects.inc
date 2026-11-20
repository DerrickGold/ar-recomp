/* Floor atmosphere must be submitted after generated walking caps and before
 * the player, regardless of which level supplied the voxel geometry. */
typedef struct PlayfieldEffectProbe { bool floor;unsigned effects,actors; } PlayfieldEffectProbe;
static void PlayfieldEffectFace(void *userdata,const DioramaVoxelPickFace *face) {
  PlayfieldEffectProbe *p=userdata;
  if(face->plane==kDioramaPlane_Bg1Far && face->kind==kDioramaVoxelFace_Top)p->floor=true;
}
static void PlayfieldEffectSlot(void *userdata,int plane,const DioramaProjection *projection) {
  PlayfieldEffectProbe *p=userdata;assert(projection->voxel_scenery);
  if(plane==SR_PPU_OVERLAY_BG1) {assert(p->floor&&!p->actors);++p->effects;}
  if(plane==kDioramaPlane_Obj2) {assert(p->effects==1);++p->actors;}
}
static unsigned PlayfieldEffectFixture(unsigned profile) {
  s_count=0;Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=5;g->active_room=4;g->profile=(ActionVoxelProfile)profile;
  g->settings=(ActionVoxelSettings){.depth=.24f,.play_recess=.08f};g->width=1024;g->height=512;g->ready=true;
  g->edge_count=1;g->edges[0]=(VoxelEdge){.x=80,.y=176,.length=128,.side=0,.far_rank=2,.colliding=1};
  g->pass_counts[0]=1;g->pass_edges[0][0]=0;
  PlayfieldEffectProbe probe={0};
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
    .voxel_observe_face=PlayfieldEffectFace,.voxel_observer_userdata=&probe};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.5f,.tilt_y=.4f}};
  const DioramaScene scene={.render=&options,.map_group=5,.map_number=4,.voxel_geometry=g,
    .effect_bg_plane_mask=1u<<SR_PPU_OVERLAY_BG1,.effect_obj_priority_mask=1u<<2,
    .plane_effect=PlayfieldEffectSlot,.plane_effect_userdata=&probe};
  for(unsigned angle=0;angle<2;++angle) {
    view.camera.tilt_y=angle?-.4f:.4f;probe=(PlayfieldEffectProbe){0};
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(probe.floor&&probe.effects==1&&probe.actors==1);
  }
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
