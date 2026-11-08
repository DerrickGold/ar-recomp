/* Independent staged Ice oracle. Native IDs are fixture locators only.
 * Requires the ordinary voxel-test helpers and staged native-inward API. */
static bool IceInwardReceiver(uint64_t key) {
  const uint64_t keys[]={UINT64_C(0x0e301aed6cb575df),UINT64_C(0x82127cdbeb4f8635),
      UINT64_C(0x6f8d672775468e7e),UINT64_C(0x3765b12c5d24f004),
      UINT64_C(0x25acf780590959b0),UINT64_C(0x2ddd24045c3c3ae2)};
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(keys[i]==key)return true;
  return false;
}
static bool IceInwardSource(uint64_t key) {
  const uint64_t keys[]={UINT64_C(0x0c15b3e42f3c9d8b),UINT64_C(0x50d2ce51b55b70f7),
      UINT64_C(0x757a8cedd169cbfe),UINT64_C(0x64e52bb01a46783b)};
  if(IceInwardReceiver(key))return true;
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(keys[i]==key)return true;
  return false;
}
static bool IceInwardExpected(const ActionRoomScene *s,int x,int y,uint16_t colors[8]) {
  unsigned reference=0;
  for(unsigned d=0;d<8;++d) {
    uint16_t words[4];unsigned collision;
    if(!TombCapWords(s,NULL,(int)floorf(x/16.f),(int)floorf((y+(int)d)/16.f),words,&collision) ||
        !IceInwardSource(TombCapKey(s,words,collision,0)))return false;
    const unsigned yy=(unsigned)(y+(int)d),q=((yy&15)/8)*2+(x&15)/8,w=words[q];
    const unsigned ink=RockFixtureWordPixel(s,w,x&7,yy&7);
    if(!ink)return false;
    if(!d)reference=w;
    if((w^reference)&0x3c00)return false;
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    colors[d]=(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8);
  }
  return true;
}
static void IceInwardCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g,bool baseline,
    unsigned *caps,unsigned *fallback,unsigned *risers) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);*caps=*fallback=*risers=0;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->surface_texture==kVoxelSurfaceTextureNativeInward)
      assert(!e->side || ((e->side&1)&&e->surface_top_material));
    uint16_t words[4];unsigned collision;
    if(!TombCapWords(s,NULL,(int)floorf(e->x/16.f),(int)floorf(e->y/16.f),words,&collision) ||
        !IceInwardReceiver(TombCapKey(s,words,collision,0)))continue;
    const bool tiny=(e->side&1)&&e->surface_top_material&&e->soft_surface&&
        (e->fold_length?e->fold_length:e->length)<=2;
    if(e->side&&!tiny)continue;
    if(e->far_rank!=2)continue;
    bool complete=true;uint16_t profile[16][8]={{0}};
    const unsigned columns=tiny?1:e->length;
    for(unsigned p=0;p<columns;++p)complete=IceInwardExpected(s,e->x+(int)p,e->y,profile[p])&&complete;
    const bool active=e->surface_texture==kVoxelSurfaceTextureNativeInward;
    if(baseline)assert(!active);else assert(active==complete);
    if(tiny)++*risers;else if(complete)*caps+=e->length;else *fallback+=e->length;
    if(!active)continue;
    assert(e->surface_derived&&e->top_sample_size==16&&e->top_owned_art);
    assert(!!e->surface_top_material==tiny);
    assert(e->top_sample_x==(int)floorf(e->x/16.f)*16&&e->top_sample_y==(int)floorf(e->y/16.f)*16);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    const unsigned first=(unsigned)e->x&15;
    for(unsigned x=0;x<16;++x)for(unsigned d=0;d<16;++d) {
      const unsigned p=x<first?0:x>=first+columns?columns-1:x-first;
      assert(art[d*16+x]==profile[p][(d*7+7)/15]);
    }
    if(tiny) {
      bool top=false;
      for(unsigned j=0;j<count;++j)if(!edges[j].side&&edges[j].y==e->y&&
          e->x>=edges[j].x&&e->x<edges[j].x+edges[j].length&&
          edges[j].near_rank==e->near_rank&&edges[j].far_rank==e->far_rank&&
          edges[j].high_priority==e->high_priority&&edges[j].colliding==e->colliding)
        top=true;
      assert(top); /* An actual cap, not a first-opaque guess, owns this riser. */
    }
  }
}
static void IceInwardFixtures(const char *directory) {
  if(!directory)return;
  const bool baseline=getenv("AR_VOXEL_ICE_INWARD_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=2;room<=4;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/060%u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(encoded,1,sizeof(encoded),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(encoded,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;assert(s->group==6&&s->map==room);
    const uint64_t source=ForestBackingHash(s,sizeof(*s));
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned caps,fallback,risers;
    IceInwardCheck(s,g,baseline,&caps,&fallback,&risers);assert(caps&&risers);
    /* Four truncated native tips each require foreign58/59/79 source in
     * their last seven columns.17 already reach non-ice ink;11 more still
     * start cyan but remain outside the exact reviewed source-key family. */
    assert(fallback==(room==4?28u:0u));
    const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height);
    const uint64_t alpha=ForestBackingHash(g->cutouts,g->cutouts?(size_t)g->width/8*g->height:0);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    assert(TombCapGeometryHash(other)==geometry);
    unsigned c2,f2,r2;IceInwardCheck(&remapped,other,baseline,&c2,&f2,&r2);
    assert(c2==caps&&f2==fallback&&r2==risers);
    assert(g->width==other->width&&g->height==other->height&&
        !memcmp(g->front,other->front,(size_t)g->width*g->height));
    if(!baseline) {
      /* Mask below an intact crest: source removal must reject its profile,
       * never retain/bridge the erased pixel in an opaque new cap. */
      const int x=room==2?23:room==3?37:9,y=room==2?13:room==3?83:21;
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};
      terrain.pixel_layers[0].edits[0].transparent[1]=0x8000;
      assert(ActionVoxel_Build(other,s,&terrain,settings));unsigned ec;const VoxelEdge *ee=ActionVoxel_Edges(other,&ec);
      for(unsigned i=0;i<ec;++i)if(ee[i].x==x*16&&ee[i].y==y*16)
        assert(ee[i].surface_texture!=kVoxelSurfaceTextureNativeInward);
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_TopArt};
      edit->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
      assert(TombCapWords(s,NULL,x,y,edit->top_art.words,&collision));
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(other,x,y);
      assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_sample_size[1]);
      /* Unreviewed palette/priority and changed CHR stay outside the family.
       * A material-only profile must not acquire an unrelated ice identity. */
      memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].count=1;
      DioramaTileStamp stamp={.x=x,.y=y};terrain.stamp_layers[0].cells=&stamp;
      uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&stamp.words[0],&id));stamp.metatile=id;
      for(unsigned change=0;change<2;++change) {
        assert(TombCapWords(s,NULL,x,y,stamp.words,&collision));
        for(unsigned q=0;q<4;++q)stamp.words[q]^=change?0x2000:0x0400;
        assert(ActionVoxel_Build(other,s,&terrain,settings));ee=ActionVoxel_Edges(other,&ec);
        for(unsigned i=0;i<ec;++i)if(ee[i].x/16==x&&ee[i].y/16==y)
          assert(ee[i].surface_texture!=kVoxelSurfaceTextureNativeInward);
      }
      remapped=*s;uint16_t original_words[4];assert(TombCapWords(s,NULL,x,y,original_words,&collision));
      remapped.characters[(original_words[0]&1023)*32]^=0x80;
      assert(ActionVoxel_Build(other,&remapped,NULL,settings));ee=ActionVoxel_Edges(other,&ec);
      for(unsigned i=0;i<ec;++i)if(ee[i].x/16==x&&ee[i].y/16==y)
        assert(ee[i].surface_texture!=kVoxelSurfaceTextureNativeInward);
      assert(source==ForestBackingHash(s,sizeof(*s)));
    }
    fprintf(stderr,"ice inward06/%02u region%u caps%u fallback%u risers%u geometry%016llx front%016llx alpha%016llx source%016llx\n",
        room,region,caps,fallback,risers,(unsigned long long)geometry,(unsigned long long)front,
        (unsigned long long)alpha,(unsigned long long)source);
    ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
}
