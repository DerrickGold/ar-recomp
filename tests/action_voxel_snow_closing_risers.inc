/* Frozen native pre-effect witnesses: edge bytes omit only the new effect marker;
 * native front/art/patch and physical-span hashes remain independent controls. */
static const uint64_t kSnowClosingOriginal[12][5]={
  {UINT64_C(0x500ab0217cf89069),UINT64_C(0xf047560d670fa42a),UINT64_C(0x2d60bb082cebd655),UINT64_C(0x2e7758ece8b2e31c),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x500ab0217cf89069),UINT64_C(0xf047560d670fa42a),UINT64_C(0x2d60bb082cebd655),UINT64_C(0x2e7758ece8b2e31c),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x2f2a1bc35d7381d5),UINT64_C(0x74d1364cc3e80f82),UINT64_C(0xd008811f60347784),UINT64_C(0xcbf29ce484222325),UINT64_C(0x89164edc19393b05)},
  {UINT64_C(0xd200dc26dad59f67),UINT64_C(0xa609b93279e4209a),UINT64_C(0x742518d874b1f889),UINT64_C(0xcbf29ce484222325),UINT64_C(0xbf3c3b84c357e711)},
  {UINT64_C(0x987a483f3ff391f5),UINT64_C(0xf62a6d8b7508acee),UINT64_C(0x7fda9aa8891f0095),UINT64_C(0xda36fc71108869bc),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x987a483f3ff391f5),UINT64_C(0xf62a6d8b7508acee),UINT64_C(0x7fda9aa8891f0095),UINT64_C(0xda36fc71108869bc),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x2f2a1bc35d7381d5),UINT64_C(0x74d1364cc3e80f82),UINT64_C(0xd008811f60347784),UINT64_C(0xcbf29ce484222325),UINT64_C(0x89164edc19393b05)},
  {UINT64_C(0xd200dc26dad59f67),UINT64_C(0xa609b93279e4209a),UINT64_C(0x742518d874b1f889),UINT64_C(0xcbf29ce484222325),UINT64_C(0xbf3c3b84c357e711)},
  {UINT64_C(0x500ab0217cf89069),UINT64_C(0xf047560d670fa42a),UINT64_C(0x28efd47048c64895),UINT64_C(0x2e7758ece8b2e31c),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x500ab0217cf89069),UINT64_C(0xf047560d670fa42a),UINT64_C(0x28efd47048c64895),UINT64_C(0x2e7758ece8b2e31c),UINT64_C(0x85d1eaa80a7dc57d)},
  {UINT64_C(0x2f2a1bc35d7381d5),UINT64_C(0x74d1364cc3e80f82),UINT64_C(0xd008811f60347784),UINT64_C(0xcbf29ce484222325),UINT64_C(0x89164edc19393b05)},
  {UINT64_C(0xd200dc26dad59f67),UINT64_C(0xa609b93279e4209a),UINT64_C(0x742518d874b1f889),UINT64_C(0xcbf29ce484222325),UINT64_C(0xbf3c3b84c357e711)},
};
static uint64_t SnowClosingEdges(const ActionVoxelGeometry *g) {
  uint64_t h=0;for(unsigned i=0;i<g->edge_count;++i) {
    VoxelEdge e=g->edges[i];e.snow_closing_riser[0]=e.snow_closing_riser[1]=0;
    h+=ForestBackingHash(&e,sizeof(e));
  }return h;
}
static bool SnowClosingNative(const ActionRoomScene *s,int x,int y) {
  static const uint64_t keys[]={UINT64_C(0xa1ec2b494f9ef9bd),UINT64_C(0x308771cd48f97d50),
    UINT64_C(0x938b1af6f09188e4),UINT64_C(0x80862dbb38c56ad2),UINT64_C(0x3559e7754c42ee05),
    UINT64_C(0xb6f7b39053e8cd19),UINT64_C(0x52d197b10ca04589),UINT64_C(0x483c7371c8f55c54),
    UINT64_C(0xc85818cd083b7464),UINT64_C(0x692126e48de9f0a4),UINT64_C(0x0b024085dfcf0de5),
    UINT64_C(0xa5cf0cab3a860e0d)};
  uint16_t w[4];unsigned collision;
  if(!TombCapWords(s,NULL,(int)floorf(x/16.f),(int)floorf(y/16.f),w,&collision))return false;
  for(unsigned flip=0;flip<4;++flip)for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)
    if(TombCapKey(s,w,collision,flip)==keys[i])return true;
  return false;
}
static unsigned SnowClosingCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned n=0,left=0,right=0,phase=0,raw=0;
  for(unsigned i=0;i<g->edge_count;++i)for(unsigned end=0;end<2;++end) {
    const VoxelEdge*t=g->edges+i;unsigned drop=t->snow_closing_riser[end];if(!drop)continue;
    assert(!t->side&&t->soft_surface&&t->far_rank==2&&!t->high_priority&&drop<=2);
    const int x=end?t->x+t->length:t->x;bool side=false,lower=false;
    for(unsigned j=0;j<g->edge_count;++j) {
      const VoxelEdge*e=g->edges+j;
      if(e->side==(end?1:3)&&e->x+(end?1:0)==x&&e->y==t->y&&e->length==drop) {
        assert((e->fold_length?e->fold_length:e->length)<=2);
        if(e->surface_top_material) {
          assert(e->top_owned_art==t->top_owned_art&&e->top_sample_x==t->top_sample_x&&e->top_sample_y==t->top_sample_y);++phase;
        } else {assert(SnowClosingNative(s,e->x,e->y));++raw;}
        side=true;
      }
      if(!e->side&&e->y==t->y+drop&&(end?e->x==x:e->x+e->length==x)&&e->far_rank==t->far_rank&&e->near_rank==t->near_rank)lower=true;
    }
    assert(side&&lower);++n;if(end)++right;else++left;
  }
  printf("Snow supported tiny risers: left%u right%u total%u existing-phase%u native-source%u\n",left,right,n,phase,raw);assert(left==177&&right==281&&phase==250&&raw==208);return n;
}
static void SnowClosingRiserFixtures(const char*directory) {
  if(!directory)return;unsigned original_row=0;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snap;static ActionRoomScene remap;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=8;room+=room==1?4:3) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/06%02u.arscene",directory,region,room);FILE*sf=fopen(path,"rb");assert(sf);
    size_t n=fread(bytes,1,sizeof(bytes),sf);fclose(sf);assert(ActionSceneSnapshot_Decode(bytes,n,&snap));const ActionRoomScene*s=&snap.scene;
    ActionVoxelGeometry*g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);const ActionVoxelSettings set=ActionVoxel_SettingsForRoom(6,room);
    unsigned total=0;
    for(unsigned shipped=0;shipped<(room==1?2u:1u);++shipped) {
      TombCapTerrain(6,room,region,&terrain);assert(ActionVoxel_Build(g,s,shipped?&terrain:NULL,set));
      uint64_t hashes[]={SnowClosingEdges(g),TombCapGeometryHash(g),ForestBackingHash(g->front,(size_t)g->width*g->height),
          ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches)),ForestBackingHash(g->authored_art,g->authored_art_count*sizeof(*g->authored_art))};
      for(unsigned k=0;k<5;++k)assert(hashes[k]==kSnowClosingOriginal[original_row][k]);++original_row;
      if(room==1)total=SnowClosingCheck(s,g);
      else for(unsigned i=0;i<g->edge_count;++i)assert(!g->edges[i].snow_closing_riser[0]&&!g->edges[i].snow_closing_riser[1]);
      printf("Snow region%u room%u shipped%u original edge/art/front/patch/span %s\n",region,room,shipped,"exact");DioramaLayerOrder_ClearRoom(&terrain);
    }
    if(room==1) {
      assert(ActionVoxel_Build(g,s,NULL,set));remap=*s;Renumber(&remap);assert(ActionVoxel_Build(other,&remap,NULL,set));assert(SnowClosingCheck(&remap,other)==total);SameGeometry(g,other);
      unsigned witnesses=0;for(unsigned i=0;i<g->edge_count;++i)if(!g->edges[i].side&&
        ((g->edges[i].x==1318&&g->edges[i].y==664)||(g->edges[i].x==1312&&g->edges[i].y==658))) {
        assert(g->edges[i].snow_closing_riser[1]==2);++witnesses;}assert(witnesses==2);
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=82,.y=41};terrain.pixel_layers[0].edits[0].transparent[10]=1u<<(15-8);
      assert(ActionVoxel_Build(other,s,&terrain,set));unsigned elsewhere=0;
      for(unsigned i=0;i<other->edge_count;++i){const VoxelEdge*e=other->edges+i;if(e->x/16==82&&e->y/16==41)assert(!e->snow_closing_riser[0]&&!e->snow_closing_riser[1]);else elsewhere+=!!e->snow_closing_riser[0]+!!e->snow_closing_riser[1];}assert(elsewhere);
      /* Explicit shape/width ownership keeps the native effect marker out of
       * the authored replacement, while unrelated banks remain eligible. */
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=82,.y=41,.fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(other,s,&terrain,set));elsewhere=0;
      for(unsigned i=0;i<other->edge_count;++i){const VoxelEdge*e=other->edges+i;if(e->x/16==82&&e->y/16==41)assert(!e->snow_closing_riser[0]&&!e->snow_closing_riser[1]);else elsewhere+=!!e->snow_closing_riser[0]+!!e->snow_closing_riser[1];}assert(elsewhere);
      memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(other,s,&terrain,set));assert(SnowClosingCheck(s,other)==total);
    }
    ActionVoxel_Release(g);ActionVoxel_Release(other);
  }assert(original_row==12);
}
