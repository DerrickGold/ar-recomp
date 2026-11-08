/* Include after action_voxel_tomb_caps.inc. The frozen
 * structural hashes below were recorded BEFORE contour skins existed, in
 * surface-polish/castle-caps/fixed-tests.log. They exclude art handles/UVs. */
static unsigned TombContourRole(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  const uint64_t keys[]={UINT64_C(0xa5d6fa6ee48d8a14),UINT64_C(0xf0bf245766861e4e),
      UINT64_C(0xce0cd58ecedd6301),UINT64_C(0xee8b2ff901056847)};
  for(unsigned flip=0;flip<4;++flip)for(unsigned n=0;n<4;++n)
    if(TombCapKey(s,words,collision,flip)==keys[n])return ((n>=2) ^ !!(flip&2))?2:1;
  return 0;
}
static size_t TombContourAt(const ActionVoxelGeometry *g,int x,int y) {
  assert(x>=g->origin_x&&y>=g->origin_y&&x<g->origin_x+g->width&&y<g->origin_y+g->height);
  return (size_t)(y-g->origin_y)*g->width+x-g->origin_x;
}
static void TombContourNativeFront(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,int cx,int cy) {
  uint16_t words[4];unsigned collision;assert(TombCapWords(s,terrain,cx,cy,words,&collision));
  assert(TombContourRole(s,words,collision));
  uint32_t low[256],rear[256],front[256];
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,w=words[q],ink=RockFixtureWordPixel(s,w,x&7,y&7);
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    const uint16_t color=(uint16_t)(s->palette[at]|s->palette[at+1]<<8);
    low[y*16+x]=ink?ActionVoxel_AuthoredColor((uint16_t)(0x8000|color),&frame):0;
    const unsigned actual=g->front[TombContourAt(g,cx*16+x,cy*16+y)];
    if(!ink){assert(!actual);continue;}
    assert((actual&kVoxelKindMask)==(ink==15?kVoxelRearOnly:kVoxelSolid));
    assert(!!(actual&kVoxelColliding)==!!(collision&(1u<<q)));
    assert(!!(actual&kVoxelHighPriority)==!!(w&0x2000));
  }
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned q=(y/8)*2+x/8,ink=RockFixtureWordPixel(s,words[q],x&7,y&7),at=y*16+x;
    assert(rear[at]==low[at]);assert(front[at]==(ink==15?0:low[at]));
  }
}
/* The oracle knows the reviewed native family and exposed contour direction,
 * not the compiler's donor-search implementation or generated table. */
static unsigned TombContourAudit(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool must_cover) {
  unsigned count,covered=0,upper=0,lower=0,risers=0,missing=0;
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];const int cx=(int)floorf(e->x/16.f),cy=(int)floorf(e->y/16.f);
    uint16_t words[4];unsigned collision,role=0;
    if(TombCapWords(s,terrain,cx,cy,words,&collision))role=TombContourRole(s,words,collision);
    const unsigned original_length=e->fold_length?e->fold_length:e->length;
    const bool eligible=role&&ActionVoxel_Profile(g)==kActionVoxelProfile_KassandoraTomb&&
        e->far_rank==2&&(e->side==(role==1?0:2)||((e->side&1)&&original_length<=3));
    if(!eligible){assert(!e->contour_skin);continue;}
    if(!e->contour_skin){missing+=e->length;continue;}
    assert(e->contour_skin==(role==1?kVoxelContourSkinTop:kVoxelContourSkinBottom));
    assert(e->surface_texture&&e->top_sample_size==16&&e->top_owned_art);
    assert(!e->surface_top_material&&!e->soft_surface);
    VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpanAtY(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),e->y,&span));
    assert(span.near_depth==0&&span.far_depth==ActionVoxel_Depth(g));
    uint16_t donor[4];unsigned donor_collision;
    assert(TombCapWords(s,terrain,e->top_sample_x/16,e->top_sample_y/16,donor,&donor_collision));
    assert(TombCapRole(s,donor,donor_collision)==2);
    assert(abs(e->top_sample_x/16-cx)+abs(e->top_sample_y/16-cy)<=8);
    const unsigned q=((e->y-cy*16)/8)*2+(e->x-cx*16)/8;
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned w=donor[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,w,x&7,y&7);
      const unsigned at=(((w>>10)&7)*16+ink)*2;
      assert(ink&&!((words[q]^w)&0x3c00));
      assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
    }
    covered+=e->length;upper+=role==1;lower+=role==2;risers+=!!(e->side&1);
  }
  if(must_cover){assert(!missing);assert(covered);}
  printf("tomb contour skins %02u/%02u: pixels%u missing%u upper%u lower%u risers%u\n",
      s->group,s->map,covered,missing,upper,lower,risers);
  return covered;
}
static bool TombContourFind(const ActionRoomScene *s,uint64_t key,int *x,int *y,uint8_t *id) {
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      uint16_t words[4];unsigned collision;
      if(TombCapWords(s,NULL,cx,cy,words,&collision)&&TombCapKey(s,words,collision,0)==key) {
        assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&words[0],id));*x=cx;*y=cy;return true;
      }
    }
  return false;
}
static void TombContourNativeJoins(const ActionVoxelGeometry *g,unsigned room) {
  /* Recorded native boundaries cross a material cell before surface splitting.
   * The incoming and outgoing pieces must each retain their own contract. */
  const struct {unsigned room;int x,y;unsigned side,skin;bool long_wall;} cases[]={
    {3,255,368,0,0,false},{3,256,368,0,1,false},
    {3,365,335,2,2,false},{3,368,335,2,0,false},
    {3,1327,335,2,0,false},{3,1328,335,2,2,false},
    {4,1311,144,0,0,false},{4,1312,144,0,1,false},
    {4,1487,320,0,1,false},{4,1488,320,0,0,false},
    /* The native wall ends atY272 with near_rank1. Its adjacent two-pixel
     * near_rank0 closing face is a distinct physical interval, not a suffix. */
    {4,1391,264,1,0,true},{4,1391,272,1,2,false}};
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned c=0;c<sizeof(cases)/sizeof(*cases);++c)if(cases[c].room==room) {
    unsigned matches=0;
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];if(e->side!=cases[c].side)continue;
      const bool inside=(e->side&1)?e->x==cases[c].x&&e->y<=cases[c].y&&e->y+e->length>cases[c].y:
          e->y==cases[c].y&&e->x<=cases[c].x&&e->x+e->length>cases[c].x;
      if(!inside)continue;
      assert(e->contour_skin==cases[c].skin);
      if(cases[c].long_wall)assert((e->fold_length?e->fold_length:e->length)>3);
      ++matches;
    }
    assert(matches==1);
  }
}
static void TombContourStampEdits(const ActionRoomScene *s) {
  const uint64_t keys[]={UINT64_C(0xa5d6fa6ee48d8a14),UINT64_C(0xf0bf245766861e4e),
      UINT64_C(0xce0cd58ecedd6301),UINT64_C(0xee8b2ff901056847)};
  static DioramaRoomOverride terrain;static ActionRoomScene reflected;DioramaTileStamp stamps[2];
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  int donor_tx,donor_ty;assert(FixtureCell(s,0x0a,&donor_tx,&donor_ty));
  uint16_t donor_words[4];unsigned unused;assert(TombCapWords(s,NULL,donor_tx/2,donor_ty/2,donor_words,&unused));
  for(unsigned family=0;family<4;++family) {
    int x,y;uint8_t id;if(!TombContourFind(s,keys[family],&x,&y,&id))continue;
    uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
    for(unsigned flip=0;flip<4;++flip) {
      reflected=*s;
      /* Reflect the physical quadrant selector coherently as well as artwork.
       * A random art-only edit must not counterfeit a reviewed fingerprint. */
      for(unsigned q=0;q<4;++q)reflected.bg[0].metatiles[id*8+q*2]=
          (s->bg[0].metatiles[id*8+q*2]&~2u)|((collision>>(q^flip)&1u)<<1);
      const ActionRoomScene *edited_source=&reflected;
      memset(&terrain,0,sizeof(terrain));memset(stamps,0,sizeof(stamps));
      terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-3,.y0=-3,.x1=-1,.y1=-2,
          .cells=stamps,.count=2};
      stamps[0]=(DioramaTileStamp){.x=-3,.y=-3,.metatile=id};
      stamps[1]=(DioramaTileStamp){.x=-2,.y=-3,.metatile=0x0a};
      for(unsigned q=0;q<4;++q) {
        stamps[0].words[q]=words[q^flip]^((flip&1)?0x4000:0)^((flip&2)?0x8000:0);
        stamps[1].words[q]=donor_words[q];
      }
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));
      TombContourNativeFront(edited_source,&terrain,g,-3,-3);assert(TombContourAudit(edited_source,&terrain,g,true));
      const uint64_t reset_geometry=TombCapGeometryHash(g);
      terrain.voxel.cell_count=1;DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=-3,.y=-3,.fields=kDioramaVoxelCell_Width|kDioramaVoxelCell_Front|
          kDioramaVoxelCell_Back|kDioramaVoxelCell_TopArt|kDioramaVoxelCell_BottomArt|kDioramaVoxelCell_SideArt,
          .width=12,.front=.03f,.back=.17f,.top_art={.set=true,.bg=1,.size=16}};
      memcpy(edit->top_art.words,words,sizeof(words));edit->bottom_art=edit->side_art=edit->top_art;
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,-3,-3);assert(a&&a->width==12);
      const unsigned faces[]={1,2,4,5,6};for(unsigned f=0;f<5;++f) {
        const unsigned face=faces[f];assert(!a->surface_sample_size[face]&&!(a->automatic_art&(1u<<face)));
        for(unsigned p=0;p<256;++p) {
          const unsigned w=words[(p/16/8)*2+(p%16/8)],ink=RockFixtureWordPixel(s,w,p%8,p/16%8);
          const unsigned at=(((w>>10)&7)*16+ink)*2;
          assert(a->pixels[face][p]==(ink?(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8):0));
        }
      }
      edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_Front;
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,-3,-3);
      assert(a&&a->faces==kDioramaVoxelFaceMask_Front);
      *edit=(DioramaVoxelCell){.x=-3,.y=-3};
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));assert(TombCapGeometryHash(g)==reset_geometry);
      assert(TombContourAudit(edited_source,&terrain,g,true));
      unsigned erased=256;
      for(unsigned p=0;p<256&&erased==256;++p) {
        const unsigned w=stamps[0].words[(p/16/8)*2+(p%16/8)];
        const unsigned ink=RockFixtureWordPixel(edited_source,w,p%8,p/16%8);
        if(ink&&ink!=15)erased=p;
      }
      assert(erased<256);terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=-3,.y=-3};
      terrain.pixel_layers[0].edits[0].transparent[erased/16]=1u<<(15-erased%16);
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));
      assert(!g->front[TombContourAt(g,-48+(int)(erased%16),-48+(int)(erased/16))]);
      terrain.pixel_layers[0].count=0;assert(ActionVoxel_Build(g,edited_source,&terrain,settings));
      assert(TombCapGeometryHash(g)==reset_geometry);
      /* The sole safe full-square donor is explicitly erased. No replacement
       * skin may retain that source or synthesize opaque ink. */
      terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=-2,.y=-3};
      for(unsigned row=0;row<16;++row)terrain.pixel_layers[0].edits[0].transparent[row]=0xffff;
      assert(ActionVoxel_Build(g,edited_source,&terrain,settings));unsigned n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
      for(unsigned e=0;e<n;++e)assert(!edges[e].contour_skin);
    }
  }
  ActionVoxel_Release(g);
}
static void TombContourSkinFixtures(const char *directory) {
  if(!directory)return;
  /* Rows:03/03,03/04,03/05,03/06,07/04. Columns:US/JP/EU. */
  const unsigned rooms[][2]={{3,3},{3,4},{3,5},{3,6},{7,4}},expected_cells[]={14,44,46,0,0};
  const uint64_t front_hash[5][3]={
    {UINT64_C(0x33eeeeb88659c928),UINT64_C(0x33eeeeb88659c928),UINT64_C(0x33eeeeb88659c928)},
    {UINT64_C(0xf668a74e8b90f89d),UINT64_C(0xa25eb14715d1fbdd),UINT64_C(0xe6fb7a16f309f89d)},
    {UINT64_C(0xca3ad2be97fec3f5),UINT64_C(0xca3ad2be97fec3f5),UINT64_C(0xef62d75b78aafcf5)},
    {UINT64_C(0xbe43e3d73416762c),UINT64_C(0xbe43e3d73416762c),UINT64_C(0xbe43e3d73416762c)},
    {UINT64_C(0x2db26ced5ea435a5),UINT64_C(0x2db26ced5ea435a5),UINT64_C(0x2db26ced5ea435a5)}};
  const uint64_t geometry_hash[5][3]={
    {UINT64_C(0x1586b45548616840),UINT64_C(0x1586b45548616840),UINT64_C(0x1586b45548616840)},
    {UINT64_C(0x830f35705c658ee9),UINT64_C(0x9eb84db186c551f9),UINT64_C(0x830f35705c658ee9)},
    {UINT64_C(0xe7a46c101163fc3f),UINT64_C(0xe7a46c101163fc3f),UINT64_C(0xe7a46c101163fc3f)},
    {UINT64_C(0xc03ebd3cd77268d4),UINT64_C(0xc03ebd3cd77268d4),UINT64_C(0xc03ebd3cd77268d4)},
    {UINT64_C(0xc929a56cb2a740dc),UINT64_C(0xc929a56cb2a740dc),UINT64_C(0xc929a56cb2a740dc)}};
  const uint64_t profile_hash[5][3]={
    {UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325)},
    {UINT64_C(0xbfbf78ed5031aab3),UINT64_C(0x24bb84727a3f933e),UINT64_C(0xbfbf78ed5031aab3)},
    {UINT64_C(0xda593b21d17c14cf),UINT64_C(0xda593b21d17c14cf),UINT64_C(0xda593b21d17c14cf)},
    {UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325)},
    {UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325),UINT64_C(0xcbf29ce484222325)}};
  static ActionSceneSnapshot snapshot;static ActionRoomScene before,renamed;
  static DioramaRoomOverride terrain;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned r=0;r<5;++r) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,rooms[r][0],rooms[r][1]);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;before=*s;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*remapped=ActionVoxel_Create();assert(g&&remapped);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
    assert(ActionVoxel_Build(g,s,NULL,settings));
    assert(ForestBackingHash(g->front,(size_t)g->width*g->height)==front_hash[r][region]);
    assert(TombCapGeometryHash(g)==geometry_hash[r][region]);
    assert(ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches))==profile_hash[r][region]);
    unsigned cells=0;
    for(int y=0;y<(int)ActionRoomScene_TileHeight(s,1)/2;++y)for(int x=0;x<(int)ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
      if(TombContourRole(s,words,collision)){++cells;TombContourNativeFront(s,NULL,g,x,y);}
    }
    assert(cells==expected_cells[r]);const unsigned covered=TombContourAudit(s,NULL,g,cells!=0);
    if(region==0&&s->group==3)TombContourNativeJoins(g,s->map);
    if(!cells)assert(!covered);
    renamed=*s;Renumber(&renamed);assert(ActionVoxel_Build(remapped,&renamed,NULL,settings));
    SameGeometry(g,remapped);assert(!memcmp(g->front,remapped->front,(size_t)g->width*g->height));
    assert(TombContourAudit(&renamed,NULL,remapped,cells!=0)==covered);
    TombCapTerrain(s->group,s->map,region,&terrain);assert(ActionVoxel_Build(g,s,&terrain,settings));
    if(r==4) {
      assert(!TombContourAudit(s,&terrain,g,false));
      for(int y=g->origin_y/16;y<(g->origin_y+g->height)/16;++y)
        for(int x=g->origin_x/16;x<(g->origin_x+g->width)/16;++x) {
          uint16_t words[4];unsigned collision;
          if(TombCapWords(s,&terrain,x,y,words,&collision))assert(!TombContourRole(s,words,collision));
        }
    } else (void)TombContourAudit(s,&terrain,g,cells!=0);
    if(s->group==3&&s->map==4)TombContourStampEdits(s);
    assert(!memcmp(s,&before,sizeof(before)));ActionVoxel_Release(g);ActionVoxel_Release(remapped);
    DioramaLayerOrder_ClearRoom(&terrain);
  }
}
