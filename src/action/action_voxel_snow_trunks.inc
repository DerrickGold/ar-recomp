/* Rounded native bark returns meet the adjacent shallow outline. Ownership
 * follows reviewed art and native opacity, not room coordinates or tile IDs. */
static bool SnowTrunkMaterial(const CastleMaterial *m,bool body) {
  if(!(m->flags&kVoxelTreeBarkMaterial))return false;
  for(unsigned q=0;q<4;++q) {
    if(m->words[q]&0x2000)return false;
    if(body?m->kind[q]!=kVoxelSolid:
        m->kind[q]!=kVoxelForeground || !(m->flags&kVoxelRearMountedMaterial))return false;
    for(unsigned p=0;p<64;++p)if(!m->pixels[q][p])return false;
  }
  return true;
}
static bool BuildSnowTrunkRounds(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_NorthwallSnow)return true;
  const int left=c->origin_x/16,top=c->origin_y/16,right=left+c->width/16,bottom=top+c->height/16;
  for(int y=top;y<bottom;++y)for(int x=left;x<right;) {
    CastleMaterial m;uint8_t id;
    if(!LookupMaterial(s,stamps,native,x,y,&m,&id,tiles,tile_pitch) || !SnowTrunkMaterial(&m,true)){++x;continue;}
    int end=x+1;
    while(end<right && LookupMaterial(s,stamps,native,end,y,&m,&id,tiles,tile_pitch) && SnowTrunkMaterial(&m,true))++end;
    if(end-x>=2)for(unsigned side=0;side<2;++side) {
      const int outline=side?end:x-1;
      if(outline<left || outline>=right ||
          !LookupMaterial(s,stamps,native,outline,y,&m,&id,tiles,tile_pitch) || !SnowTrunkMaterial(&m,false))continue;
      const int width=(end-x)*8<32?(end-x)*8:32;
      const VoxelTrunkCurve curve={.round_x=(side?end:x)*16,.round_width=side?-width:width};
      unsigned owner=0;
      while(owner<c->column_curve_count && (c->column_curves[owner].round_x!=curve.round_x ||
          c->column_curves[owner].round_width!=curve.round_width))++owner;
      if(owner==c->column_curve_count) {
        if(owner==kVoxelMaxCastleColumns)return false;
        c->column_curves[c->column_curve_count++]=curve;
      }
      if(!c->column_cells) {
        c->column_cells=calloc((size_t)c->width/8,c->height/8);
        if(!c->column_cells)return false;
      }
      const int start=side?end*16-width:x*16;
      for(int px=start;px<start+width;px+=8) {
        if(NativeContourEdited(terrain,px/16,y))continue;
        for(unsigned dy=0;dy<2;++dy) {
          const size_t at=(size_t)(y*2-top*2+dy)*(c->width/8)+px/8-left*2;
          if(c->column_cells[at] && c->column_cells[at]!=owner+1)return false;
          c->column_cells[at]=(uint8_t)(owner+1);
        }
      }
    }
    x=end;
  }
  return true;
}
