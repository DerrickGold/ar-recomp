/* Include after the existing compositor fixtures. No production recorder
 * mutation: wrappers observe actual source submissions and shared depth. */
typedef struct NebulaProbe {
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  ArRenderTexture textures[kDioramaPlane_Count];
  unsigned geometry_hash, actor_hash, background_vertices, depth_vertices;
  unsigned order[128], order_count;
  bool expected;
  int visible_width;
} NebulaProbe;
static NebulaProbe *s_nebula_probe;

static unsigned NebulaHash(unsigned hash,const void *bytes,size_t size) {
  const unsigned char *p=bytes;
  for(size_t i=0;i<size;++i) hash=(hash^p[i])*16777619u;
  return hash;
}
static int NebulaSource(Recorder *r,ArRenderTexture texture) {
  if(r->copied_sources[texture.value].value)texture=r->copied_sources[texture.value];
  for(unsigned i=0;i<kDioramaPlane_Count;++i)
    if(ArRenderTexture_Equals(texture,s_nebula_probe->textures[i]))return (int)i;
  return -1;
}
static bool NebulaBackground(int plane) {
  return plane==SR_PPU_OVERLAY_BG1||plane==kDioramaPlane_Bg1Hi||plane==kDioramaPlane_Bg1Far;
}
static const DioramaPlaneProjection *NebulaPlane(const DioramaProjection *p,int plane) {
  return plane==SR_PPU_OVERLAY_BG1?&p->bg1_plane:
      plane==kDioramaPlane_Bg1Hi?&p->bg1_high_plane:&p->bg1_far_plane;
}
static bool NebulaTexture(void *ctx,ArRenderTexture texture,
    const ArRenderRectF *source,const ArRenderRectF *destination,const ArRenderDrawState *state) {
  Recorder *r=ctx;
  if(r->state.target.value)r->copied_sources[r->state.target.value]=
      r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  return Texture(ctx,texture,source,destination,state);
}
static bool NebulaGeometry(void *ctx,ArRenderTexture texture,
    const ArRenderVertex2D *v,int count,const int32_t *indices,int index_count,
    const ArRenderDrawState *state) {
  NebulaProbe *p=s_nebula_probe;
  Recorder *r=ctx;
  const int plane=NebulaSource(r,texture);
  p->geometry_hash=NebulaHash(p->geometry_hash,&plane,sizeof(plane));
  p->geometry_hash=NebulaHash(p->geometry_hash,v,(size_t)count*sizeof(*v));
  p->geometry_hash=NebulaHash(p->geometry_hash,indices,(size_t)index_count*sizeof(*indices));
  if(plane>=0) {
    assert(p->order_count<128);p->order[p->order_count++]=(unsigned)plane;
    if(!NebulaBackground(plane)) {
      p->actor_hash=NebulaHash(p->actor_hash,&plane,sizeof(plane));
      p->actor_hash=NebulaHash(p->actor_hash,v,(size_t)count*sizeof(*v));
      p->actor_hash=NebulaHash(p->actor_hash,indices,(size_t)index_count*sizeof(*indices));
    }
  }
  if(p->expected && NebulaBackground(plane)) {
    const DioramaPlaneProjection *mapping=NebulaPlane(p->projection,plane);
    assert(mapping->valid&&mapping->native_atmosphere);
    assert(count==9*(kDioramaPlaneSubdivY+1));
    assert(index_count==8*kDioramaPlaneSubdivY*6);
    const float left=p->capture->obj_apron+(p->capture->width-p->visible_width)*.5f;
    for(int i=0;i<count;++i) {
      const float s=(i%9)/8.f,t=(i/9)/(float)kDioramaPlaneSubdivY;
      assert(fabsf(v[i].position.x-s*p->projection->output_width)<.001f);
      assert(fabsf(v[i].position.y-t*p->projection->output_height)<.001f);
      ArRenderPointF uv=v[i].tex_coord;
      if(r->copied_sources[texture.value].value) {
        uv.x=(uv.x*p->capture->width+p->capture->obj_apron)/640;
        uv.y=uv.y*p->capture->height/352;
      }
      assert(fabsf(uv.x*640-(left+s*p->visible_width))<.001f);
      assert(fabsf(uv.y*352-(p->capture->authentic_y0+t*224))<.001f);
      ArRenderPointF projected;
      bool ok=plane==SR_PPU_OVERLAY_BG1
          ?Diorama_ProjectCapturedBg1Point(p->projection,uv.x*640-p->capture->obj_apron,
              uv.y*352,&projected,NULL,NULL)
          :plane==kDioramaPlane_Bg1Hi
          ?Diorama_ProjectCapturedBg1HighPoint(p->projection,uv.x*640-p->capture->obj_apron,
              uv.y*352,&projected,NULL,NULL)
          :Diorama_ProjectCapturedBg1FarPoint(p->projection,uv.x*640-p->capture->obj_apron,
              uv.y*352,&projected,NULL,NULL);
      assert(ok && fabsf(projected.x-p->projection->output_x-v[i].position.x)<.002f &&
          fabsf(projected.y-p->projection->output_y-v[i].position.y)<.002f);
      ++p->background_vertices;
    }
  }
  return Geometry(ctx,texture,v,count,indices,index_count,state);
}
static bool NebulaDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  NebulaProbe *p=s_nebula_probe;
  if(p->expected&&NebulaBackground(draw->plane)) {
    const DioramaPlaneProjection *plane=NebulaPlane(p->projection,draw->plane);
    assert(plane->native_atmosphere&&!draw->native_actor);
    for(int i=0;i<draw->vertex_count;++i) {
      /* Independent ray/plane oracle already used by actor tests. The new
       * source mapping must not flatten or replace the native depth plane. */
      assert(fabsf(draw->depths[i]-PlaneDepth(p->projection,plane->z_world,
          draw->vertices[i].position))<.000002f);
      ++p->depth_vertices;
    }
  }
  return DepthGeometry(ctx,device,draw);
}

static unsigned DeathHeimNebulaFixture(unsigned scenario) {
  assert(scenario<8);s_count=0;
  const bool shared=(scenario&1)!=0;
  const bool expected=scenario<4;
  unsigned before_geometry=0,before_actor=0,before_order_count=0,before_order[128]={0};
  DioramaProjection before_projection={0};
  for(unsigned candidate=0;candidate<2;++candidate) {
    Recorder r={.width=1280,.height=800,.bound_effect=-1};
    ArRenderBackendOps ops=kOps;ops.draw_geometry=NebulaGeometry;ops.draw_texture=NebulaTexture;
    ArRenderDevice device;
    assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
    ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count]={0};
    const uint8_t present=255;
    for(unsigned i=0;i<kDioramaPlane_Count;++i) {
      assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,
          .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));
      pixels[i]=&present;
    }
    const int top=(int[]){1,17,32,17,17,17,17,17}[scenario];
    DioramaCapture capture={.width=624,.height=256,.authentic_y0=top,
        .obj_apron=8,.textures=textures,.pixels=pixels};
    DioramaView view={.viewport={0,0,1280,800},.visible_width=360,.visible_height=224,
        .distance_scale=1,.camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f}};
    DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
        .skybox=kDioramaSky_Off};
    DioramaDepthSink sink={.userdata=&r,.submit=NebulaDepth};
    ActionVoxelGeometry *empty=shared?ActionVoxel_Create():NULL;
    if(empty) {
      empty->ready=true;empty->active_group=7;empty->active_room=8;
      empty->profile=kActionVoxelProfile_DeathHeim;
      empty->settings=ActionVoxel_SettingsForRoom(7,8);
      empty->width=empty->height=256;
    }
    DioramaScene scene={.render=&options,.map_group=7,.map_number=8,
        .voxel_mode=candidate!=0,.depth_sink=shared?&sink:NULL,.voxel_geometry=empty};
    /* Portable positives have NULL geometry. Shared-depth variants use an
     * empty ready arena because the existing depth-sink contract requires
     * that owner, independently of the new atmospheric eligibility. */
    if(scenario==4)scene.voxel_mode=false;
    if(scenario==5)scene.map_number=1;
    if(scenario==6)scene.layer_section=kDioramaLayerSection_DeathHeimCompletion;
    if(scenario==7) {
      /* Edge-on plane: shared preflight must retain BOTH original mesh and
       * effect mapping, rather than aborting the frame or flattening depth. */
      view.camera.tilt_x=0;view.camera.tilt_y=1.5707963267948966f;
    }
    DioramaProjection projection;
    NebulaProbe probe={.capture=&capture,.projection=&projection,.expected=candidate&&expected,
        .visible_width=view.visible_width,.geometry_hash=2166136261u,.actor_hash=2166136261u};
    memcpy(probe.textures,textures,sizeof(textures));s_nebula_probe=&probe;
    r.capture=&capture;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.bg1_plane.native_atmosphere==(bool)(candidate&&expected));
    assert(!projection.bg2_plane.native_atmosphere);
    for(unsigned i=0;i<kDioramaObjectPriorityCount;++i)assert(!projection.object_planes[i].native_atmosphere);
    if(!candidate) {
      before_geometry=probe.geometry_hash;before_actor=probe.actor_hash;
      before_projection=projection;before_order_count=probe.order_count;
      memcpy(before_order,probe.order,sizeof(before_order));
    } else {
      assert(probe.actor_hash==before_actor); /* Stars, actors and HUD exact. */
      assert(probe.order_count==before_order_count&&!memcmp(before_order,probe.order,
          probe.order_count*sizeof(probe.order[0]))); /* Same native blend slots. */
      assert(!memcmp(projection.matrix,before_projection.matrix,sizeof(projection.matrix)));
      assert(!memcmp(projection.object_planes,before_projection.object_planes,sizeof(projection.object_planes)));
      if(expected) {
        assert(probe.background_vertices>0&&probe.geometry_hash!=before_geometry);
        if(shared)assert(probe.depth_vertices>0);
      } else assert(probe.geometry_hash==before_geometry);
    }
    s_nebula_probe=NULL;
    ActionVoxel_Release(empty);
    Diorama_ResetCompositorResources(&device);
    for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
    assert(!r.live);
  }
  printf("Death Heim nebula case %u: native224-row window, actor/priority controls checked\n",scenario);
  return s_count;
}
