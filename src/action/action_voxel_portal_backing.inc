/* Reviewed portal art already has its rear jamb. Moving the opaque front
 * jamb must retain the dark interior underneath, without cloning its gold
 * trim into a second opening. Only the existing opaque source footprint is
 * covered; native voids and explicit transparent edits remain open. */
static bool BuildOpaquePortalBacking(ActionVoxelGeometry *c,const CastleMaterial *material,
    int cx,int cy,const DioramaPixelEdit *edit) {
  VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};
  bool any=false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8;
    if(!(material->words[q]&0x2000) || !material->pixels[q][(y&7)*8+(x&7)] ||
       MaterialTransparent(material,edit,x,y))continue;
    fill.colors[y*16+x]=0x8000;any=true;
  }
  return !any || AppendRearFill(c,&fill);
}
