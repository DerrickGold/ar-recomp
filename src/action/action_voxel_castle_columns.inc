/* Native castle shafts can pass behind another landing. Their artwork is
 * noncolliding even when its reviewed material has a solid structural shape.
 * Recognize the same cap/shaft relationship as PlatformCap; tile addresses
 * and live RGB never participate. Recede immediately below the upper cap,
 * then keep its verified shaft against the rear so lower landings stay clear. */
static bool CastleColumnShaft(const CastleMaterial *shaft,const CastleMaterial *support) {
  if(shaft->collision)return false;
  for(unsigned q=0;q<4;++q)
    if(shaft->kind[q]!=kVoxelSolid ||
        ((shaft->words[q]^support->words[q])&0x3c00))return false;
  return true;
}
static bool CastleColumnOccupied(const ActionVoxelGeometry *c,int cx,int cy) {
  const int x0=cx*16-c->origin_x,y0=cy*16-c->origin_y;
  if(x0<0 || y0<0 || x0+16>c->width || y0+16>c->height)return false;
  unsigned quadrants=0;
  for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
    const unsigned pixel=c->front[(size_t)(y0+y)*c->width+x0+x];
    if(!pixel)continue;
    if((pixel&kVoxelKindMask)!=kVoxelSolid || (pixel&kVoxelColliding))return false;
    quadrants|=1u<<((y/8)*2+x/8);
  }
  return quadrants==15;
}
static bool BuildCastleColumns(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_Castle)return true;
  const int x0=c->origin_x/16,y0=c->origin_y/16;
  const int width=c->width/16,height=c->height/16;
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    const int cx=x+x0,cy=y+y0;
    CastleMaterial cap,below,shaft;uint8_t provenance;
    if(!LookupMaterial(s,stamps,native,cx,cy,&cap,&provenance,tiles,tile_pitch) ||
        !LookupMaterial(s,stamps,native,cx,cy+1,&below,&provenance,tiles,tile_pitch) ||
        !PlatformCap(&cap,&below))continue;
    const int top=cy+1;
    int end=top;
    /* The upper cap proves the shaft family. Opposite halves and shadow
     * glyphs share its palette/native priority; collision, other artwork or
     * an authored hole stops ownership. The cap remains at native contact. */
    while(end<y0+height &&
        LookupMaterial(s,stamps,native,cx,end,&shaft,&provenance,tiles,tile_pitch) &&
        CastleColumnShaft(&shaft,&below) && CastleColumnOccupied(c,cx,end))++end;
    if(end==top)continue;
    /* One native metatile forms the underside transition. The clamped curve
     * keeps all later shaft cells exactly at rear depth, without a second
     * bulge above the next platform or a disconnected vertical face. */
    const VoxelTrunkCurve curve={(short)(top*16),(short)((top+1)*16)};
    unsigned owner=0;
    while(owner<c->column_curve_count &&
        (c->column_curves[owner].top_y!=curve.top_y ||
         c->column_curves[owner].base_y!=curve.base_y))++owner;
    if(owner==c->column_curve_count) {
      if(owner==kVoxelMaxCastleColumns)return false;
      c->column_curves[c->column_curve_count++]=curve;
    }
    if(!c->column_cells) {
      c->column_cells=calloc((size_t)c->width/8,c->height/8);
      if(!c->column_cells)return false;
    }
    for(int row=top*2;row<end*2;++row)for(int col=cx*2;col<cx*2+2;++col) {
      const size_t at=(size_t)(row-c->origin_y/8)*(c->width/8)+col-c->origin_x/8;
      /* Never silently replace a different owner at an authored join. */
      if(c->column_cells[at] && c->column_cells[at]!=owner+1)return false;
      c->column_cells[at]=(uint8_t)(owner+1);
    }
  }
  return true;
}
