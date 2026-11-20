typedef struct RoundedTrunkProbe { Recorder *recorder;unsigned vertices;bool canopy; } RoundedTrunkProbe;
static bool RoundedTrunkDepth(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  RoundedTrunkProbe *p=context;
  if(!draw->native_actor && draw->plane==kDioramaPlane_Bg1Far)for(int i=0;i+3<draw->vertex_count;i+=4) {
    bool inside=true;
    for(unsigned v=0;v<4;++v) {
      const float x=draw->vertices[i+v].tex_coord.x*p->recorder->capture->width,
          y=draw->vertices[i+v].tex_coord.y*p->recorder->capture->height;
      inside&=x>=79.99f && x<=(p->canopy?96.01f:112.01f) && y>=95.99f && y<=176.01f;
    }
    if(!inside)continue;
    for(unsigned v=0;v<4;++v) {
      const ArRenderVertex2D *vertex=draw->vertices+i+v;float xyz[3];
      const float x=vertex->tex_coord.x*p->recorder->capture->width;
      const float y=vertex->tex_coord.y*p->recorder->capture->height;
      const float t=fmaxf(0,fminf(1,(x-80)/32));
      const float phase=fmaxf(0,fminf(1,(y+1-(112+(x-80)*1.5f))/fmaxf(1,32-(x-80))));
      const float expected=p->canopy?.15f*phase*phase*(3-2*phase):.15f*(1-sqrtf(fmaxf(0,1-(1-t)*(1-t))));
      const DioramaProjection *projection=p->recorder->projection;
      const float scale=1+expected/projection->camera_distance;
      const float px=(x/p->recorder->capture->width-.5f)*projection->aspect_x*scale,
          py=(.5f-y/p->recorder->capture->height)*projection->height_scale*scale;
      const float *m=projection->matrix;
      /* Native front submissions keep affine UVs; derive the expected W
       * from the independent world profile and verify the submitted depth. */
      const float w=m[3]*px+m[7]*py-m[11]*expected+m[15];
      GroundCoverNativePoint(p->recorder,vertex,draw->depths[i+v],w,xyz);
      assert(fabsf(xyz[0]-x)<.01f && fabsf(xyz[1]-(y+1))<.01f && fabsf(xyz[2]-expected)<.0001f);
      ++p->vertices;
    }
  }
  return DepthGeometry(p->recorder,device,draw);
}
static unsigned RoundedTrunkFixture(unsigned angle) {
  const bool canopy=angle>=2;
  s_count=0;Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=6;g->active_room=5;g->profile=kActionVoxelProfile_NorthwallSnow;
  g->settings=(ActionVoxelSettings){.depth=.2f,.play_recess=.065f};g->width=g->height=256;g->ready=true;
  g->front=calloc(256,256);g->column_cells=calloc(32,32);assert(g->front && g->column_cells);
  g->column_curve_count=1;g->column_curves[0]=(VoxelTrunkCurve){.round_x=80,.round_width=32};
  if(canopy) {
    g->column_curves[0]=(VoxelTrunkCurve){.contact_x=80,.contact_count=9,.native_lip=1};
    for(unsigned x=0;x<9;++x) {
      g->column_curves[0].contact[x]=128+x;g->column_curves[0].canopy_top[x]=112+3*x;
    }
  }
  for(unsigned y=96;y<176;++y)for(unsigned x=80;x<(canopy?96:112);++x) {
    g->front[y*256+x]=kVoxelSolid;g->column_cells[(y/8)*32+x/8]=1;
  }
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=canopy?-.45f:.3f,.tilt_y=angle&1?-.5f:.5f,.distance=3.25f}};
  RoundedTrunkProbe probe={.recorder=&r,.canopy=canopy};const DioramaDepthSink sink={.submit=RoundedTrunkDepth,.userdata=&probe};
  DioramaScene scene={.render=&options,.map_group=6,.map_number=5,.voxel_geometry=g,.depth_sink=&sink};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;ReviseVoxelFixture(g);
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(probe.vertices>=80);
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
