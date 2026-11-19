/* A mixed closed-door tile owns independent grille ink and rear masonry.
 * The native bar mask must not promote the neighbouring stone into a post. */
static unsigned RearGateJambChecks(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    unsigned first,unsigned second) {
  unsigned jamb_pixels=0,grille_pixels=0;
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned ty=0;ty<ActionRoomScene_TileHeight(s,1);ty+=2)
    for(unsigned tx=0;tx<ActionRoomScene_TileWidth(s,1);tx+=2) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,tx,ty,&word,&id));
      if(id!=first&&id!=second)continue;
      uint32_t native[256],rear[256],front[256];
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        native[y*16+x]=ForestNativeColor(s,tx*8+x,ty*8+y,&frame);
      assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      for(unsigned q=0;q<4;++q) {
        uint8_t rows[8],high[8];
        ActionVoxel_GateInkRows(g,tx+(q&1),ty+(q>>1),false,rows);
        ActionVoxel_GateInkRows(g,tx+(q&1),ty+(q>>1),true,high);
        assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&word,NULL));
        assert(!(word&0x2000));
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
          const unsigned p=((q>>1)*8+y)*16+(q&1)*8+x;
          const bool bar=(rows[y]&(128>>x))!=0;
          assert(!high[y]&&rear[p]==native[p]&&front[p]==(bar?native[p]:0));
          if(native[p]){if(bar)++grille_pixels;else ++jamb_pixels;}
        }
      }
    }
  unsigned gates;ActionVoxel_Gates(g,&gates);assert(!gates||(jamb_pixels&&grille_pixels));
  return jamb_pixels+grille_pixels;
}
