#include "diorama/diorama_sand_surface.h"
#include "diorama/diorama_voxel_source_clip.h"
static void SandSourceBoundsFixture(void) {
  const float guard[]={0,224,256,64},gap[]={0,225,256,63},narrow[]={90,224,32,64};
  assert(DioramaSandSurface_Covered(80,208,8,256,224,NULL,16));
  assert(!DioramaSandSurface_Covered(80,209,8,256,224,NULL,16));
  assert(DioramaSandSurface_Covered(80,216,8,256,224,guard,16));
  assert(!DioramaSandSurface_Covered(80,216,8,256,224,gap,16));
  assert(!DioramaSandSurface_Covered(80,216,8,256,224,narrow,16));
  assert(!DioramaSandSurface_Covered(80,273,8,256,224,guard,16));
  assert(!DioramaSandSurface_Covered(-1,96,8,256,224,guard,16));
  /* Same physical horizontal cap crosses the source seam at depth, not at
   * geometry Y. Both clipped source pieces must retain its full triangle. */
  DioramaVoxelSourceVertex triangle[3]={0},a[4],b[4];
  const float x[]={0,8,8},y[]={0,0,12},v[]={212.5f,212.5f,227.5f};
  for(unsigned i=0;i<3;++i) {triangle[i].vertex.position=(ArRenderPointF){x[i],y[i]};
    triangle[i].vertex.tex_coord.y=v[i]/352;triangle[i].clip_w=1;}
  unsigned na=DioramaVoxelSource_ClipTriangle(triangle,216.f/352,false,true,a);
  unsigned nb=DioramaVoxelSource_ClipTriangle(triangle,216.f/352,true,true,b);assert(na>=3&&nb>=3);
  double area=0;for(unsigned half=0;half<2;++half) {DioramaVoxelSourceVertex *p=half?b:a;unsigned n=half?nb:na;
    for(unsigned i=1;i+1<n;++i)area+=fabs((p[i].vertex.position.x-p[0].vertex.position.x)*(p[i+1].vertex.position.y-p[0].vertex.position.y)-(p[i].vertex.position.y-p[0].vertex.position.y)*(p[i+1].vertex.position.x-p[0].vertex.position.x))*.5;
  }assert(fabs(area-48)<.0001);
}
static void SandLiveRenderFixture(unsigned scenario) {
  const bool left=scenario&1;s_depth_fixture=!!(scenario&2);s_count=0;
  static NativeAffineProbe probe,plain;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;}
  probe.native_texture=textures[kDioramaPlane_Bg1Far];probe.rear_texture=textures[kDioramaPlane_Bg1Hi];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);g->active_group=3;g->active_room=2;
  g->profile=kActionVoxelProfile_KassandoraDesert;g->settings=ActionVoxel_SettingsForRoom(3,2);
  g->width=1024;g->height=768;g->ready=true;
  DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.2f,.tilt_y=left?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.voxel_observe_face=TombContourObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,.map_group=3,.map_number=2,.voxel_geometry=g};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  unsigned joints=0,endpoints=0;
  for(unsigned frame=0;frame<4;++frame) {
    const bool mode=frame&1,unavailable=frame>=2;const int y=unavailable?213:96;
    ++g->revision;g->edge_count=g->pass_counts[1]=3;g->pass_counts[0]=0;
    g->high_deck_count=g->pass_counts[3]=2;g->pass_edges[3][0]=0;g->pass_edges[3][1]=1;
    for(unsigned i=0;i<3;++i) {
      g->pass_edges[1][i]=i;g->edges[i]=(VoxelEdge){.x=i==2?(left?80:95):80+8*i,.y=y,.length=i==2?1:8,
        .side=i==2?(left?3:1):0,.far_rank=2,.high_priority=1,.colliding=1,.soft_surface=1,.textured_top=1};
      if(mode) {VoxelEdge *e=g->edges+i;e->surface_texture=kVoxelSurfaceTextureLiveInward;
        e->top_sample_size=16;e->top_sample_x=e->x;e->top_sample_y=e->y;e->surface_derived=1;e->surface_top_material=i==2;}
    }
    probe.count=0;memset(probe.submitted,0,sizeof(probe.submitted));r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid&&probe.count);for(unsigned f=0;f<probe.count;++f)assert(probe.submitted[f]);
    if(!mode){plain=probe;continue;}assert(probe.count==plain.count);
    for(unsigned f=0;f<probe.count;++f)for(unsigned j=0;j<4;++j) {
      const DioramaVoxelPickFace *face=&probe.faces[f];const ArRenderVertex2D *v=&probe.vertices[f][j];
      assert(CapPointEqual(v->position,plain.vertices[f][j].position));
      assert(!memcmp(&v->color,&plain.vertices[f][j].color,sizeof(v->color)));
      assert(probe.after_actor[f]==plain.after_actor[f]);
      if(unavailable){assert(CapUvEqual(v->tex_coord,plain.vertices[f][j].tex_coord));continue;}
      const float x=face->world_xy[j*2],yy=face->world_xy[j*2+1];
      const float z=NativeAffineDepth(&projection,&capture,x,yy,face->clip_w[j]);
      const float expected=95.5f+15*z/ActionVoxel_Depth(g); /* native worldY96 -> captureY95 */
      if(fabsf(v->tex_coord.y*352-expected)>.002f)fprintf(stderr,"SAND_UV case%u face%u vertex%u z%f actual%f expected%f\\n",scenario,f,j,z,v->tex_coord.y*352,expected);
      assert(fabsf(v->tex_coord.y*352-expected)<.002f);
      if(z<.0001f||fabsf(z-ActionVoxel_Depth(g))<.0001f)++endpoints;
      for(unsigned o=0;o<f;++o)for(unsigned k=0;k<4;++k)if(CapPointEqual(v->position,probe.vertices[o][k].position)) {
        if(face->kind!=probe.faces[o].kind)assert(CapUvEqual(v->tex_coord,probe.vertices[o][k].tex_coord));
        if(probe.after_actor[f]!=probe.after_actor[o] &&
            face->world_xy[0]==probe.faces[o].world_xy[0] &&
            face->world_xy[2]==probe.faces[o].world_xy[2])assert(CapUvEqual(v->tex_coord,probe.vertices[o][k].tex_coord));++joints;
      }
    }
  }
  assert(endpoints&&joints);ActionVoxel_Release(g);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);assert(!r.live);
}
