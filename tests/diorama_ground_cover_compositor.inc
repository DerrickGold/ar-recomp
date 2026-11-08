/* Exercise the real native/authored/hollow adapters, not a duplicate cap
 * implementation. The command recorder verifies emitted triangles; the
 * optional observer verifies support/empty ownership at the same draw. */
typedef struct GroundCoverMesh {
  ArRenderVertex2D vertices[8192];
  unsigned count;
} GroundCoverMesh;
static GroundCoverMesh s_ground_cover_reference[5][2];
static GroundCoverMesh s_ground_cover_neutral_reference[5][2];
static GroundCoverMesh s_ground_pressure_reference[2][5];
typedef struct GroundCoverWorldMesh {float xyz[8192][3];unsigned count;} GroundCoverWorldMesh;
/* Native-priority cuts add samples to the nonlinear snow surface. Compare
 * identical emitted X/Z samples, including the physical actor contact;
 * interpolation between different sample grids is not a pressure oracle. */
static void GroundCoverSamePressureSamples(const GroundCoverWorldMesh *a,
    const GroundCoverWorldMesh *b,float actor_z) {
  unsigned common=0;bool actor_contact=false,left=false,right=false,away=false;
  for(unsigned i=0;i<a->count;++i)for(unsigned j=0;j<b->count;++j) {
    if(fabsf(a->xyz[i][0]-b->xyz[j][0])>.002f||fabsf(a->xyz[i][2]-b->xyz[j][2])>.00001f)continue;
    assert(fabsf(a->xyz[i][1]-b->xyz[j][1])<.002f);++common;
    left|=a->xyz[i][0]<87.99f;right|=a->xyz[i][0]>88.01f;away|=fabsf(a->xyz[i][2]-actor_z)>.02f;
    actor_contact|=fabsf(a->xyz[i][0]-88)<.002f&&fabsf(a->xyz[i][2]-actor_z)<.00001f;
    break;
  }
  assert(common>=8&&actor_contact&&left&&right&&away);
}

static void GroundCoverObserve(void *userdata,const DioramaGroundCoverObservation *o) {
  GroundCoverProbe *p=userdata;assert(o&&o->bank==0&&o->complete&&!p->unsupported);
  if(o->event==kDioramaGroundCoverBegin) {
    ++p->begins;assert(o->point_count>=2&&o->point_count<=16);
    assert(fabsf(o->path_xy[0]-p->x0)<.001f&&fabsf(o->path_xy[(o->point_count-1)*2]-p->x1)<.001f);
    for(unsigned i=0;i<o->point_count;++i)assert(fabsf(o->path_xy[i*2+1]-p->floor_y)<.001f);
  } else if(o->event==kDioramaGroundCoverSupport) {
    ++p->supports;assert(!p->empty&&o->dedicated==p->dedicated);
    assert(o->claim==kActionGroundCoverClaimedGeometry);
    assert(o->support_xy[0]>=p->x0-.002f&&o->support_xy[2]<=p->x1+.002f);
    const int column=(int)floorf(o->support_xy[0]-p->x0+.001f);
    const float floor_y=p->snow_receiver?p->floor_y-8+(p->pressure_transform?0:(column&3)):p->floor_y;
    assert(fabsf(o->support_xy[1]-floor_y)<.002f&&fabsf(o->support_xy[3]-floor_y)<.002f);
    if(p->snow_receiver) {
      assert(column>=0&&column<48);
      if(!p->pressure_transform)assert(o->support_xy[2]-o->support_xy[0]<=1.002f);
      for(int x=column;x<(int)ceilf(o->support_xy[2]-p->x0-.001f);++x){assert(x<48);p->snow_columns[x]=1;}
    }
    if(p->kind==4)for(int x=(int)floorf(o->support_xy[0]);x<(int)ceilf(o->support_xy[2]);++x){assert(x>=304&&x<320);p->snow_columns[x-304]=1;}
    /* The authored alpha stripe and native missing interval are not terrain. */
    assert(o->support_xy[2]<=p->gap0+.002f||o->support_xy[0]>=p->gap1-.002f);
  } else {
    assert(o->event==kDioramaGroundCoverEnd);++p->ends;
    if(o->claim!=(unsigned)(p->empty?kActionGroundCoverClaimedEmpty:kActionGroundCoverClaimedGeometry))
      fprintf(stderr,"Cover kind%u shared%d empty%d: claim%u caps%u triangles%u supports%u\n",p->kind,p->shared_depth,p->empty,o->claim,o->caps,o->triangles,p->supports);
    assert(o->claim==(p->empty?kActionGroundCoverClaimedEmpty:kActionGroundCoverClaimedGeometry));
    p->caps=o->caps;p->triangles=o->triangles;
    if(p->empty)assert(!o->caps&&!o->triangles&&!o->light_samples);
    else assert(o->caps&&o->triangles&&o->light_samples);
  }
}

/* Invert actual submitted screen XYZ, independently of the cover adapter.
 * This catches cover painted on a parallel plane or stretched across a pit. */
static void GroundCoverNativePoint(const Recorder *r,const ArRenderVertex2D *v,float depth,float out[3]) {
  const DioramaProjection *p=r->projection;const float *m=p->matrix;
  const double ndc[3]={2.*v->position.x/p->output_width-1,1-2.*v->position.y/p->output_height,2.*depth-1};
  double a[3][4];
  for(unsigned row=0;row<3;++row) {
    for(unsigned col=0;col<3;++col)a[row][col]=m[col*4+row]-ndc[row]*m[col*4+3];
    a[row][3]=ndc[row]*m[15]-m[12+row];
  }
  for(unsigned col=0;col<3;++col) {
    unsigned pivot=col;for(unsigned row=col+1;row<3;++row)if(fabs(a[row][col])>fabs(a[pivot][col]))pivot=row;
    assert(fabs(a[pivot][col])>1e-8);
    for(unsigned j=col;j<4;++j){double t=a[col][j];a[col][j]=a[pivot][j];a[pivot][j]=t;}
    const double d=a[col][col];for(unsigned j=col;j<4;++j)a[col][j]/=d;
    for(unsigned row=0;row<3;++row)if(row!=col) {
      const double d=a[row][col];for(unsigned j=col;j<4;++j)a[row][j]-=d*a[col][j];
    }
  }
  const double z=a[2][3],scale=r->ground_cover->dedicated?1:1-z/p->camera_distance;
  const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f-r->capture->obj_apron;
  const float oy=r->capture->camera_y-r->capture->authentic_y0+1;
  out[0]=ox+(a[0][3]/scale/p->aspect_x+.5)*r->capture->width;
  out[1]=oy+(.5-a[1][3]/scale/p->height_scale)*r->capture->height;
  out[2]=r->ground_cover->dedicated?z:-z;
}
static void CheckGroundCoverDraw(Recorder *r,const ArRenderVertex2D *vertices,int count,const float *depths) {
  GroundCoverProbe *p=r->ground_cover;assert(p&&!p->empty&&!p->unsupported);
  const unsigned step=depths?3:4;assert(count>0&&(unsigned)count%step==0);
  for(unsigned at=0;at<(unsigned)count;at+=step) {
    assert(p->vertices+3<=8192);++p->draw_triangles;
    if(r->actor_seen)++p->after_actor;else ++p->before_actor;
    float centre_x=0,min_y=INFINITY,max_y=-INFINITY,xyz[3][3];
    bool lip=depths!=NULL;
    const float near_z=p->dedicated?(p->kind==2?0:-.24f):0;
    if(depths)for(unsigned j=0;j<3;++j) {
      GroundCoverNativePoint(r,&vertices[at+j],depths[at+j],xyz[j]);
      if(p->pressure_transform){xyz[j][0]=88+(xyz[j][0]-128)*.5f;xyz[j][1]=112+(xyz[j][1]-128)/1.5f;}
      lip&=fabsf(xyz[j][2]-near_z)<.00002f;
    }
    for(unsigned j=0;j<3;++j) {
      const ArRenderVertex2D *v=&vertices[at+j];assert(isfinite(v->position.x)&&isfinite(v->position.y));
      assert(v->color.a>=0&&v->color.a<=1&&v->color.r>=0&&v->color.g>=0&&v->color.b>=0);
      p->mesh[p->vertices++]=*v;
      if(depths) {
        const float *native=xyz[j];
        if(p->pressure_transform&&!lip){assert(p->top_vertices<8192);memcpy(p->pressure_xyz[p->top_vertices++],native,3*sizeof(float));}
        assert(native[0]>=p->x0-.02f&&native[0]<=p->x1+.02f);
        assert(native[1]<=p->floor_y+(lip?1.52f:.02f)&&native[1]>=p->floor_y-12);
        assert(native[2]>=p->z0-.002f&&native[2]<=p->z1+.002f);
        p->seen_z0=fminf(p->seen_z0,native[2]);p->seen_z1=fmaxf(p->seen_z1,native[2]);
        centre_x+=native[0]/3;
        min_y=fminf(min_y,native[1]);max_y=fmaxf(max_y,native[1]);
      }
    }
    if(depths)assert(centre_x<=p->gap0+.02f||centre_x>=p->gap1-.02f);
    if(depths&&p->snow_receiver) {
      const float floor_y=p->floor_y-8+(p->pressure_transform?0:(((int)floorf(centre_x-p->x0+.001f))&3));
      assert(max_y<=floor_y+(lip?1.52f:.02f)&&min_y>=floor_y-12);
    }
    if(lip) {
      ++p->lip_triangles;
      const float floor_y=p->snow_receiver?p->floor_y-8+(p->pressure_transform?0:(((int)floorf(centre_x-p->x0+.001f))&3)):p->floor_y;
      /* Real front only: no actor-plane or UV-depth-cut curtains. Every
       * feather reaches exactly1.5 native pixels below its actual floor,
       * and the lower vertices alone are transparent. */
      for(unsigned j=0;j<3;++j) {
        const float y=xyz[j][1];
        if(y>floor_y+.02f){assert(fabsf(y-floor_y-1.5f)<.02f);assert(vertices[at+j].color.a==0);}
      }
      if(min_y>=floor_y-.02f&&max_y>floor_y+.02f)
        p->lip_feather_area+=fabsf((xyz[1][0]-xyz[0][0])*(xyz[2][1]-xyz[0][1])-
            (xyz[1][1]-xyz[0][1])*(xyz[2][0]-xyz[0][0]))*.5f;
    }
  }
}
static int GroundCoverVertexOrder(const void *a,const void *b) {
  const ArRenderVertex2D *x=a,*y=b;
  if(x->position.x!=y->position.x)return x->position.x<y->position.x?-1:1;
  if(x->position.y!=y->position.y)return x->position.y<y->position.y?-1:1;
  if(x->color.a!=y->color.a)return x->color.a<y->color.a?-1:1;
  return 0;
}
static unsigned GroundCoverFixture(unsigned scenario) {
  s_count=0;const unsigned kind=scenario/4;const bool right=scenario&1,shared=(scenario%4)<2;
  GroundCoverProbe probe={.kind=kind,.shared_depth=shared,.dedicated=kind>=2,
      .x0=kind==2?408:kind==3?720:kind==4?304:80,.x1=kind==1?96:kind==2?440:kind==3?752:kind==4?352:160,
      .floor_y=kind==1?112:kind==4?752:120,.z0=kind==2?-.24f:kind==3?-.42f:kind==4?-.33f:0,.z1=kind>=3?-.24f:kind==2?0:.4f};
  probe.gap0=kind==1?86:kind==2?420:kind==3?732:kind==4?320:112;
  probe.gap1=kind==1?90:kind==2?428:kind==3?740:kind==4?352:128;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.ground_cover=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=256;voxel->height=224;voxel->ready=true;voxel->revision=10000+scenario*4;
  if(kind==0) {
    voxel->edge_count=voxel->pass_counts[0]=2;
    voxel->edges[0]=(VoxelEdge){.x=80,.y=120,.length=32,.side=0,.far_rank=2,.colliding=1};
    voxel->edges[1]=(VoxelEdge){.x=128,.y=120,.length=32,.side=0,.far_rank=2,.colliding=1};
    voxel->pass_edges[0][0]=0;voxel->pass_edges[0][1]=1;
  } else if(kind==1) {
    voxel->authored=calloc(1,sizeof(*voxel->authored));voxel->authored_at=calloc(16*14,sizeof(*voxel->authored_at));
    voxel->authored_art=calloc(2,sizeof(*voxel->authored_art));voxel->authored_contours=calloc(2,sizeof(*voxel->authored_contours));
    assert(voxel->authored&&voxel->authored_at&&voxel->authored_art&&voxel->authored_contours);
    voxel->authored_count=1;voxel->authored_at[7*16+5]=1;voxel->authored_art_count=voxel->authored_art_capacity=2;
    voxel->authored_contour_count=voxel->authored_contour_capacity=2;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      voxel->authored_art[0][y*16+x]=0x83e0;voxel->authored_art[1][y*16+x]=x>=6&&x<10?0:0x83e0;
    }
    VoxelAuthoredCell *a=voxel->authored;*a=(VoxelAuthoredCell){.x=80,.y=112,.width=16,.surface=true,
        .faces=kDioramaVoxelFaceMask_All,.contour_count=2};
    for(unsigned q=0;q<4;++q){a->front[q]=0;a->back[q]=.24f;}
    for(unsigned f=0;f<7;++f){a->art_id[f]=f==1;a->pixels[f]=voxel->authored_art[f==1];}
    voxel->authored_contours[0]=(VoxelAuthoredContour){0,0,8,0};
    voxel->authored_contours[1]=(VoxelAuthoredContour){8,0,8,0};
  }
  static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6;trunk.height=kind==4?1024:512;trunk.revision=20000+scenario*4;trunk.side_width=kind>=3?1024:0;
  for(unsigned i=0;i<32*32;++i)trunk.bark[i]=0xff304050;
  for(unsigned i=0;i<16*16;++i)trunk.side_snow[i]=0xffd0e0f0;
  for(unsigned y=(unsigned)probe.floor_y;y<(unsigned)probe.floor_y+8;++y)for(unsigned x=(unsigned)probe.x0;x<(unsigned)probe.x1;++x) {
    if(x>=(unsigned)probe.gap0&&x<(unsigned)probe.gap1)continue;
    if(kind==2)trunk.masks[0][y*48+(x-320)/8]|=1u<<((x-320)&7);
    if(kind>=3) {
      const unsigned at=y*kActionTrunkSideMaskPitch+x/8,bit=1u<<(x&7);
      trunk.side_masks[kActionTrunkSideSolid][at]|=bit;trunk.side_masks[kActionTrunkSideSnow][at]|=bit;
    }
  }
  /* Keep the core opaque across the side bank: no core top receives cover.
   * At x319..320 triangle012 collapses but023 still owns a real tapered cap. */
  if(kind==4)for(unsigned y=736;y<784;++y)for(unsigned x=320;x<352;++x)
    trunk.masks[0][y*48+(x-320)/8]|=1u<<((x-320)&7);
  static ActionSceneEffectFrame effects;effects=(ActionSceneEffectFrame){.authored_count=1};
  effects.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,.generation=77,
      .flags=kActionEffectFlag_Visible,.world_y=probe.floor_y,
      .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
      .particle_lifetime=240,.pulse_ticks=180,.tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},
      .particle_style={.size_min=.5f,.size_max=1,.color_end=0xfff4df,.seed=621,.spread=1},
      .field_style={.strands=4,.placement=2,.ground_depth=4,.ground_width=8,.point_count=2,
          .points={{probe.x0,0},{probe.x1,0}}}};
  DioramaCapture capture={.width=256,.height=224,.bg1_camera_x=kind==2?384:kind==3?608:kind==4?192:0,.camera_y=kind==4?640:0,
      .textures=textures,.pixels=pixels};
  DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=kind==4?.25f:.35f,.tilt_y=right?.35f:-.35f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_ground_cover=GroundCoverObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=shared?&sink:NULL,.map_group=kind>=2?6:1,
      .map_number=kind>=2?6:1,.voxel_geometry=kind<2?voxel:NULL,.trunk=kind>=2?&trunk:NULL,
      .trunk_settings=DioramaTrunk_Default(),.trunk_effects=&effects,.trunk_particles=true};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;r.actor_texture=textures[kDioramaPlane_Obj2];
  for(unsigned frame=0;frame<5;++frame) {
    probe.empty=frame==3;probe.unsupported=frame==4;
    view.camera.tilt_x=frame==2?0:kind==4?.25f:.35f;view.camera.tilt_y=frame==2?0:right?.35f:-.35f;
    probe.begins=probe.ends=probe.supports=probe.caps=probe.triangles=probe.draw_triangles=probe.vertices=0;
    probe.lip_triangles=probe.top_vertices=0;probe.lip_feather_area=0;
    probe.before_actor=probe.after_actor=0;memset(probe.snow_columns,0,sizeof(probe.snow_columns));r.actor_seen=false;
    probe.seen_z0=INFINITY;probe.seen_z1=-INFINITY;
    /* Settled authored cover is scenery; disabling airborne flakes must not
     * erase it or change the actual submitted mesh. */
    if(frame==1)scene.trunk_particles=false;
    if(frame==3) {
      ++voxel->revision;++trunk.revision;
      if(kind==0)voxel->edge_count=voxel->pass_counts[0]=0;
      if(kind==1)voxel->authored[0].faces&=~kDioramaVoxelFaceMask_Top;
      if(kind>=2){memset(trunk.masks,0,sizeof(trunk.masks));memset(trunk.side_masks,0,sizeof(trunk.side_masks));}
    }
    if(frame==4){scene.voxel_geometry=NULL;scene.trunk=NULL;}
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid);
    if(frame==4)assert(!probe.begins&&!probe.ends&&!probe.supports&&!probe.vertices&&!projection.voxel_ground_cover_claims[0]);
    else {
      assert(probe.begins==1&&probe.ends==1&&projection.voxel_ground_cover_claims[0]==1);
      assert(probe.draw_triangles==probe.triangles);
      if(frame==3)assert(!probe.supports&&!probe.vertices);
      else {
        assert(probe.supports&&probe.vertices&&probe.caps);
        if(shared) {
          assert(probe.lip_triangles>0);
          const float expected_width=probe.x1-probe.x0-(probe.gap1-probe.gap0);
          assert(fabsf(probe.lip_feather_area-expected_width*1.5f)<.03f);
        }
        if(kind==4)for(unsigned x=0;x<48;++x)assert(!!probe.snow_columns[x]==(x<16));
        if(shared&&kind>=2&&frame!=2) {
          assert(fabsf(probe.seen_z0-probe.z0)<.002f);
          assert(fabsf(probe.seen_z1-probe.z1)<.002f);
        }
        qsort(probe.mesh,probe.vertices,sizeof(*probe.mesh),GroundCoverVertexOrder);
        GroundCoverMesh *reference=frame==2?&s_ground_cover_neutral_reference[kind][right]:&s_ground_cover_reference[kind][right];
        if(shared&&(frame==0||frame==2)){reference->count=probe.vertices;memcpy(reference->vertices,probe.mesh,probe.vertices*sizeof(*probe.mesh));}
        else {
          assert(reference->count==probe.vertices);
          for(unsigned i=0;i<probe.vertices;++i) {
            assert(fabsf(reference->vertices[i].position.x-probe.mesh[i].position.x)<.002f);
            assert(fabsf(reference->vertices[i].position.y-probe.mesh[i].position.y)<.002f);
            assert(fabsf(reference->vertices[i].color.a-probe.mesh[i].color.a)<.00001f);
          }
        }
      }
    }
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);return s_count;
}

/* Actual native Snow query: a tile-bottom decorative trace must attach to
 * the jagged cap above it, but not across missing ink or another profile.
 * Test translated negative world cells through the same rendering adapter. */
/* Scenarios4..7 exercise real authored-cap hooks after XY translation and
 * nonuniform scale. Standing pressure follows the rendered floor; jumping
 * and the former source-space foot are unchanged. Native priority cuts must
 * not alter the pressed surface. */
static unsigned GroundCoverSnowFixture(unsigned scenario) {
  s_count=0;const bool transformed=scenario>=4,right=scenario&1,shared=(scenario%4)<2;
  GroundCoverProbe probe={.kind=5,.pressure_transform=transformed,.shared_depth=shared,.snow_receiver=true,
      .x0=80,.x1=128,.gap0=96,.gap1=112,.floor_y=120,.z0=0,.z1=.4f};
  Recorder r={.width=960,.height=600,.bound_effect=-1,.ground_cover=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=6;voxel->active_room=5;voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->settings=ActionVoxel_SettingsForRoom(6,5);
  voxel->front=calloc(256*224,1);voxel->soft_surfaces=calloc(16*14,1);assert(voxel->front&&voxel->soft_surfaces);
  if(transformed) {
    voxel->authored=calloc(1,sizeof(*voxel->authored));voxel->authored_at=calloc(16*14,sizeof(*voxel->authored_at));
    voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));voxel->authored_contours=calloc(2,sizeof(*voxel->authored_contours));
    assert(voxel->authored&&voxel->authored_at&&voxel->authored_art&&voxel->authored_contours);
    voxel->authored_count=voxel->authored_art_count=voxel->authored_art_capacity=1;voxel->authored_at[7*16+5]=1;
    voxel->authored_contour_count=voxel->authored_contour_capacity=2;
    for(unsigned i=0;i<256;++i)voxel->authored_art[0][i]=0x83e0;
    VoxelAuthoredCell *a=voxel->authored;*a=(VoxelAuthoredCell){.x=80,.y=112,.width=16,.surface=true,
        .native_front_art=true,.automatic_art=1u<<1,.faces=kDioramaVoxelFaceMask_All,.contour_count=2,
        .transform={.px=88,.py=112,.dx=40,.dy=16,.sx=2,.sy=1.5f,.sz=1}};
    for(unsigned q=0;q<4;++q){a->front[q]=0;a->back[q]=.24f;}
    for(unsigned f=0;f<7;++f)a->pixels[f]=voxel->authored_art[0];
    voxel->authored_contours[0]=(VoxelAuthoredContour){0,0,8,0};voxel->authored_contours[1]=(VoxelAuthoredContour){8,0,8,0};
  }
  GroundCoverMesh unpressed={0},pressed={0};GroundCoverWorldMesh pressed_world={0},current_world={0};
  static ActionSceneEffectFrame effects;
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.35f,.tilt_y=right?.35f:-.35f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_ground_cover=GroundCoverObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=shared?&sink:NULL,.map_group=6,.map_number=5,
      .voxel_geometry=voxel,.trunk_effects=&effects,.trunk_particles=false};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;r.actor_texture=textures[kDioramaPlane_Obj2];
  for(unsigned frame=0;frame<5;++frame) {
    const bool negative=!transformed&&frame==4;const int ox=negative?-96:0,oy=negative?-128:0;
    probe.empty=!transformed&&frame>=1&&frame<=3;probe.x0=80+ox;probe.x1=(transformed?96:128)+ox;probe.gap0=96+ox;probe.gap1=(transformed?96:112)+ox;probe.floor_y=120+oy;
    probe.begins=probe.ends=probe.supports=probe.caps=probe.triangles=probe.draw_triangles=probe.vertices=0;
    probe.lip_triangles=probe.top_vertices=0;probe.lip_feather_area=0;
    probe.before_actor=probe.after_actor=0;memset(probe.snow_columns,0,sizeof(probe.snow_columns));r.actor_seen=false;
    probe.seen_z0=INFINITY;probe.seen_z1=-INFINITY;
    voxel->origin_x=ox;voxel->origin_y=oy;voxel->revision=100000+scenario*64+frame;
    voxel->profile=!transformed&&frame==2?kActionVoxelProfile_FillmoreForest:kActionVoxelProfile_NorthwallSnow;
    memset(voxel->front,0,256*224);memset(voxel->soft_surfaces,!transformed&&frame==3?0:1,16*14);
    voxel->edge_count=voxel->pass_counts[0]=0;
    for(unsigned x=80;x<(transformed?96:128);++x) {
      if(!transformed&&x>=96&&x<112)continue;
      const unsigned y=112+(transformed?0:(x&3)),end=!transformed&&frame==1?119:120;
      for(unsigned yy=y;yy<end;++yy)voxel->front[yy*256+x]=kVoxelSolid;
      const unsigned i=voxel->edge_count++;
      voxel->edges[i]=(VoxelEdge){.x=(int)x+ox,.y=(int)y+oy,.length=1,.side=0,.far_rank=2,.colliding=1,.soft_surface=1};
      voxel->pass_edges[0][voxel->pass_counts[0]++]=i;
      int bottom=0;const bool queried=ActionVoxel_SnowReceiverRun(voxel,(int)x+ox,(int)y+oy,&bottom);
      if(!transformed&&(frame==2||frame==3))assert(!queried);else assert(queried&&bottom==(int)end+oy);
    }
    if(transformed)voxel->edge_count=voxel->pass_counts[0]=0;
    int bottom=0;assert(!ActionVoxel_SnowReceiverRun(voxel,100+ox,112+oy,&bottom));
    capture.bg1_camera_x=ox;capture.camera_y=oy;
    effects=(ActionSceneEffectFrame){.authored_count=1};
    effects.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,.generation=transformed?91:91+frame,
      .flags=kActionEffectFlag_Visible,.world_y=probe.floor_y,
      .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
      .particle_lifetime=240,.pulse_ticks=180,.tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},
      .particle_style={.size_min=.5f,.size_max=1,.color_end=0xfff4df,.seed=621,.spread=1},
      .field_style={.strands=4,.placement=2,.ground_depth=4,.ground_width=8,.wake=1,.point_count=2,
          .points={{probe.x0,0},{probe.x1,0}}}};
    if(transformed&&frame){effects.actor_count=1;effects.actors[0]=(ActionEffectActor){.visible=1,.player=1,.body_radius=8,
        .x=frame==3?88:128,.feet_y=frame==3?112:frame==2?108:128,.wake={.age=kActionActorWakeTicks}};}
    if(transformed)for(unsigned q=0;q<4;++q)voxel->authored[0].priority[q]=frame==4&&(q&1);
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection))&&projection.valid);
    assert(probe.begins==1&&probe.ends==1&&projection.voxel_ground_cover_claims[0]==1);
    assert(probe.draw_triangles==probe.triangles);
    if(probe.empty)assert(!probe.supports&&!probe.vertices);
    else {
      assert(probe.vertices&&probe.supports);
      if(shared){assert(probe.lip_triangles>0);assert(fabsf(probe.lip_feather_area-(transformed?16:32)*1.5f)<.03f);}
      for(unsigned x=0;x<48;++x)assert(!!probe.snow_columns[x]==(x<16||(!transformed&&x>=32)));
      if(transformed) {
        qsort(probe.mesh,probe.vertices,sizeof(*probe.mesh),GroundCoverVertexOrder);
        GroundCoverMesh *reference=frame==1?&pressed:&unpressed;
        if(frame<2){reference->count=probe.vertices;memcpy(reference->vertices,probe.mesh,probe.vertices*sizeof(*probe.mesh));}
        else if(frame<4) {
          assert(reference->count==probe.vertices);
          for(unsigned i=0;i<probe.vertices;++i){assert(fabsf(reference->vertices[i].position.x-probe.mesh[i].position.x)<.002f);assert(fabsf(reference->vertices[i].position.y-probe.mesh[i].position.y)<.002f);}
        }
        if(frame==1){bool changed=unpressed.count!=pressed.count;for(unsigned i=0;i<probe.vertices&&!changed;++i)changed=fabsf(unpressed.vertices[i].position.y-pressed.vertices[i].position.y)>.01f;assert(changed);}
        if(shared&&(frame==1||frame==4)) {
          GroundCoverWorldMesh *world=frame==1?&pressed_world:&current_world;world->count=probe.top_vertices;
          memcpy(world->xyz,probe.pressure_xyz,probe.top_vertices*sizeof(*world->xyz));
          if(frame==4){GroundCoverSamePressureSamples(&pressed_world,&current_world,ActionVoxel_PlayRecess(voxel));GroundCoverSamePressureSamples(&current_world,&pressed_world,ActionVoxel_PlayRecess(voxel));}
        }
        GroundCoverMesh *path_reference=&s_ground_pressure_reference[right][frame];
        if(shared){path_reference->count=probe.vertices;memcpy(path_reference->vertices,probe.mesh,probe.vertices*sizeof(*probe.mesh));}
        else{assert(path_reference->count==probe.vertices);for(unsigned i=0;i<probe.vertices;++i){assert(fabsf(path_reference->vertices[i].position.x-probe.mesh[i].position.x)<.002f);assert(fabsf(path_reference->vertices[i].position.y-probe.mesh[i].position.y)<.002f);}}
      }
    }
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);return s_count;
}

/* Four paths per family: both camera signs, shared depth and portable draws. */
static unsigned GroundCoverTransformTaperFixture(unsigned scenario) {
  assert(scenario<8);return scenario<4?GroundCoverFixture(16+scenario):GroundCoverSnowFixture(scenario);
}
