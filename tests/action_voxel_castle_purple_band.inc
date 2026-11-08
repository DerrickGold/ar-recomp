/* Staged level-only candidate. Native IDs locate independent specimens;
 * production follows exact displayed-art/collision fingerprints. Include after
 * action_voxel_rock_surfaces.inc for RockFixtureWordPixel and shared fixtures. */
static void PurpleBandArt(const ActionRoomScene *s,const uint16_t *pixels,
    const uint16_t words[4],unsigned unused) {
  (void)unused;assert(pixels);
  /* Independent explicit source trace: each of the five native rows occurs
   * in one contiguous interval, with the front rim appearing only first. */
  static const unsigned source_y[16]={0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4};
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned w=words[x/8],ink=RockFixtureWordPixel(s,w,x&7,source_y[y]);
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    assert(ink && pixels[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
}
static void PurpleBandExplicit8(const ActionRoomScene *s,const uint16_t *pixels,const uint16_t words[4]) {
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned w=words[1],ink=RockFixtureWordPixel(s,w,x%8,y%8),at=(((w>>10)&7)*16+ink)*2;
    assert(pixels[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
}
static const uint16_t *PurpleBandPixels(const ActionVoxelGeometry *g,int x,int y) {
  unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)if(!e[i].side && e[i].x==x*16 && e[i].y==y*16 && e[i].surface_derived) {
    assert(e[i].top_owned_art);return ActionVoxel_AuthoredArt(g,e[i].top_owned_art-1);
  }
  return NULL;
}
static void PurpleBandCell(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    int x,int y,bool automatic) {
  uint16_t words[4];
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&words[q],NULL));
  VoxelCellGeometry c;assert(ActionVoxel_CellGeometry(g,x,y,&c));
  assert(!c.authored && c.width==16 && c.offset_x==0);
  bool visible[4]={0};
  for(unsigned q=0;q<4;++q) {
    for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
      const unsigned ink=RockFixtureWordPixel(s,words[q],px,py),at=(((words[q]>>10)&7)*16+ink)*2;
      visible[q]|=ink && (s->palette[at]||s->palette[at+1]);
    }
    /* Native black bracket void quadrants stay flat/absent. Solid material
     * metadata alone must not turn that unchanged void into a block. */
    assert(c.front[q]==(visible[q]?0:ActionVoxel_Depth(g)) && c.back[q]==ActionVoxel_Depth(g));
  }
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned q=(py/8)*2+px/8,at=(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x;
    const unsigned role=g->front[at];
    if(!visible[q]){assert(!role);continue;}
    assert((role&kVoxelKindMask)==kVoxelSolid);
    assert(!!(role&kVoxelColliding)==(q<2));
    assert(!!(role&kVoxelHighPriority)==!!(words[q]&0x2000));
  }
  unsigned count,covered=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side || e->far_rank!=2 || e->y!=y*16 || e->x<x*16 || e->x>=(x+1)*16)continue;
    assert(e->colliding && !e->high_priority);
    VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
    assert(span.near_depth==0 && span.far_depth==ActionVoxel_Depth(g));
    if(automatic) {
      assert(e->surface_texture && e->surface_derived && e->top_owned_art && e->top_sample_size==16);
      assert(e->length<=8 && e->top_sample_x==x*16 && e->top_sample_y==y*16);
      PurpleBandArt(s,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),words,0);
    } else assert(!e->surface_texture && !e->top_owned_art && !e->top_sample_size);
    covered+=e->length;
  }
  assert(covered==16);
}
static void PurpleBandEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,
    const ActionVoxelGeometry *base,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;
  const int x=16,y=8;VoxelCellGeometry neighbor,after;
  assert(ActionVoxel_CellGeometry(base,x+1,y,&neighbor));
  uint16_t words[4];uint8_t native_id=0;
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&words[q],&native_id));
  /* Upper-band source edits fail closed for that whole owning cell; an
   * unrelated lower-bracket erase cannot remove the valid upper stone band. */
  for(unsigned which=0;which<3;++which) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
    *p=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};
    if(which==1)p->black[3]=0x1000;else p->transparent[which==2?7:3]=0x1000;
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    const uint16_t *art=PurpleBandPixels(g,x,y);
    if(which==2){assert(art);PurpleBandArt(s,art,words,0);}else assert(!art);
    assert(ActionVoxel_CellGeometry(g,x+1,y,&after) && !memcmp(&neighbor,&after,sizeof(after)));
    assert(PurpleBandPixels(g,x+1,y));
  }
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Width|
      kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.width=12,.front=.03f,.back=.17f};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && a->surface && a->surface_sample_size[1]==16 && a->surface_derived[1]);
  PurpleBandArt(s,a->pixels[1],words,0);
  assert(ActionVoxel_CellGeometry(g,x,y,&after) && after.width==12);
  for(unsigned q=0;q<4;++q)assert(after.front[q]==.03f && after.back[q]==.17f);
  cell->fields|=kDioramaVoxelCell_TopArt;
  cell->top_art=(DioramaVoxelArt){.set=true,.size=8,.x=8,.y=0};
  memcpy(cell->top_art.words,words,sizeof(words));
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && !a->surface_sample_size[1] && !a->surface_derived[1] && !(a->automatic_art&(1u<<1)));
  PurpleBandExplicit8(s,a->pixels[1],words);
  cell->fields|=kDioramaVoxelCell_Faces;cell->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && !(a->faces&kDioramaVoxelFaceMask_Top));
  assert(ActionVoxel_CellGeometry(g,x+1,y,&after) && !memcmp(&neighbor,&after,sizeof(after)));
  /* Last covering reset restores native geometry and source-top policy. */
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(g,s,&terrain,settings));SameGeometry(base,g);
  /* Negative translated/H-mirrored stamps retain source width phase. V-reflection
   * intentionally does not acquire the directional upper-band contract. */
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-2,.y=-3,.metatile=native_id};
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1?0x4000:0)|(flip&2?0x8000:0));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,
        .x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    unsigned count,covered=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];if(e->side || e->far_rank!=2 || e->y!=-48)continue;
      if(!(flip&2)) {
        assert(e->surface_texture && e->surface_derived && e->top_owned_art && e->top_sample_size==16);
        assert(e->top_sample_x==-32 && e->top_sample_y==-48);
        PurpleBandArt(s,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),stamp.words,0);
        covered+=e->length;
      } else assert(!e->surface_derived);
    }
    if(!(flip&2))assert(covered==16);
  }
}
static void CastlePurpleBandFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,variant;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0204.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));original=snapshot.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,4);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*g=ActionVoxel_Create();assert(base&&g);
    assert(ActionVoxel_Build(base,&snapshot.scene,NULL,settings));
    uint64_t front_hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<(size_t)base->width*base->height;++i)front_hash=(front_hash^base->front[i])*UINT64_C(1099511628211);
    printf("purple-band native-front region=%u hash=%016llx\n",region,(unsigned long long)front_hash);fflush(stdout);
    for(unsigned remap=0;remap<2;++remap) {
      variant=snapshot.scene;if(remap)Renumber(&variant);
      assert(ActionVoxel_Build(g,&variant,NULL,settings));SameGeometry(base,g);
      assert(!memcmp(base->front,g->front,(size_t)base->width*base->height));
      unsigned ends=0,middles=0;
      for(unsigned y=0;y<ActionRoomScene_TileHeight(&snapshot.scene,1)/2;++y)
        for(unsigned x=0;x<ActionRoomScene_TileWidth(&snapshot.scene,1)/2;++x) {
          uint16_t w;uint8_t id;assert(ActionRoomScene_LookupTile(&snapshot.scene,1,x*2,y*2,&w,&id));
          if(id!=0x65&&id!=0x66&&id!=0x7f)continue;
          PurpleBandCell(&variant,g,x,y,true);
          if(id==0x7f)++middles;else ++ends;
        }
      assert(ends==30 && middles==25);
      unsigned joins=0;
      for(unsigned y=0;y<ActionRoomScene_TileHeight(&variant,1)/2;++y)
        for(unsigned x=0;x+1<ActionRoomScene_TileWidth(&variant,1)/2;++x) {
          const uint16_t *left=PurpleBandPixels(g,x,y),*right=PurpleBandPixels(g,x+1,y);
          if(!left || !right)continue;
          for(unsigned row=0;row<16;++row)assert(left[row*16+15]==right[row*16]);
          ++joins;
        }
      assert(joins==40);
    }
    PurpleBandEdits(&snapshot.scene,g,base,settings);
    /* Palette changes recolor owned native ink without changing contracts. */
    variant=snapshot.scene;variant.palette[(7*16+8)*2]^=1;
    assert(ActionVoxel_Build(g,&variant,NULL,settings));SameGeometry(base,g);
    PurpleBandCell(&variant,g,16,8,true);
    assert(!memcmp(&original,&snapshot.scene,sizeof(original)));
    printf("Castle02/04 terrain%u: 55purple pieces retain own five-row band once,40nativejoins exact; geometry/collision/remap/masks/explicitart/negative stamps checked\n",region);
    ActionVoxel_Release(base);ActionVoxel_Release(g);
  }
}
