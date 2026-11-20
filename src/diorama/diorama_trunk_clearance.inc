/* Carve the foreground under native standing contacts. The same source mesh
 * generator serves primary and guard captures; captured RGBA is never edited. */
typedef struct HollowFrontSource {
  ArRenderDevice *device;
  const DioramaCapture *capture;
  const DioramaViewGeometry *geometry;
  const DioramaLayerDraw *layer;
  DioramaFaceSource *primary;
  const DioramaTrunkGuardStrip *strip;
  ArRenderTexture texture;
  bool filtered;
  PresentationOutcome outcome;
} HollowFrontSource;
static bool FlushHollowFront(HollowFrontSource *source,DioramaLayerMesh *mesh) {
  if(!mesh->vertex_count)return true;
  const DioramaCapture *capture=source->capture;const DioramaLayerDraw *layer=source->layer;
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  if(!source->strip) {
    DioramaAttachedMesh attached={0};
    source->outcome=PresentationOutcome_Combine(source->outcome,
        DrawDioramaLayerFace(source->device,capture,source->geometry,layer,mesh,&attached,source->primary));
  } else {
    const DioramaTrunkGuardStrip *strip=source->strip;
    const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
    const float oy=capture->camera_y-capture->authentic_y0+1;
    const float width=source->filtered?strip->width:kDioramaTrunkGuardWidth;
    const float height=source->filtered?kDioramaTrunkGuardRows:kDioramaTrunkGuardHeight;
    const float atlas_y=source->filtered?0:strip->atlas_y;
    for(int i=0;i<mesh->vertex_count;++i) {
      ArRenderPointF *uv=&mesh->vertices[i].tex_coord;
      const float x=ox+uv->x*SR_PPU_SURFACE_MAX_WIDTH,y=oy+uv->y*SR_PPU_SURFACE_MAX_HEIGHT;
      *uv=(ArRenderPointF){(x-strip->world_x)/width,(y-strip->world_y+atlas_y)/height};
    }
    const ArRenderRectF uv={(strip->world_x-ox)/SR_PPU_SURFACE_MAX_WIDTH,
        (strip->world_y-oy-atlas_y)/SR_PPU_SURFACE_MAX_HEIGHT,
        width/SR_PPU_SURFACE_MAX_WIDTH,height/SR_PPU_SURFACE_MAX_HEIGHT};
    DioramaSceneryLightEffectParams light=layer->lighting;light.mode=0;light.blur_radius=0;light.attributes_uv=uv;
    light.texel_width=1.f/(width*(source->filtered?kDioramaSupersample:1));
    light.texel_height=1.f/(height*(source->filtered?kDioramaSupersample:1));
    if(!RenderShadedDioramaDepthGeometryWithW(source->device,source->texture,mesh->vertices,mesh->depths,
        mesh->has_depth?mesh->clip_w:NULL,mesh->vertex_count,mesh->indices,mesh->index_count,
        layer->blend,layer->exposure,uv,&light,layer->depth_sink,layer->description->plane,false))
      source->outcome=kPresentationOutcome_CoreFailure;
  }
  mesh->vertex_count=mesh->index_count=0;
  return PresentationOutcome_IsUsable(source->outcome);
}
/* -1 needs pixel contours; 0 is native unknown/high art; 1 is recessed body.
 * The contact map is immutable and only a small native neighborhood is read. */
static int HollowFrontCell(int x,int y) {
  const int pitch=kActionTrunkSideWidth/8,byte=x/8,cy=y/8;
  bool body=false;
  for(int yy=cy*8;yy<cy*8+8&&(unsigned)yy<s_trunk_assets->height;++yy) {
    const uint8_t *solid=ActionTrunk_SideMaskRow(s_trunk_assets,yy,kActionTrunkSideSolid);
    const uint8_t *high=ActionTrunk_SideMaskRow(s_trunk_assets,yy,kActionTrunkSidePriority);
    body|=!!(solid[byte]&~high[byte]);
  }
  if(!body)return 0;
  const uint8_t *grid=s_trunk_assets->side_clearance_grid+cy*(pitch+1)+byte;
  if(grid[0]!=grid[1]||grid[0]!=grid[pitch+1]||grid[0]!=grid[pitch+2])return -1;
  if(!grid[0])return 0;
  if(s_trunk_assets->side_detail_cells[cy*pitch+byte])return -1;
  for(int yy=cy*8-7;yy<cy*8+8;++yy)
    if(yy>=0&&(unsigned)yy<s_trunk_assets->height&&
        s_trunk_assets->side_contacts[yy*kActionTrunkSideMaskPitch+byte])return -1;
  return 1;
}
static bool AppendHollowFrontQuad(HollowFrontSource *source,DioramaLayerMesh *mesh,
    float x0,float y0,float x1,float y1,int kind,int owner_x,int owner_y) {
  if(mesh->vertex_count+4>DIORAMA_VERTS_PER_LAYER||mesh->index_count+6>DIORAMA_INDICES_PER_LAYER)
    if(!FlushHollowFront(source,mesh))return false;
  const DioramaCapture *capture=source->capture;const DioramaViewGeometry *g=source->geometry;
  const DioramaLayerDraw *layer=source->layer;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const unsigned base=(unsigned)mesh->vertex_count;
  for(unsigned j=0;j<4;++j) {
    const float x=j==1||j==2?x1:x0,y=j>=2?y1:y0;
    const float fraction=kind<0?ActionTrunk_SideFrontFraction(s_trunk_assets,owner_x,owner_y,x,y):kind*.75f;
    const float z=-layer->hollow_recess-DioramaTrunk_SideDepth(layer->hollow_recess,x,true)*fraction;
    const float u=(x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
    const unsigned at=(unsigned)mesh->vertex_count++;ArRenderVertex2D *vertex=&mesh->vertices[at];
    if(!ProjectHollowPoint(g,mesh,at,((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x,
        (.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale,z))return false;
    vertex->tex_coord=(ArRenderPointF){u,v};vertex->color=layer->shade;
  }
  const int indices[]={base,base+1,base+2,base,base+2,base+3};
  memcpy(mesh->indices+mesh->index_count,indices,sizeof(indices));mesh->index_count+=6;
  return true;
}
static bool DrawHollowCarvedFront(HollowFrontSource *source,float left,float right,float top,float bottom) {
  if(right<=left||bottom<=top)return true;
  DioramaLayerMesh mesh={.constrained=true,.has_depth=source->layer->depth_sink!=NULL};
  for(float y=top;y<bottom;) {
    const float y1=fminf(bottom,(floorf(y/8)+1)*8);
    for(float x=left;x<right;) {
      float x1=fminf(right,(floorf(x/8)+1)*8);const int kind=HollowFrontCell((int)floorf(x),(int)floorf(y));
      if(kind>=0) {
        while(x1<right&&x1!=288&&x1!=736&&HollowFrontCell((int)x1,(int)floorf(y))==kind)
          x1=fminf(right,x1+8);
        if(!AppendHollowFrontQuad(source,&mesh,x,y,x1,y1,kind,0,0))return false;
      } else {
        for(float py=y;py<y1;) {
          const float py1=fminf(y1,floorf(py)+1);
          for(float px=x;px<x1;) {
            const float px1=fminf(x1,floorf(px)+1);
            if(!AppendHollowFrontQuad(source,&mesh,px,py,px1,py1,-1,(int)floorf(px),(int)floorf(py)))return false;
            px=px1;
          }
          py=py1;
        }
      }
      x=x1;
    }
    y=y1;
  }
  return FlushHollowFront(source,&mesh);
}
