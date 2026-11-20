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
    const DioramaLayerDraw *layer,const DioramaTrunkGuardStrip *strip,bool rear,DioramaLayerMesh *mesh) {
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
    if(region<2&&layer->hollow_voxel&&s_trunk_assets->side_clearance&&
        layer->description->plane==SR_PPU_OVERLAY_BG1)continue;
    if(region==2&&layer->hollow_voxel&&layer->description->plane==kDioramaPlane_Bg1Hi&&!rear) {
      if(!AppendHollowRimMesh(capture,g,layer,left,right,first_y,last_y,mesh))return false;
      continue;
    }
    if(region==2&&layer->hollow_column&&layer->description->plane==SR_PPU_OVERLAY_BG1&&!rear) {
      if(!AppendHollowColumnMesh(capture,g,layer,left,right,first_y,last_y,mesh))return false;
      continue;
    }
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
      const float z=front-(rear?DioramaTrunk_SideDepth(layer->hollow_recess,world_x,layer->hollow_voxel):0);
      const unsigned index=(unsigned)mesh->vertex_count++;ArRenderVertex2D *vertex=&mesh->vertices[index];
      if(!ProjectHollowPoint(g,mesh,index,x,y,z))return false;
      vertex->tex_coord=(ArRenderPointF){u,v};vertex->color=layer->shade;
    }
    for(unsigned col=0;col+1<count;++col) {
      const int a=base+(int)col,b=a+1,c=b+(int)count,d=a+(int)count;
      const int indices[]={a,b,c,a,c,d};
      memcpy(mesh->indices+mesh->index_count,indices,sizeof(indices));mesh->index_count+=6;
    }
  }
  return true;
}
static PresentationOutcome DrawHollowGuard(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,const DioramaTrunkGuardView *guard) {
  if(!HollowGuardMatches(capture,guard))return kPresentationOutcome_Complete;
  const unsigned band=layer->description->plane==SR_PPU_OVERLAY_BG1?0:
      layer->description->plane==kDioramaPlane_Bg1Hi?1:2;
  const bool rear=band==2;
  PresentationOutcome outcome=kPresentationOutcome_Complete;
  for(unsigned i=0;i<kDioramaTrunkGuardStrips;++i) {
    const DioramaTrunkGuardStrip *strip=&guard->layout.strips[i];
    if(!strip->valid.width||!strip->valid.height)continue;
    DioramaLayerDraw source=*layer;source.texture=guard->textures[band];
    if(!rear&&(!DrawHollowSolidEdges(device,capture,g,&source,band==0?1:0,strip)||
        !DrawHollowPassageEdges(device,capture,g,&source,band==1,strip)))
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
    const HollowGuardSource guard_source=PrepareHollowGuardSource(capture,layer,strip,filtered);
    if(band==0&&layer->hollow_voxel&&s_trunk_assets->side_clearance) {
      const float regions[2][2]={{0,320},{704,(float)s_trunk_assets->side_width}};
      const float x0=strip->world_x+strip->valid.x,x1=x0+strip->valid.width;
      const float y0=strip->world_y+strip->valid.y,y1=y0+strip->valid.height;
      HollowFrontSource carved={.device=device,.capture=capture,.geometry=g,.layer=layer,
          .guard=&guard_source,.texture=texture,.outcome=outcome};
      for(unsigned region=0;region<2;++region)
        if(!DrawHollowCarvedFront(&carved,fmaxf(regions[region][0],x0),fminf(regions[region][1],x1),y0,y1))
          return kPresentationOutcome_CoreFailure;
      outcome=carved.outcome;
    }
    DioramaLayerMesh mesh;
    if(!PrepareHollowGuardMesh(capture,g,layer,strip,rear,&mesh) ||
        !SubmitHollowGuardMesh(device,texture,layer,&guard_source,&mesh))
      return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}
