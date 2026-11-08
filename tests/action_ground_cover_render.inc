/* Compare the new native-coordinate light adapter against existing rendered
 * snow vertices, then exercise exclusive ownership of the old broad top. */
static void TestVoxelGroundCoverLightingAndClaims(void) {
  static ActionSceneEffectFrame frame;
  static ActionSceneEffectRenderBatch batch;
  frame=(ActionSceneEffectFrame){.authored_count=2};
  ActionEffectInstance *bank=&frame.authored[0];
  *bank=(ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,
      .flags=kActionEffectFlag_Visible,.world_x=100,.world_y=200,
      .render_layer=kActionEffectRenderLayer_WorldDust,
      .projection_plane=kActionEffectProjectionPlane_Bg1,
      .particle_lifetime=240,.pulse_ticks=180,
      .tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},
      .particle_style={.size_min=.5f,.size_max=1,.color_end=0xfff4df,.seed=621,.spread=1},
      .field_style={.strands=4,.placement=2,.ground_depth=4,.ground_width=8,
          .canopy_response=.7f,.point_count=2,.points={{0,0},{160,0}}},
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={0,-8,160,4}}};
  frame.authored[1]=(ActionEffectInstance){.kind=kActionEffect_AuthoredGradient,
      .flags=kActionEffectFlag_Visible,.world_x=100,.world_y=180,
      .render_layer=kActionEffectRenderLayer_WorldOverlay,
      .projection_plane=kActionEffectProjectionPlane_Bg1,
      .tuning={.active=1,.intensity=2,.color=0x4c8aff},
      .particle_style={.color_end=0xff8855},
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-100,-100,300,100}}};
  unsigned comparisons=0;
  for(unsigned lighting=0;lighting<2;++lighting) {
    CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,
        lighting,true,IdentityProjection,NULL,NULL,&batch));
    CHECK(batch.vertex_count>0);
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderVertex2D *v=&batch.vertices[i];
      if(v->position.y>200||v->position.x<=100||v->position.x>=260)continue;
      ActionGroundCoverLightSample sample={.native_x=v->position.x,
          .native_floor_y=200,.height=200-v->position.y,
          .body={216/255.f,230/255.f,246/255.f,1},
          .crest={1,244/255.f,223/255.f,1}};
      CHECK(ActionGroundCover_LightSamples(&frame,bank,lighting,false,&sample,1));
      const ArRenderColorF tint=sample.height>0?sample.crest:sample.body;
      CHECK(fabsf(tint.r-v->color.r)<.00001f && fabsf(tint.g-v->color.g)<.00001f &&
          fabsf(tint.b-v->color.b)<.00001f);
      ++comparisons;
    }
  }
  CHECK(comparisons>10);
  ActionGroundCoverLightSample samples[2]={{.native_x=180,.native_floor_y=200,.height=3,
      .body={.2f,.4f,.6f,1},.crest={.8f,.7f,.6f,1}}};
  samples[1]=samples[0];
  const ActionGroundCoverLightSample before=samples[0];
  CHECK(ActionGroundCover_LightSamples(&frame,bank,true,true,samples,1));
  CHECK(!memcmp(&before,samples,sizeof(before))); /* Lighting disabled in atmosphere. */
  samples[1].height=NAN;
  CHECK(!ActionGroundCover_LightSamples(&frame,bank,true,false,samples,2));
  CHECK(!memcmp(&before,samples,sizeof(before))); /* Validate before partial tint. */

  /* Native bank indexes remain stable past the first64 authored instances. */
  frame.authored_count=66;frame.authored[1]=(ActionEffectInstance){0};
  bank->particle_count=24;
  frame.authored[65]=*bank;frame.authored[65].world_x+=240;
  frame.authored[65].particle_style.seed+=317;
  static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
  static ActionEffectSourceLightJob lights[kActionSourceMaxLightJobs];
  DioramaProjection projection={.valid=true};
  ActionEffectSourceBatch source={.primitives=primitives,.capacity=kActionSourceMaximumPrimitives,
      .lights=lights,.light_capacity=kActionSourceMaxLightJobs,
      .context={.diorama_projection=&projection}};
#define CLAIM_BUILD(layer) do { source.count=source.light_count=0;source.failed=false; \
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,true, \
      ActionEffectSource_ProjectPoint,NULL,&source,&batch)); } while(0)
  CLAIM_BUILD(kActionEffectRenderLayer_Bg1Mist);const unsigned both=source.count;
  CHECK(both>0);
  projection.voxel_ground_cover_claims[1]=UINT64_C(1)<<1;
  CLAIM_BUILD(kActionEffectRenderLayer_Bg1Mist);CHECK(source.count*2==both);
  projection.voxel_ground_cover_claims[0]=1;
  CLAIM_BUILD(kActionEffectRenderLayer_Bg1Mist);CHECK(!source.count);
  memset(projection.voxel_ground_cover_claims,0,sizeof(projection.voxel_ground_cover_claims));
  /* Keep independent unclaimed packets for both original bank indexes.
   * Ambient grains use particle primitives; the dense front lip is triangles.
   * Claimed output must be precisely the former packet minus those triangles,
   * including unchanged grain positions, clocks, colors and native priority. */
  static ActionEffectSourcePrimitive reference[2][512];
  unsigned counts[2]={0},grains[2]={0},dense[2]={0};
  for(unsigned b=0;b<2;++b) {
    frame.authored[b?0:65].flags=0;
    CLAIM_BUILD(kActionEffectRenderLayer_WorldDust);
    CHECK(source.count>0&&source.count<=512);counts[b]=source.count;
    memcpy(reference[b],source.primitives,source.count*sizeof(*source.primitives));
    for(unsigned i=0;i<source.count;++i) {
      CHECK(primitives[i].meta[0]==kActionSourceParticle||primitives[i].meta[0]==kActionSourceTriangle);
      if(primitives[i].meta[0]==kActionSourceParticle)++grains[b];else ++dense[b];
    }
    CHECK(grains[b]>0&&dense[b]>0);
    frame.authored[b?0:65].flags=kActionEffectFlag_Visible;
  }
  for(unsigned staged=0;staged<2;++staged)for(unsigned mask=0;mask<4;++mask) {
    memset(projection.voxel_ground_cover_claims,0,sizeof(projection.voxel_ground_cover_claims));
    memset(source.context.staged_ground_cover_claims,0,sizeof(source.context.staged_ground_cover_claims));
    source.context.diorama_projection=staged?NULL:&projection;
    uint64_t *claims=staged?source.context.staged_ground_cover_claims:projection.voxel_ground_cover_claims;
    claims[0]=(mask&1)?UINT64_C(1):0;claims[1]=(mask&2)?UINT64_C(2):0;
    CLAIM_BUILD(kActionEffectRenderLayer_WorldDust);
    unsigned expected=0;
    for(unsigned b=0;b<2;++b)for(unsigned i=0;i<counts[b];++i) {
      const ActionEffectSourcePrimitive *p=&reference[b][i];
      if((mask&(1u<<b))&&p->meta[0]==kActionSourceTriangle)continue;
      CHECK(expected<source.count&&!memcmp(p,&source.primitives[expected],sizeof(*p)));++expected;
    }
    CHECK(source.count==expected);
    if(mask==3)CHECK(source.count==grains[0]+grains[1]);
    CLAIM_BUILD(kActionEffectRenderLayer_Bg1Mist);
    CHECK(source.count==both*((mask&1?0:1)+(mask&2?0:1))/2);
  }
  /* Ordinary/off consumers have no claims and retain the complete legacy
   * packet; a preceding claimed frame must not leak ownership into them. */
  source.context=(ActionEffectProjectionContext){0};
  CLAIM_BUILD(kActionEffectRenderLayer_WorldDust);
  CHECK(source.count==counts[0]+counts[1]);
  CHECK(!memcmp(source.primitives,reference[0],counts[0]*sizeof(*source.primitives)));
  CHECK(!memcmp(source.primitives+counts[0],reference[1],counts[1]*sizeof(*source.primitives)));
  CLAIM_BUILD(kActionEffectRenderLayer_Bg1Mist);CHECK(source.count==both);
#undef CLAIM_BUILD
}
