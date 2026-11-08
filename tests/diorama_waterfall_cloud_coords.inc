/* Staged regression: actual per-draw metadata must address receiver world Y,
 * while waterfall art keeps its existing repeat and native mesh ordering. */
typedef struct WaterfallCloudProbe {
  Recorder *recorder;
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  DioramaCloudLight cloud;
  ArRenderTexture textures[3];
  DioramaSceneryLightEffectParams material,attached_material[3];
  bool have_material,split;
  unsigned frame,draws[3],vertex_count[3],index_count[3];
  ArRenderVertex2D vertices[3][512],baseline_vertices[3][512];
  int32_t indices[3][2048],baseline_indices[3][2048];
  unsigned baseline_nv[3],baseline_ni[3],world_checks;
} WaterfallCloudProbe;
static WaterfallCloudProbe *s_waterfall_cloud;
static void WaterfallCloudMaterial(const DioramaSceneryLightEffectParams *material) {
  if(s_waterfall_cloud) {
    s_waterfall_cloud->material=*material;
    s_waterfall_cloud->have_material=true;
  }
}
static bool WaterfallCloudTexture(void *ctx,ArRenderTexture texture,const ArRenderRectF *src,
    const ArRenderRectF *dst,const ArRenderDrawState *state) {
  Recorder *r=ctx;
  if(r->state.target.value)r->copied_sources[r->state.target.value]=
      r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  return Texture(ctx,texture,src,dst,state);
}
static bool WaterfallCloudGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const ArRenderDrawState *state) {
  WaterfallCloudProbe *p=s_waterfall_cloud;Recorder *r=ctx;
  const ArRenderTexture source=r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  int band=-1;for(unsigned i=0;p&&i<3;++i)if(source.value==p->textures[i].value)band=(int)i;
  if(band<0)return Geometry(ctx,texture,v,nv,indices,ni,state);
  const unsigned ordinal=p->draws[band]++;
  const int attached_nv=9*(kDioramaPlaneSubdivY+2),host_nv=9*(kDioramaPlaneSubdivY+1);
  const bool attached=p->split&&ordinal==0;
  assert(ordinal<(p->split?2u:1u));
  assert(nv>=(p->split?(attached?attached_nv:host_nv):p->frame==4?host_nv:attached_nv+host_nv));
  const unsigned base=p->vertex_count[band],index_base=p->index_count[band];
  assert(base+(unsigned)nv<=512&&index_base+(unsigned)ni<=2048);
  memcpy(p->vertices[band]+base,v,(size_t)nv*sizeof(*v));
  for(int i=0;i<ni;++i)p->indices[band][index_base+i]=indices[i]+(int)base;
  p->vertex_count[band]+=nv;p->index_count[band]+=ni;
  if(p->frame==1||p->frame==2||p->frame==4) {
    assert(p->have_material&&r->bound_effect==kDioramaEffect_Count);
    DioramaCloudLight expected=p->cloud;
    if(attached)expected.source[1]+=224;
    if(attached)p->attached_material[band]=p->material;
    if(p->split&&!attached) {
      DioramaSceneryLightEffectParams same=p->attached_material[band];
      same.cloud_light.source[1]-=224;
      assert(!memcmp(&same,&p->material,sizeof(same))); /* No shifted light map/filter/attributes. */
    }
    if(p->split) {
      DioramaProjection native=*p->projection;
      const float z[]={-.30f,-.29f,-.45f};
      native.bg2_plane.z_world=z[band];native.bg2_plane.overflow_valid=false;
      const int count=attached?attached_nv:host_nv;
      for(int i=0;i<count;++i) {
        const ArRenderRectF uv=p->material.attributes_uv;
        const float full_u=uv.x+v[i].tex_coord.x*uv.w;
        const float full_v=uv.y+v[i].tex_coord.y*uv.h;
        const float native_y=full_v*352+(attached?224:0);
        const float native_x=full_u*640;
        const float cloud_x=full_u*p->material.cloud_light.source[2]+p->material.cloud_light.source[0];
        const float cloud_y=full_v*p->material.cloud_light.source[3]+p->material.cloud_light.source[1];
        assert(fabsf(cloud_x-(native_x+p->cloud.source[0]))<.0001f);
        if(fabsf(cloud_y-(native_y+p->cloud.source[1]))>=.0001f)
          fprintf(stderr,"Waterfall cloud receiver mismatch: band%d vertex%d receiverY%.6f cloudWorldY%.6f expectedWorldY%.6f\n",
              band,i,native_y,cloud_y,native_y+p->cloud.source[1]);
        assert(fabsf(cloud_y-(native_y+p->cloud.source[1]))<.0001f);
        ArRenderPointF point;
        assert(Diorama_ProjectCapturedBg2Point(&native,native_x-native.texture_x_origin,native_y,&point,NULL,NULL));
        assert(fabsf(point.x-v[i].position.x)<.003f&&fabsf(point.y-v[i].position.y)<.003f);
        ++p->world_checks;
      }
      if(!attached) {
        /* Identical receiver positions in the overlap evaluate the same cloud
         * field, though the continuation intentionally samples224 rows above. */
        const float row=(float)p->capture->height;
        const float host=row/352*p->material.cloud_light.source[3]+p->material.cloud_light.source[1];
        const float continuation=(row-224)/352*p->attached_material[band].cloud_light.source[3]+
            p->attached_material[band].cloud_light.source[1];
        assert(fabsf(host-continuation)<.0001f);
      }
    }
    assert(!memcmp(&p->material.cloud_light,&expected,sizeof(expected)));
  }
  return Geometry(ctx,texture,v,nv,indices,ni,state);
}
static unsigned WaterfallCloudFixture(unsigned scenario) {
  assert(scenario<8);s_count=0;s_depth_fixture=false;
  const unsigned room=2+((scenario>>1)&1);
  Recorder r={.width=1280,.height=800,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.draw_texture=WaterfallCloudTexture;ops.draw_geometry=WaterfallCloudGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=4;voxel->active_room=room;voxel->profile=kActionVoxelProfile_AitosMountains;
  voxel->settings=ActionVoxel_SettingsForRoom(4,room);voxel->width=voxel->height=1024;voxel->ready=true;
  const int bands[]={SR_PPU_OVERLAY_BG2,kDioramaPlane_Bg2Hi,kDioramaPlane_Bg2Far};
  const DioramaCapture capture={.width=496,.height=room==2?288:352,.obj_apron=64,
    .authentic_y0=room==2?32:64,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,1280,800},.visible_width=360,.distance_scale=1,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<bands[0])|(1u<<bands[1])|(1u<<bands[2]),
    .edge_aa=!!(scenario&2),.depth_of_field=!!(scenario&4),.margin_fix=true};
  DioramaScene scene={.render=&options,.map_group=4,.map_number=room,
    .layer_section=kDioramaLayerSection_AitosWaterfall,.voxel_geometry=voxel};
  DioramaProjection projection;
  WaterfallCloudProbe probe={.recorder=&r,.capture=&capture,.projection=&projection,
    .cloud={.source={-112,16-(float)capture.authentic_y0,640,352},.meta={1,1},
      .regions={{.bounds={259,300,1200,1000},.core={1,1,1,1},.color={1,.8f,.5f,1}}},.background_dim={.76f}}};
  for(unsigned i=0;i<3;++i)probe.textures[i]=textures[bands[i]];
  s_waterfall_cloud=&probe;s_waterfall_cloud_observe=WaterfallCloudMaterial;
  r.capture=&capture;r.projection=&projection;
  for(unsigned frame=0;frame<5;++frame) {
    probe.frame=frame;probe.split=frame==1;probe.have_material=false;
    memset(probe.draws,0,sizeof(probe.draws));memset(probe.vertex_count,0,sizeof(probe.vertex_count));
    memset(probe.index_count,0,sizeof(probe.index_count));
    scene.cloud_light=(frame==1||frame==2||frame==4)?probe.cloud:(DioramaCloudLight){0};
    scene.voxel_geometry=frame==2?NULL:voxel;scene.map_number=voxel->active_room=frame==4?1:room;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    for(unsigned i=0;i<3;++i) {
      assert(probe.draws[i]==(probe.split?2u:1u));
      if(frame==0) {
        probe.baseline_nv[i]=probe.vertex_count[i];probe.baseline_ni[i]=probe.index_count[i];
        memcpy(probe.baseline_vertices[i],probe.vertices[i],probe.vertex_count[i]*sizeof(ArRenderVertex2D));
        memcpy(probe.baseline_indices[i],probe.indices[i],probe.index_count[i]*sizeof(int32_t));
      } else if(frame==1||frame==3) {
        assert(probe.vertex_count[i]==probe.baseline_nv[i]&&probe.index_count[i]==probe.baseline_ni[i]);
        assert(!memcmp(probe.vertices[i],probe.baseline_vertices[i],probe.vertex_count[i]*sizeof(ArRenderVertex2D)));
        assert(!memcmp(probe.indices[i],probe.baseline_indices[i],probe.index_count[i]*sizeof(int32_t)));
      }
    }
  }
  assert(probe.world_checks==3u*9u*(2*kDioramaPlaneSubdivY+3));
  s_waterfall_cloud_observe=NULL;s_waterfall_cloud=NULL;
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  return s_count;
}
