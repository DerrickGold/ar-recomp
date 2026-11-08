/* Sand's native character window animates. Keep source coordinates, not an
 * owned RGB atlas: existing scanouts supply the current phase and palette.
 * Qualification is immutable and bounded to the actual emitted contour. */
static unsigned SurfaceSandLiveTop(VoxelSurfaceBuild *b,const VoxelEdge *top) {
  if(!b->native_surfaces || b->geometry->profile!=kActionVoxelProfile_KassandoraDesert ||
      top->side || top->far_rank!=2 || top->length<=0 || top->length>8 ||
      !top->soft_surface || top->top_sample_size)return false;
  CastleMaterial owner;const DioramaPixelEdit *edit;
  const int cx=(int)floorf(top->x/16.f)*16,cy=(int)floorf(top->y/16.f)*16;
  if(!SurfaceMaterial(b,cx,cy,&owner,&edit) ||
      owner.native_surface_family!=kVoxelNativeSurfaceDesertSandLive ||
      !(owner.native_surface_faces&1))return false;
  unsigned extent=0;
  for(unsigned along=0;along<(unsigned)top->length;++along) {
    const int x=top->x+(int)along;
    unsigned reference=0,column_extent=0;
    for(unsigned depth=0;depth<16;++depth) {
      const int y=top->y+(int)depth;
      const int mx=(int)floorf(x/16.f)*16,my=(int)floorf(y/16.f)*16;
      CastleMaterial material;
      if(!SurfaceMaterial(b,mx,my,&material,&edit) ||
          material.native_surface_family!=kVoxelNativeSurfaceDesertSandLive)break;
      if(ActionVoxel_AuthoredSurfaceAt(b->geometry,x,y))return 0;
      const unsigned px=(unsigned)(x-mx),py=(unsigned)(y-my),q=(py/8)*2+px/8;
      const unsigned word=material.words[q],bit=1u<<(15-px);
      if(!depth)reference=word;
      const unsigned ink=AuthoredPixel(b->scene,word,px&7,py&7);
      /* Authored erasure/black and explicit cutouts keep the established
       * fail-closed policy. Native material/priority/alpha ends the real band. */
      if((material.cutouts[py]&bit) ||
          (edit && ((edit->black[py]|edit->transparent[py])&bit)))return 0;
      if(material.kind[q]!=kVoxelSolid || ((word^reference)&0x3c00) || !ink ||
          (material.rear_rows[py]&bit) ||
          ((material.rear_colors[q]|material.face_colors[q]|material.gate_colors[q])&(1u<<ink)))break;
      ++column_extent;
    }
    if(!column_extent || (along && column_extent!=extent))return 0;
    extent=column_extent;
  }
  return extent;
}
static bool SurfaceSandLiveEdge(VoxelSurfaceBuild *b,VoxelEdge *edge) {
  VoxelEdge anchor=*edge;
  if(edge->side) {
    const unsigned length=edge->fold_length?edge->fold_length:edge->length;
    if(!(edge->side&1) || !edge->soft_surface || length>2)return false;
    bool found=false;
    for(unsigned i=0;i<b->geometry->edge_count;++i) {
      const VoxelEdge *top=&b->geometry->edges[i];
      if(top->side || top->y!=edge->y || edge->x<top->x || edge->x>=top->x+top->length ||
          top->near_rank!=edge->near_rank || top->far_rank!=edge->far_rank ||
          top->high_priority!=edge->high_priority || top->colliding!=edge->colliding ||
          top->rear_mounted!=edge->rear_mounted || top->soft_surface!=edge->soft_surface)continue;
      anchor=*top;anchor.x=edge->x;anchor.length=1;found=true;break;
    }
    if(!found)return false;
  }
  const unsigned extent=SurfaceSandLiveTop(b,&anchor);
  if(!extent)return false;
  edge->surface_texture=kVoxelSurfaceTextureLiveInward;
  edge->top_sample_x=anchor.x;edge->top_sample_y=anchor.y;edge->top_sample_size=(unsigned char)extent;
  edge->top_owned_art=0;edge->surface_derived=true;
  edge->surface_top_material=!!edge->side;edge->contour_skin=0;
  return true;
}
