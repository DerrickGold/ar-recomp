/* Sand's two live phases are verified directly from native words; no cached RGB donor. */
static bool SandKey(uint64_t key) {
  static const uint64_t keys[]={UINT64_C(0x01898f41e5cee69f),UINT64_C(0x062c17ed8cf81295),UINT64_C(0x0894b25a5fdc101d),UINT64_C(0x25c615d84d7e2956),UINT64_C(0x396d8261503e0d7b),UINT64_C(0x3e39523801ad5d32),UINT64_C(0x403623135f35ee81),UINT64_C(0x475053c6a568d5a5),UINT64_C(0x47a5353f912495d1),UINT64_C(0x4a70cb6aa93ad755),UINT64_C(0x505808192096de31),UINT64_C(0x59305b8f4b0cb4b6),UINT64_C(0x5bd220bb026f3f8e),UINT64_C(0x5f22f966a9ecdbfa),UINT64_C(0x6a3a4d8e2a3c2126),UINT64_C(0x6bf30240f99967e9),UINT64_C(0x89b7ba27078191f5),UINT64_C(0x8a211c12c184da73),UINT64_C(0x9840fae352203279),UINT64_C(0x9e010c01961bd90d),UINT64_C(0x9ee08b5f010b27e2),UINT64_C(0xa80b839bd5c687ea),UINT64_C(0xa926d1f3ac4bc229),UINT64_C(0xaee05001c36aa129),UINT64_C(0xaefcebd084f0d6f2),UINT64_C(0xaefeb9559fe42c5c),UINT64_C(0xb60dce56063da0b8),UINT64_C(0xbcce2bea9db07692),UINT64_C(0xc00b6edfd03aebc3),UINT64_C(0xc26e929980578e99),UINT64_C(0xc4791b434bbc4a31),UINT64_C(0xc4d081c3e90dba7e),UINT64_C(0xcd50e7ee130c4ed5),UINT64_C(0xce733210fbc93f3f),UINT64_C(0xd6eb8d606a5fa7d4),UINT64_C(0xda60ddc4ccfc2295),UINT64_C(0xdd1296c97b6ceb2e),UINT64_C(0xe2600b31728ba6f8),UINT64_C(0xe73f6775904bfa5d),UINT64_C(0xe86425553ac603e5),UINT64_C(0xe8e19adb4ea998ec),UINT64_C(0xec03ee03986a5fa5),UINT64_C(0xf219b8acf282faa8),UINT64_C(0xf9615dd4fc968495)};
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(keys[i]==key)return true;
  return false;
}
static bool SandWords(const ActionRoomScene *s,int x,int y,uint16_t words[4]) {
  unsigned collision;
  if(!TombCapWords(s,NULL,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision))return false;
  for(unsigned flip=0;flip<4;++flip)if(SandKey(TombCapKey(s,words,collision,flip)))return true;
  return false;
}
static unsigned SandCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g,bool baseline) {
  unsigned n,top=0,riser=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=edges+i;
    if(e->surface_texture!=kVoxelSurfaceTextureLiveInward)continue;
    assert(!baseline && e->surface_derived && !e->top_owned_art && e->top_sample_size>=1 && e->top_sample_size<=16);
    assert(!e->contour_skin && e->soft_surface && e->far_rank==2);
    assert(!e->side || ((e->side&1)&&e->surface_top_material&&e->length<=2));
    assert(e->top_sample_x==e->x && e->top_sample_y==e->y);
    const unsigned width=e->side?1:e->length;
    for(unsigned x=0;x<width;++x)for(unsigned d=0;d<e->top_sample_size;++d) {
      const int xx=e->top_sample_x+x,yy=e->top_sample_y+d;
      uint16_t words[4];assert(SandWords(s,xx,yy,words));
      const unsigned word=words[((yy&15)/8)*2+(xx&15)/8];
      assert(RockFixtureWordPixel(s,word,xx&7,yy&7));
      assert(!!(word&0x2000)==!!e->high_priority);
    }
    if(e->side)riser+=e->length;else top+=e->length;
  }
  fprintf(stderr,"sand phase room%u liveTop%u liveRiser%u\n",s->map,top,riser);
  if(baseline)assert(!top&&!riser);else assert(top>900&&riser>400);
  return top;
}
static void SandLiveFixtures(void) {
  const char *directory=getenv("AR_SAND_SCENES");assert(directory);
  const bool baseline=getenv("AR_SAND_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene phase,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=2;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/030%u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);
    uint64_t geometry=0,front=0;unsigned expected=0;
    for(unsigned p=0;p<2;++p) {
      phase=*s;assert(ActionRoomScene_BuildCharacters(s,0,p,phase.characters,sizeof(phase.characters)));
      assert(ActionVoxel_Build(g,&phase,NULL,settings));unsigned count=SandCheck(&phase,g,baseline);
      uint64_t gh=TombCapGeometryHash(g),fh=ForestBackingHash(g->front,(size_t)g->width*g->height);
      fprintf(stderr,"SAND_HASH %u %u %u %016llx %016llx\n",region,room,p,(unsigned long long)gh,(unsigned long long)fh);
      if(!p){geometry=gh;front=fh;expected=count;}else assert(geometry==gh&&front==fh&&expected==count);
      assert(ActionVoxel_AnimationIsPresentationOnly(&phase,s,NULL,NULL,0));
      if(!baseline && !region && !p) {
        remapped=phase;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
        assert(SandCheck(&remapped,other,false)==count&&TombCapGeometryHash(other)==geometry);
        remapped=phase;remapped.palette[4*32+3*2]^=1;
        assert(ActionVoxel_Build(other,&remapped,NULL,settings));assert(SandCheck(&remapped,other,false)==count);
        assert(TombCapGeometryHash(other)==geometry&&!memcmp(g->front,other->front,(size_t)g->width*g->height));
        unsigned ec;const VoxelEdge *es=ActionVoxel_Edges(g,&ec),*w=NULL;
        for(unsigned i=0;i<ec;++i)if(!es[i].side&&es[i].surface_texture==3){w=es+i;break;}assert(w);
        const int wx=w->x,wy=w->y+5;
        for(unsigned black=0;black<2;++black) {
          memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
          DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[0];
          *edit=(DioramaPixelEdit){.by_cell=true,.x=wx/16,.y=wy/16};
          if(black)edit->black[wy&15]=1u<<(15-(wx&15));else edit->transparent[wy&15]=1u<<(15-(wx&15));
          assert(ActionVoxel_Build(other,&phase,&terrain,settings));
          const VoxelEdge *oe=ActionVoxel_Edges(other,&ec);
          for(unsigned i=0;i<ec;++i)if(oe[i].surface_texture==3) {
            const int width=oe[i].side?1:oe[i].length;
            assert(!(wx>=oe[i].top_sample_x&&wx<oe[i].top_sample_x+width&&
                wy>=oe[i].top_sample_y&&wy<oe[i].top_sample_y+16));
          }
        }
        memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
        DioramaVoxelCell *edit=&terrain.voxel.cells[0];
        *edit=(DioramaVoxelCell){.x=w->x/16,.y=w->y/16,.fields=kDioramaVoxelCell_TopArt};
        edit->top_art=(DioramaVoxelArt){.set=true,.size=16};uint16_t words[4];assert(SandWords(&phase,w->x,w->y,words));memcpy(edit->top_art.words,words,sizeof(words));
        assert(ActionVoxel_Build(other,&phase,&terrain,settings));
        const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(other,edit->x,edit->y);assert(a&&!(a->automatic_art&(1u<<1)));
      }
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
