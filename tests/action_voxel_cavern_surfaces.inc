/* Staged Cavern native boundary-material fixture; no driver registration yet.
 * Reuses established snapshot/source/hash helpers in action_voxel_test.c.
 * Material identity is displayed native art, not metatile or CHR numbering. */
static unsigned CavernSurfaceRole(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  static const uint64_t receiver[]={
    UINT64_C(0x041a578b427ed30b),
    UINT64_C(0x1de4a147b01d1cc0),
    UINT64_C(0x1e0bcdd30c231726),
    UINT64_C(0x2289a889697c6909),
    UINT64_C(0x3643c67111970e76),
    UINT64_C(0x3f9dfe6ecd762bae),
    UINT64_C(0x47ef530b937d4ae7),
    UINT64_C(0x4e4ae74bc24990a4),
    UINT64_C(0x51d92a06b8f8ed74),
    UINT64_C(0x5d717f944333c447),
    UINT64_C(0x5f4ff2818b1cce80),
    UINT64_C(0x6d54c894e79f3d5b),
    UINT64_C(0x6fda0241017a9d5f),
    UINT64_C(0x8498b2ae0454ea94),
    UINT64_C(0x8aedb7100ab8674e),
    UINT64_C(0x8f134be1674185f9),
    UINT64_C(0x9f079cd56fa27071),
    UINT64_C(0xb315c5fde8e737f3),
    UINT64_C(0xb5cc437951036b2d),
    UINT64_C(0xb8928854b42f2864),
    UINT64_C(0xc0fdbaa5f9a40bf6),
    UINT64_C(0xd69d93c4d810eb36),
    UINT64_C(0xde1c1b58f86fab79),
  };
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    for(unsigned i=0;i<sizeof(receiver)/sizeof(*receiver);++i)if(key==receiver[i])return 1;
    if(key==UINT64_C(0x6b2934b7c55a65a5))return 2;
  }
  return 0;
}
/* Direct native lookup is independent of the new atlas builder. */
static unsigned CavernSurfaceInk(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *color,unsigned *attributes) {
  uint16_t w[4];unsigned collision;
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),w,&collision)||
      !CavernSurfaceRole(s,w,collision))return 0;
  const unsigned q=((y&15)/8)*2+(x&15)/8,word=w[q],ink=RockFixtureWordPixel(s,word,x&7,y&7);
  if(attributes)*attributes=word&0x3c00;
  if(color){const unsigned at=(((word>>10)&7)*16+ink)*2;
    *color=ink?(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8):0;}
  return ink;
}
static void CavernSurfaceCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool baseline,unsigned totals[4],unsigned *missing,unsigned *later) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  memset(totals,0,4*sizeof(*totals));*missing=*later=0;
  const int dx[4]={0,-1,0,1},dy[4]={1,0,-1,0};
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];if(e->far_rank!=2)continue;
    const bool owned=e->surface_texture==2&&e->surface_derived&&e->top_owned_art&&e->top_sample_size==16;
    const uint16_t *art=owned?ActionVoxel_AuthoredArt(g,e->top_owned_art-1):NULL;
    if(owned){assert(art);assert(!baseline);}
    /* The original compiler can merge a boundary across material owners.
     * Classify each native pixel independently, not just the merged start. */
    for(int j=0;j<e->length;++j) {
      const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0);
      uint16_t words[4];unsigned collision;
      if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision)||
          CavernSurfaceRole(s,words,collision)!=1)continue;
      ++totals[e->side];
      if(!e->side){int first=(int)floorf(y/16.f)*16;
        while(first<y&&!CavernSurfaceInk(s,terrain,x,first,NULL,NULL))++first;
        if(first<y)++*later;}
      if(!owned){++*missing;continue;}
      uint16_t last=0;unsigned reference=0;assert(CavernSurfaceInk(s,terrain,x,y,&last,&reference));
      assert(!!e->high_priority==!!(reference&0x2000));
      bool ended=false;
      for(unsigned z=0;z<16;++z) {
        uint16_t native=0;unsigned attrs=0;
        if(!ended && CavernSurfaceInk(s,terrain,x+dx[e->side]*(int)z,
            y+dy[e->side]*(int)z,&native,&attrs) && attrs==reference)last=native;
        else ended=true;
        const unsigned along=(unsigned)((e->side&1)?y:x)&15;
        const unsigned at=!e->side?z*16+along:e->side==1?along*16+15-z:
            e->side==2?(15-z)*16+along:along*16+z;
        assert(art[at]==last); /* Includes original-depth shelves, never the cell's unrelated first crest. */
      }
    }
  }
}
/* Independent edit reachability: follow original opaque native rays until
 * their actual alpha/family/priority termination. An unrelated pixel in the
 * same16px cell is not an input to this fragment. */
static bool CavernSurfaceReadsPixel(const ActionRoomScene *s,const VoxelEdge *e,int tx,int ty) {
  const int dx[4]={0,-1,0,1},dy[4]={1,0,-1,0};
  for(int j=0;j<e->length;++j) {
    const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0);
    unsigned reference=0;if(!CavernSurfaceInk(s,NULL,x,y,NULL,&reference))continue;
    for(int depth=0;depth<16;++depth) {
      const int xx=x+dx[e->side]*depth,yy=y+dy[e->side]*depth;unsigned attrs=0;
      if(!CavernSurfaceInk(s,NULL,xx,yy,NULL,&attrs)||attrs!=reference)break;
      if(xx==tx&&yy==ty)return true;
    }
  }
  return false;
}
static void CavernSurfaceEdits(const ActionRoomScene *s,const ActionVoxelSettings settings) {
  /* Actual second contour/depth shelf:0B20,56 atworld328,904 begins five rows
   * below its first opaque ink. This is not a synthetic flat test object. */
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  assert(ActionVoxel_Build(g,s,NULL,settings));
  const int unchanged_xy[2][2]={{320,896},{344,920}};
  VoxelEdge unchanged[2]={0};VoxelEdgeSpan unchanged_span[2]={0};
  uint16_t unchanged_art[2][256];bool saved[2]={false,false};unsigned before_count,affected=0;
  const VoxelEdge *before=ActionVoxel_Edges(g,&before_count);
  for(unsigned i=0;i<before_count;++i)if(before[i].surface_texture==2) {
    affected+=CavernSurfaceReadsPixel(s,&before[i],328,904);
    for(unsigned k=0;k<2;++k)if(!before[i].side&&before[i].x==unchanged_xy[k][0]&&before[i].y==unchanged_xy[k][1]) {
      assert(!CavernSurfaceReadsPixel(s,&before[i],328,904));unchanged[k]=before[i];saved[k]=true;
      const uint16_t *art=ActionVoxel_AuthoredArt(g,before[i].top_owned_art-1);assert(art);
      memcpy(unchanged_art[k],art,sizeof(unchanged_art[k]));
      assert(ActionVoxel_ResolveEdgeSpan(g,&before[i],kVoxelEdgeNative,0,ActionVoxel_Depth(g),&unchanged_span[k]));
    }
  }
  assert(affected&&saved[0]&&saved[1]);
  terrain.voxel.cell_count=1;DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=20,.y=56,.fields=kDioramaVoxelCell_TopArt};
  edit->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
  assert(TombCapWords(s,NULL,20,56,edit->top_art.words,&collision));
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,20,56);
  assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_sample_size[1]);
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=20,.y=56};
  terrain.pixel_layers[0].edits[0].transparent[8]=1u<<(15-8);
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned count;const VoxelEdge *e=ActionVoxel_Edges(g,&count);
  bool preserved[2]={false,false};
  for(unsigned i=0;i<count;++i)if(e[i].surface_texture==2) {
    assert(!CavernSurfaceReadsPixel(s,&e[i],328,904));
    for(unsigned k=0;k<2;++k)if(!e[i].side&&e[i].x==unchanged_xy[k][0]&&e[i].y==unchanged_xy[k][1]) {
      const VoxelEdge *b=&unchanged[k];assert(e[i].length==b->length&&e[i].near_rank==b->near_rank&&
          e[i].far_rank==b->far_rank&&e[i].high_priority==b->high_priority&&e[i].colliding==b->colliding);
      VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpan(g,&e[i],kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
      assert(span.near_depth==unchanged_span[k].near_depth&&span.far_depth==unchanged_span[k].far_depth);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e[i].top_owned_art-1);assert(art);
      assert(!memcmp(art,unchanged_art[k],sizeof(unchanged_art[k])));preserved[k]=true;
    }
  }
  assert(preserved[0]&&preserved[1]);
  assert((g->front[(904-g->origin_y)*g->width+328-g->origin_x]&kVoxelKindMask)==0);
  memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(g,s,&terrain,settings));
  unsigned totals[4],missing,later;CavernSurfaceCheck(s,&terrain,g,false,totals,&missing,&later);assert(!missing&&later);
  ActionVoxel_Release(g);
}
static void CavernNativeSurfaceFixtures(const char *directory) {
  if(!directory)return;
  const bool baseline=getenv("AR_VOXEL_CAVERN_NATIVE_BASELINE")!=NULL;
  static unsigned char bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snap;
  static ActionRoomScene remapped;static DioramaRoomOverride terrain;
  const unsigned rooms[5][2]={{4,4},{4,5},{4,6},{4,7},{7,5}};
  const unsigned raw_tops[5]={1604,244,1418,409,393};
  const unsigned later_tops[5]={281,55,191,59,62};
  for(unsigned region=0;region<3;++region)for(unsigned ri=0;ri<5;++ri) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,rooms[ri][0],rooms[ri][1]);
    FILE *f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snap));const ActionRoomScene *s=&snap.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned totals[4],missing,later;
    CavernSurfaceCheck(s,NULL,g,baseline,totals,&missing,&later);assert(totals[0]==raw_tops[ri]&&later==later_tops[ri]);
    if(!baseline)assert(!missing);
    const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height);
    const uint64_t alpha=ForestBackingHash(g->cutouts,g->cutouts?(size_t)g->width/8*g->height:0);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    assert(TombCapGeometryHash(other)==geometry);assert(other->width==g->width&&other->height==g->height);
    assert(!memcmp(g->front,other->front,(size_t)g->width*g->height));unsigned tt[4],mm,ll;
    CavernSurfaceCheck(&remapped,NULL,other,baseline,tt,&mm,&ll);
    assert(!memcmp(tt,totals,sizeof(tt))&&mm==missing&&ll==later);
    printf("cavern native surface %02u/%02u region%u raw top%u right%u bottom%u left%u missing%u later%u geometry%016llx front%016llx alpha%016llx source%016llx\n",
      s->group,s->map,region,totals[0],totals[1],totals[2],totals[3],missing,later,
      (unsigned long long)geometry,(unsigned long long)front,(unsigned long long)alpha,(unsigned long long)source);
    TombCapTerrain(s->group,s->map,region,&terrain);assert(ActionVoxel_Build(other,s,&terrain,settings));
    /* Shipped authored surfaces/masks may intentionally bypass the native route;
     * every remaining opted-in face still needs exact source-pixel provenance. */
    CavernSurfaceCheck(s,&terrain,other,baseline,tt,&mm,&ll);
    printf("cavern native surface %02u/%02u region%u shipped top%u right%u bottom%u left%u missing%u later%u geometry%016llx front%016llx\n",
      s->group,s->map,region,tt[0],tt[1],tt[2],tt[3],mm,ll,
      (unsigned long long)TombCapGeometryHash(other),(unsigned long long)ForestBackingHash(other->front,(size_t)other->width*other->height));
    if(!baseline&&ri==0)CavernSurfaceEdits(s,settings);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
  DioramaLayerOrder_ClearRoom(&terrain);
}
