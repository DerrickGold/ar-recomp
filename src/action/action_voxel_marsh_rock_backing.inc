/* The two reviewed sloping rock feet mix foreground black shadow with the
 * rear painted earth band. Folding that band needs complete ground beneath
 * the shadow; borrow only missing pixels from a compatible adjacent band. */
static bool MarshEarthSupport(const CastleMaterial *m) {
  if(!(m->flags&kVoxelTerrainBodyMaterial) || m->collision!=15)return false;
  for(unsigned q=0;q<4;++q)
    if(m->kind[q]!=kVoxelSolid || (m->words[q]&0x2000))return false;
  return true;
}
static bool BuildMarshRockGroundBacking(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const CastleMaterial *target,int cx,int cy,
    const DioramaPixelEdit *target_edit,const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_BloodpoolMarsh || target->rear_fold_pixels!=8 ||
      target->collision || !(target->rear_colors[2]|target->rear_colors[3]))
    return true;
  CastleMaterial support;uint8_t id;
  if(!LookupMaterial(s,stamps,native,cx,cy+1,&support,&id,tiles,tile_pitch) ||
      !MarshEarthSupport(&support))return true;
  for(int distance=1;distance<=4;++distance)for(int direction=-1;direction<=1;direction+=2) {
    CastleMaterial donor;const int dx=cx+direction*distance;
    bool continuous=true;
    for(int step=1;step<=distance && continuous;++step)
      continuous=LookupMaterial(s,stamps,native,cx+direction*step,cy+1,&support,&id,tiles,tile_pitch) &&
          MarshEarthSupport(&support);
    if(!continuous || !LookupMaterial(s,stamps,native,dx,cy,&donor,&id,tiles,tile_pitch) ||
        donor.rear_fold_pixels!=target->rear_fold_pixels || !RearFillDonor(target,&donor))continue;
    const DioramaPixelEdit *donor_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,dx,cy,id):NULL;
    if(!MaterialVisible(&donor,donor_edit))continue;
    VoxelRearFill fill={.x=cx*16,.y=cy*16,.preserve_existing=true};bool any=false;
    for(unsigned y=8;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned q=(y/8)*2+x/8,at=(y&7)*8+(x&7),bit=1u<<(15-x);
      const unsigned p=target->pixels[q][at];
      if(!p || (target->words[q]&0x2000) || target->kind[q]!=kVoxelForeground ||
          (target->rear_colors[q]&(1u<<p)) || MaterialTransparent(target,target_edit,x,y) ||
          MaterialTransparent(&donor,donor_edit,x,y))continue;
      if((target_edit && (target_edit->black[y]&bit)) || (donor_edit && (donor_edit->black[y]&bit))) {
        fill.colors[y*16+x]=0x8000;any=true;continue;
      }
      const unsigned sample=donor.pixels[q][at];
      if(!sample)continue;
      fill.colors[y*16+x]=palette[((donor.words[q]>>10)&7)*16+sample]|0x8000;any=true;
    }
    if(any)return AppendRearFill(c,&fill);
  }
  return true;
}
