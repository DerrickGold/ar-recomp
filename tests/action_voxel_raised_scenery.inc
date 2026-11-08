/* Fixture IDs identify the reviewed column/arch artwork only. Production
 * classification must survive CHR and metatile renumbering unchanged. */
static bool RaisedSceneryColumn(unsigned id) {
  return id==0x42 || id==0x43 || id==0x73 || id==0x74;
}
static bool RaisedSceneryArch(unsigned id) {
  return (id>=0x08 && id<=0x0b) || (id>=0x10 && id<=0x13) ||
      id==0x38 || id==0x39 || id==0x75;
}
static unsigned RaisedSceneryCell(const ActionVoxelGeometry *g,
    const ActionRoomScene *source,int tx,int ty,int cx,int cy,bool raised) {
  uint16_t word;uint8_t id;
  assert(ActionRoomScene_LookupTile(source,1,tx,ty,&word,&id));
  const float depth=ActionVoxel_Depth(g),expected_front=depth*(raised?.625f:.75f);
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,cx,cy,&shape));
  assert(!shape.authored && shape.width==16 && shape.offset_x==0);
  uint32_t low[256],rear[256],front[256];DesertNativeLow(source,tx,ty,low);
  unsigned visible_pixels=0;
  for(unsigned q=0;q<4;++q) {
    assert(ActionRoomScene_LookupTile(source,1,tx+(q&1),ty+(q>>1),&word,NULL));
    const unsigned raw=(source->bg[0].metatiles[id*8+q*2]<<8)|source->bg[0].metatiles[id*8+q*2+1];
    /* Decorative architecture remains noncolliding and behind native actors. */
    assert(!(raw&0x200) && !(word&0x2000));
    uint8_t cutout[8]={0};ActionVoxel_CutoutRows(g,cx*2+(q&1),cy*2+(q>>1),cutout);
    unsigned visible=0;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const unsigned at=((q>>1)*8+y)*16+(q&1)*8+x;
      if(cutout[y]&(1u<<(7-x)))low[at]=0;
      visible+=low[at]!=0;
    }
    if(visible) {
      assert(fabsf(shape.front[q]-expected_front)<.00001f);
      assert(fabsf(shape.back[q]-depth)<.00001f);
      assert(fabsf(ActionVoxel_FrontRecess(g,cx*2+(q&1),cy*2+(q>>1))-expected_front)<.00001f);
    }
    visible_pixels+=visible;
  }
  assert(visible_pixels);
  if(raised) {
    /* Independent native silhouette counts: no newly erased bands can pass
     * merely because both split outputs use the same damaged cutout mask. */
    assert(visible_pixels==((id==0x42 || id==0x43)?224:213));
  }
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i) {
    assert(front[i]==low[i]);
    assert(!rear[i]); /* Exactly one owner for every preserved native pixel. */
  }
  return visible_pixels;
}
static unsigned RaisedSceneryBody(const ActionVoxelGeometry *g,const ActionRoomScene *source) {
  unsigned columns=0,arches=0,pixels=0,raised_edges=0,arch_edges=0;
  for(int ty=0;ty<ActionRoomScene_TileHeight(source,1);ty+=2)
    for(int tx=0;tx<ActionRoomScene_TileWidth(source,1);tx+=2) {
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(source,1,tx,ty,&word,&id));
      const bool raised=RaisedSceneryColumn(id);
      if(!raised && !RaisedSceneryArch(id))continue;
      pixels+=RaisedSceneryCell(g,source,tx,ty,tx/2,ty/2,raised);
      if(raised)++columns;else ++arches;
    }
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&id));
    const bool raised=RaisedSceneryColumn(id);
    if(!raised && !RaisedSceneryArch(id)) {
      assert(!e->raised_scenery);continue;
    }
    assert(e->raised_scenery==raised && e->rear_mounted);
    assert(e->far_rank==1 && !e->high_priority && !e->colliding);
    const float front=ActionVoxel_Depth(g)*(raised?.625f:.75f);
    assert(fabsf(ActionVoxel_EdgeRecess(g,e)-front)<.00001f);
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
    assert(span.near_depth>=front-.00001f && span.far_depth<=ActionVoxel_Depth(g)+.00001f);
    if(!e->near_rank) {
      assert(fabsf(span.near_depth-front)<.00001f);
      assert(fabsf(span.far_depth-ActionVoxel_Depth(g))<.00001f);
    }
    if(raised)++raised_edges;else ++arch_edges;
  }
  assert(columns==96 && arches && raised_edges && arch_edges && pixels);
  return pixels;
}
static void RaisedSceneryEdits(ActionVoxelGeometry *base,ActionVoxelGeometry *edited,
    const ActionRoomScene *source,ActionVoxelSettings settings) {
  int tx,ty,ax,ay;assert(FixtureCell(source,0x42,&tx,&ty));
  assert(FixtureCell(source,0x08,&ax,&ay));
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=tx/2,.y=ty/2,.fields=kDioramaVoxelCell_Width,.width=8};
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(edited,tx/2,ty/2,&shape));
  assert(shape.authored && shape.width==8);
  for(unsigned q=0;q<4;++q) {
    assert(fabsf(shape.front[q]-.15f)<.00001f && fabsf(shape.back[q]-.24f)<.00001f);
  }
  unsigned count;const VoxelAuthoredCell *cells=ActionVoxel_AuthoredCells(edited,&count);
  assert(count==1);
  for(unsigned q=0;q<4;++q)assert(!cells[0].priority[q]);
  /* Width-only editing adopts the new native depth; explicit front/back
   * edits win, and clearing that cell restores the untouched whole room. */
  edit->fields|=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back;
  edit->front=.11f;edit->back=.21f;
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  assert(ActionVoxel_CellGeometry(edited,tx/2,ty/2,&shape) && shape.authored);
  for(unsigned q=0;q<4;++q)assert(shape.front[q]==.11f && shape.back[q]==.21f);
  terrain.voxel.cell_count=0;
  assert(ActionVoxel_Build(edited,source,&terrain,settings));SameGeometry(base,edited);

  /* Authored stamps reuse the same semantic art beyond both negative world
   * axes. Unrelated empty cells must not acquire raised geometry. */
  DioramaTileStamp stamps[12];unsigned stamp_count=0;
  memset(stamps,0,sizeof(stamps));memset(&terrain,0,sizeof(terrain));
  for(int y=-2;y<1;++y)for(int x=-2;x<2;++x) {
    DioramaTileStamp *stamp=&stamps[stamp_count++];
    stamp->x=x;stamp->y=y;stamp->blank=true;
    if(y!=-1 || (x!=-1 && x!=0))continue;
    stamp->blank=false;
    const int sx=x==-1?tx:ax,sy=x==-1?ty:ay;
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(source,1,
        sx+(q&1),sy+(q>>1),&stamp->words[q],&stamp->metatile));
  }
  terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,
      .x0=-2,.y0=-2,.x1=2,.y1=1,.cells=stamps,.count=stamp_count};
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  RaisedSceneryCell(edited,source,tx,ty,-1,-1,true);
  RaisedSceneryCell(edited,source,ax,ay,0,-1,false);
  assert(ActionVoxel_CellGeometry(edited,-2,-2,&shape));
  for(unsigned q=0;q<4;++q)assert(shape.front[q]==settings.depth && shape.back[q]==settings.depth);
  memset(&terrain,0,sizeof(terrain));
  assert(ActionVoxel_Build(edited,source,&terrain,settings));SameGeometry(base,edited);
}
static void RaisedSceneryFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene source,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  const unsigned rooms[][2]={{1,4},{7,2}};
  for(unsigned r=0;r<sizeof(rooms)/sizeof(*rooms);++r)for(unsigned region=0;region<3;++region) {
    const unsigned group=rooms[r][0],room=rooms[r][1];
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,group,room);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));source=snapshot.scene;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(group,room);
    assert(fabsf(settings.depth-.24f)<.00001f);
    assert(ActionVoxel_Build(base,&source,NULL,settings) && ActionVoxel_Active(base,group,room));
    const unsigned pixels=RaisedSceneryBody(base,&source);
    variant=source;Renumber(&variant);
    assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(base,edited);
    assert(RaisedSceneryBody(edited,&source)==pixels);
    variant=source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(base,edited);
    assert(RaisedSceneryBody(edited,&source)==pixels);
    RaisedSceneryEdits(base,edited,&source,settings);
    settings.depth=.32f;
    assert(ActionVoxel_Build(edited,&source,NULL,settings));
    assert(RaisedSceneryBody(edited,&source)==pixels);
    assert(!memcmp(&source,&snapshot.scene,sizeof(source)));
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
    printf("raised scenery %02u/%02u terrain %u: 96 columns, %u single-owner pixels, unchanged arches/priority/collision, remapping/fade/authored/stamp reset passed\n",
        group,room,region,pixels);
  }
}
