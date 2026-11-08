/* A finite native opaque-black void may end inside an oblique viewport.
 * Continue only its compiled terminal runs, on the exact existing BG1 rear
 * plane. No native texture, mesh, collision or interior opening is changed. */
static bool DrawCastleTerminalMatte(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaScene *scene,const DioramaViewGeometry *g,const DioramaLayerDraw *layer) {
  const VoxelTerminalMatte *source=ActionVoxel_TerminalMatte(layer->voxel_geometry);
  if(!source||layer->voxel_depth<=0||layer->description->plane!=SR_PPU_OVERLAY_BG1||
      layer->authored->rake||layer->authored->bow||
      !ActionVoxel_Active(layer->voxel_geometry,scene->map_group,scene->map_number))return true;
  const float du=g->u1-g->u0,dv=g->v1-g->v0;
  if(!(du>0)||!(dv>0)||g->width<=0||g->height<=0)return true;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float z=layer->authored->z-.5f;
  const float scale=VoxelPlaneScale(g,layer->voxel_geometry,SR_PPU_OVERLAY_BG1,layer->authored->z);
  if(!(scale>0)||!isfinite(scale))return true;
  CastleTerminalMatteRect view={INFINITY,INFINITY,-INFINITY,-INFINITY},rects[2];
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
  const CastleTerminalMatteRuns run={source->world_right,source->world_bottom,
    source->bottom_start,source->right_start,source->bottom_opaque_black,source->right_opaque_black};
  const unsigned count=CastleTerminalMatte_Resolve(scene->map_group,scene->map_number,true,run,view,rects);
  ArRenderVertex2D vertices[8];float depths[8],clip_w[8];int32_t indices[12];
  for(unsigned i=0;i<count;++i) {
    const CastleTerminalMatteRect r=rects[i];
    const float x[4]={r.x0,r.x1,r.x1,r.x0},y[4]={r.y0,r.y0,r.y1,r.y1};
    for(unsigned j=0;j<4;++j) {
      const unsigned at=i*4+j;
      const float u=(x[j]-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(y[j]-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
      const float wx=((u-g->u0)/du-.5f)*g->aspect_x*scale;
      const float wy=(.5f-(v-g->v0)/dv)*g->height_scale*scale;
      Scene3DClipPoint p;
      if(!Scene3D_TransformToClip(g->matrix,wx,wy,z,&p)||p.w<=kScene3DMinimumProjectionDepth)return true;
      const float inv=1.f/p.w;clip_w[at]=p.w;depths[at]=p.z*inv*.5f+.5f;
      vertices[at]=(ArRenderVertex2D){.position={(p.x*inv*.5f+.5f)*g->width,
        (1-(p.y*inv*.5f+.5f))*g->height},.color={0,0,0,1}};
      if(!isfinite(depths[at])||!isfinite(vertices[at].position.x)||!isfinite(vertices[at].position.y))return true;
    }
    const int tri[]={0,1,2,0,2,3};
    for(unsigned j=0;j<6;++j)indices[i*6+j]=(int)i*4+tri[j];
  }
  /* An untextured native-black matte must not inherit cloud light, heat,
   * source UV sampling or additive blending from an unrelated layer. */
  return !count||RenderShadedDioramaDepthGeometryWithW(device,ArRenderTexture_Invalid(),
    vertices,depths,clip_w,(int)count*4,indices,(int)count*6,kArRenderBlendMode_Alpha,
    NULL,(ArRenderRectF){0},NULL,layer->depth_sink,SR_PPU_OVERLAY_BG1,false);
}
