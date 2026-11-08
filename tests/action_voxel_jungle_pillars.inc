/* These native high-priority ornaments are single foreground posts, not a
 * front/rear pair of castle battlements. IDs locate fixture artwork only;
 * the production contract and remapped repeat use artwork fingerprints. */
static bool JunglePillarFixtureId(unsigned room,unsigned id) {
  return id==0x33 || id==0x2b || (room<=2 && id==0x04) ||
      (room==1 && (id==0x1b || id==0x1c));
}
static void JunglePillarShape(const ActionVoxelGeometry *g,int x,int y) {
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,x,y,&shape));
  assert(!shape.authored);
  for(unsigned q=0;q<4;++q) {
    assert(shape.front[q]==0 && fabsf(shape.back[q]-.05f)<.000001f);
    for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
      const unsigned material=g->front[(y*16+(q>>1)*8+py-g->origin_y)*g->width+
          x*16+(q&1)*8+px-g->origin_x];
      if(!material)continue;
      assert((material&kVoxelKindMask)==kVoxelForeground);
      assert(material&kVoxelHighPriority);
      assert(!(material&(kVoxelColliding|kVoxelRearTrim)));
    }
  }
}
static void JungleRearColumnShape(const ActionRoomScene *s,const ActionVoxelGeometry *g,int x,int y) {
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,x,y,&shape));
  uint32_t low[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,low);
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned q=0;q<4;++q) {
    uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,&id));
    assert(!(word&0x2000) && !(s->bg[0].metatiles[id*8+q*2]&2));
    assert(fabsf(shape.front[q]-.2f)<.000001f && fabsf(shape.back[q]-.2f)<.000001f);
  }
  for(unsigned p=0;p<256;++p)assert(rear[p]==low[p] && !front[p]);
}
static void JungleForegroundPillarFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  for(unsigned room=1;room<=3;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/05%02u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(g&&edited);
    assert(ActionVoxel_Build(g,s,NULL,settings));
    unsigned count;ActionVoxel_RearTrims(g,&count);assert(!count);
    ActionVoxel_EdgesForPass(g,kVoxelEdgeRearTrim,true,&count);assert(!count);
    unsigned posts=0,columns=0;int first_x=-1,first_y=-1,column_x=-1,column_y=-1;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;
        assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(id==0x25 || id==0x2e) {
          JungleRearColumnShape(s,g,x,y);++columns;
          if(column_x<0){column_x=(int)x;column_y=(int)y;}
        }
        if(!JunglePillarFixtureId(room,id))continue;
        assert(word&0x2000);JunglePillarShape(g,x,y);++posts;
        if(first_x<0){first_x=(int)x;first_y=(int)y;}
      }
    assert(posts || room==3);assert(columns || room==3);
    unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(g,&edge_count);
    unsigned post_edges=0;
    for(unsigned i=0;i<edge_count;++i) {
      const VoxelEdge *e=&edges[i];assert(!e->rear_trim);
      if(e->far_rank!=1 || !e->high_priority || e->colliding)continue;
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(s,1,e->x/8,e->y/8,&word,&id));
      if(!JunglePillarFixtureId(room,id))continue;
      VoxelEdgeSpan span;
      assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,.2f,&span));
      assert(span.near_depth>=0 && span.far_depth<=.050001f);
      assert(!ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeRearTrim,0,.2f,&span));
      ++post_edges;
    }
    assert(post_edges || !posts);
    if(room==1)for(int x=19;x<=40;x+=(x==19?18:3)) {
      VoxelCellGeometry cap;assert(ActionVoxel_CellGeometry(g,x,26,&cap));
      for(unsigned q=0;q<4;++q)assert(cap.front[q]==0 && fabsf(cap.back[q]-.2f)<.000001f);
      assert(JungleBackingAt(g,x,23));
    }
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(edited,&remapped,NULL,settings));
    SameGeometry(g,edited);assert(!edited->rear_trim_count);
    for(unsigned type=0;type<2;++type) {
      const int x=type?column_x:first_x,y=type?column_y:first_y;if(x<0)continue;
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=x,.y=y,
          .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.012f,.back=.11f};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      VoxelCellGeometry authored;assert(ActionVoxel_CellGeometry(edited,x,y,&authored));
      assert(authored.authored);
      for(unsigned q=0;q<4;++q)assert(fabsf(authored.front[q]-.012f)<.000001f &&
          fabsf(authored.back[q]-.11f)<.000001f);
      assert(!edited->rear_trim_count);
    }
    printf("Marahna05/%02u terrain%u: %u single foreground post cells/%u restrained edges, %u rear vine/base cells; no rear clones, full walking cap and explicit depth preserved\n",
        room,region,posts,post_edges,columns);
    ActionVoxel_Release(g);ActionVoxel_Release(edited);
  }
}
