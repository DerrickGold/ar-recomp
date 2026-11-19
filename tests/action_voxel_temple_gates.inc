/* The lower-hall grille is an eight-pixel strip beside the masonry, not
 * another solid column. Its exact native ink closes the doorway in depth. */
static void TempleGateChecks(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  unsigned count;const VoxelGate *gates=ActionVoxel_Gates(base,&count);
  if(s->map==4){assert(!count);return;}
  assert(count==1 && gates[0].x==528 && gates[0].y==1576 && gates[0].width==8 && gates[0].height==80);
  assert(gates[0].centered && !gates[0].solid_face && !gates[0].high_priority);
  assert(ActionVoxel_ClosureInset(base)>0 && ActionVoxel_ClosureInset(base)<ActionVoxel_Depth(base));
  const uint8_t expected[8]={0x76,0x76,0x76,0x76,0xff,0x76,0x76,0x76};
  const ActionRoomSceneFrameState frame={.brightness=15};
  const unsigned width=ActionRoomScene_TileWidth(s,1),height=ActionRoomScene_TileHeight(s,1);
  uint16_t *tiles=malloc((size_t)width*height*sizeof(*tiles));assert(tiles);
  unsigned strips=0,ink=0,blank=0;
  while(blank<sizeof(s->characters)/32) {
    unsigned any=0;for(unsigned i=0;i<32;++i)any|=s->characters[blank*32+i];
    if(!any)break;++blank;
  }
  assert(blank<sizeof(s->characters)/32);
  for(unsigned ty=0;ty<height;++ty)for(unsigned tx=0;tx<width;++tx) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,tx,ty,&word,NULL));
    tiles[ty*width+tx]=word;
    uint8_t rows[8],high[8];ActionVoxel_GateInkRows(base,tx,ty,false,rows);
    ActionVoxel_GateInkRows(base,tx,ty,true,high);
    const bool grille=(word&0x1fff)==0x187e;
    if(grille) {
      assert(!memcmp(rows,expected,sizeof(rows)));++strips;
      tiles[ty*width+tx]=(word&0xfc00)|blank;
    }
    for(unsigned y=0;y<8;++y)assert(!high[y] && (grille || !rows[y]));
    if(!grille)continue;
    uint32_t native[64],rear[64],front[64];
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
      native[y*8+x]=ForestNativeColor(s,tx*8+x,ty*8+y,&frame);
    assert(ActionVoxel_Split(base,tx*8,ty*8,8,8,8,&frame,native,8,rear,front));
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const bool bar=(expected[y]&(128>>x))!=0;const unsigned at=y*8+x;
      /* The established gate path keeps the native rear endpoint and
       * copies only bar ink forward for the panel/closure inset. */
      assert(front[at]==(bar?native[at]:0) && rear[at]==native[at]);ink+=bar;
    }
  }
  assert(strips==10 && ink==430);
  /* A live native opening replaces the grille characters. No cached panel
   * may survive; restoring the source must restore precisely one gate. */
  assert(ActionVoxel_BuildWithTiles(edited,s,NULL,settings,tiles,width));
  ActionVoxel_Gates(edited,&count);assert(!count);free(tiles);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *erase=&terrain.pixel_layers[0].edits[0];
  *erase=(DioramaPixelEdit){.by_cell=true,.x=33,.y=99};
  for(unsigned y=0;y<16;++y)erase->transparent[y]=0xffff;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  gates=ActionVoxel_Gates(edited,&count);unsigned total=0;
  for(unsigned i=0;i<count;++i) {
    assert(gates[i].y+gates[i].height<=99*16 || gates[i].y>=100*16);total+=gates[i].height;
  }
  assert(total==64); /* Erasing one cell cannot leave a panel over its hole. */
  assert(ActionVoxel_Build(edited,s,NULL,settings));SameGeometry(base,edited);
  gates=ActionVoxel_Gates(edited,&count);assert(count==1 && gates[0].height==80);
  printf("temple grille: 80px closure, 430 native bar pixels, preserved gaps/priority, live opening and edit reset passed\n");
}
