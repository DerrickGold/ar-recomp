/* Include after the accepted Sand fixture; its independent exact-key/native
 * decoder is reused, not the compiler qualification helper. */
static unsigned SandTransitionExtent(const ActionRoomScene *s,const ActionVoxelGeometry *g,int x,int y) {
  (void)g; /* Closed44source keys all have four structural native material roles. */
  unsigned reference=0,n=0;
  for(unsigned d=0;d<16;++d) {
    uint16_t words[4];const int yy=y+d;
    if(!SandWords(s,x,yy,words))break;
    const unsigned w=words[((yy&15)/8)*2+(x&15)/8];
    if(!d)reference=w;
    if(((w^reference)&0x3c00)||!RockFixtureWordPixel(s,w,x&7,yy&7))break;
    ++n;
  }
  return n;
}
static uint64_t SandTransitionFullHash(const ActionVoxelGeometry *g) {
  uint64_t h=UINT64_C(14695981039346656037);unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)if(e[i].surface_texture==3&&e[i].top_sample_size==16) {
    const uint8_t *p=(const uint8_t*)(e+i);for(size_t j=0;j<sizeof(*e);++j)h=(h^p[j])*UINT64_C(1099511628211);
  }return h;
}
static void SandTransitionFixtures(void) {
  const char *root=getenv("AR_SAND_TRANSITION_SCENES");assert(root);bool baseline=getenv("AR_SAND_TRANSITION_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene phase;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=2;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/030%u.arscene",root,region,room);FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);uint64_t phase_geometry=0;unsigned phase_extents[17]={0};
    const uint64_t source=ForestBackingHash(&snapshot.scene,sizeof(snapshot.scene));
    for(unsigned p=0;p<2;++p) {
      phase=snapshot.scene;assert(ActionRoomScene_BuildCharacters(&snapshot.scene,0,p,phase.characters,sizeof(phase.characters)));
      assert(ActionVoxel_Build(g,&phase,NULL,ActionVoxel_SettingsForRoom(3,room)));
      unsigned ne;const VoxelEdge *es=ActionVoxel_Edges(g,&ne);unsigned extents[17]={0},risers=0,missing=0;
      for(unsigned i=0;i<ne;++i) {
        const VoxelEdge *e=es+i;
        if(e->side) {
          if(e->surface_texture==3&&e->top_sample_size<16) {
            assert(!baseline&&(e->side&1)&&e->surface_top_material&&e->length<=2);
            assert(SandTransitionExtent(&phase,g,e->x,e->y)==e->top_sample_size);++risers;
          }continue;
        }
        if(e->far_rank!=2||!e->soft_surface)continue;
        for(int x=e->x;x<e->x+e->length;++x) {
          const unsigned extent=SandTransitionExtent(&phase,g,x,e->y);if(!extent||extent==16)continue;
          ++extents[extent];
          if(baseline){assert(e->surface_texture!=3);++missing;}
          else {assert(e->surface_texture==3&&!e->top_owned_art&&e->surface_derived);assert(e->top_sample_size==extent);}
        }
      }
      unsigned total=0;for(unsigned j=1;j<16;++j)total+=extents[j];assert(total==(room==1?128:region==1?96:144));
      assert(baseline?missing==total:risers>20);
      const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height),full=SandTransitionFullHash(g);
      fprintf(stderr,"SAND_PREFIX_INVARIANTS %u %u %u %016llx %016llx %016llx\n",region,room,p,(unsigned long long)geometry,(unsigned long long)front,(unsigned long long)full);
      fprintf(stderr,"sand prefix region%u room%u phase%u columns%u shortRisers%u missing%u extents",region,room,p,total,risers,missing);for(unsigned j=1;j<16;++j)if(extents[j])fprintf(stderr," %u:%u",j,extents[j]);fprintf(stderr,"\n");
      if(!p){phase_geometry=geometry;memcpy(phase_extents,extents,sizeof(extents));}else assert(phase_geometry==geometry&&!memcmp(phase_extents,extents,sizeof(extents)));
      assert(ActionVoxel_AnimationIsPresentationOnly(&phase,&snapshot.scene,NULL,NULL,0));
    }
    assert(source==ForestBackingHash(&snapshot.scene,sizeof(snapshot.scene)));ActionVoxel_Release(g);
  }
}
