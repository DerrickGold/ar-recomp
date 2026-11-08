/* Free mist owns its finite authored lower envelope, not missing terrain. */
static void TestVoxelCloudAuthoredLowerExtent(void) {
  DioramaProjection p=RakedApronProjection();
  p.voxel_scenery=true;p.texture_width=640;p.texture_height=512;p.texture_x_origin=0;
  p.bg1_plane=(DioramaPlaneProjection){.valid=true,.u1=1,.v1=1,.z_world=-.2f,
      .source_bounds_valid=true,.source_u1=1,.source_v1=.5f};
  ActionEffectProjectionContext context={.diorama_projection=&p};
  ActionEffectInstance e={.kind=kActionEffect_AuthoredCloud,.generation=197,.pulse_generation=197,
      .flags=kActionEffectFlag_Visible,.world_x=256,.world_y=250,
      .render_layer=kActionEffectRenderLayer_Bg1Mist,.projection_plane=kActionEffectProjectionPlane_Bg1,
      .particle_lifetime=840,.pulse_ticks=0,.tuning={.active=1,.color=0xe8e3d4,.intensity=1.15f},
      .particle_style={.size_min=.25f,.size_max=.65f,.color_end=0xb6bcb5,.seed=197,.spread=1},
      .field_style={.textured=1,.rolling=.7f,.strands=8,.amplitude=7,.drift=24,.volume=1},
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-300,-104,300,104}}};
  const ActionEffectInstance original=e;
  ActionEffectLocalRect bounds;ArRenderPointF actual,expected;
  CHECK(ActionEffectProjection_AuthoredCloudExtent(&context,&e));
  CHECK(ActionEffectProjection_ClipBounds(&context,&e,&bounds));
  CHECK(bounds.x0==-256 && bounds.x1==300 && bounds.y0==-104 && bounds.y1==104);
  CHECK(ActionEffectProjection_ProjectPoint(&context,&e,0,40,&actual));
  CHECK(Diorama_ProjectCapturedBg1Point(&p,256,290,&expected,NULL,NULL));
  CHECK(actual.x==expected.x&&actual.y==expected.y);
  CHECK(!ActionEffectProjection_ProjectPoint(&context,&e,-257,40,&actual));
  CHECK(!ActionEffectProjection_ProjectPoint(&context,&e,0,105,&actual));
  e.flags|=kActionEffectFlag_ClipToRect;e.clip_rect=(ActionEffectLocalRect){-3,-3,3,20};
  CHECK(ActionEffectProjection_ClipBounds(&context,&e,&bounds)&&bounds.y1==20);
  CHECK(!ActionEffectProjection_ProjectPoint(&context,&e,0,21,&actual));e=original;
  /* Ground/path snow, native particles, light receiver jobs and ordinary modes
   * retain their exact old finite bounds. No source geometry is changed. */
  for(unsigned negative=0;negative<10;++negative) {
    ActionEffectInstance n=e;ActionEffectProjectionContext c=context;DioramaProjection q=p;c.diorama_projection=&q;
    if(negative==0)q.voxel_scenery=false;
    if(negative==1)n.field_style.point_count=2;
    if(negative==2)n.field_style.ground_depth=4;
    if(negative==3)n.kind=kActionEffect_AuthoredSpray;
    if(negative==4)n.field_style.textured=0;
    if(negative==5)c.scenery_lighting_only=true;
    if(negative==6)c.light_receiver=1;
    if(negative==7)n.field_style.placement=1;
    if(negative==8){n.projection_plane=kActionEffectProjectionPlane_Obj;n.obj_priority=2;q.object_planes[2]=q.bg1_plane;}
    if(negative==9)n.field_style.placement=3;
    CHECK(!ActionEffectProjection_AuthoredCloudExtent(&c,&n));
    CHECK(ActionEffectProjection_ClipBounds(&c,&n,&bounds)&&bounds.y1==6);
    CHECK(!ActionEffectProjection_ProjectPoint(&c,&n,0,40,&actual));
    ActionEffectSourcePrimitive primitive[1];ActionEffectSourceBatch packet={.context=c,.primitives=primitive,.capacity=1};
    const ArRenderPointF corners[4]={{0,0},{1,0},{1,1},{0,1}};
    const ArRenderColorF colors[4]={{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};
    CHECK(ActionEffectSource_Quad(&packet,&n,corners,colors)&&packet.count==1);
    CHECK(((unsigned)primitive[0].origin[2]&32u)==0);
  }
  ActionEffectProjectionContext staged={.staged_voxel_scenery=true};
  CHECK(ActionEffectProjection_AuthoredCloudExtent(&staged,&e));staged.staged_voxel_scenery=false;
  CHECK(!ActionEffectProjection_AuthoredCloudExtent(&staged,&e));
  /* Actual authored meshes equal the ordinary reference on an independently
   * extended clip: same transform, phase, UV, palette, alpha and priority. */
  static ActionSceneEffectFrame frame;frame=(ActionSceneEffectFrame){.authored_count=1};frame.authored[0]=e;
  static ActionSceneEffectRenderBatch mesh,reference,scratch;
  CHECK(ActionSceneDecorationRender_Build(&frame,e.render_layer,false,true,ActionEffectProjection_ProjectPoint,
      ActionEffectProjection_ClipBounds,&context,&mesh));CHECK(mesh.vertex_count>0);
  DioramaProjection wide=p;wide.voxel_scenery=false;wide.bg1_plane.source_v1=361.f/512;
  ActionEffectProjectionContext ref=context;ref.diorama_projection=&wide;
  CHECK(ActionSceneDecorationRender_Build(&frame,e.render_layer,false,true,ActionEffectProjection_ProjectPoint,
      ActionEffectProjection_ClipBounds,&ref,&reference));CHECK(SceneBatchesEqual(&mesh,&reference));
  static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
  for(unsigned voxel=0;voxel<2;++voxel) {
    p.voxel_scenery=voxel;
    ActionEffectSourceBatch source={.context=context,.primitives=primitives,.capacity=kActionSourceMaximumPrimitives};
    CHECK(ActionSceneDecorationRender_Build(&frame,e.render_layer,false,true,ActionEffectSource_ProjectPoint,
        ActionEffectSource_ClipBounds,&source,&scratch));CHECK(source.count>0&&!source.failed);
    bool reaches_below=false;
    for(unsigned i=0;i<source.count;++i) {
      const ActionEffectSourcePrimitive *v=&primitives[i];
      CHECK((((unsigned)v->origin[2]&32u)!=0)==(voxel!=0));
      CHECK(v->meta[1]==kActionEffectProjectionPlane_Bg1);
      CHECK(isfinite(v->clip[3])&&v->clip[3]<=111);
      reaches_below|=v->clip[3]>6;
    }
    if(voxel)CHECK(reaches_below);
  }
  /* Shipped enum: background=0, playfield=1, foreground=2, high BG2=3.
   * Both free background and foreground banks must finish; playfield remains
   * finite. The foreground case catches the original v1 numeric misclassification. */
  for(unsigned placement=0;placement<=2;placement+=2) {
    ActionEffectInstance bank=e;bank.field_style.placement=placement;
    CHECK(ActionEffectProjection_AuthoredCloudExtent(&context,&bank));
    CHECK(ActionEffectProjection_ClipBounds(&context,&bank,&bounds)&&bounds.y1==104);
    frame.authored[0]=bank;
    CHECK(ActionSceneDecorationRender_Build(&frame,bank.render_layer,false,true,ActionEffectProjection_ProjectPoint,
        ActionEffectProjection_ClipBounds,&context,&mesh));
    CHECK(ActionSceneDecorationRender_Build(&frame,bank.render_layer,false,true,ActionEffectProjection_ProjectPoint,
        ActionEffectProjection_ClipBounds,&ref,&reference));CHECK(SceneBatchesEqual(&mesh,&reference));
    ActionEffectSourcePrimitive primitive[1];ActionEffectSourceBatch packet={.context=context,.primitives=primitive,.capacity=1};
    const ArRenderPointF corners[4]={{0,0},{1,0},{1,1},{0,1}};
    const ArRenderColorF colors[4]={{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};
    CHECK(ActionEffectSource_Quad(&packet,&bank,corners,colors)&&packet.count==1);
    CHECK(((unsigned)primitive[0].origin[2]&32u)!=0);
    CHECK(primitive[0].clip[0]==-1e9f&&primitive[0].clip[1]==-1e9f&&primitive[0].clip[2]==1e9f&&primitive[0].clip[3]==104);
    CHECK(!ActionEffectProjection_ProjectPoint(&context,&bank,0,105,&actual));
  }
  frame.authored[0]=e;
  CHECK(!memcmp(&e,&original,sizeof(e))&&!memcmp(&frame.authored[0],&original,sizeof(e)));
}
