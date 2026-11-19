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
static bool DesertAuditWallWords(const ActionRoomScene *s,const uint16_t w[4],unsigned collision) {
  static const uint64_t keys[]={
    UINT64_C(0x041cb0549aef3647),UINT64_C(0x07db3e46f55b8d8e),UINT64_C(0x0fade70c301aefa9),
    UINT64_C(0x25c5abc150b458a9),UINT64_C(0x2f95c0d53ad34dda),UINT64_C(0x32359145fb5aa7e6),
    UINT64_C(0x36b3b0276448a3ba),UINT64_C(0x3953809824cffdc6),UINT64_C(0x400873f417835cc9),
    UINT64_C(0x457f3e153dfee10b),UINT64_C(0x481bf7bd0f3808f1),UINT64_C(0x4d56045774d8f1c3),
    UINT64_C(0x6d83890a54052222),UINT64_C(0x6e6735341b56b709),UINT64_C(0x78e9484d626e0def),
    UINT64_C(0x7dded37dcd0dab8f),UINT64_C(0x8d287b8a7b19e41d),UINT64_C(0x9b241fc923ced18f),
    UINT64_C(0xa46706312930cfc2),UINT64_C(0xad261ec63a3356c5),UINT64_C(0xb36b54c1eb17efd5),
    UINT64_C(0xb673c8cfce9d0d77),UINT64_C(0xb8e2fa45ddc64f45),UINT64_C(0xbc75598d7a4d90f3),
    UINT64_C(0xc331773356cd2a72),UINT64_C(0xcf402ceb292013c3),UINT64_C(0xf096195dfda022f9),
    UINT64_C(0xf2057a6c8596dd14)};
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,w,collision,flip);
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(key==keys[i])return true;
  }
  return false;
}
static void DesertAuditWallCell(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    int cx,int cy,const uint16_t words[4],const uint32_t low[256]) {
  uint32_t rear[256],front[256];VoxelCellGeometry shape;
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_CellGeometry(g,cx,cy,&shape));
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned q=0;q<4;++q) {
    bool stone=false;
    assert(!(words[q]&0x2000));
    for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
      const unsigned ink=RockFixtureWordPixel(s,words[q],px,py),x=(q&1)*8+px,y=(q>>1)*8+py,i=y*16+x;
      const unsigned role=g->front[(size_t)(cy*16+y-g->origin_y)*g->width+cx*16+x-g->origin_x];
      stone|=ink&&ink!=15;
      assert(role==(ink?(ink==15?kVoxelRearOnly:kVoxelForeground):0));
      /* Index15 is opaque doorway ink, not a transparency rule. */
      assert(front[i]==(ink!=15?low[i]:0));assert(rear[i]==(ink==15?low[i]:0));
    }
    if(fabsf(shape.front[q]-(stone?.15f:.2f))>=.000001f || fabsf(shape.back[q]-.2f)>=.000001f)
      fprintf(stderr,"wall room%u %d,%d q%u stone%u front%f back%f\n",s->map,cx,cy,q,stone,shape.front[q],shape.back[q]);
    assert(fabsf(shape.front[q]-(stone?.15f:.2f))<.000001f&&fabsf(shape.back[q]-.2f)<.000001f);
    if(stone)assert(shape.front[q]>ActionVoxel_PlayRecess(g)+.05f);
  }
}
static void DesertAuditWallCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned cells=0,apertures=0;
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,cx,cy,words,&collision));
      if(DesertAuditWallWords(s,words,collision)) {
        assert(!collision);++cells;uint32_t low[256];DesertAuditLow(s,cx,cy,low);
        DesertAuditWallCell(s,g,cx,cy,words,low);
      } else if(TombCapKey(s,words,collision,0)==UINT64_C(0x0b024085dfcf0de5)) {
        VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,cx,cy,&shape));++apertures;
        for(unsigned q=0;q<4;++q)assert(shape.front[q]==.2f&&shape.back[q]==.2f);
      }
    }
  assert(cells==(s->map==1?25:445)&&apertures);
}
static void DesertAuditWallEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  VoxelCellGeometry shape,neighbor;assert(ActionVoxel_CellGeometry(g,46,28,&neighbor));
  terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=47,.y=28,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.09f,.back=.19f};
  assert(ActionVoxel_Build(g,s,&terrain,settings));assert(ActionVoxel_CellGeometry(g,47,28,&shape)&&shape.authored);
  for(unsigned q=0;q<4;++q)assert(shape.front[q]==.09f&&shape.back[q]==.19f);
  assert(ActionVoxel_CellGeometry(g,46,28,&shape)&&!memcmp(&shape,&neighbor,sizeof(shape)));
  uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,46,28,words,&collision)&&!collision);
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-1,.y=-1};uint32_t low[256];
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1?0x4000:0)|(flip&2?0x8000:0));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-1,.y0=-1,.x1=0,.y1=0,.cells=&stamp,.count=1};
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned w=stamp.words[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,w,x&7,y&7),at=(((w>>10)&7)*16+ink)*2;
      low[y*16+x]=ink?CastleBackingColor32(s->palette[at]|s->palette[at+1]<<8,&(ActionRoomSceneFrameState){.brightness=15}):0;
    }
    assert(ActionVoxel_Build(g,s,&terrain,settings));DesertAuditWallCell(s,g,-1,-1,stamp.words,low);
  }
}
static unsigned DesertAuditMirroredTops(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g) {
  unsigned count=0,n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=edges+i;
    if(e->side || e->surface_texture!=kVoxelSurfaceTexturePatch || e->top_sample_size!=8 || !e->surface_derived)continue;
    assert(e->top_owned_art);const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    ++count;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const int sx=e->top_sample_x+x%8,sy=e->top_sample_y+(y<8?y:15-y);
      const int cx=(int)floorf(sx/16.f),cy=(int)floorf(sy/16.f);
      uint16_t words[4];unsigned collision;assert(TombCapWords(s,terrain,cx,cy,words,&collision));
      const unsigned px=sx-cx*16,py=sy-cy*16,w=words[(py/8)*2+px/8],ink=RockFixtureWordPixel(s,w,px&7,py&7),at=(((w>>10)&7)*16+ink)*2;
      assert(ink&&art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
      if(y==8)assert(art[8*16+x]==art[7*16+x]);
    }
  }
  return count;
}
static void DesertAuditMirroredTopEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;uint16_t words[4];unsigned collision;
  assert(TombCapWords(s,NULL,46,26,words,&collision));memset(&terrain,0,sizeof(terrain));
  terrain.voxel.cell_count=1;DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=46,.y=26,.fields=kDioramaVoxelCell_TopArt,.top_art={.set=true,.size=8}};
  for(unsigned q=0;q<4;++q)cell->top_art.words[q]=words[0];
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,46,26);assert(a);
  assert(!(a->automatic_art&(1u<<1))&&!a->surface_derived[1]);
  const uint16_t *art=ActionVoxel_AuthoredArt(g,a->art_id[1]);assert(art);
  /* Explicit painted artwork keeps ordinary repetition and its orientation. */
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned w=words[0],ink=RockFixtureWordPixel(s,w,x&7,y&7),at=(((w>>10)&7)*16+ink)*2;
    assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-2,.y=-1};
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^(flip?0x4000:0);
    /* Stamped collision comes from its native metatile provenance. */
    uint16_t provenance;assert(ActionRoomScene_LookupTile(s,1,92,52,&provenance,&stamp.metatile));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-3,.y0=-2,.x1=0,.y1=1,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));assert(DesertAuditMirroredTops(s,&terrain,g));
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
    assert(ActionVoxel_Build(g,s,NULL,settings));DesertAuditDoorCheck(s,g);DesertAuditSandCheck(s,g);DesertAuditWallCheck(s,g);
    const unsigned mirrored=DesertAuditMirroredTops(s,NULL,g);assert(mirrored);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    SameGeometry(g,other);DesertAuditDoorCheck(&remapped,other);DesertAuditSandCheck(&remapped,other);DesertAuditWallCheck(&remapped,other);
    assert(DesertAuditMirroredTops(&remapped,NULL,other)==mirrored);
    assert(ActionVoxel_AnimationIsPresentationOnly(&remapped,&remapped,NULL,NULL,0));
    TombCapTerrain(3,room,region,&terrain);assert(ActionVoxel_Build(other,s,&terrain,settings));
    SameGeometry(g,other);DesertAuditDoorCheck(s,other);DesertAuditSandCheck(s,other);DesertAuditWallCheck(s,other);
    assert(DesertAuditMirroredTops(s,&terrain,other)==mirrored);
    phase=*s;assert(ActionRoomScene_BuildCharacters(s,0,1,phase.characters,sizeof(phase.characters)));
    assert(ActionVoxel_Build(other,&phase,NULL,settings));SameGeometry(g,other);
    DesertAuditSandCheck(&phase,other);assert(ActionVoxel_AnimationIsPresentationOnly(&phase,s,NULL,NULL,0));
    if(!region)DesertAuditAnimationControls(s);
    if(room==1&&!region)DesertAuditDoorEdits(s,other,g,settings);
    if(room==2)DesertAuditWallEdits(s,other,settings);
    if(room==2)DesertAuditMirroredTopEdits(s,other,settings);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);ActionVoxel_Release(other);
    printf("Kassandora 03/%02u terrain %u: portal backing and connected native sand, priority/collision/source preserved\n",room,region);
  }
}
/* Each material-table route must actually publish the reviewed cap policy,
 * including Death Heim's reused cavern art, not merely generate its table. */
static void SurfaceMirroredTopFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static const unsigned rooms[][2]={{3,3},{3,4},{3,5},{4,5},{4,6},{4,7},{5,1},{5,4},{5,5},{5,6},{7,5}};
  static ActionSceneSnapshot snapshot;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned i=0;i<sizeof(rooms)/sizeof(*rooms);++i) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02x%02x.arscene",directory,region,rooms[i][0],rooms[i][1]);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    assert(ActionVoxel_Build(g,s,NULL,ActionVoxel_SettingsForRoom(s->group,s->map)));
    const unsigned caps=DesertAuditMirroredTops(s,NULL,g);assert(caps);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);
    fprintf(stderr,"Mirrored native tops %02x/%02x region%u caps%u: original front strip, rear reflection and seam verified\n",s->group,s->map,region,caps);
  }
}
