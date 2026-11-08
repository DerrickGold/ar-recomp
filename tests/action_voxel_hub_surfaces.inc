/* Exact gray hub-body artwork, independent of the generated receiver table.
 * Rib/skull ornaments and the separate completion platform are not this family. */
static bool HubSurfaceMaterial(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  static const uint64_t body[]={
    UINT64_C(0x096537792b91795a),UINT64_C(0x14867122f2fffd7d),UINT64_C(0x21b643d7d448d145),
    UINT64_C(0x25d7f55990f792ac),UINT64_C(0x2f61085e9a568aa0),UINT64_C(0x2fdb380c2b4b3cd3),
    UINT64_C(0x34523e6f1e67503f),UINT64_C(0x365c9c719308edb6),UINT64_C(0x3f1e6b08b11ae4bf),
    UINT64_C(0x43ad167f6a0d70bc),UINT64_C(0x64d967b8452bf308),UINT64_C(0x732abfe0f4e0192b),
    UINT64_C(0x892a8c7c971509be),UINT64_C(0x8cd2d2e6161380b0),UINT64_C(0xb89d8de77679f90d),
    UINT64_C(0xc12333187eea0417),UINT64_C(0xd27723c1908e8f88),UINT64_C(0xd3a08159a9294e32),
    UINT64_C(0xe967ab6d797c481d),UINT64_C(0xeb43237bc476b95e),UINT64_C(0xf4f8c243eee85625)};
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    for(unsigned i=0;i<sizeof(body)/sizeof(*body);++i)if(key==body[i])return true;
  }
  return false;
}
static unsigned HubSurfaceInk(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *color,unsigned *attributes) {
  uint16_t words[4];unsigned collision;
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision)||
      !HubSurfaceMaterial(s,words,collision))return 0;
  const unsigned word=words[((y&15)/8)*2+(x&15)/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
  if(attributes)*attributes=word&0x3c00;
  if(color){const unsigned at=(((word>>10)&7)*16+ink)*2;
    *color=ink?(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8):0;}
  return ink;
}
static void HubSurfaceCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool candidate,unsigned totals[4]) {
  unsigned n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);memset(totals,0,4*sizeof(*totals));
  const int dx[4]={0,-1,0,1},dy[4]={1,0,-1,0};
  for(unsigned i=0;i<n;++i)for(int j=0;j<edges[i].length;++j) {
    const VoxelEdge *e=&edges[i];const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0);
    uint16_t last;unsigned reference;
    if(!HubSurfaceInk(s,terrain,x,y,&last,&reference)) {
      /* All other native hub contours are thin ornaments, not stone donors. */
      assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;
    }
    ++totals[e->side];assert(!!e->high_priority==!!(reference&0x2000));
    if(!candidate){assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;}
    assert(e->surface_texture==kVoxelSurfaceTextureNativeInward&&e->surface_derived&&
        e->top_owned_art&&e->top_sample_size==16);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    bool ended=false;
    for(unsigned depth=0;depth<16;++depth) {
      uint16_t pixel;unsigned attrs;
      if(!ended&&HubSurfaceInk(s,terrain,x+dx[e->side]*(int)depth,
          y+dy[e->side]*(int)depth,&pixel,&attrs)&&attrs==reference)last=pixel;
      else ended=true;
      const unsigned along=(unsigned)((e->side&1)?y:x)&15;
      const unsigned at=!e->side?depth*16+along:e->side==1?along*16+15-depth:
          e->side==2?(15-depth)*16+along:along*16+depth;
      assert(art[at]==last);
    }
  }
}
static const VoxelEdge *HubSurfaceTopAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)if(!e[i].side&&e[i].y==y&&e[i].x<=x&&e[i].x+e[i].length>x)return e+i;
  return NULL;
}
static uint64_t HubSurfaceColumn(const ActionVoxelGeometry *g,int x,int y) {
  const VoxelEdge *e=HubSurfaceTopAt(g,x,y);assert(e&&e->surface_texture==kVoxelSurfaceTextureNativeInward);
  const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);uint16_t column[16];
  for(unsigned d=0;d<16;++d)column[d]=art[d*16+(x&15)];return ForestBackingHash(column,sizeof(column));
}
static void HubSurfaceEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;static ActionRoomScene changed;
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(7,1);
  unsigned n;const VoxelEdge *e=ActionVoxel_Edges(base,&n);int x=-1,y=-1,nx=-1,ny=-1;
  for(unsigned i=0;i<n;++i)if(!e[i].side&&e[i].surface_texture==kVoxelSurfaceTextureNativeInward) {
    if(x<0) {
      if(!HubSurfaceInk(s,NULL,e[i].x,e[i].y+1,NULL,NULL))continue;
      x=e[i].x;y=e[i].y;
    } else if(abs(e[i].x/16-x/16)>1){nx=e[i].x;ny=e[i].y;break;}
  }
  assert(x>=0&&nx>=0);const uint64_t neighbor=HubSurfaceColumn(base,nx,ny);
  for(unsigned black=0;black<2;++black) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
    *p=(DioramaPixelEdit){.by_cell=true,.x=x/16,.y=(y+1)/16};
    if(black)p->black[(y+1)&15]=1u<<(15-(x&15));
    else p->transparent[(y+1)&15]=1u<<(15-(x&15));
    assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelEdge *edited=HubSurfaceTopAt(g,x,y);
    assert(edited&&edited->surface_texture!=kVoxelSurfaceTextureNativeInward);
    assert(HubSurfaceColumn(g,nx,ny)==neighbor);
  }
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=x/16,.y=y/16,.fields=kDioramaVoxelCell_TopArt};
  cell->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
  assert(TombCapWords(s,NULL,x/16,y/16,cell->top_art.words,&collision));
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(g,x/16,y/16);
  assert(authored&&authored->surface&&!(authored->automatic_art&(1u<<1))&&!authored->surface_derived[1]);
  assert(HubSurfaceColumn(g,nx,ny)==neighbor);
  changed=*s;uint16_t words[4];assert(TombCapWords(s,NULL,x/16,y/16,words,&collision));
  const unsigned q=((y&15)/8)*2+(x&15)/8;
  changed.characters[(words[q]&1023)*32+2]^=0x40;
  assert(ActionVoxel_Build(g,&changed,NULL,settings));
  const VoxelEdge *unknown=HubSurfaceTopAt(g,x,y);
  assert(!unknown||unknown->surface_texture!=kVoxelSurfaceTextureNativeInward);
}
static uint16_t HubSurfaceRemapWord(const ActionRoomScene *s,uint16_t word) {
  /* Renumber() permutes the two captured CHR banks, preserving animation
   * phases. Frozen editor words must follow that same address permutation. */
  if((word&1023)>=512)return word;
  const unsigned stride=ActionRoomScene_HasCharacterAnimation(s)?ActionRoomScene_CharacterAnimationStride(s)/32:0;
  const unsigned animated=stride*ActionRoomScene_CharacterAnimationPhaseCount(s),i=word&255;
  const unsigned next=i<animated?i/stride*stride+(i+37)%stride: animated+(i-animated+37)%(256-animated);
  return (uint16_t)((word&~255u)|next);
}
static void HubSurfaceRemapTerrain(const ActionRoomScene *source,const ActionRoomScene *renamed,
    DioramaRoomOverride *terrain) {
  for(unsigned bg=0;bg<2;++bg) {
    DioramaStampLayerOverride *stamps=&terrain->stamp_layers[bg];
    for(size_t i=0;i<stamps->count;++i) {
      DioramaTileStamp *stamp=&stamps->cells[i];const unsigned old=stamp->metatile;
      stamp->metatile=(old+73)&255;
      for(unsigned q=0;q<4;++q) {
        const unsigned original=stamp->words[q];stamp->words[q]=HubSurfaceRemapWord(source,original);
        if(!stamp->blank)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
          assert(RockFixtureWordPixel(source,original,x,y)==RockFixtureWordPixel(renamed,stamp->words[q],x,y));
        assert((source->bg[bg].metatiles[old*8+q*2]&2)==
            (renamed->bg[bg].metatiles[stamp->metatile*8+q*2]&2));
      }
    }
    for(unsigned i=0;i<terrain->pixel_layers[bg].count;++i) {
      DioramaPixelEdit *edit=&terrain->pixel_layers[bg].edits[i];
      if(!edit->by_cell)edit->metatile=(edit->metatile+73)&255;
    }
    DioramaVirtualLayerOverride *v=&terrain->virtual_layers[bg];
    for(unsigned region=0;region<3;++region) {
      uint8_t *set=region?v->regional_metatile_set[region-1]:v->metatile_set;
      uint8_t *bands=region?v->regional_metatile_bands[region-1]:v->metatile_bands;
      uint8_t old_set[32],old_bands[256];memcpy(old_set,set,32);memcpy(old_bands,bands,256);
      memset(set,0,32);memset(bands,0,256);
      for(unsigned id=0;id<256;++id)if(old_set[id/8]&(1u<<(id&7))) {
        const unsigned next=(id+73)&255;set[next/8]|=1u<<(next&7);bands[next]=old_bands[id];
      }
    }
  }
  for(unsigned i=0;i<terrain->voxel.cell_count;++i) {
    DioramaVoxelCell *c=&terrain->voxel.cells[i];
    DioramaVoxelArt *art[]={&c->backing_art,&c->top_art,&c->side_art,&c->front_art,&c->left_art,&c->right_art,&c->bottom_art};
    for(unsigned a=0;a<sizeof(art)/sizeof(*art);++a)if(art[a]->set)
      for(unsigned q=0;q<4;++q)art[a]->words[q]=HubSurfaceRemapWord(source,art[a]->words[q]);
  }
}
static void HubSurfaceFixtures(const char *directory) {
  if(!directory)return;const bool candidate=getenv("AR_VOXEL_HUB_SURFACE_BASELINE")==NULL;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snap;
  static ActionRoomScene remapped;static DioramaRoomOverride terrain,remap_terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0701-variant0.arscene",directory,region);
    FILE*f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snap));const ActionRoomScene *s=&snap.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(7,1);
    for(unsigned shipped=0;shipped<2;++shipped) {
      TombCapTerrain(7,1,region,&terrain);const DioramaRoomOverride *t=shipped?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));unsigned totals[4];HubSurfaceCheck(s,t,g,candidate,totals);
      /* Shipped hub stamps continue the island shaft below the native256-row
       * snapshot. Its additional contours belong only to that extended map. */
      const unsigned expected[2][4]={{102,84,114,84},{126,116,138,116}};
      if(memcmp(totals,expected[shipped],sizeof(totals)))
        fprintf(stderr,"hub contour census region%u shipped%u got%u,%u,%u,%u expected%u,%u,%u,%u\n",
            region,shipped,totals[0],totals[1],totals[2],totals[3],
            expected[shipped][0],expected[shipped][1],expected[shipped][2],expected[shipped][3]);
      assert(!memcmp(totals,expected[shipped],sizeof(totals)));
      const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height);
      remapped=*s;Renumber(&remapped);const DioramaRoomOverride *rt=NULL;
      if(shipped) {
        TombCapTerrain(7,1,region,&remap_terrain);HubSurfaceRemapTerrain(s,&remapped,&remap_terrain);rt=&remap_terrain;
      }
      assert(ActionVoxel_Build(other,&remapped,rt,settings));
      unsigned remap_totals[4];HubSurfaceCheck(&remapped,rt,other,candidate,remap_totals);
      assert(!memcmp(totals,remap_totals,sizeof(totals))&&TombCapGeometryHash(other)==geometry);
      assert(!memcmp(g->front,other->front,(size_t)g->width*g->height));
      printf("hub native surface region%u shipped%u top%u right%u bottom%u left%u geometry%016llx front%016llx source%016llx\n",
          region,shipped,totals[0],totals[1],totals[2],totals[3],(unsigned long long)geometry,
          (unsigned long long)front,(unsigned long long)source);
      if(candidate&&!shipped)HubSurfaceEdits(s,g,other);
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));DioramaLayerOrder_ClearRoom(&terrain);DioramaLayerOrder_ClearRoom(&remap_terrain);
    ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
