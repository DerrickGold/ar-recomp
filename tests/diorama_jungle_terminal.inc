/* Actual compositor proof: terminal source is colored/graded in the existing
 * BG1 subscreen target, with original native low/high actor slots. Include
 * after the JungleWater fixtures (shared recorder light observation hook). */
typedef struct JungleTerminalProbe {
  unsigned frame,low,high,depths,uploads;bool expected;
  bool terminal_texture[128],high_texture[128],after_actor;uint32_t colors[128];
  DioramaSceneryLightEffectParams light;
  uint64_t native_hash;
  float base_shade[2],exposure_u,exposure_v;unsigned exposure_samples,exposure_regions;
} JungleTerminalProbe;
static JungleTerminalProbe *s_jungle_terminal;
static float JungleTerminalExposure(void *userdata,float u,float v,float inside_u,float inside_v) {
  (void)u;(void)v;JungleTerminalProbe *p=userdata;++p->exposure_samples;
  return .35f+(inside_u>p->exposure_u?.2f:0)+(inside_v>p->exposure_v?.4f:0);
}
static void JungleTerminalLight(const DioramaSceneryLightEffectParams *p) {
  if(s_jungle_terminal)s_jungle_terminal->light=*p;
}
static void JungleTerminalActor(void *userdata,int plane,const DioramaProjection *projection) {
  (void)userdata;(void)projection;
  if(s_jungle_terminal&&plane==kDioramaPlane_Obj2)s_jungle_terminal->after_actor=true;
}
static bool JungleTerminalUpdate(void *ctx,ArRenderTexture texture,const ArRenderRectI *rect,
    const void *pixels,int pitch) {
  Recorder *r=ctx;JungleTerminalProbe *p=s_jungle_terminal;
  if(p&&r->textures[texture.value].width==1&&r->textures[texture.value].height==1&&pixels) {
    const uint32_t color=*(const uint32_t *)pixels;
    ActionRoomSceneFrameState full={.brightness=15};
    if(color==ActionVoxel_AuthoredColor(0x954a,&full)||color==ActionVoxel_AuthoredColor(0xa0e5,&full)||
        (color==0xff000000&&p->terminal_texture[texture.value])) {
      assert(pitch==4);if(color!=0xff000000)p->high_texture[texture.value]=color==ActionVoxel_AuthoredColor(0xa0e5,&full);
      p->terminal_texture[texture.value]=true;p->colors[texture.value]=color;
      ++p->uploads;++r->uploads;return true;
    }
  }
  return Update(ctx,texture,rect,pixels,pitch);
}
static void JungleTerminalHash(const void *data,size_t size) {
  const unsigned char *b=data;for(size_t i=0;i<size;++i)
    s_jungle_terminal->native_hash=(s_jungle_terminal->native_hash^b[i])*UINT64_C(1099511628211);
}
static bool JungleTerminalGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const ArRenderDrawState *state) {
  Recorder *r=ctx;JungleTerminalProbe *p=s_jungle_terminal;
  if(p&&texture.value<128&&p->terminal_texture[texture.value]) {
    assert(p->expected&&nv==4&&ni==6&&state&&state->blend==kArRenderBlendMode_Alpha);
    assert(r->state.target.value&&r->bound_effect==kDioramaEffect_Count);
    assert(p->light.grade.strength==.48f&&p->light.grade.saturation==1.25f);
    assert(p->light.mode==0); /* Native color is not an untextured/priority atlas path. */
    assert(p->high_texture[texture.value]==p->after_actor);
    if(p->high_texture[texture.value])++p->high;else ++p->low;
    const unsigned high=p->high_texture[texture.value];
    if(p->frame==0)p->base_shade[high]=v[0].color.r;
    if(p->frame==9) {
      float u0=v[0].tex_coord.x,u1=u0,v0=v[0].tex_coord.y,v1=v0;
      for(int i=1;i<nv;++i){u0=fminf(u0,v[i].tex_coord.x);u1=fmaxf(u1,v[i].tex_coord.x);
        v0=fminf(v0,v[i].tex_coord.y);v1=fmaxf(v1,v[i].tex_coord.y);}
      assert(!(u0<p->exposure_u-.000001f&&u1>p->exposure_u+.000001f));
      assert(!(v0<p->exposure_v-.000001f&&v1>p->exposure_v+.000001f));
      const unsigned region=((u0+u1)*.5f>p->exposure_u?1:0)|((v0+v1)*.5f>p->exposure_v?2:0);
      p->exposure_regions|=1u<<region;
      const float expected=p->base_shade[high]*(.35f+((region&1)?.2f:0)+((region&2)?.4f:0));
      for(int i=0;i<nv;++i)assert(fabsf(v[i].color.r-expected)<.000001f);
    }
    for(int i=0;i<nv;++i) {
      assert(isfinite(v[i].position.x)&&isfinite(v[i].position.y));
      assert(v[i].color.a==1&&v[i].tex_coord.x!=.5f);
    }
    if(p->frame==8)assert(p->colors[texture.value]==0xff000000);
  } else if(p) {
    JungleTerminalHash(&texture.value,sizeof(texture.value));JungleTerminalHash(&nv,sizeof(nv));
    JungleTerminalHash(&ni,sizeof(ni));
    for(int i=0;i<nv;++i) {
      JungleTerminalHash(&v[i].position,sizeof(v[i].position));
      JungleTerminalHash(&v[i].tex_coord,sizeof(v[i].tex_coord));
      JungleTerminalHash(&v[i].color,sizeof(v[i].color));
    }
    JungleTerminalHash(indices,(size_t)ni*sizeof(*indices));
  }
  return Geometry(ctx,texture,v,nv,indices,ni,state);
}
static bool JungleTerminalDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  Recorder *r=ctx;JungleTerminalProbe *p=s_jungle_terminal;
  if(p&&draw->texture.value<128&&p->terminal_texture[draw->texture.value]) {
    assert(draw->lighting&&draw->depths&&draw->clip_w&&draw->blend==kArRenderBlendMode_Alpha);
    assert(draw->plane==SR_PPU_OVERLAY_BG1||draw->plane==kDioramaPlane_Bg1Hi);
    assert((draw->plane==kDioramaPlane_Bg1Hi)==p->after_actor);
    const DioramaPlaneProjection *plane=draw->plane==SR_PPU_OVERLAY_BG1?
      &r->projection->bg1_plane:&r->projection->bg1_high_plane;
    assert(plane->valid);++p->depths;
    for(int i=0;i<draw->vertex_count;++i)assert(draw->clip_w[i]>0&&
      fabsf(draw->depths[i]-PlaneDepth(r->projection,plane->z_world,draw->vertices[i].position))<.000002f);
  }
  return DepthGeometry(ctx,device,draw);
}
static unsigned JungleTerminalFixture(unsigned scenario) {
  assert(scenario<4);s_count=0;s_depth_fixture=!!(scenario&2);
  Recorder r={.width=960,.height=598,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.update_texture=JungleTerminalUpdate;ops.draw_geometry=JungleTerminalGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=5;g->active_room=3;g->profile=kActionVoxelProfile_MarahnaJungle;
  g->settings=ActionVoxel_SettingsForRoom(5,3);g->origin_x=-16;g->origin_y=-96;g->width=528;g->height=400;g->ready=true;
  VoxelJungleTerminal *terminal=calloc(1,sizeof(*terminal));assert(terminal);
  *terminal=(VoxelJungleTerminal){.left=-16,.top=-96,.right=512,.bottom=304,.count=4,
    .runs={{.start=-96,.end=216,.edge=1,.color=0x954a,.palette_index=123},
      {.start=216,.end=304,.edge=1,.color=0xa0e5,.palette_index=81,.high_priority=true},
      {.start=-16,.end=512,.edge=2,.color=0xa0e5,.palette_index=81,.high_priority=true},
      {.start=208,.end=304,.edge=3,.color=0xa0e5,.palette_index=81,.high_priority=true}}};
  ActionRoomSceneFrameState frame={.brightness=15,.cgwsel=2,.cgadsub=2,.screen_enabled={6,17}};
  DioramaCapture capture={.voxel_frame=&frame,.width=624,.height=352,.authentic_y0=64,
    .bg1_camera_x=scenario&1?146:94,.camera_y=16,.textures=textures,.pixels=pixels};
  /* A slightly wider CPU lens exercises both priority planes at once; the
   * retained four artistic poses remain unchanged and are a separate gate. */
  DioramaView view={.viewport={0,1,960,597},.visible_width=360,.distance_scale=1.2f,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f}};
  DioramaRenderOptions options={.skybox=kDioramaSky_Only,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  DioramaDepthSink sink={.submit=JungleTerminalDepth,.userdata=&r};
  const uint32_t subscreen=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Hi)|
    (1u<<kDioramaPlane_Bg1Far)|(1u<<kDioramaPlane_Obj2);
  DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,.map_group=5,.map_number=3,
    .voxel_mode=true,.voxel_geometry=g,.additive_plane_mask=subscreen,
    .plane_effect=JungleTerminalActor,
    .scenery_grade={.set=true,.strength=.48f,.saturation=1.25f,.shadows=0x34675e,.highlights=0xd9d38b}};
  JungleTerminalProbe probe={0};s_jungle_terminal=&probe;s_waterfall_cloud_observe=JungleTerminalLight;
  s_jungle_water_fixture=true;r.capture=&capture;r.actor_texture=textures[kDioramaPlane_Obj2];
  probe.exposure_u=(520-(capture.bg1_camera_x-(capture.width-256)*.5f-capture.obj_apron))/SR_PPU_SURFACE_MAX_WIDTH;
  probe.exposure_v=(304-(capture.camera_y-capture.authentic_y0+1.f))/SR_PPU_SURFACE_MAX_HEIGHT;
  const SceneryShadingField exposure={.x=&probe.exposure_u,.y=&probe.exposure_v,.nx=1,.ny=1,
    .sample=JungleTerminalExposure,.userdata=&probe};
  uint64_t native_hash=0;
  for(unsigned f=0;f<10;++f) {
    g->jungle_terminal=f==1?NULL:terminal;scene.voxel_mode=f!=3;scene.additive_plane_mask=f==4?0:subscreen;
    pixels[SR_PPU_OVERLAY_BG2]=f==5?NULL:&present;frame.cgadsub=f==6?0x42:2;
    options.skybox=f==7?kDioramaSky_Off:kDioramaSky_Only;frame.brightness=f==8?0:15;
    probe.frame=f;probe.after_actor=false;probe.low=probe.high=probe.depths=0;probe.expected=f==0||f==2||f==8||f==9;
    scene.scenery_exposure=f==9?&exposure:NULL;
    probe.native_hash=UINT64_C(1469598103934665603);r.actor_seen=false;
    DioramaProjection projection;r.projection=&projection;
    const unsigned uploads=probe.uploads;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if(!(projection.valid&&!!probe.high==probe.expected))fprintf(stderr,
      "terminal scenario%u frame%u expected%d low%u high%u uploads%u actor%d valid%d sourceRect %.5g %.5g %.5g %.5g\n",
      scenario,f,probe.expected,probe.low,probe.high,probe.uploads,r.actor_seen,projection.valid,
      projection.bg1_high_plane.u0,projection.bg1_high_plane.v0,
      projection.bg1_high_plane.u1,projection.bg1_high_plane.v1);
    assert(projection.valid&&!!probe.high==probe.expected);
    if(!(scenario&1)&&!!probe.low!=probe.expected) {
      CastleMatteProbe cp={.capture=&capture,.projection=&projection};
      for(unsigned corner=0;corner<4;++corner) {
        ArRenderPointF w=CastleMatteWorldPoint(&cp,(ArRenderPointF){corner&1?960:0,corner&2?597:0});
        fprintf(stderr,"terminal low footprint corner%u %.5g %.5g\n",corner,w.x,w.y);
      }
    }
    if(!(scenario&1))assert(!!probe.low==probe.expected);
    if(!probe.expected)assert(!probe.low&&!probe.high&&!probe.depths);
    assert(probe.depths==(s_depth_fixture?probe.low+probe.high:0));
    if(f==0)native_hash=probe.native_hash;
    if(f==1||f==2)assert(probe.native_hash==native_hash);
    if(f==2)assert(probe.uploads==uploads);
    if(f==9) {
      assert(probe.exposure_samples>0&&probe.exposure_regions);
      assert((probe.exposure_regions&(probe.exposure_regions-1))!=0); /* Actual hard-cut sides both exercised. */
    }
    assert(!r.state.target.value&&!r.batch_open);
  }
  g->jungle_terminal=terminal;s_jungle_terminal=NULL;s_waterfall_cloud_observe=NULL;s_jungle_water_fixture=false;
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  printf("Jungle terminal compositor%u: nativeTS source/grade/shade, low/high actor phase/depth, untouched native draws, cachedcolors, mode/math/source negatives, exact hard-edge exposure with per-vertex W\n",scenario);
  return s_count;
}
