/* Independent actual-compositor oracle. Recover source/world coordinates from
 * the published native BG1 plane and inverse camera rays, never from the new
 * extension helper. Register eight cases after the current fixture range. */
typedef struct CastleMatteProbe {
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  bool expected, depth;
  unsigned draws, depth_draws, quads, covered;
  uint64_t native_hash;
  struct {float x0,y0,x1,y1;} rects[2];
} CastleMatteProbe;
static CastleMatteProbe *s_castle_matte;

static ArRenderPointF CastleMatteWorldPoint(const CastleMatteProbe *p,ArRenderPointF screen) {
  const DioramaProjection *projection=p->projection;
  const DioramaPlaneProjection *plane=&projection->bg1_plane;
  assert(plane->valid&&!plane->rake&&!plane->bow);
  const float *m=projection->matrix;
  const double nx=2.0*screen.x/projection->output_width-1;
  const double ny=1-2.0*screen.y/projection->output_height,z=plane->z_world;
  const double a=m[0]-nx*m[3],b=m[4]-nx*m[7],c=m[1]-ny*m[3],d=m[5]-ny*m[7];
  const double u=(nx*m[11]-m[8])*z+nx*m[15]-m[12];
  const double v=(ny*m[11]-m[9])*z+ny*m[15]-m[13],det=a*d-b*c;
  assert(fabs(det)>.000001);
  const double wx=(u*d-b*v)/det,wy=(a*v-u*c)/det;
  const double su=(plane->u0+(wx/projection->aspect_x+.5)*(plane->u1-plane->u0))*projection->texture_width;
  const double sv=(plane->v0+(.5-wy/projection->height_scale)*(plane->v1-plane->v0))*projection->texture_height;
  return (ArRenderPointF){
    p->capture->bg1_camera_x-(p->capture->width-256)*.5-p->capture->obj_apron+su,
    p->capture->camera_y-p->capture->authentic_y0+1+sv};
}
static bool CastleMatteBlack(ArRenderTexture t,const ArRenderVertex2D *v,int count) {
  if(t.value||!count)return false;
  for(int i=0;i<count;++i)if(v[i].color.r||v[i].color.g||v[i].color.b||v[i].color.a!=1)return false;
  return true;
}
static void CastleMatteHash(CastleMatteProbe *p,const void *bytes,size_t count) {
  const uint8_t *b=bytes;
  for(size_t i=0;i<count;++i)p->native_hash=(p->native_hash^b[i])*UINT64_C(1099511628211);
}
static bool CastleMatteGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,int count,
    const int32_t *indices,int index_count,const ArRenderDrawState *state) {
  CastleMatteProbe *p=s_castle_matte;Recorder *r=ctx;
  if(p&&CastleMatteBlack(texture,v,count)) {
    assert(p->expected&&state&&state->blend==kArRenderBlendMode_Alpha);
    assert(!r->actor_seen&&r->bound_effect==-1);
    assert((count==4||count==8)&&index_count==count/4*6);
    ++p->draws;assert(p->draws==1);p->quads=(unsigned)count/4;
    for(unsigned q=0;q<p->quads;++q) {
      float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
      for(unsigned j=0;j<4;++j) {
        assert(v[q*4+j].tex_coord.x==0&&v[q*4+j].tex_coord.y==0);
        const ArRenderPointF world=CastleMatteWorldPoint(p,v[q*4+j].position);
        assert(isfinite(world.x)&&isfinite(world.y));
        x0=fminf(x0,world.x);x1=fmaxf(x1,world.x);y0=fminf(y0,world.y);y1=fmaxf(y1,world.y);
      }
      assert(x1>x0&&y1>y0);
      const bool bottom=fabsf(y0-1023)<.02f&&x0>=464-.02f;
      const bool right=fabsf(x0-1023)<.02f&&y0>=400-.02f&&y1<=1023+.02f;
      /* Exactly the last source texel of the independently proven opaque
       * black run may overlap. No colored/interior nonterminal art can. */
      assert(bottom||right);
      p->rects[q].x0=x0;p->rects[q].x1=x1;p->rects[q].y0=y0;p->rects[q].y1=y1;
      const int tri[]={0,1,2,0,2,3};
      for(unsigned j=0;j<6;++j)assert(indices[q*6+j]==(int)q*4+tri[j]);
    }
    if(p->quads==2) {
      const float overlap_x=fminf(p->rects[0].x1,p->rects[1].x1)-fmaxf(p->rects[0].x0,p->rects[1].x0);
      const float overlap_y=fminf(p->rects[0].y1,p->rects[1].y1)-fmaxf(p->rects[0].y0,p->rects[1].y0);
      assert(overlap_x<.02f||overlap_y<.02f);
    }
  } else if(p) {
    /* Exact untouched native submissions when this descriptor is disabled. */
    CastleMatteHash(p,&texture.value,sizeof(texture.value));
    CastleMatteHash(p,&count,sizeof(count));
    CastleMatteHash(p,&index_count,sizeof(index_count));
    for(int i=0;i<count;++i) {
      CastleMatteHash(p,&v[i].position,sizeof(v[i].position));
      CastleMatteHash(p,&v[i].tex_coord,sizeof(v[i].tex_coord));
      CastleMatteHash(p,&v[i].color,sizeof(v[i].color));
    }
    CastleMatteHash(p,indices,(size_t)index_count*sizeof(*indices));
  }
  return Geometry(ctx,texture,v,count,indices,index_count,state);
}
static bool CastleMatteDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  CastleMatteProbe *p=s_castle_matte;
  if(p&&CastleMatteBlack(draw->texture,draw->vertices,draw->vertex_count)) {
    assert(p->depth&&draw->depths&&draw->clip_w&&!draw->lighting&&!draw->heat_world_xy);
    assert(!draw->native_actor&&draw->plane==SR_PPU_OVERLAY_BG1&&draw->blend==kArRenderBlendMode_Alpha);
    ++p->depth_draws;
    for(int i=0;i<draw->vertex_count;++i) {
      assert(isfinite(draw->clip_w[i])&&draw->clip_w[i]>0);
      assert(fabsf(draw->depths[i]-PlaneDepth(p->projection,p->projection->bg1_plane.z_world,
        draw->vertices[i].position))<.000002f);
    }
    const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Alpha};
    return ArRenderDevice_DrawGeometryWithState(device,draw->texture,draw->vertices,draw->vertex_count,
      draw->indices,draw->index_count,&state);
  }
  return DepthGeometry(ctx,device,draw);
}
static unsigned CastleTerminalMatteFixture(unsigned scenario) {
  assert(scenario<8);s_count=0;s_depth_fixture=(scenario&2)!=0;
  Recorder r={.width=1280,.height=800,.bound_effect=-1};
  ArRenderBackendOps ops=kOps;ops.draw_geometry=CastleMatteGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count]={0};const uint8_t *pixels[kDioramaPlane_Count]={0};
  const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    if(i==SR_PPU_OVERLAY_BG1||i==kDioramaPlane_Obj2)pixels[i]=&present;
  }
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->active_group=2;g->active_room=7;g->profile=kActionVoxelProfile_Castle;
  g->settings=ActionVoxel_SettingsForRoom(2,7);g->width=g->height=1024;g->ready=true;
  const VoxelTerminalMatte source={1024,1024,464,400,true,true};
  DioramaCapture capture={.width=624,.height=352,.authentic_y0=64,
    .bg1_camera_x=658,.camera_y=799,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,1280,800},.visible_width=360,.distance_scale=1.5,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f}};
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  DioramaDepthSink sink={.submit=CastleMatteDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
    .map_group=2,.map_number=7,.voxel_geometry=g};
  if(scenario==5){scene.map_number=8;g->active_room=8;}
  if(scenario==6)scene.voxel_geometry=NULL;
  if(scenario==7){capture.bg1_camera_x=200;capture.camera_y=200;view.distance_scale=1;}
  DioramaProjection projection;
  CastleMatteProbe probe={.capture=&capture,.projection=&projection,.depth=s_depth_fixture};
  s_castle_matte=&probe;r.capture=&capture;r.projection=&projection;r.actor_texture=textures[kDioramaPlane_Obj2];
  uint64_t native_hash=0;unsigned creates=0;
  for(unsigned frame=0;frame<3;++frame) {
    g->terminal_matte=source;if(frame==1||scenario==4)g->terminal_matte=(VoxelTerminalMatte){0};
    probe.expected=scenario<4&&frame!=1;probe.draws=probe.depth_draws=probe.quads=probe.covered=0;
    probe.native_hash=UINT64_C(1469598103934665603);r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.draws==(unsigned)probe.expected);
    assert(probe.depth_draws==(unsigned)(probe.expected&&probe.depth));
    if(probe.expected)for(unsigned sy=0;sy<=32;++sy)for(unsigned sx=0;sx<=32;++sx) {
      const ArRenderPointF world=CastleMatteWorldPoint(&probe,(ArRenderPointF){sx*1280.f/32,sy*800.f/32});
      const bool expected=(world.y>1023+.025f&&world.x>464+.025f)||
        (world.x>1023+.025f&&world.y>400+.025f);
      unsigned owners=0;
      for(unsigned q=0;q<probe.quads;++q)owners+=world.x>probe.rects[q].x0+.025f&&world.x<probe.rects[q].x1-.025f&&
        world.y>probe.rects[q].y0+.025f&&world.y<probe.rects[q].y1-.025f;
      assert(owners<=1&&!!owners==expected);probe.covered+=owners;
    }
    if(probe.expected)assert(probe.covered);
    if(frame){assert(native_hash==probe.native_hash);assert(creates==r.creates);}
    native_hash=probe.native_hash;creates=r.creates;
  }
  s_castle_matte=NULL;ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  printf("Castle terminal matte%u: finite native-plane coverage, only proven last-black-texel overlap, no duplicate corner, exact primary/actor stream, descriptor reset and mode controls\n",scenario);
  return s_count;
}
