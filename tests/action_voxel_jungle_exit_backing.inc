/* Native terminal portals put their right posts in the high BG1 band. The
 * adjacent low tile contains the doorway, and must never become a repeated
 * donor underneath those posts. Runtime uses reviewed material contracts. */
static unsigned JungleExitVerify(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  const int last=s->map==1?127:95,top=s->map==1?22:21,bottom=top+4;
  unsigned targets=0,missing=0;int donor_tx,donor_ty;assert(FixtureCell(s,0x10,&donor_tx,&donor_ty));
  for(int y=top;y<=bottom;++y)for(int x=last-1;x<=last;++x) {
    uint16_t words[4];uint8_t id;
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&words[q],&id));
    if(id!=4 && id!=0x2b && id!=0x33)continue;++targets;
    VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(g,x,y,&cell));
    for(unsigned q=0;q<4;++q) {
      assert(words[q]&0x2000);assert(!(s->bg[0].metatiles[id*8+q*2]&2));
      assert(cell.front[q]==0 && fabsf(cell.back[q]-.05f)<.000001f);
    }
    for(unsigned phase=0;phase<2;++phase) {
      const ActionRoomSceneFrameState frame={.brightness=phase?7:15};uint32_t low[256]={0},rear[256],front[256];
      assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,low,16,rear,front));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned at=py*16+px,role=g->front[(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x];
        assert((role&kVoxelKindMask)==kVoxelForeground && (role&kVoxelHighPriority) && !(role&kVoxelColliding));
        assert(!front[at]);
        const uint32_t expected=ForestNativeColor(s,donor_tx*8+px,donor_ty*8+py,&frame);
        if(!phase)missing+=rear[at]!=expected;
        if(rear[at])assert(rear[at]==expected); /* Complete plain wall, not a second doorway/post. */
      }
    }
  }
  assert(targets==9);
  /* The original aperture is a whole native low source; keep every black and
   * stone pixel at its existing plane. No RGB-based black-to-alpha shortcut. */
  for(int y=top+1;y<=bottom;++y) {
    const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
    ForestRootNative(s,last-2,y,&frame,low);
    for(unsigned q=0;q<4;++q) {
      uint8_t rows[8]={0};ActionVoxel_CutoutRows(g,(last-2)*2+(q&1),y*2+(q>>1),rows);
      if(q&1)for(unsigned py=0;py<8;++py)assert(!rows[py]); /* Native doorway black stays opaque. */
      for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px)
        if(rows[py]&(1u<<(7-px)))low[((q>>1)*8+py)*16+(q&1)*8+px]=0;
    }
    assert(ActionVoxel_Split(g,(last-2)*16,y*16,16,16,16,&frame,low,16,rear,front));
    for(unsigned p=0;p<256;++p)assert(rear[p]==low[p] && front[p]==low[p]);
  }
  printf("Marahna exit %u: %u high posts, missing plain-wall pixels%u; mask%016llx edges%016llx\n",
      s->map,targets,missing,(unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height),
      (unsigned long long)ForestBackingHash(g->edges,g->edge_count*sizeof(*g->edges)));
  return missing;
}
static void JungleExitEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const int cx=s->map==1?127:95,cy=s->map==1?24:23;
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256]={0},rear[256],front[256];
  terrain.voxel.cell_count=1;DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!rear[p]);
  assert(!memcmp(JungleBackingAt(base,cx-1,cy),JungleBackingAt(g,cx-1,cy),sizeof(VoxelRearFill)));
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,(cx-2)*2+(q&1),cy*2+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
    assert(rear[py*16+px]==ForestNativeColor(s,(cx-2)*16+px,cy*16+py,&frame));
  *cell=(DioramaVoxelCell){.x=cx,.y=cy};
  assert(ActionVoxel_Build(g,s,&terrain,settings));SameGeometry(base,g);assert(!JungleExitVerify(s,g));
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=cx,.y=cy};pixel->transparent[5]=1u<<11;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!!rear[p]==(p!=84));
  assert(!memcmp(JungleBackingAt(base,cx-1,cy),JungleBackingAt(g,cx-1,cy),sizeof(VoxelRearFill)));
}
static void JungleExitStamps(const ActionRoomScene *s,ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  const int sx=s->map==1?123:91,sy=s->map==1?21:20;
  static DioramaRoomOverride terrain;DioramaTileStamp stamps[35];
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));memset(stamps,0,sizeof(stamps));
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<5;++x) {
      DioramaTileStamp *stamp=&stamps[y*5+x];stamp->x=(int)x-5;stamp->y=(int)y-7;
      const int wx=sx+(flip?4-x:x);
      for(unsigned q=0;q<4;++q) {
        assert(ActionRoomScene_LookupTile(s,1,wx*2+((q&1)^flip),(sy+y)*2+(q>>1),&stamp->words[q],&stamp->metatile));
        if(flip)stamp->words[q]^=0x4000;
      }
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-5,.y0=-7,.x1=0,.y1=0,.cells=stamps,.count=35};
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    const int target=flip?-5:-1;const VoxelRearFill *fill=JungleBackingAt(g,target,-4);assert(fill);
    uint32_t low[256]={0},rear[256],front[256];const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(g,target*16,-64,16,16,16,&frame,low,16,rear,front));
    int tx,ty;assert(FixtureCell(s,0x10,&tx,&ty));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      assert(!front[y*16+x] && rear[y*16+x]==ForestNativeColor(s,tx*8+(flip?15-x:x),ty*8+y,&frame));
    /* An explicitly opened donor is not a license to borrow the nearby
     * doorway or carved post. All eligible donor cells are edited here. */
    for(unsigned i=0;i<35;++i)if(stamps[i].metatile==0x10) {
      DioramaVoxelCell *a=&terrain.voxel.cells[terrain.voxel.cell_count++];
      *a=(DioramaVoxelCell){.x=stamps[i].x,.y=stamps[i].y,
          .fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
    }
    assert(terrain.voxel.cell_count);
    assert(ActionVoxel_Build(g,s,&terrain,settings));assert(!JungleBackingAt(g,target,-4));
    terrain.voxel.cell_count=0;
    /* A missing lower course or missing complete masonry donor makes the
     * terminal reconstruction ambiguous. Do not fabricate a floating wall. */
    for(unsigned x=0;x<5;++x)stamps[30+x].blank=true;
    assert(ActionVoxel_Build(g,s,&terrain,settings));assert(!JungleBackingAt(g,target,-4));
  }
}
static void JungleExitBackingFixtures(const char *directory) {
  if(!directory)return;bool complete=true;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/050%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;original=*s;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);assert(ActionVoxel_Build(base,s,NULL,settings));
    if(room==3) {
      assert(!base->rear_fill_count);assert(!memcmp(s,&original,sizeof(original)));
      ActionVoxel_Release(base);ActionVoxel_Release(edited);continue;
    }
    const unsigned missing=JungleExitVerify(s,base);complete&=!missing;
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(edited,&remapped,NULL,settings));SameGeometry(base,edited);
    assert(base->rear_fill_count==edited->rear_fill_count && !memcmp(base->rear_fills,edited->rear_fills,base->rear_fill_count*sizeof(VoxelRearFill)));
    if(!missing){JungleExitEdits(s,base,edited,settings);JungleExitStamps(s,edited,settings);}
    assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
  assert(complete);
}
