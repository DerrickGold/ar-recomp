/* Original quad/source coordinates, actor phase and light material survive
 * replacing the stretched wave source with two native-color1x1 textures. */
typedef struct JungleWaterProbe {
  unsigned frame,caps,updates;bool depth;
  uint32_t pixels[128];ArRenderVertex2D baseline[2][4];float depths[2][4];
  DioramaSceneryLightEffectParams light,baseline_light[2];
  bool have_light,actor_reached,actor[2];ArRenderBlendMode blend[2];
} JungleWaterProbe;
static JungleWaterProbe *s_jungle_water;
static void JungleWaterLight(const DioramaSceneryLightEffectParams *p) {
  if(s_jungle_water){s_jungle_water->light=*p;s_jungle_water->have_light=true;}
}
static void JungleWaterActor(void *userdata,int plane,const DioramaProjection *projection) {
  (void)userdata;(void)projection;
  if(s_jungle_water&&plane==kDioramaPlane_Obj2)s_jungle_water->actor_reached=true;
}
static bool JungleWaterUpdate(void *ctx,ArRenderTexture t,const ArRenderRectI *rect,const void *pixels,int pitch) {
  Recorder *r=ctx;
  if(s_jungle_water&&r->textures[t.value].width==1&&r->textures[t.value].height==1) {
    assert(pixels&&pitch==4);s_jungle_water->pixels[t.value]=*(const uint32_t *)pixels;
    ++s_jungle_water->updates;++r->uploads;return true;
  }
  return Update(ctx,t,rect,pixels,pitch);
}
static bool JungleWaterQuad(const ArRenderVertex2D *v,int nv,int ni) {
  return nv==4&&ni==6&&fabsf(v[0].tex_coord.y-172.5f/352)<.000001f&&
      fabsf(v[2].tex_coord.y-174.5f/352)<.000001f&&fabsf(v[0].tex_coord.x-80.5f/640)<.000001f;
}
static bool JungleWaterGeometry(void *ctx,ArRenderTexture t,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const ArRenderDrawState *state) {
  Recorder *r=ctx;JungleWaterProbe *p=s_jungle_water;
  if(p&&JungleWaterQuad(v,nv,ni)) {
    const unsigned term=p->caps++;assert(term<2&&state);
    /* BG2 remains an ungraded main-screen term; BG1 retains its existing
     * scenery shader. Both must preserve the same pre/post material policy. */
    if(term) {
      assert(p->have_light&&r->bound_effect==kDioramaEffect_Count);
      if(!p->frame)fprintf(stderr,"water baseline lightUV %.9g %.9g %.9g %.9g grade%.9g depth%d\n",p->light.light_uv.x,p->light.light_uv.y,p->light.light_uv.w,p->light.light_uv.h,p->light.grade.strength,p->depth);
      assert(p->light.grade.strength==.48f);
      assert(!p->light.light_uv.x&&!p->light.light_uv.y&&!p->light.light_uv.w&&!p->light.light_uv.h);
    }
    else assert(r->bound_effect==-1);
    if(!p->frame) {
      memcpy(p->baseline[term],v,sizeof(p->baseline[term]));p->actor[term]=p->actor_reached;
      p->blend[term]=state->blend;if(term)p->baseline_light[term]=p->light;
    } else {
      assert(!memcmp(p->baseline[term],v,sizeof(p->baseline[term])));
      assert(p->actor[term]==p->actor_reached&&p->blend[term]==state->blend);
      if(term)assert(!memcmp(&p->light,&p->baseline_light[term],sizeof(p->light)));
    }
    const bool uniform=p->frame>=1&&p->frame<=4;
    if(uniform) {
      assert(r->textures[t.value].width==1&&r->textures[t.value].height==1);
      const uint32_t expected=p->frame==3?0xff000000u:term?0xff424a39u:0xff846363u;
      assert(p->pixels[t.value]==expected);
    } else assert(r->textures[t.value].width>1);
  }
  return Geometry(ctx,t,v,nv,indices,ni,state);
}
static bool JungleWaterDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  JungleWaterProbe *p=s_jungle_water;
  if(p&&JungleWaterQuad(draw->vertices,draw->vertex_count,draw->index_count)) {
    assert(draw->lighting&&draw->texture.value&&draw->depths&&p->caps<2);
    assert(draw->plane==(p->caps?kDioramaPlane_Bg1Hi:SR_PPU_OVERLAY_BG2));
    if(!p->frame)memcpy(p->depths[p->caps],draw->depths,sizeof(p->depths[0]));
    else assert(!memcmp(p->depths[p->caps],draw->depths,sizeof(p->depths[0])));
  }
  return DepthGeometry(ctx,device,draw);
}
static unsigned JungleWaterColorFixture(unsigned scenario) {
  assert(scenario<4);s_count=0;s_depth_fixture=!!(scenario&2);
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.update_texture=JungleWaterUpdate;ops.draw_geometry=JungleWaterGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=5;voxel->active_room=2;voxel->profile=kActionVoxelProfile_MarahnaJungle;
  voxel->settings=ActionVoxel_SettingsForRoom(5,2);voxel->width=1024;voxel->height=512;voxel->ready=true;
  voxel->water_surface_count=1;voxel->water_surfaces[0]=(VoxelWaterSurface){.x=80,.y=160,.width=64,
      .high_priority=1,.sample_y=13,.sample_height=3,.opaque=1};
  ActionRoomSceneFrameState frame={.brightness=15};
  DioramaCapture capture={.voxel_frame=&frame,.width=256,.height=224,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={8,10,944,580},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f}};
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  DioramaDepthSink sink={.submit=JungleWaterDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,.map_group=5,.map_number=2,
      .voxel_geometry=voxel,.plane_effect=JungleWaterActor,.plane_effect_userdata=&r,.scenery_grade={.set=true,.strength=.48f,.saturation=1.25f,.shadows=0x34675e,.highlights=0xd9d38b},
      .additive_plane_mask=(1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Obj2)};
  JungleWaterProbe probe={.depth=s_depth_fixture};s_jungle_water=&probe;s_jungle_water_fixture=true;s_waterfall_cloud_observe=JungleWaterLight;
  r.actor_texture=textures[kDioramaPlane_Obj2];r.capture=&capture;
  for(unsigned f=0;f<7;++f) {
    probe.frame=f;probe.caps=0;probe.have_light=false;probe.actor_reached=false;r.actor_seen=false;
    voxel->water_surfaces[0].uniform_color[0]=f?0x9d28:0;
    voxel->water_surfaces[0].uniform_color[1]=f&&f!=5?0xb190:0;
    frame.brightness=f==3?0:15;scene.voxel_geometry=f==6?NULL:voxel;
    const unsigned uploads=probe.updates,creates=r.creates;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.caps==(f==6?0u:2u));
    if(f==2)assert(probe.updates==uploads&&r.creates==creates);
    assert(!r.state.target.value&&!r.batch_open);
  }
  assert(!probe.actor[0]&&probe.actor[1]);
  s_waterfall_cloud_observe=NULL;s_jungle_water=NULL;s_jungle_water_fixture=false;
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  return s_count;
}
