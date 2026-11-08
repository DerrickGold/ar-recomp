#include "diorama/diorama_trunk_guard_upload.h"

typedef struct TrunkGuardProbe {
  Recorder *recorder;
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  const DioramaTrunkGuardView *guard;
  ArRenderTexture primary_far;
  unsigned faces[3][2], vertices[3], edges, aa_creates, aa_copies;
  unsigned primary_far_vertices, light_checks;
  uint64_t primary_far_hash;
  unsigned target_region[128], uploaded_bands;
  uint32_t strip_colors[3][2];
  bool copy_pending, fail_aa;
  unsigned aa_attempts;
} TrunkGuardProbe;
static TrunkGuardProbe *s_trunk_guard_probe;

static bool TrunkGuardCreate(void *ctx,const ArRenderTextureDesc *desc,ArRenderTexture *texture) {
  if(s_trunk_guard_probe && desc->width==s_trunk_guard_probe->capture->width*4 && desc->height==256) {
    assert(desc->usage==kArRenderTextureUsage_Target && desc->filter==kArRenderFilter_Linear);
    ++s_trunk_guard_probe->aa_attempts;
    if(s_trunk_guard_probe->fail_aa)return false;
    ++s_trunk_guard_probe->aa_creates;
  }
  return Create(ctx,desc,texture);
}
static bool TrunkGuardUpdate(void *ctx,ArRenderTexture texture,const ArRenderRectI *rect,
    const void *pixels,int pitch) {
  Recorder *r=ctx;TrunkGuardProbe *probe=s_trunk_guard_probe;
  if(probe)for(unsigned band=0;band<3;++band)if(texture.value==probe->guard->textures[band].value) {
    assert(!rect && pixels && pitch==640*4);
    const uint32_t *ink=pixels;
    for(unsigned region=0;region<2;++region) {
      const uint32_t expected=0xff000000u | ((region?0x881122u:0x114488u)+(band<<8));
      for(unsigned i=region*640*64;i<(region+1)*640*64;++i)assert(ink[i]==expected);
      probe->strip_colors[band][region]=expected;
    }
    ++probe->uploaded_bands;++r->uploads;return true;
  }
  if(probe && r->textures[texture.value].width==32 && r->textures[texture.value].height==64) {
    assert(!rect && pixels && pitch==32*4);++r->uploads;return true;
  }
  return Update(ctx,texture,rect,pixels,pitch);
}
static bool TrunkGuardTexture(void *ctx,ArRenderTexture texture,const ArRenderRectF *source,
    const ArRenderRectF *destination,const ArRenderDrawState *state) {
  Recorder *r=ctx;
  if(s_trunk_guard_probe && r->state.target.value) {
    r->copied_sources[r->state.target.value]=r->copied_sources[texture.value].value
        ?r->copied_sources[texture.value]:texture;
    for(unsigned band=0;band<3;++band)if(texture.value==s_trunk_guard_probe->guard->textures[band].value) {
      assert(source && source->x==0 && (source->y==0 || source->y==64));
      assert(source->w==s_trunk_guard_probe->capture->width && source->h==64);
      assert(destination && destination->w==source->w*4 && destination->h==256);
      /* The two atlases contain sharply different colors. A source copy may
       * touch exactly one strip; drawing must consume it before target reuse. */
      const unsigned region=(unsigned)source->y/64;
      assert(s_trunk_guard_probe->strip_colors[band][region] != s_trunk_guard_probe->strip_colors[band][1-region]);
      assert(!s_trunk_guard_probe->copy_pending);
      s_trunk_guard_probe->copy_pending=true;
      s_trunk_guard_probe->target_region[r->state.target.value]=region;
      ++s_trunk_guard_probe->aa_copies;
    }
  }
  return Texture(ctx,texture,source,destination,state);
}

static void TrunkGuardCheckFace(TrunkGuardProbe *probe,unsigned band,unsigned region,bool filtered,
    const ArRenderVertex2D *vertices,int count,const int32_t *indices,int index_count,
    const float *depths,const DioramaSceneryLightEffectParams *light) {
  const DioramaCapture *capture=probe->capture;
  const DioramaProjection *p=probe->projection;
  const DioramaTrunkGuardStrip *strip=&probe->guard->layout.strips[region];
  assert(strip->valid.width>0 && strip->valid.height>0);
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float first_x=strip->world_x+strip->valid.x,last_x=first_x+strip->valid.width;
  const float first_y=strip->world_y+strip->valid.y,last_y=first_y+strip->valid.height;
  const float sample_width=filtered?capture->width:640;
  const float sample_height=filtered?64:128;
  const float atlas_y=filtered?0:strip->atlas_y;
  assert(last_y<=oy || first_y>=oy+capture->height);
  ++probe->faces[band][region];probe->vertices[band]+=count;
  assert(probe->recorder->actor_seen==(band==1));
  if(light) {
    assert(fabsf(light->attributes_uv.x-(strip->world_x-ox)/640.f)<.000001f);
    assert(fabsf(light->attributes_uv.y-(strip->world_y-oy-atlas_y)/SR_PPU_SURFACE_MAX_HEIGHT)<.000001f);
    assert(light->attributes_uv.w==sample_width/640.f && light->attributes_uv.h==sample_height/SR_PPU_SURFACE_MAX_HEIGHT);
    assert(light->texel_width==1.f/(sample_width*(filtered?4:1)) && light->texel_height==1.f/(sample_height*(filtered?4:1)));
    ++probe->light_checks;
  }
  for(int i=0;i<count;++i) {
    const ArRenderVertex2D *v=&vertices[i];
    const float wx=strip->world_x+v->tex_coord.x*sample_width;
    const float wy=strip->world_y+v->tex_coord.y*sample_height-atlas_y;
    assert(wx>=first_x-.001f && wx<=last_x+.001f && wy>=first_y-.001f && wy<=last_y+.001f);
    bool side=wx<320 || wx>704;
    if(fabsf(wx-320)<.001f || fabsf(wx-704)<.001f) {
      for(int triangle=0;triangle<index_count;triangle+=3) {
        if(indices[triangle]!=i && indices[triangle+1]!=i && indices[triangle+2]!=i)continue;
        float center=0;
        for(unsigned j=0;j<3;++j)center+=(strip->world_x+vertices[indices[triangle+j]].tex_coord.x*sample_width)/3.f;
        side=center<320 || center>704;break;
      }
    }
    if(band==2)assert(side); /* The original curved core Far owns its source. */
    const float z=side?-.24f-(band==2?TrunkSideExpectedSpan(wx):0):band==0?-.24f:0;
    Scene3DClipPoint clip;
    assert(Scene3D_TransformToClip(p->matrix,((wx-ox)/capture->width-.5f)*p->aspect_x,
        (.5f-(wy-oy)/capture->height)*p->height_scale,z,&clip));
    assert(clip.w>0);
    const float sx=(clip.x/clip.w*.5f+.5f)*p->output_width;
    const float sy=(.5f-clip.y/clip.w*.5f)*p->output_height;
    assert(fabsf(v->position.x-sx)<.004f && fabsf(v->position.y-sy)<.004f);
    if(depths)assert(fabsf(depths[i]-(clip.z/clip.w*.5f+.5f))<.000002f);
  }
  if(band==2)for(int i=0;i<index_count;i+=3) {
    float lo=INFINITY,hi=-INFINITY;
    for(unsigned j=0;j<3;++j) {
      const float x=strip->world_x+vertices[indices[i+j]].tex_coord.x*sample_width;
      lo=fminf(lo,x);hi=fmaxf(hi,x);
    }
    assert(hi<=320.001f || lo>=703.999f); /* No triangle bridges the core. */
  }
}

static bool TrunkGuardGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,int count,
    const int32_t *indices,int index_count,const ArRenderDrawState *state) {
  Recorder *r=ctx;TrunkGuardProbe *probe=s_trunk_guard_probe;
  const ArRenderTexture source=r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  if(probe) {
    for(unsigned band=0;band<3;++band)if(probe->guard->textures[band].value && source.value==probe->guard->textures[band].value) {
      const bool filtered=texture.value!=source.value;
      if(filtered || (count && v[0].color.r==1)) {
        float mean_v=0;for(int i=0;i<count;++i)mean_v+=v[i].tex_coord.y/count;
        const unsigned region=filtered?probe->target_region[texture.value]:mean_v>.5f;
        if(filtered)assert(probe->copy_pending);
        TrunkGuardCheckFace(probe,band,region,filtered,v,count,indices,index_count,NULL,NULL);
        if(filtered)probe->copy_pending=false;
      }
      else probe->edges+=count; /* Native source samples on cached core branch caps. */
    }
    if(r->textures[texture.value].width==32)probe->edges+=count;
    if(source.value==probe->primary_far.value) {
      probe->primary_far_vertices+=count;
      const unsigned char *bytes=(const unsigned char *)v;
      for(size_t i=0;i<(size_t)count*sizeof(*v);++i)
        probe->primary_far_hash=(probe->primary_far_hash^bytes[i])*UINT64_C(1099511628211);
    }
  }
  return Geometry(ctx,texture,v,count,indices,index_count,state);
}
static bool TrunkGuardDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  Recorder *r=ctx;TrunkGuardProbe *probe=s_trunk_guard_probe;
  const ArRenderTexture source=r->copied_sources[draw->texture.value].value
      ?r->copied_sources[draw->texture.value]:draw->texture;
  const int planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  for(unsigned band=0;band<3;++band)if(probe->guard->textures[band].value && source.value==probe->guard->textures[band].value &&
      (source.value!=draw->texture.value || draw->vertices[0].color.r==1)) {
    assert(draw->plane==planes[band]);
    assert(!draw->native_actor && draw->blend==kArRenderBlendMode_Alpha);
    /* Geometry callback counts the same draw once; this callback additionally
     * verifies depth and atlas-to-primary lighting coordinates. */
    unsigned faces[3][2],vertices[3];memcpy(faces,probe->faces,sizeof(faces));memcpy(vertices,probe->vertices,sizeof(vertices));
    const bool filtered=source.value!=draw->texture.value;
    float mean_v=0;for(int i=0;i<draw->vertex_count;++i)mean_v+=draw->vertices[i].tex_coord.y/draw->vertex_count;
    const unsigned region=filtered?probe->target_region[draw->texture.value]:mean_v>.5f;
    TrunkGuardCheckFace(probe,band,region,filtered,draw->vertices,draw->vertex_count,draw->indices,draw->index_count,draw->depths,draw->lighting);
    memcpy(probe->faces,faces,sizeof(faces));memcpy(probe->vertices,vertices,sizeof(vertices));
  }
  return DepthGeometry(ctx,device,draw);
}
static void TrunkGuardClearCounts(TrunkGuardProbe *p) {
  memset(p->faces,0,sizeof(p->faces));memset(p->vertices,0,sizeof(p->vertices));
  p->edges=p->primary_far_vertices=p->light_checks=0;p->primary_far_hash=UINT64_C(1469598103934665603);
  p->recorder->actor_seen=false;
}

static unsigned TrunkGuardFixture(unsigned scenario) {
  s_count=0;assert(scenario<8);
  const bool shared=scenario<4;
  static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6;trunk.height=512;trunk.side_width=1024;trunk.revision=6000+scenario*10;
  for(unsigned i=0;i<32*32;++i)trunk.bark[i]=0xff403020;
  for(unsigned i=0;i<16*16;++i)trunk.side_snow[i]=0xffbbccdd;
  /* Three separated source regions make cache thrashing observable. Core
   * branch/rim and a low side block occupy every region, not just primary. */
  const int primary_y=scenario>=6?288:scenario>=4?0:128;
  for(unsigned region=0;region<3;++region) {
    const int y=(int[]){primary_y-32,primary_y+96,primary_y+240}[region];
    if(y<0||y+8>512)continue;
    for(int yy=y;yy<y+8;++yy) {
      for(int x=400;x<416;++x)trunk.masks[1][yy*48+(x-320)/8]|=1u<<(x&7);
      for(int x=480;x<488;++x)trunk.masks[0][yy*48+(x-320)/8]|=1u<<(x&7);
      for(int x=270;x<286;++x)trunk.side_masks[kActionTrunkSideSolid][yy*kActionTrunkSideMaskPitch+x/8]|=1u<<(x&7);
    }
  }
  Recorder r={.width=1280,.height=800,.bound_effect=-1,.shaders=true,.trunk=&trunk};
  ArRenderBackendOps ops=kOps;ops.create_texture=TrunkGuardCreate;
  ops.draw_texture=TrunkGuardTexture;ops.draw_geometry=TrunkGuardGeometry;
  ops.update_texture=TrunkGuardUpdate;
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX,
      .maximum_texture_width=8192,.maximum_texture_height=8192,.maximum_render_target_width=8192,.maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  const int capture_width=scenario&2?640:624;
  DioramaCapture capture={.width=capture_width,.height=224,.bg1_camera_x=200+(capture_width-256)/2,.camera_y=primary_y-1,
      .textures=textures,.pixels=pixels};
  DioramaTrunkGuardView guard={.capture_generation=1,.source_revision=trunk.revision};
  const DioramaTrunkGuardBounds bounds=scenario<4?(DioramaTrunkGuardBounds){230,80,560,320}:
      (DioramaTrunkGuardBounds){230,0,560,512};
  assert(DioramaTrunkGuard_Resolve(200,primary_y,capture_width,224,bounds,&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=128,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  const DioramaTrunkGuardView original=guard;
  r.actor_texture=textures[kDioramaPlane_Obj2];
  DioramaView view={.viewport={0,0,1280,800},.visible_width=624,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=TrunkGuardDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.map_group=6,.map_number=6,.trunk=&trunk,
      .trunk_settings=DioramaTrunk_Default(),.trunk_guard=&guard,.depth_sink=shared?&sink:NULL};
  DioramaProjection projection;r.capture=&capture;r.projection=&projection;
  TrunkGuardProbe probe={.recorder=&r,.capture=&capture,.projection=&projection,.guard=&guard,
      .primary_far=textures[kDioramaPlane_Bg1Far]};s_trunk_guard_probe=&probe;
  static uint32_t ink[640*128];
  for(unsigned band=0;band<3;++band) {
    for(unsigned i=0;i<640*128;++i)ink[i]=0xff000000u|((i>=640*64?0x881122u:0x114488u)+(band<<8));
    assert(ArRenderDevice_UpdateTexture(&device,guard.textures[band],NULL,ink,640*4));
  }
  assert(probe.uploaded_bands==3);
  unsigned edges=0,creates=0,uploads=0,primary_vertices=0;uint64_t primary_hash=0;
  for(unsigned frame=0;frame<3;++frame) {
    if(frame==1) { /* Invalid producer mutation deliberately challenges reuse. */
      memset(trunk.masks,0,sizeof(trunk.masks));memset(trunk.side_masks,0,sizeof(trunk.side_masks));
    }
    if(frame==2) {++trunk.revision;guard.source_revision=trunk.revision;}
    TrunkGuardClearCounts(&probe);
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    for(unsigned band=0;band<3;++band)for(unsigned region=0;region<2;++region)
      assert(probe.faces[band][region]==(guard.layout.strips[region].valid.height?1u:0u));
    assert(probe.primary_far_vertices>0);
    if(shared)assert(probe.light_checks>0);
    if(!frame) {edges=probe.edges;creates=r.creates;uploads=r.uploads;primary_vertices=probe.primary_far_vertices;primary_hash=probe.primary_far_hash;}
    else if(frame==1)assert(probe.edges==edges && r.creates==creates && r.uploads==uploads);
    else assert(!probe.edges);
    assert(probe.primary_far_vertices==primary_vertices && probe.primary_far_hash==primary_hash);
  }
  const unsigned strips=(guard.layout.strips[0].valid.height>0)+(guard.layout.strips[1].valid.height>0);
  assert(edges>0 && probe.aa_creates==1 && probe.aa_copies==9*strips && !probe.copy_pending);
  /* Reject the whole supplemental view before drawing any band. Primary
   * geometry is preserved for absent, stale and malformed descriptors. */
  for(unsigned bad=0;bad<6;++bad) {
    guard=original;guard.source_revision=trunk.revision;scene.trunk_guard=&guard;
    if(bad==0)scene.trunk_guard=NULL;
    if(bad==1)--guard.source_revision;
    if(bad==2)++guard.layout.strips[0].world_x;
    if(bad==3)guard.capture_generation=0;
    if(bad==4)guard.layout.strips[1].atlas_y=0;
    if(bad==5)guard.textures[1]=ArRenderTexture_Invalid();
    TrunkGuardClearCounts(&probe);
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    for(unsigned band=0;band<3;++band)assert(!probe.vertices[band]);
    assert(probe.primary_far_vertices==primary_vertices && probe.primary_far_hash==primary_hash);
  }
  guard=original;guard.source_revision=trunk.revision;
  scene.trunk_guard=&guard;
  Diorama_ResetCompositorResources(&device);
  probe.fail_aa=true;const unsigned attempts=probe.aa_attempts;
  for(unsigned frame=0;frame<2;++frame) {
    TrunkGuardClearCounts(&probe);
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    for(unsigned band=0;band<3;++band)for(unsigned region=0;region<2;++region)
      assert(probe.faces[band][region]==(guard.layout.strips[region].valid.height?1u:0u));
    assert(probe.aa_attempts==attempts+1); /* Failure latches until reset. */
  }
  Diorama_ResetCompositorResources(&device);probe.fail_aa=false;
  TrunkGuardClearCounts(&probe);
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(probe.aa_attempts==attempts+2 && probe.aa_creates==2 && !probe.copy_pending);
  s_trunk_guard_probe=NULL;Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<3;++i)Destroy(&r,guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);
  printf("Trunk guard case %u: finite strips, primary parity, cached%u edge vertices, one4xAA target\n",scenario,edges);
  return s_count;
}
