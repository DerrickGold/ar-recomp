#include "diorama_capture_blend.h"
/* Native RGB lives in a texture so grading precedes vertex shade in both
 * painter and shared-depth paths. Slots are stable palette identities; only
 * live frame color changes upload, never camera movement. */
static ArRenderTexture s_jungle_terminal_textures[128];
static uint32_t s_jungle_terminal_colors[128];
static void ResetJungleTerminalColors(ArRenderDevice *device) {
  for(unsigned i=0;i<128;++i) {
    ArRenderDevice_DestroyTexture(device,s_jungle_terminal_textures[i]);
    s_jungle_terminal_textures[i]=ArRenderTexture_Invalid();s_jungle_terminal_colors[i]=0;
  }
}
static ArRenderTexture JungleTerminalTexture(ArRenderDevice *device,unsigned index,uint32_t color) {
  if(index>=128||!color)return ArRenderTexture_Invalid();
  ArRenderTexture *t=s_jungle_terminal_textures+index;
  if(!ArRenderTexture_IsValid(*t)) {
    const ArRenderTextureDesc desc={1,1,kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Static,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
    if(!ArRenderDevice_CreateTexture(device,&desc,t))return ArRenderTexture_Invalid();
    s_jungle_terminal_colors[index]=0;
  }
  if(s_jungle_terminal_colors[index]!=color) {
    if(!ArRenderDevice_UpdateTexture(device,*t,NULL,&color,sizeof(color)))return ArRenderTexture_Invalid();
    s_jungle_terminal_colors[index]=color;
  }
  return *t;
}
static bool DrawJungleTerminal(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaScene *scene,const DioramaViewGeometry *g,const DioramaLayerDraw *layer,
    bool native_subscreen) {
  const VoxelJungleTerminal *source=ActionVoxel_JungleTerminal(layer->voxel_geometry);
  const unsigned plane=layer->description->plane;
  const bool high=plane==kDioramaPlane_Bg1Hi;
  const ActionRoomSceneFrameState *frame=capture->voxel_frame;
  if(!source||source->count>kVoxelMaxJungleTerminalRuns||!native_subscreen||!scene->voxel_mode||scene->map_group!=5||scene->map_number!=3||
      !frame||layer->voxel_depth<=0||(!high&&plane!=SR_PPU_OVERLAY_BG1)||
      layer->authored->rake||layer->authored->bow||layer->blend!=kArRenderBlendMode_Alpha||
      scene->render->voxel_inspection!=kDioramaVoxelInspect_All||
      !(DioramaCaptureBlend_IndependentSubscreenSources(true,frame->cgwsel,frame->cgadsub,
        frame->screen_enabled[0],frame->screen_enabled[1])&1)||
      !ActionVoxel_Active(layer->voxel_geometry,5,3))return true;
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  if(!(du>0)||!(dv>0)||g->width<=0||g->height<=0)return true;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float z=layer->authored->z-.5f;
  const float scale=VoxelPlaneScale(g,layer->voxel_geometry,plane,layer->authored->z);
  if(!(scale>0)||!isfinite(scale))return true;
  JungleTerminalRect view={INFINITY,INFINITY,-INFINITY,-INFINITY};
  for(unsigned i=0;i<4;++i) {
    const float sx=(i==1||i==2)?1+2.f/g->width:-2.f/g->width;
    const float sy=i>=2?1+2.f/g->height:-2.f/g->height;
    float x,y;
    if(!DioramaSkyboxWorldPoint(g->matrix,z,g->aspect_x*scale,g->height_scale*scale,sx,sy,&x,&y))return true;
    x=ox+(g->u0+x*du)*SR_PPU_SURFACE_MAX_WIDTH;
    y=oy+(g->v0+y*dv)*SR_PPU_SURFACE_MAX_HEIGHT;
    if(!isfinite(x)||!isfinite(y))return true;
    view.x0=fminf(view.x0,x);view.x1=fmaxf(view.x1,x);
    view.y0=fminf(view.y0,y);view.y1=fmaxf(view.y1,y);
  }
  for(unsigned i=0;i<source->count;++i) {
    const VoxelJungleTerminalRun *run=source->runs+i;
    JungleTerminalRect rect;
    if(run->high_priority!=high||!JungleTerminalResolve(source,i,view,&rect))continue;
    const uint32_t color=ActionVoxel_AuthoredColor(run->color,frame);
    const ArRenderTexture texture=JungleTerminalTexture(device,run->palette_index,color);
    if(!ArRenderTexture_IsValid(texture))return true; /* Optional source fails closed. */
    /* Exposure cuts are in native source UV. Split this finite rectangle
     * there BEFORE projection, so each new vertex gets its own exact W and
     * each hard edge samples its own interior. The generic affine exposure
     * splitter cannot carry an unchanged perspective sidecar. */
    const SceneryShadingField *field=layer->exposure;
    if(field&&!field->sample)field=NULL;
    unsigned row=field?SceneryShading_First(field->y,field->ny,
      (rect.y0-oy)/SR_PPU_SURFACE_MAX_HEIGHT,0,1):0;
    for(float top=rect.y0;top<rect.y1;) {
      const float bottom=field&&row<field->ny?
        fminf(rect.y1,oy+field->y[row++]*SR_PPU_SURFACE_MAX_HEIGHT):rect.y1;
      if(bottom<=top)continue;
      unsigned col=field?SceneryShading_First(field->x,field->nx,
        (rect.x0-ox)/SR_PPU_SURFACE_MAX_WIDTH,0,1):0;
      for(float left=rect.x0;left<rect.x1;) {
        const float right=field&&col<field->nx?
          fminf(rect.x1,ox+field->x[col++]*SR_PPU_SURFACE_MAX_WIDTH):rect.x1;
        if(right<=left)continue;
        const float inside_u=((left+right)*.5f-ox)/SR_PPU_SURFACE_MAX_WIDTH;
        const float inside_v=((top+bottom)*.5f-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
        ArRenderVertex2D vertices[4];float depths[4],clip_w[4];
        for(unsigned j=0;j<4;++j) {
          const float x=(j==1||j==2)?right:left,y=j>=2?bottom:top;
          const float u=(x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
          const float wx=((u-g->u0)/du-.5f)*g->aspect_x*scale;
          const float wy=(.5f-(v-g->v0)/dv)*g->height_scale*scale;
          Scene3DClipPoint p;
          if(!Scene3D_TransformToClip(g->matrix,wx,wy,z,&p)||p.w<=kScene3DMinimumProjectionDepth)return true;
          const float inv=1.f/p.w;depths[j]=p.z*inv*.5f+.5f;clip_w[j]=p.w;
          vertices[j]=(ArRenderVertex2D){.position={(p.x*inv*.5f+.5f)*g->width,
            (1-(p.y*inv*.5f+.5f))*g->height},.color=layer->shade,.tex_coord={u,v}};
          if(field) {
            const float exposure=field->sample(field->userdata,u,v,inside_u,inside_v);
            vertices[j].color.r*=exposure;vertices[j].color.g*=exposure;vertices[j].color.b*=exposure;
          }
        }
        ShadeDioramaVertices(layer,vertices,4);
        static const int32_t indices[]={0,1,2,0,2,3};
        if(!RenderShadedDioramaDepthGeometryWithW(device,texture,vertices,depths,clip_w,4,
            indices,6,layer->blend,NULL,(ArRenderRectF){0,0,1,1},&layer->lighting,
            layer->depth_sink,plane,false))return false;
        left=right;
      }
      top=bottom;
    }
  }
  return true;
}
