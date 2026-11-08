/* Fixed finite source guards, using the primary capture's native
 * world mapping and selected-generation matrix. No repeated guard artwork. */
static bool HollowGuardMatches(const DioramaCapture *capture,const DioramaTrunkGuardView *guard) {
  if(!guard||!guard->capture_generation||guard->source_revision!=s_trunk_assets->revision)return false;
  for(unsigned band=0;band<3;++band)if(!ArRenderTexture_IsValid(guard->textures[band]))return false;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  DioramaTrunkGuardLayout expected;
  return DioramaTrunkGuard_Resolve(ox,oy,capture->width,capture->height,guard->layout.source,&expected)&&
      !memcmp(&expected,&guard->layout,sizeof(expected));
}
static bool PrepareHollowGuardMesh(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,const DioramaTrunkGuardStrip *strip,bool rear,bool filtered,DioramaLayerMesh *mesh) {
  memset(mesh,0,sizeof(*mesh));mesh->constrained=true;mesh->has_depth=layer->depth_sink!=NULL;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float regions[3][2]={{0,320},{704,(float)s_trunk_assets->side_width},{320,704}};
  const float first_x=strip->world_x+strip->valid.x,last_x=first_x+strip->valid.width;
  const float first_y=strip->world_y+strip->valid.y,last_y=first_y+strip->valid.height;
  if(last_x<=first_x||last_y<=first_y)return true;
  for(unsigned region=0;region<(rear?2u:3u);++region) {
    const float left=fmaxf(regions[region][0],first_x),right=fminf(regions[region][1],last_x);
    if(right<=left)continue;
    float columns[4];unsigned count=0;columns[count++]=left;
    const float joins[]={288,736};
    for(unsigned i=0;i<2;++i)if(joins[i]>left&&joins[i]<right)columns[count++]=joins[i];
    columns[count++]=right;const int base=mesh->vertex_count;
    for(unsigned row=0;row<2;++row)for(unsigned col=0;col<count;++col) {
      const float world_x=columns[col],world_y=row?last_y:first_y;
      const float u=(world_x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(world_y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
      const float x=((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x;
      const float y=(.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale;
      const float front=region==2?layer->authored->z-.5f:-layer->hollow_recess;
      const float z=front-(rear?DioramaTrunk_SideDepth(layer->hollow_recess,world_x):0);
      const unsigned index=(unsigned)mesh->vertex_count++;ArRenderVertex2D *vertex=&mesh->vertices[index];
      if(!ProjectWorldPointDepth(g->matrix,x,y,z,g->width,g->height,&vertex->position,
          mesh->has_depth?mesh->depths+index:NULL))return false;
      vertex->tex_coord=(ArRenderPointF){u,v};vertex->color=layer->shade;
    }
    for(unsigned col=0;col+1<count;++col) {
      const int a=base+(int)col,b=a+1,c=b+(int)count,d=a+(int)count;
      const int indices[]={a,b,c,a,c,d};
      memcpy(mesh->indices+mesh->index_count,indices,sizeof(indices));mesh->index_count+=6;
    }
  }
  /* Existing dimming/light sampling remains in PRIMARY native-source UV. */
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  for(int i=0;i<mesh->vertex_count;++i) {
    ArRenderVertex2D *v=&mesh->vertices[i];
    const float world_x=ox+v->tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
    const float world_y=oy+v->tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT;
    v->tex_coord=(ArRenderPointF){(world_x-strip->world_x)/(filtered?strip->width:kDioramaTrunkGuardWidth),
        (world_y-strip->world_y+(filtered?0:strip->atlas_y))/(filtered?kDioramaTrunkGuardRows:kDioramaTrunkGuardHeight)};
  }
  return true;
}
static PresentationOutcome DrawHollowGuard(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,const DioramaTrunkGuardView *guard,int room) {
  if(!HollowGuardMatches(capture,guard))return kPresentationOutcome_Complete;
  const unsigned band=layer->description->plane==SR_PPU_OVERLAY_BG1?0:
      layer->description->plane==kDioramaPlane_Bg1Hi?1:2;
  const bool rear=band==2;
  PresentationOutcome outcome=kPresentationOutcome_Complete;
  for(unsigned i=0;i<kDioramaTrunkGuardStrips;++i) {
    const DioramaTrunkGuardStrip *strip=&guard->layout.strips[i];
    if(!strip->valid.width||!strip->valid.height)continue;
    DioramaLayerDraw source=*layer;source.texture=guard->textures[band];
    if(!rear&&(!DrawHollowSolidEdges(device,capture,g,&source,room,band==0?1:0,strip,i+1)||
        !DrawHollowPassageEdges(device,capture,g,&source,room,band==1,strip,i+1)))
      return kPresentationOutcome_CoreFailure;
    /* Each unrelated strip gets an exact compact source, so LINEAR cannot
     * mix across atlas row64 or the unused right tail. BeginTarget drains
     * retained shared-depth draws before rewriting this reusable target. */
    PresentationOutcome filtered_outcome=kPresentationOutcome_Complete;
    ArRenderTexture texture=BuildDioramaSupersampleFromPool(device,guard->textures[band],0,strip->atlas_y,
        strip->width,kDioramaTrunkGuardRows,&s_diorama_trunk_guard_ss,&filtered_outcome);
    outcome=PresentationOutcome_Combine(outcome,filtered_outcome);
    if(!PresentationOutcome_IsUsable(outcome))return outcome;
    const bool filtered=ArRenderTexture_IsValid(texture);
    if(!filtered)texture=guard->textures[band];
    DioramaLayerMesh mesh;
    if(!PrepareHollowGuardMesh(capture,g,layer,strip,rear,filtered,&mesh))return kPresentationOutcome_CoreFailure;
    if(!mesh.vertex_count)continue;
    const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
    const float oy=capture->camera_y-capture->authentic_y0+1;
    const float sample_width=filtered?strip->width:kDioramaTrunkGuardWidth;
    const float sample_height=filtered?kDioramaTrunkGuardRows:kDioramaTrunkGuardHeight;
    const ArRenderRectF source_uv={(strip->world_x-ox)/SR_PPU_SURFACE_MAX_WIDTH,
        (strip->world_y-oy-(filtered?0:strip->atlas_y))/SR_PPU_SURFACE_MAX_HEIGHT,
        sample_width/SR_PPU_SURFACE_MAX_WIDTH,sample_height/SR_PPU_SURFACE_MAX_HEIGHT};
    DioramaSceneryLightEffectParams light=layer->lighting;
    light.mode=0;light.blur_radius=0;light.attributes_uv=source_uv;
    light.texel_width=1.f/(sample_width*(filtered?kDioramaSupersample:1));
    light.texel_height=1.f/(sample_height*(filtered?kDioramaSupersample:1));
    if(!RenderShadedDioramaDepthGeometry(device,texture,mesh.vertices,mesh.depths,mesh.vertex_count,
        mesh.indices,mesh.index_count,layer->blend,layer->exposure,source_uv,&light,
        layer->depth_sink,layer->description->plane,false))return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}
