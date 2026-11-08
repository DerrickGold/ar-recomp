/* Fixture addresses identify the reviewed bone family for the oracle only.
 * Runtime material keys use artwork, collision and priority, not these IDs. */
static bool DesertSkeletonMaterial(unsigned id) {
  static const uint8_t ids[]={0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x70,0x71,0x72,0x73,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7a,0x7c,0x7d,0x7e,0x7f,0x85,0x87};
  for(unsigned i=0;i<sizeof(ids);++i)if(id==ids[i])return true;
  return false;
}
static unsigned DesertSkeletonBody(const ActionVoxelGeometry *g,const ActionRoomScene *source) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  unsigned cells=0,body_pixels=0,cap_pixels=0,edges_count,walking_edges=0;
  const float contact=fmaxf(0,ActionVoxel_PlayRecess(g)-.01f);
  const float back=contact+ActionVoxel_TrimDepth(g);
  const VoxelEdge *edges=ActionVoxel_Edges(g,&edges_count);
  for(unsigned ty=0;ty<ActionRoomScene_TileHeight(source,1);ty+=2)
    for(unsigned tx=0;tx<ActionRoomScene_TileWidth(source,1);tx+=2) {
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(source,1,tx,ty,&word,&id));
      if(!DesertSkeletonMaterial(id))continue;
      ++cells;
      VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,tx/2,ty/2,&shape));
      uint32_t low[256],rear[256],front[256];DesertNativeLow(source,tx,ty,low);
      unsigned collision=0,bone_quadrants=0;
      for(unsigned q=0;q<4;++q) {
        const unsigned raw=(source->bg[0].metatiles[id*8+q*2]<<8)|source->bg[0].metatiles[id*8+q*2+1];
        const bool solid=(raw&0x200)!=0;collision|=(unsigned)solid<<q;
        assert(ActionRoomScene_LookupTile(source,1,tx+(q&1),ty+(q>>1),&word,NULL));
        /* All reviewed bones are native-low. High-priority artwork cannot
         * match this family because priority is part of its material key. */
        assert(!(word&0x2000));
        /* Fixture palette5 is native bone; palette4 includes the sand at
         * lower jaw/vertebra joins. The runtime role is the reviewed art key. */
        const bool bone=((word>>10)&7)==5;
        bone_quadrants|=(unsigned)bone<<q;
        uint8_t cutout[8]={0};ActionVoxel_CutoutRows(g,tx+(q&1),ty+(q>>1),cutout);
        bool visible=false;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
          const unsigned at=((q>>1)*8+y)*16+(q&1)*8+x;
          if(cutout[y]&(1u<<(7-x)))low[at]=0;
          visible|=low[at]!=0;
        }
        if(visible) {
          assert(shape.front[q]==(bone?contact:0));
          assert(shape.back[q]==(bone?back:ActionVoxel_Depth(g)));
          assert(ActionVoxel_FrontRecess(g,tx+(q&1),ty+(q>>1))==shape.front[q]);
          if(bone)assert(shape.back[q]-shape.front[q]<ActionVoxel_Depth(g)*.3f);
        }
      }
      assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=y*16+x,q=(y/8)*2+x/8;const bool solid=(collision&(1u<<q))!=0;
        const bool bone=(bone_quadrants&(1u<<q))!=0;
        /* Every native bone pixel has exactly one front owner on the shared
         * plane. No far-back duplicate can reintroduce the chopped skull. */
        assert(front[at]==low[at]);
        assert(rear[at]==(bone?0:low[at]));
        if(low[at]){if(solid)++cap_pixels;else ++body_pixels;}
      }
    }
  for(unsigned i=0;i<edges_count;++i) {
    const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&id));
    if(!DesertSkeletonMaterial(id))continue;
    const unsigned q=(e->y/8%2)*2+e->x/8%2;
    const unsigned raw=(source->bg[0].metatiles[id*8+q*2]<<8)|source->bg[0].metatiles[id*8+q*2+1];
    assert(e->colliding==!!(raw&0x200) && !e->high_priority);
    if(((word>>10)&7)!=5)continue;
    assert(e->far_rank==1);
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
    assert(span.near_depth>=contact && span.far_depth<=back);
    if(e->side==0 && e->colliding) {
      /* Actual native landing contours include the unchanged actor contact,
       * without a full-path slab hiding the skull/vertebra artwork below. */
      assert(span.near_depth==contact);++walking_edges;
    }
  }
  assert(cells && body_pixels && cap_pixels && walking_edges);return cells;
}
static void DesertSkeletonFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,renumbered;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0301.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    memcpy(&original,&snapshot.scene,sizeof(original));
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,1);
    assert(ActionVoxel_Build(g,&snapshot.scene,NULL,settings));
    assert(!memcmp(&original,&snapshot.scene,sizeof(original)));
    const unsigned cells=DesertSkeletonBody(g,&original);
    renumbered=original;Renumber(&renumbered);
    assert(ActionVoxel_Build(g,&renumbered,NULL,settings));
    assert(DesertSkeletonBody(g,&original)==cells);
    /* Classification is independent of native palette fades. */
    renumbered=original;memset(renumbered.palette,0,sizeof(renumbered.palette));
    assert(ActionVoxel_Build(g,&renumbered,NULL,settings));
    assert(DesertSkeletonBody(g,&original)==cells);
    /* An authored depth edit takes precedence over the automatic attachment
     * and resetting it restores this same coherent native silhouette. */
    static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
    terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=53,.y=34,
        .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.12f,.back=.18f};
    assert(ActionVoxel_Build(g,&original,&terrain,settings));
    VoxelCellGeometry authored;assert(ActionVoxel_CellGeometry(g,53,34,&authored) && authored.authored);
    for(unsigned q=0;q<4;++q)assert(authored.front[q]==.12f && authored.back[q]==.18f);
    terrain.voxel.cell_count=0;assert(ActionVoxel_Build(g,&original,&terrain,settings));
    assert(DesertSkeletonBody(g,&original)==cells);
    ActionVoxel_Release(g);
    printf("desert skeleton terrain %u: %u cells, complete single-owner shallow silhouette, native landing/priority, remapping, fade and authored override passed\n",region,cells);
  }
}
