typedef struct SnowCoverRecorded {ArRenderVertex2D v[8192];float z[8192];unsigned n;} SnowCoverRecorded;
static SnowCoverRecorded snow_record,snow_paths[2];
static unsigned snow_faces;static uint64_t snow_face_hash;
static void SnowFaceObserve(void*unused,const DioramaVoxelPickFace*f) {
  (void)unused;++snow_faces;const unsigned char*p=(const unsigned char*)f;
  for(unsigned i=0;i<sizeof(*f);++i)snow_face_hash=snow_face_hash*UINT64_C(1099511628211)^p[i];
}
static void SnowRecord(const ArRenderVertex2D*v,const int32_t*ix,int ni,const float*z) {
  assert(snow_record.n+(unsigned)ni<=8192);
  for(int t=0;t<ni;t+=3) {
    const ArRenderPointF a=v[ix[t]].position,b=v[ix[t+1]].position,c=v[ix[t+2]].position;
    /* Portable geometry packs an existing triangle into a degenerate quad;
     * compare actual covered triangles, not the zero-area second half. */
    if(fabs((double)(b.x-a.x)*(c.y-a.y)-(double)(b.y-a.y)*(c.x-a.x))<.000000001)continue;
    for(int j=0;j<3;++j){const int i=t+j;snow_record.v[snow_record.n]=v[ix[i]];snow_record.z[snow_record.n]=z?z[ix[i]]:0;++snow_record.n;}
  }
}
static bool SnowCoverGeometry(void*ctx,ArRenderTexture t,const ArRenderVertex2D*v,int nv,const int32_t*ix,int ni,const ArRenderDrawState*s) {
  if(!t.value&&s->blend==kArRenderBlendMode_Alpha)SnowRecord(v,ix,ni,NULL);return Geometry(ctx,t,v,nv,ix,ni,s);
}
static bool SnowCoverDepth(void*ctx,ArRenderDevice*d,const DioramaDepthSubmission*s) {
  if(!s->texture.value){assert(!s->lighting&&!s->native_actor&&s->blend==kArRenderBlendMode_Alpha&&s->clip_w);for(int i=0;i<s->vertex_count;++i)assert(isfinite(s->depths[i])&&s->depths[i]>0&&s->depths[i]<1&&s->clip_w[i]>0);SnowRecord(s->vertices,s->indices,s->index_count,s->depths);return true;}
  return DepthGeometry(ctx,d,s);
}
static bool SnowSameVertex(const ArRenderVertex2D*a,const ArRenderVertex2D*b) {
  return fabsf(a->position.x-b->position.x)<.002f&&fabsf(a->position.y-b->position.y)<.002f&&
      fabsf(a->color.r-b->color.r)<.00001f&&fabsf(a->color.g-b->color.g)<.00001f&&
      fabsf(a->color.b-b->color.b)<.00001f&&fabsf(a->color.a-b->color.a)<.00001f;
}
static void SnowCoverRender(unsigned scenario) {
  const bool left=scenario&1,shared=scenario<2;
  if(!shared)SnowCoverRender(scenario&1); /* Each registered case is independently runnable. */
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice d;ArRenderBackendOps ops=kOps;ops.draw_geometry=SnowCoverGeometry;
  assert(ArRenderDevice_Init(&d,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t*pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i){assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},textures+i));pixels[i]=&present;}
  ActionVoxelGeometry*g=ActionVoxel_Create();assert(g);g->active_group=6;g->active_room=1;g->profile=kActionVoxelProfile_NorthwallSnow;g->settings=ActionVoxel_SettingsForRoom(6,1);g->width=256;g->height=224;g->ready=true;
  g->front=calloc(256*224,1);g->soft_surfaces=calloc(16*14,1);assert(g->front&&g->soft_surfaces);memset(g->soft_surfaces,1,16*14);
  const int upper=left?82:80,lower=left?80:82;
  for(unsigned x=80;x<84;++x)for(unsigned y=(x>=(unsigned)upper&&x<(unsigned)upper+2)?96:98;y<112;++y)g->front[y*256+x]=kVoxelSolid;
  g->edges[0]=(VoxelEdge){.x=upper,.y=96,.length=2,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edges[1]=(VoxelEdge){.x=lower,.y=98,.length=2,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edges[2]=(VoxelEdge){.x=left?82:81,.y=96,.length=2,.side=left?3:1,.far_rank=2,.colliding=1,.soft_surface=1};
  g->edge_count=g->pass_counts[0]=3;for(unsigned i=0;i<3;++i)g->pass_edges[0][i]=i;
  int receiver_bottom;assert(ActionVoxel_SnowReceiverRun(g,lower,98,&receiver_bottom));
  ActionSceneEffectFrame effects={.authored_count=1};effects.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,.generation=99,.flags=kActionEffectFlag_Visible,.world_y=94,
    .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
    .tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},.particle_style={.color_end=0xfff4df,.seed=621},
    .field_style={.placement=2,.ground_depth=4,.ground_width=8,.point_count=2,.points={{0,left?84:-80},{160,left?-76:80}}}};
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,.camera={.tilt_x=.25f,.tilt_y=left?-.3f:.3f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.voxel_observe_face=SnowFaceObserve};
  const DioramaDepthSink sink={.submit=SnowCoverDepth,.userdata=&r};DioramaScene scene={.render=&options,.depth_sink=shared?&sink:NULL,.map_group=6,.map_number=1,.voxel_geometry=g,.trunk_effects=&effects};
  SnowCoverRecorded baseline={0};uint64_t original_faces=0;unsigned original_count=0;
  for(unsigned frame=0;frame<2;++frame) {
    ++g->revision;g->edges[0].snow_closing_riser[!left]=frame?2:0;snow_record.n=0;snow_faces=0;snow_face_hash=0;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&d,&capture,&view,&scene,&projection))&&projection.valid&&snow_record.n);
    if(!frame){baseline=snow_record;original_faces=snow_face_hash;original_count=snow_faces;continue;}
    fprintf(stderr,"Snow submission case%u frame%u baseline%u candidate%u faces%u/%u marker%u\n",scenario,frame,baseline.n,snow_record.n,original_count,snow_faces,g->edges[0].snow_closing_riser[!left]);assert(snow_face_hash==original_faces&&snow_faces==original_count);assert(snow_record.n>baseline.n);
    unsigned new_vertices=0;for(unsigned i=0;i<snow_record.n;++i){bool found=false;for(unsigned j=0;j<baseline.n&&!found;++j)found=SnowSameVertex(snow_record.v+i,baseline.v+j);if(!found)++new_vertices;}
    /* The renderer projects original native-riser XYZ; equal boundary
     * samples already exist on the retained upper/lower snow caps. */
    if(shared) {
      GroundCoverProbe dummy={0};r.ground_cover=&dummy;unsigned risers=0,joins=0;
      for(unsigned i=0;i<snow_record.n;i+=3) {
        float xyz[3][3];for(unsigned j=0;j<3;++j)GroundCoverNativePoint(&r,snow_record.v+i+j,snow_record.z[i+j],xyz[j]);
        if(fabsf(xyz[0][0]-82)>.02f||fabsf(xyz[1][0]-82)>.02f||fabsf(xyz[2][0]-82)>.02f)continue;
        if(fabsf(xyz[0][2]-xyz[1][2])+fabsf(xyz[0][2]-xyz[2][2])<.00001f)continue;
        ++risers;for(unsigned j=0;j<3;++j){assert(xyz[j][1]>=84&&xyz[j][1]<=98.02f&&xyz[j][2]>=-.00002f&&xyz[j][2]<=.20002f);
          for(unsigned k=0;k<baseline.n;++k)if(SnowSameVertex(snow_record.v+i+j,baseline.v+k)){++joins;break;}}
      }r.ground_cover=NULL;assert(risers>=2&&joins>=4);snow_paths[left]=snow_record;
      printf("Snow submitted case%u riser triangles%u native cap joins%u extra vertices%u\n",scenario,risers,joins,new_vertices);
    }else {
      const SnowCoverRecorded*known=snow_paths+left;
      if(known->n!=snow_record.n)for(unsigned i=0;i<snow_record.n;i+=3){const ArRenderPointF a=snow_record.v[i].position,b=snow_record.v[i+1].position,c=snow_record.v[i+2].position;bool found=false;for(unsigned k=0;k<known->n&&!found;++k)found=SnowSameVertex(snow_record.v+i,known->v+k);if(!found)fprintf(stderr,"UNMATCHED %u area%g xyz %g,%g %g,%g %g,%g\n",i,(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x),a.x,a.y,b.x,b.y,c.x,c.y);}
      double area_current=0,area_known=0;
      for(unsigned i=0;i<snow_record.n;i+=3){const ArRenderPointF a=snow_record.v[i].position,b=snow_record.v[i+1].position,c=snow_record.v[i+2].position;area_current+=fabs((double)(b.x-a.x)*(c.y-a.y)-(double)(b.y-a.y)*(c.x-a.x));}
      for(unsigned i=0;i<known->n;i+=3){const ArRenderPointF a=known->v[i].position,b=known->v[i+1].position,c=known->v[i+2].position;area_known+=fabs((double)(b.x-a.x)*(c.y-a.y)-(double)(b.y-a.y)*(c.x-a.x));}
      fprintf(stderr,"AREAS %.12g %.12g\n",area_current,area_known);
      assert(known->n==snow_record.n);
      for(unsigned i=0;i<snow_record.n;++i){bool found=false;for(unsigned j=0;j<known->n&&!found;++j)found=SnowSameVertex(snow_record.v+i,known->v+j);assert(found);}
    }
  }
  ActionVoxel_Release(g);Diorama_ResetCompositorResources(&d);for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
}
