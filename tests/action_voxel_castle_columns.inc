/* Semantic cap undersides and landing joins, exercised with every regional
 * asset set and again after character/metatile renumbering. */
static void CastleColumnNativeFace(const ActionVoxelGeometry *geometry,
    const ActionRoomScene *scene,int tx,int ty) {
  uint32_t low[256],rear[256],front[256];DesertNativeLow(scene,tx,ty,low);
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(geometry,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)if(low[i]&0xffffffu)assert(front[i]==low[i]);
}
static void CastleColumnSplitOwnership(const ActionVoxelGeometry *geometry,
    const ActionRoomScene *scene,int tx,int ty) {
  const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForCell(geometry,tx,ty);assert(curve);
  const uint32_t empty[256]={0};
  uint32_t native[256],source[256],backing[256],unused[256],rear[256],front[256];
  DesertNativeLow(scene,tx,ty,native);
  const ActionRoomSceneFrameState frame={.brightness=15};
  /* Transparent source establishes the existing rear coverage independently
   * of native front ink. It must survive under holes or fractional alpha. */
  assert(ActionVoxel_Split(geometry,tx*8,ty*8,16,16,16,&frame,empty,16,backing,unused));
  for(unsigned i=0;i<256;++i)assert(!unused[i]);
  memcpy(source,native,sizeof(source));
  assert(ActionVoxel_Split(geometry,tx*8,ty*8,16,16,16,&frame,source,16,rear,front));
  assert(!memcmp(source,native,sizeof(source)));
  unsigned opaque=0;
  for(unsigned i=0;i<256;++i) {
    assert(front[i]==native[i]); /* Native colors and alpha remain exact. */
    const bool fully_rear=ty*8+(int)(i/16)>=curve->base_y;
    if((native[i]>>24)==255) {
      assert((backing[i]>>24)==255);
      assert(rear[i]==(fully_rear?0:backing[i]));++opaque;
    } else assert(rear[i]==backing[i]);
  }
  assert(opaque);
  const unsigned alpha[]={0,127,254};
  for(unsigned a=0;a<sizeof(alpha)/sizeof(*alpha);++a) {
    for(unsigned i=0;i<256;++i)
      source[i]=native[i] ? (native[i]&0xffffffu)|(alpha[a]<<24) : 0;
    uint32_t retained[256];memcpy(retained,source,sizeof(retained));
    assert(ActionVoxel_Split(geometry,tx*8,ty*8,16,16,16,&frame,source,16,rear,front));
    assert(!memcmp(source,retained,sizeof(source)));
    for(unsigned i=0;i<256;++i) {
      assert(front[i]==source[i]);
      assert(rear[i]==backing[i]);
    }
  }
}
static unsigned CastleColumnJoins(const ActionVoxelGeometry *geometry,const ActionRoomScene *scene) {
  const unsigned width=ActionRoomScene_TileWidth(scene,1),height=ActionRoomScene_TileHeight(scene,1);
  const float depth=ActionVoxel_Depth(geometry);
  unsigned edge_count,starts=0,landings=0,rear_cells=0;
  const VoxelEdge *edges=ActionVoxel_Edges(geometry,&edge_count);
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
    const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForCell(geometry,x,y);
    if(!curve)continue;
    /* A bounded immediate underside, never the final32px above a landing. */
    assert(curve->top_y>=16 && curve->base_y-curve->top_y==16);
    assert((int)y*8>=curve->top_y);
    assert(ActionVoxel_ColumnFrontDepth(geometry,curve,curve->top_y)==0);
    assert(fabsf(ActionVoxel_ColumnFrontDepth(geometry,curve,curve->base_y)-depth)<.000001f);
    assert(fabsf(ActionVoxel_ColumnFrontDepth(geometry,curve,curve->top_y+8)-depth*.5f)<.000001f);
    float last=-1;
    for(int yy=curve->top_y;yy<=curve->base_y;++yy) {
      const float front=ActionVoxel_ColumnFrontDepth(geometry,curve,yy);
      assert(front>=last && front>=0 && front<=depth);last=front;
    }
    if((int)y*8>=curve->base_y) {
      assert(ActionVoxel_ColumnFrontDepth(geometry,curve,y*8)==depth);
      assert(ActionVoxel_ColumnFrontDepth(geometry,curve,(y+1)*8)==depth);++rear_cells;
    }
    uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(scene,1,x,y,&word,&id));
    const unsigned quadrant=(y&1)*2+(x&1);
    assert(!(scene->bg[0].metatiles[id*8+quadrant*2]&2));
    if(!(x&1) && !(y&1)) {
      CastleColumnNativeFace(geometry,scene,x,y);
      CastleColumnSplitOwnership(geometry,scene,x,y);
    }
    if((int)y*8==curve->top_y) {
      /* Every run starts directly below a native colliding upper cap. The
       * cap itself keeps its source face and is never bent into the shaft. */
      assert(!ActionVoxel_ColumnCurveForCell(geometry,x,y-1));
      uint16_t cap_word;uint8_t cap_id;
      assert(ActionRoomScene_LookupTile(scene,1,x,y-2,&cap_word,&cap_id));
      unsigned collision=0;
      for(unsigned q=0;q<4;++q)collision|=!!(scene->bg[0].metatiles[cap_id*8+q*2]&2)<<q;
      assert(collision==3);
      if(!(x&1))CastleColumnNativeFace(geometry,scene,x,y-2);
      ++starts;
    }
    const VoxelTrunkCurve *next=ActionVoxel_ColumnCurveForCell(geometry,x,y+1);
    if(next) {
      /* Adjacent source rows share one exact depth function: no omitted
       * vertical segment or restart at the old near-landing taper. */
      assert(next==curve);continue;
    }
    if(y+1>=height || !ActionRoomScene_LookupTile(scene,1,x,y+1,&word,&id))continue;
    const unsigned cap_q=((y+1)&1)*2+(x&1);
    if(!(scene->bg[0].metatiles[id*8+cap_q*2]&2))continue;
    const int landing_y=(int)(y+1)*8;
    assert(ActionVoxel_ColumnFrontDepth(geometry,curve,landing_y)==depth);
    unsigned covered=0;
    for(unsigned i=0;i<edge_count;++i) {
      const VoxelEdge *edge=&edges[i];
      if(edge->side || edge->y!=landing_y || !edge->colliding || !edge->neighbor_column_curve)continue;
      const int left=edge->x>(int)x*8?edge->x:(int)x*8;
      const int right=edge->x+edge->length<(int)x*8+8?edge->x+edge->length:(int)x*8+8;
      if(right<=left)continue;
      assert(edge->high_priority==!!(word&0x2000));
      VoxelEdgeSpan span;
      assert(ActionVoxel_ResolveEdgeSpan(geometry,edge,kVoxelEdgeNative,0,depth,&span));
      const float far=edge->high_priority?fmaxf(0,ActionVoxel_PlayRecess(geometry)-.01f):depth;
      assert(span.near_depth==0 && fabsf(span.far_depth-far)<.000001f);
      if(edge->high_priority) {
        assert(ActionVoxel_ResolveEdgeSpan(geometry,edge,kVoxelEdgeHighDeckBehind,0,depth,&span));
        assert(fabsf(span.near_depth-far)<.000001f && fabsf(span.far_depth-depth)<.000001f);
      }
      covered+=(unsigned)(right-left);
    }
    if(covered!=8)fprintf(stderr,"castle landing missing top: room=%u cell=%u,%d covered=%u\n",
        scene->map,x*8,landing_y,covered);
    assert(covered==8);++landings;
  }
  for(unsigned i=0;i<edge_count;++i) {
    const VoxelEdge *edge=&edges[i];
    if(!(edge->side&1) || (!edge->column_curve && !edge->neighbor_column_curve))continue;
    VoxelEdgeSpan whole;
    assert(ActionVoxel_ResolveEdgeSpan(geometry,edge,kVoxelEdgeNative,0,depth,&whole));
    for(int y=edge->y;y<=edge->y+edge->length;++y) {
      VoxelEdgeSpan point;
      if(!ActionVoxel_ResolveEdgeSpanAtY(geometry,edge,kVoxelEdgeNative,0,depth,y,&point))continue;
      assert(point.near_depth>=whole.near_depth-.000001f && point.far_depth<=whole.far_depth+.000001f);
    }
  }
  if(starts)assert(landings && rear_cells);
  return starts;
}
static unsigned CastleColumnFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;static ActionRoomScene remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  unsigned checked=0;
  for(unsigned room=2;room<=8;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/02%02u-%u.arscene",directory,room,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    ActionVoxelGeometry *geometry=ActionVoxel_Create(),*renumbered=ActionVoxel_Create();assert(geometry&&renumbered);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,room);
    assert(ActionVoxel_Build(geometry,&fixture.scene,NULL,settings));
    const unsigned joins=CastleColumnJoins(geometry,&fixture.scene);
    assert(ActionVoxel_HasColumns(geometry)==!!joins);
    if(room==3 || room==5 || room==7)assert(joins);
    remapped=fixture.scene;Renumber(&remapped);
    assert(ActionVoxel_Build(renumbered,&remapped,NULL,settings));
    SameGeometry(geometry,renumbered);
    assert(CastleColumnJoins(renumbered,&remapped)==joins);
    if(joins) {
      /* Native collision ends a decorative shaft. Changing that fact must
       * remove its curve even though artwork, map and palette still match. */
      int found_x=-1,found_y=-1;
      for(unsigned y=0;y<ActionRoomScene_TileHeight(&fixture.scene,1)&&found_x<0;++y)
        for(unsigned x=0;x<ActionRoomScene_TileWidth(&fixture.scene,1);++x)
          if(ActionVoxel_ColumnCurveForCell(geometry,x,y)) {found_x=x;found_y=y;break;}
      assert(found_x>=0);
      remapped=fixture.scene;
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(&remapped,1,found_x,found_y,&word,&id));
      for(unsigned q=0;q<4;++q)remapped.bg[0].metatiles[id*8+q*2]|=2;
      assert(ActionVoxel_Build(renumbered,&remapped,NULL,settings));
      assert(!ActionVoxel_ColumnCurveForCell(renumbered,found_x,found_y));
    }
    printf("castle columns %u terrain %u: %u native8px cap undersides, immediate16px taper and rear shaft continuity\n",room,region,joins);
    ActionVoxel_Release(geometry);ActionVoxel_Release(renumbered);++checked;
  }
  return checked;
}
