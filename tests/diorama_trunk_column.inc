/* Verify actual submitted primary and supplemental geometry, including a
 * colliding platform across the decorative shaft and runtime mode changes. */
typedef struct TrunkColumnProbe {
  Recorder *recorder;bool enabled;const SceneryShadingField *exposure;unsigned front,rear,caps,guards,shaded,frost,round_caps;
} TrunkColumnProbe;
static TrunkColumnProbe *s_trunk_column_probe;
static bool TrunkColumnGeometry(void *context,ArRenderTexture texture,const ArRenderVertex2D *v,int nv,
    const int32_t *indices,int ni,const ArRenderDrawState *state) {
  TrunkColumnProbe *probe=s_trunk_column_probe;
  if(probe&&!texture.value&&nv&&fabsf(v[0].color.b-.97f)<.000001f) {
    assert(nv%4==0);
    Recorder *r=probe->recorder;const DioramaProjection *p=r->projection;
    const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f;
    const float oy=r->capture->camera_y-r->capture->authentic_y0+1;
    for(int first=0;first<nv;first+=4) {
      ArRenderPointF center={0};for(unsigned j=0;j<4;++j) {
        center.x+=v[first+j].position.x*.25f;center.y+=v[first+j].position.y*.25f;
      }
      bool anchored=false;
      /* Any retained sparkle must belong to one of the fixture's 8x8 native
       * rim pixels, on the current shell front rather than floating at Z=0. */
      for(unsigned y=160;y<168;++y)for(unsigned x=328;x<336;++x) {
        Scene3DPoint point;
        assert(Scene3D_ProjectWorldPoint(p->matrix,((x+.5f-ox)/r->capture->width-.5f)*p->aspect_x,
            (.5f-(y+.35f-oy)/r->capture->height)*p->height_scale,probe->enabled?TrunkRimExpectedFront(x+.5f):0,
            p->output_width,p->output_height,&point));
        anchored|=fabsf(point.x-center.x)<.02f&&fabsf(point.y-center.y)<.02f;
      }
      assert(anchored);++probe->frost;
    }
  }
  return Geometry(context,texture,v,nv,indices,ni,state);
}
static bool TrunkColumnDepth(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  TrunkColumnProbe *probe=context;Recorder *r=probe->recorder;
  if(probe->enabled&&draw->plane==kDioramaPlane_Bg1Hi&&
      r->textures[draw->texture.value].width==32&&r->textures[draw->texture.value].height==32) {
    /* All caps on this fixture belong to its native 8x8 rim patch. Recover
     * the exact independently projected endpoints, including the curved front. */
    const DioramaProjection *p=r->projection;
    const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f,oy=r->capture->camera_y+1;
    for(int at=0;at<draw->vertex_count;++at) {
      bool matched=false;
      for(unsigned y=160;y<=168&&!matched;++y)for(unsigned x=328;x<=336&&!matched;++x)for(unsigned end=0;end<2;++end) {
        const float z=end?-.24f:TrunkRimExpectedFront(x);Scene3DPoint xy;float depth;
        assert(Scene3D_ProjectWorldPointWithDepth(p->matrix,((x-ox)/r->capture->width-.5f)*p->aspect_x,
            (.5f-(y-oy)/r->capture->height)*p->height_scale,z,p->output_width,p->output_height,&xy,&depth));
        matched=fabsf(xy.x-draw->vertices[at].position.x)<.005f&&
            fabsf(xy.y-draw->vertices[at].position.y)<.005f&&fabsf(depth-draw->depths[at])<.000002f;
        if(matched)break;
      }
      assert(matched);++probe->round_caps;
    }
  }
  if(draw->plane==SR_PPU_OVERLAY_BG1&&!draw->native_actor) {
    const bool face=draw->clip_w||fabsf(draw->vertices[0].color.r-1)<.000001f;
    if(!probe->enabled) {
      for(int i=0;i<draw->vertex_count;++i)
        assert(fabsf(draw->depths[i]-PlaneDepth(r->projection,-.24f,draw->vertices[i].position))<.000002f);
    } else if(face) {
      assert(draw->lighting&&draw->clip_w);
      const ArRenderRectF uv=draw->lighting->attributes_uv;
      const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f-r->capture->obj_apron;
      const float oy=r->capture->camera_y-r->capture->authentic_y0+1;
      for(int triangle=0;triangle<draw->index_count;triangle+=3) {
        float world[3][2],cx=0,cy=0;
        const ArRenderVertex2D *vertices[3];float ws[3];
        for(unsigned j=0;j<3;++j) {
          const unsigned i=draw->indices[triangle+j];vertices[j]=draw->vertices+i;ws[j]=draw->clip_w[i];
          const ArRenderPointF t=vertices[j]->tex_coord;
          world[j][0]=ox+(uv.x+t.x*uv.w)*SR_PPU_SURFACE_MAX_WIDTH;
          world[j][1]=oy+(uv.y+t.y*uv.h)*SR_PPU_SURFACE_MAX_HEIGHT;
          cx+=world[j][0]/3;cy+=world[j][1]/3;
        }
        const bool recessed=cx>=480&&cx<544&&((int)floorf(cy/8)%8)!=4;
        const float z=recessed?-.305f:-.24f;
        for(unsigned j=0;j<3;++j) {
          const unsigned i=draw->indices[triangle+j];const ArRenderVertex2D *v=vertices[j];
          assert(fabsf(draw->depths[i]-PlaneDepth(r->projection,z,v->position))<.000002f);
          Scene3DClipPoint clip;
          assert(Scene3D_TransformToClip(r->projection->matrix,
              ((world[j][0]-ox)/r->capture->width-.5f)*r->projection->aspect_x,
              (.5f-(world[j][1]-oy)/r->capture->height)*r->projection->height_scale,z,&clip));
          assert(fabsf(ws[j]-clip.w)<.000001f);
          if(probe->exposure) {
            const float shade=probe->exposure->sample(probe->exposure->userdata,
                (world[j][0]-ox)/640.f,(world[j][1]-oy)/352.f,(cx-ox)/640.f,(cy-oy)/352.f);
            assert(fabsf(v->color.r-shade)<.000001f);++probe->shaded;
          }
          assert(fabsf(v->position.x-(clip.x/clip.w*.5f+.5f)*r->projection->output_width)<.005f);
          assert(fabsf(v->position.y-(.5f-clip.y/clip.w*.5f)*r->projection->output_height)<.005f);
        }
        /* The native center must survive perspective interpolation inside
         * the triangle, not merely match the three projected endpoints. */
        Scene3DClipPoint center;
        assert(Scene3D_TransformToClip(r->projection->matrix,
            ((cx-ox)/r->capture->width-.5f)*r->projection->aspect_x,
            (.5f-(cy-oy)/r->capture->height)*r->projection->height_scale,z,&center));
        const float px=(center.x/center.w*.5f+.5f)*r->projection->output_width;
        const float py=(.5f-center.y/center.w*.5f)*r->projection->output_height;
        const ArRenderPointF a=vertices[0]->position,b=vertices[1]->position,c=vertices[2]->position;
        const float area=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
        /* Coincident authored/native cuts can leave zero-area fan triangles;
         * they have no covered fragments and no interior to interpolate. */
        if(fabsf(area)>.01f) {
          const float la=((b.y-c.y)*(px-c.x)+(c.x-b.x)*(py-c.y))/area;
          const float lb=((c.y-a.y)*(px-c.x)+(a.x-c.x)*(py-c.y))/area;
          const float weights[3]={la,lb,1-la-lb};float denominator=0,tx=0,ty=0;
          for(unsigned j=0;j<3;++j) {
            const float weight=weights[j]/ws[j];denominator+=weight;
            tx+=weight*world[j][0];ty+=weight*world[j][1];
          }
          assert(fabsf(tx/denominator-cx)<.01f&&fabsf(ty/denominator-cy)<.01f);
        }
        if(recessed)++probe->rear;else ++probe->front;
        if(cy<oy||cy>=oy+r->capture->height)++probe->guards;
      }
    } else {
      for(int i=0;i<draw->vertex_count;++i) {
        const ArRenderPointF xy=draw->vertices[i].position;
        assert(fabsf(draw->depths[i]-PlaneDepth(r->projection,-.24f,xy))<.000002f ||
            fabsf(draw->depths[i]-PlaneDepth(r->projection,-.305f,xy))<.000002f);
        ++probe->caps;
      }
    }
  }
  return DepthGeometry(r,device,draw);
}
static float TrunkColumnExposure(void *context,float u,float v,float inside_u,float inside_v) {
  (void)u;(void)v;
  const SceneryShadingField *field=context;
  return inside_u<field->x[0]?(inside_v<field->y[0]?.4f:.6f):1;
}
static unsigned TrunkColumnFixture(unsigned scenario) {
  assert(scenario<4);s_count=0;
  static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6+(scenario>>1);trunk.height=512;trunk.revision=9000+scenario;trunk.column_geometry=true;trunk.side_width=1024;
  for(unsigned y=160;y<168;++y)trunk.masks[0][y*48+1]=255;
  for(unsigned y=0;y<512;++y)if(y/8%8==4) {
    trunk.column_front[y/8]=255;
    memset(trunk.column_caps+y*8,255,8);
  }
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true,.trunk=&trunk};
  ArRenderBackendOps ops=kOps;ops.draw_geometry=TrunkColumnGeometry;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  const int width=scenario>=2?624:384,origin=scenario>=2?224:320;
  const DioramaCapture capture={.width=width,.height=224,.bg1_camera_x=origin+(width-256)/2,.camera_y=127,.textures=textures,.pixels=pixels};
  DioramaTrunkGuardView guard={.capture_generation=1,.source_revision=trunk.revision};
  assert(DioramaTrunkGuard_Resolve(origin,128,width,224,(DioramaTrunkGuardBounds){0,0,1024,512},&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=128,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  const DioramaView view={.viewport={0,0,960,600},.visible_width=width,.distance_scale=1,
      .camera={.tilt_x=.45f,.tilt_y=scenario&1?-.5f:.5f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.depth_of_field=true};
  TrunkColumnProbe probe={.recorder=&r};s_trunk_column_probe=&probe;const DioramaDepthSink sink={.submit=TrunkColumnDepth,.userdata=&probe};
  DioramaScene scene={.render=&options,.map_group=6,.map_number=trunk.room,.trunk=&trunk,
      .trunk_settings=DioramaTrunk_Default(),.trunk_guard=&guard,.depth_sink=&sink};
  static ActionSceneEffectFrame effects;memset(&effects,0,sizeof(effects));
  effects.trunk=DioramaTrunk_Default();effects.authored_count=1;
  effects.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredFan,.world_x=332,.world_y=164,
      .field_style={.opening=true},.tuning={.intensity=2.5f,.color=0xffffff}};
  scene.trunk_settings.wrap=0;scene.environment_effects=&effects;scene.environment_lighting=true;
  DioramaProjection projection;r.capture=&capture;r.projection=&projection;
  unsigned frost=0;
  const float cut_x=(512-origin)/640.f,cut_y=36/352.f;
  SceneryShadingField exposure={.x=&cut_x,.y=&cut_y,.nx=1,.ny=1,.sample=TrunkColumnExposure};
  exposure.userdata=&exposure;
  for(unsigned frame=0;frame<6;++frame) {
    probe.exposure=scene.scenery_exposure=frame==4?&exposure:NULL;
    r.trunk_voxel=probe.enabled=scene.voxel_mode=frame!=2;probe.front=probe.rear=probe.caps=probe.guards=probe.shaded=0;
    probe.round_caps=0;
    r.depth_rim_vertices=probe.frost=0;scene.environment_lighting=frame!=5;
    const unsigned creates=r.creates;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if(probe.enabled)assert(probe.front&&probe.rear&&probe.caps&&probe.guards);
    else assert(!probe.caps);
    if(frame==1)assert(r.creates==creates);
    if(frame==4)assert(probe.shaded);
    if(probe.enabled)assert(r.depth_rim_vertices);
    if(probe.enabled)assert(probe.round_caps);
    if(!frame){frost=probe.frost;assert(frost);}
    else assert(probe.frost==(frame==5?0:frost));
  }
  Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<3;++i)Destroy(&r,guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);s_trunk_column_probe=NULL;return s_count;
}
