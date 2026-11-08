/* Native IDs locate specimens only. Contract checks use complete displayed
 * fingerprints, including collision/priority. Include after TombCap helpers. */
static bool DesertSurfaceKey(uint64_t key) {
  static const uint64_t keys[]={UINT64_C(0x174ecbdc5b7ba29f),UINT64_C(0x1a169c9bb90ed43b),
    UINT64_C(0x1a84226941d44722),UINT64_C(0x47829ff63cf4bbef),UINT64_C(0x5889f056d47187e8),
    UINT64_C(0x5ef396ebf9a2f154),UINT64_C(0x82db750dd2d4e74c),UINT64_C(0x9ed09859d6c3e324),
    UINT64_C(0xa59b130aa9c13479),UINT64_C(0xc9c0dd215348c453),UINT64_C(0xf32433da1ac56c2a)};
  for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(keys[i]==key)return true;
  return false;
}
static bool DesertSurfaceWords(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t words[4],unsigned *collision) {
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,collision))return false;
  return DesertSurfaceKey(TombCapKey(s,words,*collision,0)) ||
      DesertSurfaceKey(TombCapKey(s,words,*collision,1));
}
static unsigned DesertSurfaceColor(const ActionRoomScene *s,const uint16_t words[4],
    int x,int y,uint16_t *color) {
  const unsigned q=((y&15)/8)*2+(x&15)/8,w=words[q];
  const unsigned ink=RockFixtureWordPixel(s,w,x&7,y&7),index=(((w>>10)&7)*16+ink)*2;
  *color=ink?(uint16_t)(0x8000|s->palette[index]|s->palette[index+1]<<8):0;
  return ink;
}
static unsigned DesertSurfaceCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool baseline,unsigned *secondary) {
  unsigned count,covered=0,missing=0;*secondary=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side) {assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;}
    if(e->far_rank!=2)continue;
    for(int x=e->x;x<e->x+e->length;++x) {
      uint16_t words[4];unsigned collision;
      if(!DesertSurfaceWords(s,terrain,x,e->y,words,&collision)) {
        assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;
      }
      ++covered;const int cy=(int)floorf(e->y/16.f)*16;
      uint16_t original;unsigned first=0;
      while(first<16&&!DesertSurfaceColor(s,words,x,cy+(int)first,&original))++first;
      if(cy+(int)first<e->y)++*secondary;
      if(e->surface_texture!=kVoxelSurfaceTextureNativeInward) {++missing;continue;}
      assert(!baseline && e->surface_derived && e->top_owned_art && e->top_sample_size==16);
      assert(e->top_sample_x==(int)floorf(x/16.f)*16 && e->top_sample_y==cy);
      assert(!e->surface_top_material && !e->contour_skin);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      const unsigned q=((e->y&15)/8)*2+(x&15)/8,reference=words[q];
      assert(!!e->high_priority==!!(reference&0x2000));
      assert(!!e->colliding==!!(collision&(1u<<q)));
      uint16_t last=0;bool ended=false;
      for(unsigned depth=0;depth<16;++depth) {
        const int yy=e->y+(int)depth;uint16_t source_words[4],native=0;unsigned bits;
        if(!ended) {
          const bool same=DesertSurfaceWords(s,terrain,x,yy,source_words,&bits);
          const unsigned w=same?source_words[((yy&15)/8)*2+(x&15)/8]:0;
          if(!same || ((w^reference)&0x3c00) || !DesertSurfaceColor(s,source_words,x,yy,&native))ended=true;
          else last=native;
        }
        assert(last && art[depth*16+(x&15)]==last);
      }
    }
  }
  fprintf(stderr,"desert inward room%u coverage%u secondary%u missing%u\n",s->map,covered,*secondary,missing);
  assert(covered && missing==(baseline?covered:0));
  return covered;
}
static unsigned DesertSurfaceOwnedAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count,n=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)if(edges[i].surface_texture==kVoxelSurfaceTextureNativeInward &&
      edges[i].top_sample_x==x*16 && edges[i].top_sample_y==y*16)n+=edges[i].length;
  return n;
}
static void DesertSurfaceEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,
    const ActionVoxelGeometry *base,ActionVoxelSettings settings) {
  const int x=145,y=29;static DioramaRoomOverride terrain;
  uint16_t words[4];unsigned collision;uint8_t id;
  assert(TombCapWords(s,NULL,x,y,words,&collision));
  assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&words[0],&id));
  assert(DesertSurfaceOwnedAt(base,x,y)==16);
  VoxelCellGeometry before,after;assert(ActionVoxel_CellGeometry(base,x+1,y,&before));
  for(unsigned black=0;black<2;++black) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[0];
    *edit=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};
    if(black)edit->black[3]=0x8000;else edit->transparent[3]=0x8000;
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
    for(unsigned i=0;i<n;++i)if(!e[i].side && e[i].x==x*16 && e[i].y==y*16)
      assert(e[i].surface_texture!=kVoxelSurfaceTextureNativeInward);
    assert(ActionVoxel_CellGeometry(g,x+1,y,&after) && !memcmp(&before,&after,sizeof(before)));
  }
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_TopArt};
  edit->top_art=(DioramaVoxelArt){.set=true,.size=8};memcpy(edit->top_art.words,words,sizeof(words));
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && !(a->automatic_art&(1u<<1)) && !a->surface_derived[1]);
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned ink=RockFixtureWordPixel(s,words[0],px%8,py%8),at=(((words[0]>>10)&7)*16+ink)*2;
    assert(a->pixels[1][py*16+px]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
  edit->fields|=kDioramaVoxelCell_Width|kDioramaVoxelCell_Front|kDioramaVoxelCell_Back;
  edit->width=12;edit->front=.03f;edit->back=.15f;
  assert(ActionVoxel_Build(g,s,&terrain,settings));assert(ActionVoxel_CellGeometry(g,x,y,&after));
  assert(after.width==12);for(unsigned q=0;q<4;++q)assert(after.front[q]==.03f&&after.back[q]==.15f);
  edit->fields|=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && !(a->faces&kDioramaVoxelFaceMask_Top));
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(g,s,&terrain,settings));assert(DesertSurfaceOwnedAt(g,x,y)==16);
  assert(TombCapGeometryHash(g)==TombCapGeometryHash(base));
  /* Same artwork/priority/collision at a negative stamped origin. Horizontal
   * reflection keeps source direction; vertical reflection cannot add ceilings. */
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-2,.y=-3,.metatile=id};
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1?0x4000:0)|(flip&2?0x8000:0));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    if(flip&2)assert(!DesertSurfaceOwnedAt(g,-2,-3));
    else {unsigned secondary;assert(DesertSurfaceCheck(s,&terrain,g,false,&secondary)==16);}
  }
}
static void DesertSurfaceFixtures(const char *directory) {
  if(!directory)return;const bool baseline=getenv("AR_VOXEL_DESERT_SURFACE_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=2;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/030%u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned secondary;
    const unsigned pixels=DesertSurfaceCheck(s,NULL,g,baseline,&secondary);
    assert(pixels==(room==1?1288u:region==1?1602u:1686u));
    assert(secondary==(room==1?24u:region==1?18u:22u));
    const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height);
    fprintf(stderr,"desert source room%u region%u geometry%016llx front%016llx\n",room,region,(unsigned long long)geometry,(unsigned long long)front);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    assert(TombCapGeometryHash(other)==geometry && !memcmp(g->front,other->front,(size_t)g->width*g->height));
    unsigned again;assert(DesertSurfaceCheck(&remapped,NULL,other,baseline,&again)==pixels&&again==secondary);
    TombCapTerrain(3,room,region,&terrain);assert(ActionVoxel_Build(other,s,&terrain,settings));
    assert(TombCapGeometryHash(other)==geometry && !memcmp(g->front,other->front,(size_t)g->width*g->height));
    assert(DesertSurfaceCheck(s,&terrain,other,baseline,&again)==pixels&&again==secondary);
    /* Native dune animation remains live; static rock sources are independent
     * of both sand phases and still qualify after address renumbering. */
    if(!baseline && !region)for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
      remapped=*s;assert(ActionRoomScene_BuildCharacters(s,0,phase,remapped.characters,sizeof(remapped.characters)));
      assert(ActionVoxel_AnimationIsPresentationOnly(&remapped,s,NULL,NULL,0));
      assert(ActionVoxel_Build(other,&remapped,NULL,settings));
      assert(TombCapGeometryHash(other)==geometry);
      assert(DesertSurfaceCheck(&remapped,NULL,other,false,&again)==pixels && again==secondary);
    }
    if(!baseline && !region && room==1) {
      remapped=*s;remapped.palette[(6*16+11)*2]^=1;
      assert(ActionVoxel_Build(other,&remapped,NULL,settings));
      assert(DesertSurfaceCheck(&remapped,NULL,other,false,&again)==pixels && again==secondary);
      DesertSurfaceEdits(s,other,g,settings);
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));
    ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
