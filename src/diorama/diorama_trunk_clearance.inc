/* A strip's texture and lighting share one source domain across all meshes.
 * Shade in primary UV before remapping into the filtered strip or atlas. */
typedef struct HollowGuardSource {
  float origin_x, origin_y, width, height, atlas_y;
  const DioramaTrunkGuardStrip *strip;
  DioramaSceneryLightEffectParams light;
} HollowGuardSource;

static HollowGuardSource PrepareHollowGuardSource(const DioramaCapture *capture,
    const DioramaLayerDraw *layer,const DioramaTrunkGuardStrip *strip,bool filtered) {
  HollowGuardSource source={
    .origin_x=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron,
    .origin_y=capture->camera_y-capture->authentic_y0+1,
    .width=filtered?strip->width:kDioramaTrunkGuardWidth,
    .height=filtered?kDioramaTrunkGuardRows:kDioramaTrunkGuardHeight,
    .atlas_y=filtered?0:strip->atlas_y,.strip=strip,.light=layer->lighting,
  };
  source.light.mode=0;
  source.light.blur_radius=0;
  source.light.attributes_uv=(ArRenderRectF){
    (strip->world_x-source.origin_x)/SR_PPU_SURFACE_MAX_WIDTH,
    (strip->world_y-source.origin_y-source.atlas_y)/SR_PPU_SURFACE_MAX_HEIGHT,
    source.width/SR_PPU_SURFACE_MAX_WIDTH,source.height/SR_PPU_SURFACE_MAX_HEIGHT,
  };
  const float scale=filtered?kDioramaSupersample:1;
  source.light.texel_width=1.f/(source.width*scale);
  source.light.texel_height=1.f/(source.height*scale);
  return source;
}

static bool SubmitHollowGuardMesh(ArRenderDevice *device,ArRenderTexture texture,
    const DioramaLayerDraw *layer,const HollowGuardSource *source,DioramaLayerMesh *mesh) {
  if(!mesh->vertex_count)return true;
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  for(int i=0;i<mesh->vertex_count;++i) {
    ArRenderPointF *uv=&mesh->vertices[i].tex_coord;
    const float world_x=source->origin_x+uv->x*SR_PPU_SURFACE_MAX_WIDTH;
    const float world_y=source->origin_y+uv->y*SR_PPU_SURFACE_MAX_HEIGHT;
    *uv=(ArRenderPointF){(world_x-source->strip->world_x)/source->width,
        (world_y-source->strip->world_y+source->atlas_y)/source->height};
  }
  const bool perspective=layer->hollow_voxel&&mesh->has_depth;
  return RenderShadedDioramaDepthGeometryWithW(device,texture,mesh->vertices,mesh->depths,
      perspective?mesh->clip_w:NULL,mesh->vertex_count,mesh->indices,mesh->index_count,
      layer->blend,layer->exposure,source->light.attributes_uv,&source->light,
      layer->depth_sink,layer->description->plane,false);
}

/* Carve the foreground under native standing contacts. The same source mesh
 * generator serves primary and guard captures; captured RGBA is never edited. */
typedef struct HollowFrontSource {
  ArRenderDevice *device;
  const DioramaCapture *capture;
  const DioramaViewGeometry *geometry;
  const DioramaLayerDraw *layer;
  DioramaFaceSource *primary;
  const HollowGuardSource *guard;
  ArRenderTexture texture;
  PresentationOutcome outcome;
} HollowFrontSource;
static bool FlushHollowFront(HollowFrontSource *source,DioramaLayerMesh *mesh) {
  if(!mesh->vertex_count)return true;
  const DioramaCapture *capture=source->capture;const DioramaLayerDraw *layer=source->layer;
  if(source->guard) {
    if(!SubmitHollowGuardMesh(source->device,source->texture,layer,source->guard,mesh))
      source->outcome=kPresentationOutcome_CoreFailure;
  } else {
    ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
    DioramaAttachedMesh attached={0};
    source->outcome=PresentationOutcome_Combine(source->outcome,
        DrawDioramaLayerFace(source->device,capture,source->geometry,layer,mesh,&attached,source->primary));
  }
  mesh->vertex_count=mesh->index_count=0;
  return PresentationOutcome_IsUsable(source->outcome);
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
      float x1=fminf(right,(floorf(x/8)+1)*8);const int kind=ActionTrunk_SideFrontCell(s_trunk_assets,(int)floorf(x),(int)floorf(y));
      if(kind>=0) {
        while(x1<right&&x1!=288&&x1!=736&&ActionTrunk_SideFrontCell(s_trunk_assets,(int)x1,(int)floorf(y))==kind)
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
