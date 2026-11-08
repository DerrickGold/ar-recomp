#include "diorama/diorama_voxel_guard_upload.h"
typedef struct VoxelGuardProbe {
  Recorder *r;const DioramaCapture *capture;const DioramaVoxelGuardView *guard;
  const DioramaProjection *projection;const ActionVoxelGeometry *geometry;
  unsigned draws[3],body[3],edges,owned[2],depth_checks,copies;
  DioramaVoxelPickFace faces[4096],soft[8];unsigned face_count,soft_count,soft_checks;
  ArRenderTexture stale[3];unsigned helper_high[2];
  unsigned source_runs[3],body_batches[3];int last_source[3];
  double primary_area;unsigned primary_triangles,prologue_checks;
  bool check_prologue,empty_primary,rear_copy_seen;
  bool pending_copy;
} VoxelGuardProbe;
static VoxelGuardProbe *s_voxel_guard_probe;
static void VoxelGuardRejectStale(ArRenderTexture texture) {
  for(unsigned i=0;i<3;++i)if(s_voxel_guard_probe->stale[i].value)
    assert(texture.value!=s_voxel_guard_probe->stale[i].value);
}
static int VoxelGuardSourceBand(ArRenderTexture texture) {
  VoxelGuardProbe *p=s_voxel_guard_probe;
  for(unsigned i=0;i<3;++i)if(texture.value==p->guard->textures[i].value)return (int)i;
  return -1;
}
static bool VoxelGuardTexture(void *ctx,ArRenderTexture texture,const ArRenderRectF *src,
    const ArRenderRectF *dst,const ArRenderDrawState *state) {
  Recorder *r=ctx;VoxelGuardProbe *p=s_voxel_guard_probe;
  VoxelGuardRejectStale(texture);
  if(r->state.target.value) {
    r->copied_sources[r->state.target.value]=r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
    if(VoxelGuardSourceBand(texture)>=0) {
      if(VoxelGuardSourceBand(texture)==0&&!p->rear_copy_seen) {
        p->rear_copy_seen=true;
        if(p->check_prologue) {
          /* Independent native8x6 grid oracle. Before the first guard target
           * copy, V3 had submitted five full rows and only the first triangle
           * of row six, clipped at eight pixels above the capture end. */
          const double width=p->guard->layout.lower.width,height=p->capture->height;
          const double cell_area=(width/8)*(height/6)*.5;
          const double omitted=8/(height/6);
          const double expected=width*height*5/6+cell_area*(1-omitted*omitted);
          assert(fabs(p->primary_area-(p->empty_primary?0:expected))<.15);
          assert(p->primary_triangles==(p->empty_primary?0u:82u));
          ++p->prologue_checks;
        }
      }
      assert(src&&src->x==0&&src->y==0&&src->w==p->guard->layout.lower.width&&src->h==80);
      assert(dst);
      if(dst->h==320) {
        assert(dst->w==src->w*4&&!p->pending_copy);p->pending_copy=true;
      } else {assert(dst->w==src->w&&dst->h==80&&(dst->y==0||dst->y==80));}
      ++p->copies;
    }
  }
  return Texture(ctx,texture,src,dst,state);
}
static bool VoxelGuardUpdate(void *ctx,ArRenderTexture texture,const ArRenderRectI *rect,const void *pixels,int pitch) {
  Recorder *r=ctx;
  if(r->textures[texture.value].width==512&&r->textures[texture.value].height==2048) {
    assert(rect&&pixels&&pitch==512*4);++r->uploads;return true;
  }
  return Update(ctx,texture,rect,pixels,pitch);
}
static void VoxelGuardObserve(void *ctx,const DioramaVoxelPickFace *face) {
  VoxelGuardProbe *p=ctx;
  if(face->authored&&face->kind==kDioramaVoxelFace_Front&&face->cell_x==8) {
    if(face->cell_y==13)++p->owned[0];else if(face->cell_y==15)++p->owned[1];
  }
  if(!face->authored&&(face->kind==kDioramaVoxelFace_Front||face->kind==kDioramaVoxelFace_Backing)) {
    assert(p->face_count<4096);p->faces[p->face_count++]=*face;
  }
  if(!face->authored&&face->kind==kDioramaVoxelFace_Side&&
      fabsf(face->world_xy[0]-192)<.1f&&face->world_xy[1]>=219.9f&&face->world_xy[1]<=222.1f) {
    assert(p->soft_count<8);p->soft[p->soft_count++]=*face;
  }
}
static bool VoxelGuardBary(const float xy[6],float x,float y,float weights[3]) {
  const float d=(xy[2]-xy[0])*(xy[5]-xy[1])-(xy[3]-xy[1])*(xy[4]-xy[0]);
  if(fabsf(d)<.000001f)return false;
  weights[1]=((x-xy[0])*(xy[5]-xy[1])-(y-xy[1])*(xy[4]-xy[0]))/d;
  weights[2]=((xy[2]-xy[0])*(y-xy[1])-(xy[3]-xy[1])*(x-xy[0]))/d;
  weights[0]=1-weights[1]-weights[2];
  return weights[0]>=-.0001f&&weights[1]>=-.0001f&&weights[2]>=-.0001f;
}
static void VoxelGuardCheck(ArRenderTexture texture,const ArRenderVertex2D *v,int nv,const int32_t *indices,int ni,
    const float *depths,const float *w,const DioramaSceneryLightEffectParams *light) {
  VoxelGuardRejectStale(texture);
  VoxelGuardProbe *p=s_voxel_guard_probe;Recorder *r=p->r;
  const ArRenderTexture source=r->copied_sources[texture.value].value?r->copied_sources[texture.value]:texture;
  int band=VoxelGuardSourceBand(source);const bool guard=band>=0;
  if(!guard) {
    const int planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
    for(unsigned i=0;i<3;++i)if(source.value==p->capture->textures[planes[i]].value)band=(int)i;
  }
  if(band<0)return;
  const bool body=!w&&v[0].color.r==1;
  if(body) {
    if(p->last_source[band]!=(int)guard){++p->source_runs[band];p->last_source[band]=(int)guard;}
    ++p->body_batches[band];
    if(band==0&&!guard&&!p->rear_copy_seen) {
      const float width=source.value==texture.value?640:p->guard->layout.lower.width;
      const float height=source.value==texture.value?352:p->capture->height;
      for(int i=0;i<ni;i+=3) {
        const ArRenderPointF a=v[indices[i]].tex_coord,b=v[indices[i+1]].tex_coord,c=v[indices[i+2]].tex_coord;
        const double area=fabs(((double)b.x-a.x)*(c.y-a.y)-((double)b.y-a.y)*(c.x-a.x))*width*height*.5;
        if(area>.0001){p->primary_area+=area;++p->primary_triangles;}
      }
    }
  }
  if(!guard)return;
  if(r->textures[texture.value].height==160)band=light&&light->mode==2?light->priority.high_band?1:2:r->actor_seen?1:2;
  const bool filtered=texture.value!=source.value;
  const float width=filtered?p->guard->layout.lower.width:640;
  const float first=p->guard->layout.primary_height-16;
  ++p->draws[band];if(body)++p->body[band];else ++p->edges;
  if(filtered)p->pending_copy=false;
  if(body)assert(r->actor_seen==(band==1));
  if(light) {
    assert(fabsf(light->attributes_uv.y-first/352)<.000001f);
    assert(light->attributes_uv.w==width/640&&light->attributes_uv.h==80.f/352);
    ++p->depth_checks;
  }
  for(int i=0;i<nv;++i) {
    const float sx=v[i].tex_coord.x*width,sy=first+v[i].tex_coord.y*80;
    assert(sy>=p->guard->layout.primary_height-8-.001f&&sy<=p->guard->layout.primary_height+64+.001f);
    if(!body&&sx>=320&&sx<=344&&sy>=235.9f&&sy<=252.1f) {
      /* High native branch/rear art is submitted before actors in the low
       * slot. Its guard source must remain High regardless of that slot. */
      assert(band==1&&!r->actor_seen);
      ++p->helper_high[sx<332?0:1];
    }
    if(body) {
      /* Independent world projection uses the original visible width and
       * its64px apron. The finite guard is not stretched into a new viewport. */
      const DioramaCapture *c=p->capture;const DioramaProjection *pr=p->projection;
      const float z=band==0?ActionVoxel_Depth(p->geometry):0;
      const float scale=1+z/fmaxf(1,pr->camera_distance);
      Scene3DClipPoint clip;
      assert(Scene3D_TransformToClip(pr->matrix,((sx-c->obj_apron)/c->width-.5f)*pr->aspect_x*scale,
          (.5f-sy/c->height)*pr->height_scale*scale,-z,&clip));
      if(fabsf(sy-(p->guard->layout.primary_height-8))>.001f) {
        assert(fabsf(v[i].position.x-(clip.x/clip.w*.5f+.5f)*pr->output_width)<.006f);
        assert(fabsf(v[i].position.y-(.5f-clip.y/clip.w*.5f)*pr->output_height)<.006f);
        if(depths)assert(fabsf(depths[i]-(clip.z/clip.w*.5f+.5f))<.000003f);
      } else {
        /* Existing native body triangles use screen-linear UVs. Their new
         * seam point must stay on that exact submitted triangle, rather than
         * being reprojected onto an idealized, differently sampled plane. */
        bool found=false;const int planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
        const unsigned tri[2][3]={{0,1,2},{0,2,3}};
        for(unsigned f=0;f<p->face_count&&!found;++f)if(p->faces[f].plane==planes[band])
          for(unsigned t=0;t<2&&!found;++t) {
            float xy[6],weights[3];for(unsigned j=0;j<3;++j) {
              xy[j*2]=p->faces[f].world_xy[tri[t][j]*2];xy[j*2+1]=p->faces[f].world_xy[tri[t][j]*2+1];
            }
            if(!VoxelGuardBary(xy,sx,sy,weights))continue;
            float px=0,py=0;for(unsigned j=0;j<3;++j) {
              px+=weights[j]*p->faces[f].screen_xy[tri[t][j]*2];py+=weights[j]*p->faces[f].screen_xy[tri[t][j]*2+1];
            }
            found=fabsf(v[i].position.x-px)<.006f&&fabsf(v[i].position.y-py)<.006f;
          }
        assert(found);
      }
    } else if(fabsf(sx-193)<.001f&&p->soft_count) {
      /* The left riser samples y+.5+6*z/depth. Recover each original corner's
       * z independently from W, then interpolate exactly as the native path. */
      const unsigned tri[2][3]={{0,1,2},{0,2,3}};bool found=false;
      for(unsigned f=0;f<p->soft_count&&!found;++f)for(unsigned t=0;t<2&&!found;++t) {
        const DioramaVoxelPickFace *face=&p->soft[f];float xy[6],weights[3];
        for(unsigned j=0;j<3;++j){xy[j*2]=face->screen_xy[tri[t][j]*2];xy[j*2+1]=face->screen_xy[tri[t][j]*2+1];}
        if(!VoxelGuardBary(xy,v[i].position.x,v[i].position.y,weights))continue;
        float expected=0,denom=0;
        for(unsigned j=0;j<3;++j) {
          const unsigned at=tri[t][j];const float yy=roundf(face->world_xy[at*2+1]);
          const DioramaProjection *pr=p->projection;
          const float wx=((192.f-p->capture->obj_apron)/p->capture->width-.5f)*pr->aspect_x;
          const float wy=(.5f-yy/p->capture->height)*pr->height_scale;
          const float base=pr->matrix[3]*wx+pr->matrix[7]*wy+pr->matrix[15];
          const float slope=(pr->matrix[3]*wx+pr->matrix[7]*wy)/pr->camera_distance-pr->matrix[11];
          const float z=(face->clip_w[at]-base)/slope;
          const float weight=weights[j]/(w?face->clip_w[at]:1);
          expected+=weight*(yy+.5f+6*z/ActionVoxel_Depth(p->geometry));denom+=weight;
        }
        found=fabsf(sy-expected/denom)<.003f;
      }
      assert(found);++p->soft_checks;
    }
  }
}
static bool VoxelGuardGeometry(void *ctx,ArRenderTexture texture,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const ArRenderDrawState *state) {
  /* Depth callback checks the same geometry with explicit receiver metadata. */
  if(!s_depth_fixture)VoxelGuardCheck(texture,v,nv,indices,ni,NULL,NULL,NULL);
  return Geometry(ctx,texture,v,nv,indices,ni,state);
}
static bool VoxelGuardDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  VoxelGuardCheck(draw->texture,draw->vertices,draw->vertex_count,draw->indices,draw->index_count,draw->depths,draw->clip_w,draw->lighting);
  return DepthGeometry(ctx,device,draw);
}
static unsigned VoxelGuardFixture(unsigned scenario) {
  assert(scenario<8);s_count=0;s_depth_fixture=scenario>=4;
  Recorder r={.width=1280,.height=800,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.draw_texture=VoxelGuardTexture;ops.draw_geometry=VoxelGuardGeometry;ops.update_texture=VoxelGuardUpdate;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=4;voxel->active_room=2;voxel->profile=kActionVoxelProfile_AitosMountains;
  voxel->settings=ActionVoxel_SettingsForRoom(4,2);voxel->width=1024;voxel->height=512;voxel->ready=true;
  const VoxelEdge edges[]={
    {.x=160,.y=216,.length=24,.side=1,.far_rank=2,.colliding=1},
    {.x=192,.y=220,.length=2,.side=3,.far_rank=2,.colliding=1,.soft_surface=1},
    {.x=208,.y=240,.length=16,.side=0,.far_rank=2,.colliding=1},
    {.x=224,.y=248,.length=16,.side=2,.far_rank=2,.colliding=1},
    {.x=256,.y=236,.length=16,.side=1,.far_rank=2,.high_priority=1}};
  voxel->edge_count=5;memcpy(voxel->edges,edges,sizeof(edges));
  voxel->pass_counts[0]=4;for(unsigned i=0;i<4;++i)voxel->pass_edges[0][i]=i;
  voxel->pass_counts[1]=1;voxel->pass_edges[1][0]=4;
  voxel->high_deck_count=1;voxel->patch_count=1;
  voxel->patches[0]=(VoxelPatch){.x=320,.y=236,.width=8,.height=8,.high_priority=1,.kind=kVoxelPatch_BranchRear};
  voxel->rear_trim_count=1;voxel->rear_trims[0]=(VoxelRearTrim){.x=336,.y=236,.width=8};
  voxel->authored=calloc(2,sizeof(*voxel->authored));voxel->authored_count=voxel->hidden_native_front_count=2;
  voxel->authored_at=calloc(64*32,sizeof(*voxel->authored_at));voxel->authored_art=calloc(1,sizeof(*voxel->authored_art));
  voxel->authored_art_count=voxel->authored_art_capacity=1;
  assert(voxel->authored&&voxel->authored_at&&voxel->authored_art);
  for(unsigned i=0;i<256;++i)voxel->authored_art[0][i]=0xffff;
  for(unsigned i=0;i<2;++i) {
    VoxelAuthoredCell *a=&voxel->authored[i];
    *a=(VoxelAuthoredCell){.x=128,.y=i?240:208,.width=16,.surface=true,.native_front_hidden=true,
      .faces=kDioramaVoxelFaceMask_Front};
    a->transform=(DioramaVoxelTransform){.sx=1,.sy=1,.sz=1};
    for(unsigned j=0;j<7;++j)a->pixels[j]=voxel->authored_art[0];
    for(unsigned q=0;q<4;++q)a->back[q]=ActionVoxel_Depth(voxel);
    voxel->authored_at[a->y/16*64+8]=(uint16_t)(i+1);
  }
  DioramaCapture capture={.width=496,.height=224,.obj_apron=64,.bg_apron_mask=1,
    .bg1_camera_x=184,.camera_y=-1,.textures=textures,.pixels=pixels};
  const ActionRoomSceneFrameState colors={.brightness=15};capture.voxel_frame=&colors;
  DioramaVoxelGuardView guard={.capture_generation=1,.source_revision=ActionVoxel_Revision(voxel)};
  assert(DioramaVoxelGuard_Resolve(0,0,624,224,(DioramaTrunkGuardBounds){0,0,1024,512},&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=80,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  const DioramaVoxelGuardView original=guard;
  DioramaProjection projection;VoxelGuardProbe probe={.r=&r,.capture=&capture,.projection=&projection,.guard=&guard,.geometry=voxel};
  s_voxel_guard_probe=&probe;r.capture=&capture;r.projection=&projection;r.actor_texture=textures[kDioramaPlane_Obj2];
  const DioramaView view={.viewport={0,0,1280,800},.visible_width=496,.distance_scale=1,
    .camera={.tilt_x=.25f,.tilt_y=scenario&1?.3f:-.3f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.priority_surface=!!(scenario&2),
    .voxel_observe_face=VoxelGuardObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=VoxelGuardDepth,.userdata=&r};
  DioramaScene scene={.render=&options,.map_group=4,.map_number=2,.voxel_geometry=voxel,.voxel_guard=&guard,
    .depth_sink=s_depth_fixture?&sink:NULL};
  for(unsigned frame=0;frame<12;++frame) {
    guard=original;memset(probe.draws,0,sizeof(probe.draws));memset(probe.body,0,sizeof(probe.body));
    memset(probe.owned,0,sizeof(probe.owned));probe.edges=probe.depth_checks=probe.face_count=probe.soft_count=probe.soft_checks=0;r.actor_seen=false;
    memset(probe.stale,0,sizeof(probe.stale));memset(probe.helper_high,0,sizeof(probe.helper_high));
    memset(probe.source_runs,0,sizeof(probe.source_runs));memset(probe.body_batches,0,sizeof(probe.body_batches));
    probe.primary_area=0;probe.primary_triangles=probe.prologue_checks=0;probe.rear_copy_seen=false;
    probe.check_prologue=frame==0||frame==4||frame==9||frame==10;probe.empty_primary=frame==10;
    for(unsigned i=0;i<3;++i)probe.last_source[i]=-1;
    scene.voxel_guard=&guard;scene.voxel_geometry=voxel;
    const ArRenderTexture high=textures[kDioramaPlane_Bg1Hi];
    if(frame==1)scene.voxel_guard=NULL;
    if(frame==2)--guard.source_revision;
    if(frame==3)++guard.layout.lower.world_x;
    if(frame==4||frame==5){textures[kDioramaPlane_Bg1Hi]=ArRenderTexture_Invalid();pixels[kDioramaPlane_Bg1Hi]=NULL;}
    if(frame==5)++guard.layout.lower.world_x;
    if(frame==6)scene.voxel_geometry=NULL;
    if(frame==7)assert(DioramaVoxelGuard_Resolve(0,0,624,224,(DioramaTrunkGuardBounds){0,0,1024,224},&guard.layout));
    if(frame==8)guard.capture_generation=0;
    if(frame==11) {
      /* Even a numerically flat relief field retains the ordered mesh path:
       * grouping eligibility comes from provenance, never a guessed depth. */
      voxel->rear_relief=calloc(64*32,1);assert(voxel->rear_relief);
    }
    /* Empty current content retains its old allocated texture in real native
     * and editor uploaders. Guard admission must never sample that old ink. */
    const int bands[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
    if(frame==9||frame==10)for(unsigned i=0;i<3;++i)if(frame==10||i==1) {
      pixels[bands[i]]=NULL;probe.stale[i]=textures[bands[i]];
    }
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(probe.owned[0]==(frame==6?0u:4u));
    if(frame==0||frame==4||frame==9||frame==10||frame==11) {
      for(unsigned band=0;band<3;++band)assert(probe.draws[band]&&probe.body[band]);
      assert(probe.edges&&probe.owned[1]==4);
      if(probe.soft_count)assert(probe.soft_checks);
      if(s_depth_fixture)assert(probe.depth_checks);
      assert(probe.helper_high[0]&&probe.helper_high[1]);
      if(frame==11)assert(probe.source_runs[0]>2);
      else {
        assert(probe.source_runs[0]==(frame==10?1u:2u));
        assert(probe.prologue_checks==1);
        assert(probe.body_batches[0]==(frame==10?2u:4u));
      }
    } else {
      for(unsigned band=0;band<3;++band)assert(!probe.draws[band]);
      /* Room-owned art already uses a conservative depth/transform visibility
       * guard. Its availability is independent of the native pixel capture. */
      assert(probe.owned[1]==(frame==6?0u:4u));
    }
    assert(!probe.pending_copy);
    textures[kDioramaPlane_Bg1Hi]=high;pixels[kDioramaPlane_Bg1Hi]=&present;
    for(unsigned i=0;i<3;++i)pixels[bands[i]]=&present;
    if(frame==0)printf("Voxel guard batching%u: flat rear %u source runs / %u body submissions\n",scenario,probe.source_runs[0],probe.body_batches[0]);
  }
  s_voxel_guard_probe=NULL;Diorama_ResetCompositorResources(&device);ActionVoxel_Release(voxel);
  for(unsigned i=0;i<3;++i)Destroy(&r,guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
  printf("Voxel lower guard%u: apron world phase, native samples, owned once and stale/empty primary PASS\n",scenario);
  return s_count;
}
