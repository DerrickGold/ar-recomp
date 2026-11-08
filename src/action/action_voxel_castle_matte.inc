/* Castle02/07 ends in an opaque, noncolliding native black void. Continue
 * only trailing boundary runs of that exact semantic source; ordinary walls,
 * windows and authored removals cannot become a manufactured backstop. */
static bool CastleTerminalVoidCell(const ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],int cx,int cy,
    const uint16_t *tiles,size_t tile_pitch) {
  CastleMaterial m;uint8_t id;
  if(!LookupMaterial(s,stamps,native,cx,cy,&m,&id,tiles,tile_pitch)||
      MaterialKey(&m,0)!=UINT64_C(0xabde6e24e3e445e5)||m.collision||
      SceneryBackingEdited(terrain,cx,cy))return false;
  const DioramaVoxelCell *authored=terrain?DioramaVoxelEdits_Cell(&terrain->voxel,cx,cy):NULL;
  if(authored&&authored->fields)return false;
  const DioramaPixelEdit *paint=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,id):NULL;
  for(unsigned q=0;q<4;++q)if(m.words[q]&0x2000)return false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,ink=m.pixels[q][(y&7)*8+(x&7)];
    if(!ink||palette[((m.words[q]>>10)&7)*16+ink]||
        MaterialTransparent(&m,paint,x,y)||(m.cutouts[y]&(1u<<(15-x))))return false;
    const size_t at=(size_t)(cy*16+(int)y-c->origin_y)*c->width+cx*16+(int)x-c->origin_x;
    if(c->front[at])return false;
  }
  return true;
}
static void BuildCastleTerminalMatte(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  c->terminal_matte=(VoxelTerminalMatte){0};
  if(s->group!=2||s->map!=7||c->profile!=kActionVoxelProfile_Castle||
      c->origin_x||c->origin_y||
      c->width!=(int)ActionRoomScene_TileWidth(s,1)*8||
      c->height!=(int)ActionRoomScene_TileHeight(s,1)*8)return;
  const int right=c->width/16,bottom=c->height/16;
  int x=right,y=bottom;
  while(x>0&&CastleTerminalVoidCell(c,s,terrain,stamps,native,palette,x-1,bottom-1,tiles,tile_pitch))--x;
  while(y>0&&CastleTerminalVoidCell(c,s,terrain,stamps,native,palette,right-1,y-1,tiles,tile_pitch))--y;
  c->terminal_matte=(VoxelTerminalMatte){.world_right=c->width,.world_bottom=c->height,
    .bottom_start=x*16,.right_start=y*16,.bottom_opaque_black=x<right,.right_opaque_black=y<bottom};
}
