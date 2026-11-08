/* Independent source-pixel oracle for the four remaining native Ice tips.
 * Included after action_voxel_ice_inward.inc; no generated mask table is read.
 * Palette indices8..F identify cold ice only inside the three exact reviewed
 * source fingerprints below. Production uses explicit pixel ownership masks. */
static unsigned IceTipSourceKind(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    if(key==UINT64_C(0xd80ac9f9cabd2c8c)||key==UINT64_C(0xe7ee401cd44fe179))return 1;
    if(key==UINT64_C(0x98a9ac47fbcfbbf9))return 2;
    if(IceInwardSource(key))return 3;
  }
  return 0;
}
static bool IceTipReceiverAt(const ActionRoomScene *s,const DioramaRoomOverride *terrain,int x,int y) {
  uint16_t words[4];unsigned collision;
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision))return false;
  /* A horizontal source reflection is supported; an implicit upside-down
   * top is not. An exact independently reviewed native key still matches0. */
  return IceInwardReceiver(TombCapKey(s,words,collision,0)) ||
      IceInwardReceiver(TombCapKey(s,words,collision,1));
}
static unsigned IceTipExpected(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t colors[8],bool *crossed) {
  unsigned reference=0;*crossed=false;
  for(unsigned d=0;d<8;++d) {
    uint16_t words[4];unsigned collision;
    if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf((y+(int)d)/16.f),words,&collision))return 0;
    const unsigned kind=IceTipSourceKind(s,words,collision);
    if(!kind)return 0;
    const unsigned yy=(unsigned)(y+(int)d),q=((yy&15)/8)*2+(x&15)/8,w=words[q];
    const unsigned ink=RockFixtureWordPixel(s,w,x&7,yy&7);
    if(!ink)return 0;
    if(!d)reference=w;
    if((w^reference)&0x3c00)return 0;
    if(kind!=3) {
      *crossed=true;
      if(kind==2 || ink<8)return d;
    }
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    colors[d]=(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8);
  }
  return 8;
}
static void IceTipCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool baseline,unsigned *complete,unsigned *tips,unsigned *shortened) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  *complete=*tips=*shortened=0;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->far_rank!=2 || !IceTipReceiverAt(s,terrain,e->x,e->y))continue;
    const bool tiny=(e->side&1)&&e->surface_top_material&&e->soft_surface&&
        (e->fold_length?e->fold_length:e->length)<=2;
    if(e->side&&!tiny)continue;
    const unsigned columns=tiny?1:(unsigned)e->length,first=(unsigned)e->x&15;
    uint16_t profile[16][8]={{0}};unsigned length[16]={0};bool cross[16]={0},new_tip=false;
    for(unsigned p=0;p<columns;++p) {
      length[p]=IceTipExpected(s,terrain,e->x+(int)p,e->y,profile[p],&cross[p]);
      assert(length[p]);new_tip=new_tip||cross[p];
      if(!tiny) {if(cross[p]) {++*tips;if(length[p]<8)++*shortened;}else ++*complete;}
    }
    const bool active=e->surface_texture==kVoxelSurfaceTextureNativeInward;
    assert(active==(!baseline||!new_tip));
    if(!active)continue;
    assert(e->surface_derived&&e->top_sample_size==16&&e->top_owned_art);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned x=0;x<16;++x)for(unsigned d=0;d<16;++d) {
      const unsigned p=x<first?0:x>=first+columns?columns-1:x-first;
      /* Independent nearest endpoint mapping. First and last native ink must
       * survive, including the honest single-source-pixel terminal column. */
      const unsigned row=(unsigned)floorf((float)d*(length[p]-1)/15.f+0.5f);
      assert(art[d*16+x]==profile[p][row]);
      if(!d)assert(art[x]==profile[p][0]);
      if(d==15)assert(art[d*16+x]==profile[p][length[p]-1]);
    }
  }
}
static const VoxelEdge *IceTipEdgeAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)if(!edges[i].side && edges[i].y==y && x>=edges[i].x && x<edges[i].x+edges[i].length)return &edges[i];
  return NULL;
}
static uint64_t IceTipColumnHash(const ActionVoxelGeometry *g,int x,int y) {
  const VoxelEdge *e=IceTipEdgeAt(g,x,y);assert(e&&e->surface_texture==kVoxelSurfaceTextureNativeInward&&e->top_owned_art);
  const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);uint16_t col[16];
  for(unsigned d=0;d<16;++d)col[d]=art[d*16+(x&15)];return ForestBackingHash(col,sizeof(col));
}
static void IceTipEditControls(const ActionRoomScene *s,const ActionVoxelGeometry *base,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,4);
  const uint64_t neighbor=IceTipColumnHash(base,169,361);
  /* The queried new source starts at body59(13,30), local9,0. Black and
   * transparent paint on it both reject the cap; adjacent tip10 stays exact. */
  for(unsigned black=0;black<2;++black) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *e=&terrain.pixel_layers[0].edits[0];e->by_cell=true;e->x=13;e->y=30;
    if(black)e->black[0]=1u<<(15-9);else e->transparent[0]=1u<<(15-9);
    assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelEdge *cap=IceTipEdgeAt(g,217,473);assert(cap);
    assert(cap->surface_texture!=kVoxelSurfaceTextureNativeInward);
    assert(IceTipColumnHash(g,169,361)==neighbor);
  }
  /* Explicit cap art remains authoritative, and resetting the whole record
   * returns the same profile without writing into the immutable native art. */
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];*cell=(DioramaVoxelCell){.x=13,.y=29,.fields=kDioramaVoxelCell_TopArt};
  cell->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
  assert(TombCapWords(s,NULL,13,29,cell->top_art.words,&collision));
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,13,29);
  assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_derived[1]);
  memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(IceTipColumnHash(g,217,473)==IceTipColumnHash(base,217,473));
  /* Palette/priority changes on the continuation body may not import the
   * exact-source mask from an unedited material. */
  DioramaTileStamp stamp={.x=13,.y=30};uint16_t original[4];uint8_t id;
  assert(ActionRoomScene_LookupTile(s,1,26,60,&stamp.words[0],&id));stamp.metatile=id;
  assert(TombCapWords(s,NULL,13,30,original,&collision));
  for(unsigned change=0;change<2;++change) {
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].count=1;terrain.stamp_layers[0].cells=&stamp;
    for(unsigned q=0;q<4;++q)stamp.words[q]=original[q]^(change?0x2000:0x0400);
    assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelEdge *cap=IceTipEdgeAt(g,217,473);assert(cap);
    assert(cap->surface_texture!=kVoxelSurfaceTextureNativeInward);
    assert(IceTipColumnHash(g,169,361)==neighbor);
  }
  /* A two-cell negative-origin stamp tests reflected exact mixed-source masks.
   * H keeps upper-cap ownership. V may not acquire an implicit ceiling cap. */
  int ice58_x=-1;
  for(int x=20;x<=22;++x) {uint16_t w[4];unsigned c;
    if(TombCapWords(s,NULL,x,30,w,&c)&&TombCapKey(s,w,c,0)==UINT64_C(0xd80ac9f9cabd2c8c) &&
        TombCapWords(s,NULL,x,29,w,&c)&&TombCapKey(s,w,c,0)==UINT64_C(0x3765b12c5d24f004))ice58_x=x;
  }
  assert(ice58_x>=0);
  for(unsigned source_case=0;source_case<3;++source_case)for(unsigned flip=0;flip<4;++flip) {
    const int source_x=source_case==0?10:source_case==1?13:ice58_x,source_top=source_case?29:22;
    DioramaTileStamp pair[2]={{.x=-2,.y=-3},{.x=-2,.y=-2}};
    for(unsigned row=0;row<2;++row) {
      uint16_t words[4];const int source_y=source_top+(int)row;
      assert(ActionRoomScene_LookupTile(s,1,source_x*2,source_y*2,&words[0],&id));pair[row].metatile=id;
      assert(TombCapWords(s,NULL,source_x,source_y,words,&collision));
      /* A native mirrored slope carries mirrored collision quadrants too.
       * Reusing the original asymmetric collision ID would intentionally
       * fail the exact fingerprint before this source-mask test is reached. */
      unsigned reflected=0;for(unsigned q=0;q<4;++q)reflected|=((collision>>(q^flip))&1u)<<q;
      bool found_collision=false;
      for(unsigned candidate=0;candidate<256&&!found_collision;++candidate) {
        unsigned bits=0;for(unsigned q=0;q<4;++q)bits|=!!(s->bg[0].metatiles[candidate*8+q*2]&2)<<q;
        if(bits==reflected) {pair[row].metatile=(uint8_t)candidate;found_collision=true;}
      }
      assert(found_collision);
      for(unsigned q=0;q<4;++q)pair[row].words[q]=words[q^flip]^((flip&1?0x4000:0)|(flip&2?0x8000:0));
    }
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-1,.cells=pair,.count=2};
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    if(!(flip&2)) {unsigned complete,tips,shortened;IceTipCheck(s,&terrain,g,false,&complete,&tips,&shortened);fprintf(stderr,"negative stamp source%u flip%u complete%u tips%u shortened%u\n",source_case,flip,complete,tips,shortened);assert(tips==7&&shortened>0);}
    else {unsigned n;const VoxelEdge *ee=ActionVoxel_Edges(g,&n);for(unsigned j=0;j<n;++j)assert(ee[j].surface_texture!=kVoxelSurfaceTextureNativeInward);}
  }
}
static void IceTipFixtures(const char *directory) {
  if(!directory)return;const bool baseline=getenv("AR_VOXEL_ICE_TIP_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=2;room<=4;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/060%u.arscene",directory,region,room);FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(encoded,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;const uint64_t source=ForestBackingHash(s,sizeof(*s));
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);assert(ActionVoxel_Build(g,s,NULL,settings));
    unsigned complete,tips,shortened;IceTipCheck(s,NULL,g,baseline,&complete,&tips,&shortened);
    assert(tips==(room==4?28u:0u));assert(shortened==(room==4?17u:0u));
    const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height),
        alpha=ForestBackingHash(g->cutouts,g->cutouts?(size_t)g->width/8*g->height:0);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    unsigned c,t,sh;IceTipCheck(&remapped,NULL,other,baseline,&c,&t,&sh);assert(c==complete&&t==tips&&sh==shortened);
    assert(TombCapGeometryHash(other)==geometry);assert(g->width==other->width&&g->height==other->height&&!memcmp(g->front,other->front,(size_t)g->width*g->height));
    if(!baseline&&room==4) {
      IceTipEditControls(s,g,other);
      remapped=*s;uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,13,30,words,&collision));
      remapped.characters[(words[1]&1023)*32]^=0x80;
      assert(ActionVoxel_Build(other,&remapped,NULL,settings));const VoxelEdge *cap=IceTipEdgeAt(other,217,473);assert(cap);
      assert(cap->surface_texture!=kVoxelSurfaceTextureNativeInward);
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));
    fprintf(stderr,"ice tips room%u region%u complete%u recovered%u shortened%u geometry%016llx front%016llx alpha%016llx source%016llx\n",room,region,complete,tips,shortened,(unsigned long long)geometry,(unsigned long long)front,(unsigned long long)alpha,(unsigned long long)source);
    ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
}
