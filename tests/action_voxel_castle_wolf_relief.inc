/* Optional02/08 native fixtures. Only exact displayed art identifies bodies;
 * raw IDs are deliberately absent from the eligibility oracle. */
static const uint64_t kCastleWolfKeys[]={
  UINT64_C(0x1ddffad0449f71db),
  UINT64_C(0x35b3d8a7cfbd6f0d),
  UINT64_C(0x5c87258714410a46),
  UINT64_C(0x5eff1a806f63bc55),
  UINT64_C(0x664d35727d2cf390),
  UINT64_C(0x6656fdd9550c2bc2),
  UINT64_C(0x8b7bb75ff6a84ec2),
  UINT64_C(0x94bd12feb29ff09e),
  UINT64_C(0x9885170ad29d5990),
  UINT64_C(0x994d58da96c5cd3a),
  UINT64_C(0x9f2d32d7f9055ff6),
  UINT64_C(0xacaec37103d2ef4b),
  UINT64_C(0xc806844e4136eb6d),
  UINT64_C(0xc907acb1775b3ee9),
  UINT64_C(0xe20b0a1df2518d8e),
  UINT64_C(0xe6b5faf69b93f2ce),
  UINT64_C(0xf34d6f7d6555f07a),
  UINT64_C(0xf916b8e105b54d42)
};

static bool CastleWolfWords(const ActionRoomScene *s,const uint16_t w[4],unsigned collision) {
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,w,collision,flip);
    for(unsigned i=0;i<sizeof(kCastleWolfKeys)/sizeof(*kCastleWolfKeys);++i)
      if(key==kCastleWolfKeys[i])return true;
  }
  return false;
}
static unsigned CastleWolfPixel(const ActionVoxelGeometry *g,int x,int y) {
  return g->front[(size_t)(y-g->origin_y)*g->width+x-g->origin_x];
}
static void CastleWolfBody(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,int x,int y) {
  uint16_t words[4];unsigned collision;
  assert(TombCapWords(s,terrain,x,y,words,&collision));assert(!collision);
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,x,y,&shape)&&!shape.authored);
  const float depth=ActionVoxel_Depth(g),actor=ActionVoxel_PlayRecess(g);
  for(unsigned q=0;q<4;++q) {
    assert(!(words[q]&0x2000));
    bool visible=false;
    for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
      const unsigned ink=RockFixtureWordPixel(s,words[q],px,py),at=(((words[q]>>10)&7)*16+ink)*2;
      visible|=ink&&(s->palette[at]||s->palette[at+1]);
    }
    if(!visible) {assert(shape.front[q]==depth&&shape.back[q]==depth);continue;}
    /* Independent user contract: shallow rear-quarter relief, wholly behind
     * the player, attached to the original back plane. Not a new front slab. */
    assert(shape.front[q]>.70f*depth && shape.front[q]<.90f*depth);
    assert(shape.front[q]>actor+.01f && fabsf(shape.back[q]-depth)<.000001f);
    assert(fabsf(ActionVoxel_FrontRecess(g,x*2+(q&1),y*2+(q>>1))-shape.front[q])<.000001f);
  }
  uint32_t low[256],original[256],rear[256],front[256];
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned q=(py/8)*2+px/8,w=words[q],ink=RockFixtureWordPixel(s,w,px&7,py&7);
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    const uint16_t color=s->palette[at]|s->palette[at+1]<<8;
    const unsigned role=CastleWolfPixel(g,x*16+px,y*16+py);
    assert(role==(ink&&color?kVoxelThin:0)); /* Same black-matte silhouette as before. */
    low[py*16+px]=ink?CastleBackingColor32(color,&frame):0;
  }
  memcpy(original,low,sizeof(low));
  assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,low,16,rear,front));
  assert(!memcmp(original,low,sizeof(low)));
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned at=py*16+px,role=CastleWolfPixel(g,x*16+px,y*16+py);
    assert(front[at]==(role?low[at]:0));
    assert(rear[at]==(role?UINT32_C(0xff000000):low[at]));
  }
}
static unsigned CastleWolfCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g) {
  unsigned count=0;
  for(int y=g->origin_y/16;y<(g->origin_y+g->height)/16;++y)
    for(int x=g->origin_x/16;x<(g->origin_x+g->width)/16;++x) {
      uint16_t w[4];unsigned collision;
      if(TombCapWords(s,terrain,x,y,w,&collision)&&CastleWolfWords(s,w,collision)) {
        CastleWolfBody(s,terrain,g,x,y);++count;
      }
    }
  return count;
}
static void CastleWolfCapJoin(const ActionVoxelGeometry *g,int x,int y) {
  VoxelCellGeometry cap,body;
  assert(ActionVoxel_CellGeometry(g,x,y,&cap)&&ActionVoxel_CellGeometry(g,x,y+1,&body));
  for(unsigned q=0;q<4;++q)assert(cap.front[q]==0&&fabsf(cap.back[q]-ActionVoxel_Depth(g))<.000001f);
  unsigned n,joins=0,tops=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(int px=x*16;px<(x+1)*16;++px) {
    if((CastleWolfPixel(g,px,(y+1)*16)&kVoxelKindMask)!=kVoxelThin)continue;
    bool found=false;
    for(unsigned i=0;i<n;++i) {
      const VoxelEdge *e=&edges[i];
      if(e->side!=2||e->y!=(y+1)*16-1||px<e->x||px>=e->x+e->length)continue;
      VoxelEdgeSpan span={0};assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
      assert(span.near_depth==0&&fabsf(span.far_depth-body.front[(px-x*16)/8])<.000001f);
      assert(!e->high_priority);found=true;
    }
    assert(found);++joins;
  }
  for(unsigned i=0;i<n;++i)if(edges[i].side==0&&edges[i].y==y*16&&edges[i].x>=x*16&&edges[i].x<(x+1)*16) {
    VoxelEdgeSpan span={0};assert(ActionVoxel_ResolveEdgeSpan(g,&edges[i],kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
    /* A middle capital has another wolf above its rear portion. The same
     * complete cap stays0..Depth; only the actually exposed top ends at that
     * body's front. It still covers the entire native walking plane. */
    float visible_back=ActionVoxel_Depth(g);
    if((CastleWolfPixel(g,edges[i].x,y*16-1)&kVoxelKindMask)==kVoxelThin)
      visible_back=ActionVoxel_FrontRecess(g,edges[i].x/8,(y*16-1)/8);
    assert(span.near_depth==0&&fabsf(span.far_depth-visible_back)<.000001f);
    assert(span.far_depth>ActionVoxel_PlayRecess(g)+.01f);
    assert(edges[i].colliding&&!edges[i].high_priority);++tops;
  }
  assert(joins&&tops);
}
static void CastleWolfEdits(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.voxel.cell_count=1;DioramaVoxelCell *c=&terrain.voxel.cells[0];
  *c=(DioramaVoxelCell){.x=3,.y=6,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,
      .front=.04f,.back=.16f,.width=11};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));VoxelCellGeometry a,b;
  assert(ActionVoxel_CellGeometry(edited,3,6,&a)&&a.authored&&a.width==11);
  for(unsigned q=0;q<4;++q)assert(a.front[q]==.04f&&a.back[q]==.16f);
  assert(ActionVoxel_CellGeometry(g,4,6,&a)&&ActionVoxel_CellGeometry(edited,4,6,&b)&&!memcmp(&a,&b,sizeof(a)));
  assert(ActionVoxel_CellGeometry(g,3,5,&a)&&ActionVoxel_CellGeometry(edited,3,5,&b)&&!memcmp(&a,&b,sizeof(a)));
  *c=(DioramaVoxelCell){.x=3,.y=6,.fields=kDioramaVoxelCell_Faces,
      .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const VoxelAuthoredCell *owned=ActionVoxel_AuthoredCellAt(edited,3,6);assert(owned&&owned->native_front_hidden&&!owned->surface);
  c->faces=kDioramaVoxelFaceMask_All;assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(g,edited);
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=3,.y=6};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(g,edited);
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-2,.y=-3};
    for(unsigned q=0;q<4;++q) {
      assert(ActionRoomScene_LookupTile(s,1,6+((q^flip)&1),12+((q^flip)>>1),&stamp.words[q],&stamp.metatile));
      stamp.words[q]^=(flip&1?0x4000:0)|(flip&2?0x8000:0);
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(CastleWolfCheck(s,&terrain,edited)==1);
  }
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];*pixel=(DioramaPixelEdit){.by_cell=true,.x=3,.y=6};
  pixel->transparent[0]=0x8000;assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(!CastleWolfPixel(edited,48,96));
  assert(ActionVoxel_CellGeometry(g,4,6,&a)&&ActionVoxel_CellGeometry(edited,4,6,&b)&&!memcmp(&a,&b,sizeof(a)));
  memset(pixel->transparent,0,sizeof(pixel->transparent));assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(g,edited);
}
static void CastleWolfReliefFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  static ActionRoomScene original,renumbered,without;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0208.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));const ActionRoomScene *s=&snapshot.scene;original=*s;
    ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,8);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*r=ActionVoxel_Create();assert(g&&r);
    assert(ActionVoxel_Build(g,s,NULL,settings));assert(CastleWolfCheck(s,NULL,g)==30);
    const int caps[][2]={{3,5},{4,5},{11,5},{12,5},{3,8},{4,8},{11,8},{12,8},{1,11},{2,11},{13,11},{14,11}};
    for(unsigned i=0;i<sizeof(caps)/sizeof(*caps);++i)CastleWolfCapJoin(g,caps[i][0],caps[i][1]);
    /* Removing only mapped body placements disables the optional policy;
     * every remaining modeled cell retains its old front/back contract. */
    without=*s;unsigned removed=0;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,w,&collision));
      if(CastleWolfWords(s,w,collision)){without.bg[0].map[y*16+x]=0;++removed;}
    }
    assert(removed==30&&ActionVoxel_Build(r,&without,NULL,settings));
    assert(ActionVoxel_SceneryRecess(r)==0);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,w,&collision));
      if(CastleWolfWords(s,w,collision))continue;
      VoxelCellGeometry a,b;assert(ActionVoxel_CellGeometry(g,x,y,&a)&&ActionVoxel_CellGeometry(r,x,y,&b));
      assert(!memcmp(&a,&b,sizeof(a)));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        assert(CastleWolfPixel(g,x*16+px,y*16+py)==CastleWolfPixel(r,x*16+px,y*16+py));
    }
    renumbered=*s;Renumber(&renumbered);assert(ActionVoxel_Build(r,&renumbered,NULL,settings));
    assert(CastleWolfCheck(&renumbered,NULL,r)==30);SameGeometry(g,r);
    CastleWolfEdits(s,g,r,settings);
    TombCapTerrain(2,8,region,&terrain);assert(ActionVoxel_Build(r,s,&terrain,settings));
    assert(CastleWolfCheck(s,&terrain,r)==30);
    settings.depth=.16f;settings.play_recess=.06f;assert(ActionVoxel_Build(r,s,NULL,settings));assert(CastleWolfCheck(s,NULL,r)==30);
    assert(!memcmp(&original,s,sizeof(original)));ActionVoxel_Release(g);ActionVoxel_Release(r);
    printf("Castle wolf region%u:30 shallow rear bodies,12 unchanged jump caps, native art/collision/priority, joins, remap and editor guards PASS\n",region);
  }
  /* The DeathHeim copy already uses rear-mounted statues. Its separate
   * reviewed material/black-matte contract must remain untouched. */
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0703.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g&&ActionVoxel_Build(g,s,NULL,ActionVoxel_SettingsForRoom(7,3)));
    unsigned count=0;
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
      uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,w,&collision));
      if(!CastleWolfWords(s,w,collision))continue;
      ++count;assert(!collision);VoxelCellGeometry a;assert(ActionVoxel_CellGeometry(g,x,y,&a));
      for(unsigned q=0;q<4;++q) {
        assert(!(w[q]&0x2000));bool visible=false;
        for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
          const unsigned ink=RockFixtureWordPixel(s,w[q],px,py);
          visible|=ink>1;
          const unsigned role=CastleWolfPixel(g,x*16+(q&1)*8+px,y*16+(q>>1)*8+py);
          assert(role==(ink?(ink==1?kVoxelRearOnly:kVoxelForeground)|kVoxelRearCopy:0));
        }
        /* Mixed black RearOnly corners can make the aggregate cell summary
         * report its back plane; native colored face placement uses this
         * exact per-quarter recess accessor in the renderer. */
        if(visible)assert(fabsf(ActionVoxel_FrontRecess(g,x*2+(q&1),y*2+(q>>1))-.18f)<.000001f);
        assert(fabsf(a.back[q]-.24f)<.000001f);
      }
    }
    assert(count==30);ActionVoxel_Release(g);
    printf("DeathHeim wolf region%u:30 already-correct rear bodies preserved\n",region);
  }
}
