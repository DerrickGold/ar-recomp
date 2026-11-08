/* Scoped source oracle for summit06/08 and its07/07 copy. Test IDs locate
 * source fixtures; production matching remains displayed-art semantics. */
static bool SummitCrestReceiver(uint64_t key) {
  const uint64_t keys[]={UINT64_C(0xc1386d819482bd57),UINT64_C(0x3fcd5106664ff507),
      UINT64_C(0x47ebf96924eab217),UINT64_C(0x4dc5601f2db0f782),
      UINT64_C(0x8b09796ffb5361f1),UINT64_C(0x6b0fa1e710ba1441),UINT64_C(0x8b7c89f79de2be0f)};
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(keys[i]==key)return true;
  return false;
}
static unsigned SummitCrestInk(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *color) {
  uint16_t words[4];unsigned collision;
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision))return 0;
  const unsigned q=((y&15)/8)*2+(x&15)/8,w=words[q];
  const unsigned p=RockFixtureWordPixel(s,w,x&7,y&7),at=(((w>>10)&7)*16+p)*2;
  if(color)*color=p?(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8):0;
  return p;
}
static unsigned SummitCrestCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool baseline,unsigned *missing) {
  unsigned count,covered=0;*missing=0;const VoxelEdge *es=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&es[i];if(e->side||e->far_rank!=2)continue;
    const int cx=(int)floorf(e->x/16.f),cy=(int)floorf(e->y/16.f);
    uint16_t words[4];unsigned collision;
    if(!TombCapWords(s,terrain,cx,cy,words,&collision)||
        !SummitCrestReceiver(TombCapKey(s,words,collision,0)))continue;
    covered+=e->length;
    const bool active=e->surface_texture&&e->top_owned_art&&e->top_sample_size==16&&e->contour_skin==3;
    if(!active){*missing+=e->length;continue;}
    assert(!baseline);
    assert(e->top_sample_x==cx*16&&e->top_sample_y==cy*16);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(int px=e->x;px<e->x+e->length;++px) {
      int first=0;while(first<16&&!SummitCrestInk(s,terrain,px,cy*16+first,NULL))++first;
      assert(first<16&&cy*16+first==e->y);
      uint16_t last=0;bool ended=false;
      for(unsigned z=0;z<16;++z) {
        uint16_t color=0;const unsigned ink=ended?0:SummitCrestInk(s,terrain,px,e->y+(int)z,&color);
        if(ink)last=color;else ended=true;
        assert(last&&art[z*16+(px&15)]==last);
      }
      const unsigned q=((e->y&15)/8)*2+(px&15)/8;
      assert(!!e->high_priority==!!(words[q]&0x2000));
      assert(!!e->colliding==!!(collision&(1u<<q)));
    }
  }
  return covered;
}
static void SummitCrestFixtures(const char *directory) {
  if(!directory)return;
  const bool baseline=getenv("AR_VOXEL_SUMMIT_CREST_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static unsigned char encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=0;room<2;++room) {
    const unsigned group=room?7:6,map=room?7:8;char path[1024];
    snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,group,map);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(encoded,1,sizeof(encoded),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(encoded,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;assert(s->group==group&&s->map==map);
    const uint64_t source_hash=ForestBackingHash(s,sizeof(*s));
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(group,map);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned missing;
    const unsigned covered=SummitCrestCheck(s,NULL,g,baseline,&missing);
    assert(covered==438);assert(missing==(baseline?438u:0u));
    const uint64_t geometry=TombCapGeometryHash(g);
    const uint64_t front=ForestBackingHash(g->front,(size_t)g->width*g->height);
    const uint64_t alpha=ForestBackingHash(g->cutouts,g->cutouts?(size_t)g->width/8*g->height:0);
    const uint64_t patches=ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches));
    const uint64_t rear=ForestBackingHash(g->rear_fills,g->rear_fill_count*sizeof(*g->rear_fills));
    assert(ActionVoxel_Build(other,s,NULL,settings));assert(TombCapGeometryHash(other)==geometry);
    unsigned again;assert(SummitCrestCheck(s,NULL,other,baseline,&again)==covered&&again==missing);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    assert(TombCapGeometryHash(other)==geometry);
    assert(SummitCrestCheck(&remapped,NULL,other,baseline,&again)==covered&&again==missing);
    assert(g->width==other->width&&g->height==other->height&&
        !memcmp(g->front,other->front,(size_t)g->width*g->height));
    if(!baseline) {
      /* One original mask forces safe fallback for that source cell, instead
       * of turning authored transparency into an opaque derived strip. */
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=5,.y=27};
      terrain.pixel_layers[0].edits[0].transparent[15]=0x8000;
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      unsigned ne;const VoxelEdge *ee=ActionVoxel_Edges(other,&ne);
      for(unsigned i=0;i<ne;++i)assert(!(ee[i].contour_skin==3&&ee[i].top_sample_x==80&&ee[i].top_sample_y==432));
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=5,.y=27,.fields=kDioramaVoxelCell_TopArt};
      edit->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
      assert(TombCapWords(s,NULL,5,27,edit->top_art.words,&collision));
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(other,5,27);
      assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_sample_size[1]);
      /* A native source stamp retains the profile; changing only its palette
       * or priority removes the exact reviewed contract. Unknown CHR must also
       * fall back, never borrowing a nearby known snow profile. */
      memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].count=1;
      DioramaTileStamp stamp_record={0};terrain.stamp_layers[0].cells=&stamp_record;
      DioramaTileStamp *stamp=&stamp_record;
      stamp->x=5;stamp->y=27;uint8_t id;
      assert(ActionRoomScene_LookupTile(s,1,10,54,&stamp->words[0],&id));stamp->metatile=id;
      assert(TombCapWords(s,NULL,5,27,stamp->words,&collision));
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      assert(SummitCrestCheck(s,&terrain,other,false,&again)==covered&&!again);
      for(unsigned change=0;change<2;++change) {
        assert(TombCapWords(s,NULL,5,27,stamp->words,&collision));
        for(unsigned q=0;q<4;++q)stamp->words[q]^=change?0x2000:0x0400;
        assert(ActionVoxel_Build(other,s,&terrain,settings));ee=ActionVoxel_Edges(other,&ne);
        for(unsigned i=0;i<ne;++i)assert(!(ee[i].contour_skin==3&&ee[i].top_sample_x==80&&ee[i].top_sample_y==432));
      }
      remapped=*s;uint16_t unknown[4];assert(TombCapWords(s,NULL,5,27,unknown,&collision));
      remapped.characters[(unknown[0]&1023)*32]^=0x80;
      assert(ActionVoxel_Build(other,&remapped,NULL,settings));ee=ActionVoxel_Edges(other,&ne);
      for(unsigned i=0;i<ne;++i)assert(!(ee[i].contour_skin==3&&ee[i].top_sample_x==80&&ee[i].top_sample_y==432));
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=5,.y=27,.fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(other,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(other,5,27);
      assert(a&&a->surface_sample_size[1]==0);
      assert(source_hash==ForestBackingHash(s,sizeof(*s)));
    }
    fprintf(stderr,"summit crest %02u/%02u region%u covered%u missing%u geometry%016llx front%016llx alpha%016llx patches%016llx rear%016llx source%016llx\n",
        group,map,region,covered,missing,(unsigned long long)geometry,(unsigned long long)front,
        (unsigned long long)alpha,(unsigned long long)patches,(unsigned long long)rear,(unsigned long long)source_hash);
    ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
}
