/* Native source witnesses for the audit's desert doorway and dune joins.
 * Include after TombCap/Sand helpers. Coordinates and artwork fingerprints
 * locate fixture evidence; production uses the reviewed material contracts. */
static bool DesertAuditDoorKey(uint64_t key) {
  static const uint64_t keys[]={UINT64_C(0x57a465fb680624c6),UINT64_C(0xc2fc3eadffb06f3d),
    UINT64_C(0x4d7babb3d8036ef2),UINT64_C(0xd542512b76cb6dc9)};
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(key==keys[i])return true;
  return false;
}
static bool DesertAuditPartialSandKey(uint64_t key) {
  return key==UINT64_C(0x403623135f35ee81) || key==UINT64_C(0xb60dce56063da0b8) ||
      key==UINT64_C(0xc26e929980578e99) || key==UINT64_C(0xf9615dd4fc968495);
}
static void DesertAuditLow(const ActionRoomScene *s,int cx,int cy,uint32_t low[256]) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    uint16_t w;assert(ActionRoomScene_LookupTile(s,1,cx*2+x/8,cy*2+y/8,&w,NULL));
    low[y*16+x]=(w&0x2000)?0:ForestNativeColor(s,cx*16+x,cy*16+y,&frame);
  }
}
static void DesertAuditDoorCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned expected=0;const ActionRoomSceneFrameState frame={.brightness=15};
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,cx,cy,w,&collision));
      bool portal=false;
      for(unsigned flip=0;flip<4;++flip)portal|=DesertAuditDoorKey(TombCapKey(s,w,collision,flip));
      const VoxelRearFill *fill=JungleBackingAt(g,cx,cy);
      if(!portal){assert(!fill);continue;}
      ++expected;assert(fill&&fill->preserve_existing&&!fill->replace_front);
      uint32_t low[256],rear[256],front[256];DesertAuditLow(s,cx,cy,low);
      assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned word=w[(y/8)*2+x/8];
        const bool opaque=!!(word&0x2000)&&RockFixtureWordPixel(s,word,x&7,y&7);
        assert(fill->colors[y*16+x]==(opaque?0x8000:0));
        assert(rear[y*16+x]==(opaque?UINT32_C(0xff000000):low[y*16+x]));
      }
    }
  assert(expected==(s->map==1?10u:0u)&&g->rear_fill_count==expected);
}
static void DesertAuditSandCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned n,partial=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=edges+i;
    /* Before the fix, room 02 has 54 internal 4–8px risers per region. Real
     * native sand silhouette steps remain 1–2px and keep their live artwork. */
    assert(!(e->soft_surface&&(e->side&1)&&e->length>=4&&e->near_rank==1&&e->far_rank==2));
  }
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,cx,cy,w,&collision));
      if(!DesertAuditPartialSandKey(TombCapKey(s,w,collision,0)))continue;
      assert(collision&&collision!=15);++partial;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned q=(y/8)*2+x/8,word=w[q];
        const unsigned p=g->front[(cy*16+y-g->origin_y)*g->width+cx*16+x-g->origin_x];
        if(!RockFixtureWordPixel(s,word,x&7,y&7)){assert(!p);continue;}
        assert((p&kVoxelKindMask)==kVoxelSolid);
        assert(!!(p&kVoxelHighPriority)==!!(word&0x2000));
        assert(!!(p&kVoxelColliding)==!!(collision&(1u<<q)));
      }
    }
  assert(s->map==1?!partial:partial>0);
}
static void DesertAuditDoorEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,
    const ActionVoxelGeometry *base,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256];DesertAuditLow(s,254,33,low);
  terrain.voxel.cell_count=1;
  terrain.voxel.cells[0]=(DioramaVoxelCell){.x=254,.y=33,
      .fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,4064,528,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert(!rear[i]);
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=254,.y=33};
  terrain.pixel_layers[0].edits[0].transparent[5]=0x0800;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,4064,528,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert(rear[i]==(i==5*16+4?0:UINT32_C(0xff000000)));
  memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(g,s,&terrain,settings));
  SameGeometry(g,base);DesertAuditDoorCheck(s,g);
  /* Reflected/moved editor art retains the contract independently of IDs or
   * room coordinates, with no filler beyond its actual native silhouette. */
  for(unsigned flip=0;flip<4;++flip) {
    DioramaTileStamp stamp={.x=-1,.y=-1};
    for(unsigned q=0;q<4;++q) {
      unsigned source=q^flip;
      assert(ActionRoomScene_LookupTile(s,1,508+(source&1),66+(source>>1),&stamp.words[q],&stamp.metatile));
      stamp.words[q]^=(flip&1?0x4000:0)|(flip&2?0x8000:0);
    }
    memset(&terrain,0,sizeof(terrain));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-1,.y0=-1,
      .x1=0,.y1=0,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));assert(g->rear_fill_count==1);
    assert(ActionVoxel_Split(g,-16,-16,16,16,16,&frame,NULL,0,rear,front));
    for(unsigned i=0;i<256;++i)assert(rear[i]==UINT32_C(0xff000000));
  }
}
static void DesertAuditAnimationControls(const ActionRoomScene *s) {
  static ActionRoomScene reference,variant;static DioramaRoomOverride terrain;
  size_t offset,bytes;
  assert(ActionVoxel_AnimationMatches(s,s->characters,&offset,&bytes));
  assert(ActionRoomScene_CharacterAnimationPhaseCount(s)>1);
  assert(ActionVoxel_AnimationIsPresentationOnly(s,s,NULL,NULL,0));
  /* The new terrain-body flag must not widen reuse to arbitrary artwork,
   * changing opacity in a future phase, or authored baked sand donors. */
  variant=*s;variant.characters[offset]^=0xff;
  assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
  int sample_x=-1,sample_y=-1;unsigned sample_word=0;
  for(int cy=0;sample_x<0&&cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;sample_x<0&&cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      uint16_t words[4];if(!SandWords(s,cx*16,cy*16,words))continue;
      for(unsigned q=0;q<4;++q) {
        const size_t at=(words[q]&1023)*32;
        if(at<offset||at>=offset+bytes)continue;
        unsigned opaque=0;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
          opaque+=RockFixtureWordPixel(s,words[q],x,y)!=0;
        if(opaque){sample_x=cx;sample_y=cy;sample_word=words[q];break;}
      }
    }
  assert(sample_x>=0);
  reference=*s;
  memset(reference.characters+bytes+(sample_word&1023)*32,0,32);
  assert(ActionVoxel_AnimationMatches(&reference,s->characters,&offset,&bytes));
  assert(!ActionVoxel_AnimationIsPresentationOnly(s,&reference,NULL,NULL,0));
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  terrain.voxel.cells[0]=(DioramaVoxelCell){.x=sample_x,.y=sample_y,
      .fields=kDioramaVoxelCell_TopArt,.top_art={.set=true,.bg=1,.size=8}};
  for(unsigned q=0;q<4;++q)terrain.voxel.cells[0].top_art.words[q]=sample_word;
  assert(!ActionVoxel_AnimationIsPresentationOnly(s,s,&terrain,NULL,0));
}
static void DesertAuditJoinFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped,phase;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=2;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/030%u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;const uint64_t source=ForestBackingHash(s,sizeof(*s));
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    assert(ActionVoxel_Build(g,s,NULL,settings));DesertAuditDoorCheck(s,g);DesertAuditSandCheck(s,g);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    SameGeometry(g,other);DesertAuditDoorCheck(&remapped,other);DesertAuditSandCheck(&remapped,other);
    assert(ActionVoxel_AnimationIsPresentationOnly(&remapped,&remapped,NULL,NULL,0));
    TombCapTerrain(3,room,region,&terrain);assert(ActionVoxel_Build(other,s,&terrain,settings));
    SameGeometry(g,other);DesertAuditDoorCheck(s,other);DesertAuditSandCheck(s,other);
    phase=*s;assert(ActionRoomScene_BuildCharacters(s,0,1,phase.characters,sizeof(phase.characters)));
    assert(ActionVoxel_Build(other,&phase,NULL,settings));SameGeometry(g,other);
    DesertAuditSandCheck(&phase,other);assert(ActionVoxel_AnimationIsPresentationOnly(&phase,s,NULL,NULL,0));
    if(!region)DesertAuditAnimationControls(s);
    if(room==1&&!region)DesertAuditDoorEdits(s,other,g,settings);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);ActionVoxel_Release(other);
    printf("Kassandora 03/%02u terrain %u: portal backing and connected native sand, priority/collision/source preserved\n",room,region);
  }
}
