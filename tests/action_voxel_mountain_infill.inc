/* Two isolated native platforms own their full rock bodies. The four large
 * connected ramp walls stay on one rear plane, including their flat landings.
 * Coordinates here are fixture expectations, never runtime classifiers. */
static bool MountainInfillId(unsigned id) {
  return id==0x0a||id==0x0b||id==0x12||id==0x13;
}
static void MountainInfillCell(const ActionVoxelGeometry *g,const ActionRoomScene *s,
    int cx,int cy,unsigned kind) {
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    if(!FixturePixel(s,cx*16+x,cy*16+y))continue;
    const unsigned at=(cy*16+y-g->origin_y)*g->width+cx*16+x-g->origin_x;
    const unsigned p=g->front[at];
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,cx*2+x/8,cy*2+y/8,&word,NULL));
    assert((p&kVoxelKindMask)==kind);
    assert(!!(p&kVoxelHighPriority)==!!(word&0x2000));
    assert(!(p&kVoxelColliding));
  }
}
static void MountainCliffInfillFixtures(const char *directory) {
  if(!directory)return;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/040%u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(encoded,1,sizeof(encoded),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    unsigned structural=0,rear=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint8_t id;uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(!MountainInfillId(id))continue;
        const bool solid=room==1&&y>=61&&y<=63&&
            ((x>=111&&x<=113)||(x>=126&&x<=129));
        MountainInfillCell(base,s,x,y,solid?kVoxelSolid:kVoxelRearOnly);
        if(solid)++structural;else ++rear;
      }
    assert(structural==(room==1?21u:0u));assert(rear);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(edited,&variant,NULL,settings));
    SameGeometry(base,edited);
    if(room==1) {
      VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(base,112,63,&cell));
      for(unsigned q=0;q<4;++q)assert(cell.front[q]==0&&cell.back[q]==ActionVoxel_Depth(base));
      assert(ActionVoxel_CellGeometry(base,198,42,&cell));
      for(unsigned q=0;q<4;++q)assert(cell.front[q]==ActionVoxel_Depth(base)&&cell.back[q]==ActionVoxel_Depth(base));
      DioramaTileStamp blank={.x=112,.y=60,.blank=true};
      memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){.cells=&blank,.count=1};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      MountainInfillCell(edited,s,112,63,kVoxelRearOnly);
      MountainInfillCell(edited,s,111,63,kVoxelSolid);
      /* Pixel erasure follows the same rule as blanking a cap/stem. */
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
      *p=(DioramaPixelEdit){.by_cell=true,.x=112,.y=60};
      for(unsigned y=0;y<16;++y)p->transparent[y]=0xffff;
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      MountainInfillCell(edited,s,112,63,kVoxelRearOnly);
      MountainInfillCell(edited,s,111,63,kVoxelSolid);
      p->y=62;assert(ActionVoxel_Build(edited,s,&terrain,settings));
      MountainInfillCell(edited,s,112,61,kVoxelSolid);
      MountainInfillCell(edited,s,112,63,kVoxelRearOnly);
      /* A relocated editor-built wall uses the same component rule. One
       * rear root vetoes both columns; a blank cap affects only its column. */
      DioramaTileStamp cluster[6]={0};
      const unsigned ids[6]={0x22,0x23,0x0a,0x0b,0x12,0x13};
      for(unsigned i=0;i<6;++i) {
        cluster[i].x=-2+(int)(i%2);cluster[i].y=-2+(int)(i/2);cluster[i].metatile=ids[i];
        int tx,ty;assert(FixtureCell(s,ids[i],&tx,&ty));
        for(unsigned q=0;q<4;++q)
          assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&cluster[i].words[q],NULL));
      }
      memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){
          .set_bounds=true,.x0=-2,.y0=-2,.x1=0,.y1=1,.cells=cluster,.count=6};
      for(unsigned mode=0;mode<3;++mode) {
        if(mode==1) {
          int tx,ty;assert(FixtureCell(s,0x7b,&tx,&ty));cluster[1].metatile=0x7b;
          for(unsigned q=0;q<4;++q)
            assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&cluster[1].words[q],NULL));
        } else if(mode==2)cluster[1].blank=true;
        assert(ActionVoxel_Build(edited,s,&terrain,settings));
        for(int x=-2;x<0;++x)for(int y=-1;y<=0;++y) {
          assert(ActionVoxel_CellGeometry(edited,x,y,&cell));
          const float expected=mode==0||(mode==2&&x==-2)?0:ActionVoxel_Depth(edited);
          for(unsigned q=0;q<4;++q)assert(cell.front[q]==expected&&cell.back[q]==ActionVoxel_Depth(edited));
        }
      }
      assert(ActionVoxel_Build(edited,s,NULL,settings));SameGeometry(base,edited);
    }
    printf("mountain cliff infill 040%u-%u: %u front body cells, %u rear scenery/ramp cells; source priority, collision, renumbering and edits passed\n",
        room,region,structural,rear);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
