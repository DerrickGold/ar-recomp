/* Exercise the existing per-pixel front partition with the two reviewed
 * snow endpoint shapes. Native source alpha remains the texture's job. */
#include <float.h>
static void VoxelEndpointObserve(void *context,const DioramaVoxelPickFace *face) {
  if(face->plane!=kDioramaPlane_Bg1Far ||
      (face->kind!=kDioramaVoxelFace_Front && face->kind!=kDioramaVoxelFace_Top))return;
  float x=0,y=0;
  for(unsigned j=0;j<4;++j){x+=face->world_xy[j*2]*.25f;y+=face->world_xy[j*2+1]*.25f;}
  if(x<80 || x>136 || y<95.9f || y>104)return;
  const unsigned cell=(unsigned)(x-80)/16;
  if(cell>=4 || x>80+cell*16+8)return;
  NativeAffineProbe *probe=context;assert(probe->count<512);
  probe->faces[probe->count++]=*face;
}
static unsigned VoxelEndpointFixture(unsigned scenario) {
  s_count=0;static NativeAffineProbe probe;memset(&probe,0,sizeof(probe));
  Recorder r={.width=960,.height=600,.bound_effect=-1,.native_affine=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  probe.native_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=6;voxel->active_room=1;voxel->profile=kActionVoxelProfile_NorthwallSnow;
  voxel->settings=ActionVoxel_SettingsForRoom(6,1);voxel->width=voxel->height=256;
  voxel->ready=voxel->mixed_scenery=true;
  voxel->front_recess=calloc(32,32);voxel->pixel_recess_cells=calloc(32,32);
  assert(voxel->front_recess && voxel->pixel_recess_cells);
  voxel->pixel_recess_mask_count=2;voxel->pixel_recess_cell_count=4;
  for(unsigned row=0;row<8;++row) {
    voxel->pixel_recess_rows[0][row]=row?255:252;
    voxel->pixel_recess_rows[1][row]=row?255:248;
  }
  for(unsigned cell=0;cell<4;++cell) {
    const unsigned x=80+cell*16,count=cell<2?2:3;
    const size_t at=12*32+x/8;
    voxel->front_recess[at]=1;voxel->pixel_recess_cells[at]=cell<2?1:2;
    voxel->edges[cell]=(VoxelEdge){.x=x+8-count,.y=96,.length=count,.side=0,
        .far_rank=2,.soft_surface=1,.textured_top=1};
    voxel->pass_edges[0][cell]=cell;
  }
  voxel->edge_count=voxel->pass_counts[0]=4;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.25f,.tilt_y=scenario&1?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=VoxelEndpointObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=scenario>=2?&sink:NULL,
      .map_group=6,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.capture=&capture;r.projection=&projection;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  unsigned front_pixels=0,rear_pixels=0,joins=0;
  for(unsigned f=0;f<probe.count;++f) {
    const DioramaVoxelPickFace *face=&probe.faces[f];assert(probe.submitted[f]);
    if(face->kind!=kDioramaVoxelFace_Front)continue;
    float cx=0,cy=0,minx=FLT_MAX,maxx=-FLT_MAX,miny=FLT_MAX,maxy=-FLT_MAX;
    for(unsigned j=0;j<4;++j) {
      const float x=face->world_xy[j*2],y=face->world_xy[j*2+1];
      cx+=x*.25f;cy+=y*.25f;minx=fminf(minx,x);maxx=fmaxf(maxx,x);
      miny=fminf(miny,y);maxy=fmaxf(maxy,y);
    }
    const unsigned cell=(unsigned)(cx-80)/16,count=cell<2?2:3;
    const bool endpoint=cy<97 && cx>80+cell*16+8-count;
    const unsigned area=(unsigned)lroundf((maxx-minx)*(maxy-miny));
    if(endpoint)front_pixels+=area;else rear_pixels+=area;
    for(unsigned j=0;j<4;++j) {
      const float depth=NativeAffineDepth(&projection,&capture,face->world_xy[j*2],
          face->world_xy[j*2+1],face->clip_w[j]);
      assert(fabsf(depth-(endpoint?0:.15f))<.00001f);
      if(!endpoint || fabsf(face->world_xy[j*2+1]-96)>.001f)continue;
      bool joined=false;
      for(unsigned c=0;c<probe.count;++c)if(probe.faces[c].kind==kDioramaVoxelFace_Top)
        for(unsigned k=0;k<4;++k)
          joined|=CapPointEqual(probe.vertices[f][j].position,probe.vertices[c][k].position);
      assert(joined);++joins;
    }
  }
  assert(front_pixels==10 && rear_pixels==246 && joins==8);
  printf("Voxel snow endpoint case %u: 10 front pixels, 246 recessed pixels, 8 exact cap/front joins\n",scenario);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
