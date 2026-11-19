/* Rear waterfall contact foam must not be copied to the front rock plane.
 * Fixture IDs locate reviewed source art; runtime matching uses art keys. */
static void WaterfallFoamFixtures(const char *directory) {
  if(!directory)return;
  static unsigned char bytes[kActionSceneSnapshotMaxBytes];
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=2;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/040%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*copy=ActionVoxel_Create();assert(g&&copy);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(g,s,NULL,settings));
    unsigned foam_count=0,rock_count=0,cells=0;int edit_x=-1,edit_y=0;unsigned edit_pixel=0;
    const ActionRoomSceneFrameState frame={.brightness=15};
    for(unsigned ty=0;ty<ActionRoomScene_TileHeight(s,1);ty+=2)
      for(unsigned tx=0;tx<ActionRoomScene_TileWidth(s,1);tx+=2) {
        uint8_t id;uint16_t word;assert(ActionRoomScene_LookupTile(s,1,tx,ty,&word,&id));
        const bool pure=id==0x36||id==0x5e||id==0x81;
        const bool mixed=id==0x4e||id==0x4f||id==0xf4||id==0xf6||id==0xfc||id==0xfe;
        if(!pure&&!mixed)continue;++cells;
        uint32_t low[256],rear[256],front[256];
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned ink=FixturePixel(s,tx*8+x,ty*8+y);
          low[y*16+x]=ink?UINT32_C(0xff123400)|ink:0;
        }
        assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned p=y*16+x,ink=FixturePixel(s,tx*8+x,ty*8+y);
          const bool foam=pure||ink==7;
          assert(rear[p]==low[p]);assert(front[p]==(foam?0:low[p]));
          if(!ink)continue;
          const unsigned q=(y/8)*2+x/8;
          assert(ActionRoomScene_LookupTile(s,1,tx+x/8,ty+y/8,&word,NULL));
          const bool collision=(s->bg[0].metatiles[id*8+q*2]&2)!=0;
          const unsigned at=(ty*8+y-g->origin_y)*g->width+tx*8+x-g->origin_x;
          const unsigned kind=foam?kVoxelRearOnly:(word&0x2000)&&!collision?kVoxelForeground:kVoxelSolid;
          assert((g->front[at]&kVoxelKindMask)==kind);
          assert(!!(g->front[at]&kVoxelHighPriority)==!!(word&0x2000));
          assert(!!(g->front[at]&kVoxelColliding)==collision);
          if(foam) {
            ++foam_count;
            if(mixed&&edit_x<0){edit_x=tx/2;edit_y=ty/2;edit_pixel=p;}
          } else ++rock_count;
        }
      }
    assert(cells&&foam_count&&rock_count&&edit_x>=0);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(copy,&remapped,NULL,settings));SameGeometry(g,copy);
    /* Opaque black is a recolor, not transparency or a new front face. */
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[0];
    *edit=(DioramaPixelEdit){.by_cell=true,.x=edit_x,.y=edit_y};
    edit->black[edit_pixel/16]=1u<<(15-edit_pixel%16);
    assert(ActionVoxel_Build(copy,s,&terrain,settings));
    uint32_t low[256],rear[256],front[256];
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      low[y*16+x]=FixturePixel(s,edit_x*16+x,edit_y*16+y)?UINT32_C(0xff123456):0;
    low[edit_pixel]=UINT32_C(0xff000000);
    assert(ActionVoxel_Split(copy,edit_x*16,edit_y*16,16,16,16,&frame,low,16,rear,front));
    assert(rear[edit_pixel]==low[edit_pixel]&&!front[edit_pixel]);
    edit->black[edit_pixel/16]=0;edit->transparent[edit_pixel/16]=1u<<(15-edit_pixel%16);
    assert(ActionVoxel_Build(copy,s,&terrain,settings));low[edit_pixel]=0;
    assert(ActionVoxel_Split(copy,edit_x*16,edit_y*16,16,16,16,&frame,low,16,rear,front));
    assert(!rear[edit_pixel]&&!front[edit_pixel]);
    printf("waterfall foam 040%u-%u: %u cells, %u rear foam pixels, %u unchanged rock pixels; source priority/collision, renumbering and edits passed\n",
        room,region,cells,foam_count,rock_count);
    ActionVoxel_Release(g);ActionVoxel_Release(copy);
  }
}
