/* Native noncolliding divots must not look like platforms across a gap.
 * IDs locate the reviewed fixture; runtime identity follows displayed art. */
static unsigned SnowDivotCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned cells=0,variants=0;const unsigned ids[]={0x45,0x46,0x4d,0x4e};
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      unsigned family=4;for(unsigned i=0;i<4;++i)if(id==ids[i])family=i;
      if(family==4)continue;variants|=1u<<family;++cells;
      /* Check the complete two-row entrance, not just its snowy crown. */
      uint8_t paired;assert(ActionRoomScene_LookupTile(s,1,x*2,(y+(family<2?1:-1))*2,&word,&paired));
      assert(paired==ids[family^2]);
      assert(!SnowAct2Collision(s,x,y));VoxelCellGeometry cell;
      assert(ActionVoxel_CellGeometry(g,x,y,&cell));
      for(unsigned q=0;q<4;++q) {
        bool opaque=false;for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px)
          opaque|=FixturePixel(s,x*16+(q&1)*8+px,y*16+(q>>1)*8+py)!=0;
        assert(fabsf(cell.front[q]-(opaque?.15f:.2f))<.000001f && fabsf(cell.back[q]-.2f)<.000001f);
      }
      uint32_t source[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,source);
      assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,source,16,rear,front));
      assert(!memcmp(source,rear,sizeof(source)));
      /* The lip's native dark void ink stays in the rear; it must not turn
       * into another projecting face when the snowy outline is recessed. */
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned owner=g->front[(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x];
        const unsigned ink=FixturePixel(s,x*16+px,y*16+py);
        const bool rear_only=family>=2 && ink==1;
        assert(front[py*16+px]==(rear_only?0:source[py*16+px]));
        if(!ink)continue;
        assert((owner&kVoxelKindMask)==(rear_only?kVoxelRearOnly:kVoxelForeground) &&
            !(owner&(kVoxelHighPriority|kVoxelColliding)));
      }
    }
  assert(cells==8 && variants==15);
  /* Snow caps supported by colliding banks remain continuous at full depth. */
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      if(id<0x41 || id>0x43)continue;
      VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(g,x,y,&cell));
      for(unsigned q=0;q<4;++q) {
        bool opaque=false;for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px)
          opaque|=FixturePixel(s,x*16+(q&1)*8+px,y*16+(q>>1)*8+py)!=0;
        if(opaque)assert(cell.front[q]==0 && fabsf(cell.back[q]-.2f)<.000001f);
      }
    }
  VoxelCellGeometry solid;assert(ActionVoxel_CellGeometry(g,21,31,&solid));
  assert(SnowAct2Collision(s,21,31)==15);
  for(unsigned q=0;q<4;++q)assert(solid.front[q]==0 && fabsf(solid.back[q]-.2f)<.000001f);
  return cells;
}
static void SnowDivotFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0605.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;original=*s;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a && b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,5);
    assert(ActionVoxel_Build(a,s,NULL,settings));const unsigned cells=SnowDivotCheck(s,a);
    const uint64_t geometry=TombCapGeometryHash(a);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(b,&variant,NULL,settings));
    assert(TombCapGeometryHash(b)==geometry);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));assert(ActionVoxel_Build(b,&variant,NULL,settings));
    assert(SnowDivotCheck(&variant,b)==cells && TombCapGeometryHash(b)==geometry);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=20,.y=31,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
      .front=.02f,.back=.19f};assert(ActionVoxel_Build(b,s,&terrain,settings));
    const VoxelAuthoredCell *edit=ActionVoxel_AuthoredCellAt(b,20,31);assert(edit && edit->surface);
    for(unsigned q=0;q<4;++q)assert(edit->front[q]==.02f && edit->back[q]==.19f);
    VoxelCellGeometry neighbor;assert(ActionVoxel_CellGeometry(b,19,31,&neighbor));
    for(unsigned q=0;q<4;++q)assert(fabsf(neighbor.front[q]-.15f)<.000001f && fabsf(neighbor.back[q]-.2f)<.000001f);
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Northwall snow divots region%u: %u noncolliding cells/4 art variants (complete two-row entrances) recessed behind actors; full-depth supported banks and neighboring floor, source art/backing, remaps, fades and local overrides preserved\n",region,cells);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
}
