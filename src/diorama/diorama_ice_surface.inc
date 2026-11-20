/* Ice is a material finish on the actual cap, never a screen-space particle
 * field. The shared thin-film batch preserves depth, cut pieces and actor order. */
static struct { bool sheen,glints;float clock,fade; } s_ice_surface;
static void IceSurfacePrepare(const DioramaScene *scene,const DioramaViewGeometry *g) {
  memset(&s_ice_surface,0,sizeof(s_ice_surface));
  if(!g->depth_sink || !scene->trunk_effects ||
      scene->render->voxel_inspection==kDioramaVoxelInspect_Backing ||
      !ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number))return;
  s_ice_surface.sheen=scene->trunk_lighting;
  s_ice_surface.glints=scene->trunk_lighting && scene->trunk_particles;
  const ActionSceneEffectFrame *frame=scene->trunk_effects;
  s_ice_surface.clock=frame->game_frame;
  s_ice_surface.fade=1-scene->native_bloom_dimming;
}
/* Clip in native X and complete material depth. Splitting a top at an atlas
 * boundary, slope step or actor band must not restart a reflection stripe. */
static void IceSurfaceQuad(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaGroundCoverCap *cap,float x0,float x1,float z0,float z1,
    const float xf[2],const float zf[2],ArRenderColorF tint) {
  const float near=cap->corners[0].depth_fraction,far=cap->corners[3].depth_fraction;
  const float left=fmaxf(x0,cap->from.x),right=fminf(x1,cap->to.x);
  const float front=fmaxf(z0,near),back=fminf(z1,far),width=cap->to.x-cap->from.x;
  if(right<=left || back<=front || width<=0 || far<=near || x1<=x0 || z1<=z0)return;
  const float opacity[]={xf[0]+(xf[1]-xf[0])*(left-x0)/(x1-x0),
      xf[0]+(xf[1]-xf[0])*(right-x0)/(x1-x0)};
  ArRenderColorF colors[2]={tint,tint};
  colors[0].a*=zf[0]+(zf[1]-zf[0])*(front-z0)/(z1-z0);
  colors[1].a*=zf[0]+(zf[1]-zf[0])*(back-z0)/(z1-z0);
  WetSurfaceQuad(capture,g,cap,(float[2]){(left-cap->from.x)/width,(right-cap->from.x)/width},
      opacity,(float[2]){(front-near)/(far-near),(back-near)/(far-near)},
      (float[2]){.25f,.25f},colors,false);
}
static void IceSurfaceGather(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaGroundCoverCap *cap) {
  if(!s_ice_surface.sheen && !s_ice_surface.glints)return;
  const float fade=s_ice_surface.fade*(capture->voxel_frame?capture->voxel_frame->brightness/15.f:1);
  if(fade<=0)return;
  const float grazing=powf(1-fminf(1,fabsf(sinf(g->camera.tilt_x))),3);
  const float centre=.48f+.14f*sinf(g->camera.tilt_y*2)+.08f*sinf(g->camera.tilt_x);
  const float one[]={1,1},up[]={0,1},down[]={1,0};
  if(s_ice_surface.sheen) {
    IceSurfaceQuad(capture,g,cap,cap->from.x,cap->to.x,0,1,one,(float[2]){.6f,1},
        (ArRenderColorF){.48f,.74f,.88f,fade*(.075f+.035f*grazing)});
    const ArRenderColorF reflection={.76f,.93f,1,fade*(.26f+.10f*grazing)};
    IceSurfaceQuad(capture,g,cap,cap->from.x,cap->to.x,centre-.13f,centre,one,up,reflection);
    IceSurfaceQuad(capture,g,cap,cap->from.x,cap->to.x,centre,centre+.13f,one,down,reflection);
  }
  if(!s_ice_surface.glints)return;
  const int cell_y=(int)floorf((cap->from.y+cap->to.y)/32);
  const int first=(int)floorf((cap->from.x-3)/16),last=(int)floorf((cap->to.x+3)/16);
  for(int cell=first;cell<=last;++cell) {
    const uint32_t seed=DeterministicHash_Mix32((uint32_t)cell*0x9e3779b9u^(uint32_t)cell_y*0x85ebca6bu);
    if(seed&1)continue;
    const float pulse=fmaxf(0,sinf((s_ice_surface.clock+(seed%600))*6.2831853f/(210+(seed%121))));
    const float strength=pulse*pulse*pulse*pulse;
    if(strength<.02f)continue;
    const float x=cell*16+3+(seed%1001)*.01f,z=.18f+((seed>>12)%1001)*.00064f;
    const ArRenderColorF glint={.9f,.98f,1,fade*strength*(.65f+.2f*grazing)};
    IceSurfaceQuad(capture,g,cap,x-3,x,z-.007f,z+.007f,up,one,glint);
    IceSurfaceQuad(capture,g,cap,x,x+3,z-.007f,z+.007f,down,one,glint);
    IceSurfaceQuad(capture,g,cap,x-.35f,x+.35f,z-.065f,z,one,up,glint);
    IceSurfaceQuad(capture,g,cap,x-.35f,x+.35f,z,z+.065f,one,down,glint);
  }
}
