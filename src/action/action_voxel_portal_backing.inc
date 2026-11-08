/* Reviewed portals retain their dark interior without cloning a second jamb.
 * Bamboo retains its exact native grove matte under the separated stalks.
 * Only the existing opaque high source footprint is covered; native voids
 * and explicit transparent edits remain open. */
static bool BuildOpaquePortalBacking(ActionVoxelGeometry *c,const CastleMaterial *material,
    int cx,int cy,const DioramaPixelEdit *edit,const uint16_t palette[128]) {
  VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};
  const uint16_t ink=material->opaque_rear_ink?palette[material->opaque_rear_ink-1]:0;
  bool any=false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8;
    if(!(material->words[q]&0x2000) || !material->pixels[q][(y&7)*8+(x&7)] ||
       MaterialTransparent(material,edit,x,y))continue;
    fill.colors[y*16+x]=0x8000|ink;any=true;
  }
  return !any || AppendRearFill(c,&fill);
}
