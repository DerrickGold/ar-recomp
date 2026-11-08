/* Reviewed marsh beam caps: trace the four native wood rows, excluding the
 * separator/rail below. Include after TombCapWords/Key and fixture helpers. */
static const uint64_t kMarshBeamTopKeys[]={
  UINT64_C(0x07273cefa6849614),UINT64_C(0x4642c7f3ac7e1143),
  UINT64_C(0x4f35688f22288cee),UINT64_C(0x5b67dd7774b1a2d4),
  UINT64_C(0x634be0915ae90b99),UINT64_C(0x65d5088e150cc12b),
  UINT64_C(0x80887612c6bcd001),UINT64_C(0x9758ad935cc25a5c),
  UINT64_C(0xab460f7af143ba5e),UINT64_C(0xd4aaf721a87e7989),
  UINT64_C(0xd728c74aa7fb8429),UINT64_C(0xf516074fcc2aa4e9),
  UINT64_C(0xf75c7c84925d1080),UINT64_C(0xf85dd4430f011fe6)};
static unsigned MarshBeamTopRole(const ActionRoomScene *s,const uint16_t words[4],
    unsigned collision,unsigned *reflection) {
  for(unsigned flip=0;flip<2;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    for(unsigned i=0;i<sizeof(kMarshBeamTopKeys)/sizeof(*kMarshBeamTopKeys);++i)
      if(key==kMarshBeamTopKeys[i]){*reflection=flip;return i+1;}
  }
  return 0;
}
static bool MarshBeamTopNativeCutout(unsigned role,unsigned reflection,unsigned x,unsigned y) {
  /* The existing reviewed mattes remove post gaps from native solid-colour
   * ink. One surviving column terminates before row3; it must not turn into
   * an untextured half-cap or import that transparent fourth source row. */
  const unsigned mask=role==8?0x4000:(role==2||role==7)?(y==3?0x0017:0x0015):0;
  return !!(mask&(1u<<(15-(reflection?15-x:x))));
}
static unsigned MarshBeamTopPrefix(const ActionRoomScene *s,const uint16_t words[4],
    unsigned role,unsigned reflection,unsigned x,uint16_t colors[4]) {
  unsigned count=0;
  for(unsigned y=0;y<4;++y) {
    const unsigned word=words[x/8],ink=RockFixtureWordPixel(s,word,x&7,y);
    if(!ink||MarshBeamTopNativeCutout(role,reflection,x,y))break;
    const unsigned at=(((word>>10)&7)*16+ink)*2;
    colors[count++]=0x8000|s->palette[at]|s->palette[at+1]<<8;
  }
  return count;
}
static void MarshBeamTopColumn(const uint16_t *art,unsigned x,
    const uint16_t colors[4],unsigned count) {
  /* Independent explicit nearest-sample sequences, including the surviving
   * three-row prefix beside a cutout. Every source row appears exactly once
   * as a contiguous interval, with no rail row, wrap or blended colour. */
  static const unsigned four[16]={0,0,0,1,1,1,1,1,2,2,2,2,2,3,3,3},
      three[16]={0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2};
  assert(art&&(count==3||count==4));
  for(unsigned depth=0;depth<16;++depth)
    assert(art[depth*16+x]==colors[count==4?four[depth]:three[depth]]);
  assert(art[x]==colors[0]&&art[15*16+x]==colors[count-1]);
}
static unsigned MarshBeamTopCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,int selected[2]) {
  unsigned seen=0,covered=0,full=0,shortened=0,varied=0,internal=0,count;
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=edges+i;if(e->side||e->far_rank!=2)continue;
    const int cx=(int)floorf(e->x/16.f),cy=(int)floorf(e->y/16.f);
    uint16_t words[4];unsigned collision,reflection=0;
    if(!TombCapWords(s,terrain,cx,cy,words,&collision))continue;
    const unsigned role=MarshBeamTopRole(s,words,collision,&reflection);if(!role)continue;
    if(e->y!=cy*16){assert(!e->surface_texture);++internal;continue;}
    assert(e->surface_texture==kVoxelSurfaceTexturePatch&&e->surface_derived&&e->surface_native_x&&e->top_owned_art);
    assert(e->top_sample_size==16&&e->top_sample_x==cx*16&&e->top_sample_y==cy*16);
    assert(!e->surface_top_material&&!e->contour_skin&&e->length<=16);
    assert(e->x+e->length<=cx*16+16);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned x=0;x<16;++x)if(MarshBeamTopNativeCutout(role,reflection,x,0))
      for(unsigned depth=0;depth<16;++depth)assert(!art[depth*16+x]);
    for(unsigned at=0;at<e->length;++at) {
      const unsigned x=e->x-cx*16+at;uint16_t colors[4];
      const unsigned prefix=MarshBeamTopPrefix(s,words,role,reflection,x,colors);
      MarshBeamTopColumn(art,x,colors,prefix);
      assert(!!(words[x/8]&0x2000)==!!e->high_priority);
      assert(!!(collision&(1u<<(x/8)))==!!e->colliding);
      full+=prefix==4;shortened+=prefix==3;varied+=colors[0]!=colors[prefix-1];++covered;
    }
    seen|=1u<<(role-1);
    if(role==6&&selected[0]==INT_MIN){selected[0]=cx;selected[1]=cy;}
  }
  assert(seen==0x3fff&&full&&shortened&&varied&&internal);
  return covered;
}
static const VoxelEdge *MarshBeamTopAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelEdge *e=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)
    if(!e[i].side&&e[i].far_rank==2&&e[i].y==y&&e[i].x<=x&&e[i].x+e[i].length>x)return e+i;
  return NULL;
}
static void MarshBeamTopEdits(const ActionRoomScene *s,int cx,int cy) {
  static DioramaRoomOverride terrain;DioramaLayerOrder_ClearRoom(&terrain);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
  ActionVoxelGeometry *base=ActionVoxel_Create(),*g=ActionVoxel_Create();assert(base&&g);
  assert(ActionVoxel_Build(base,s,NULL,settings));
  uint16_t words[4];unsigned collision,reflection=0;
  assert(TombCapWords(s,NULL,cx,cy,words,&collision));
  const unsigned role=MarshBeamTopRole(s,words,collision,&reflection);assert(role==6&&!reflection);
  VoxelCellGeometry neighbour,after;assert(ActionVoxel_CellGeometry(base,cx+1,cy,&neighbour));
  const VoxelEdge *original=MarshBeamTopAt(base,cx*16,cy*16);assert(original&&original->top_owned_art);
  uint16_t expected[256];memcpy(expected,ActionVoxel_AuthoredArt(base,original->top_owned_art-1),sizeof(expected));
  for(unsigned which=0;which<3;++which) {
    DioramaLayerOrder_ClearRoom(&terrain);terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *edit=terrain.pixel_layers[0].edits;
    *edit=(DioramaPixelEdit){.by_cell=true,.x=cx,.y=cy};
    if(which==1)edit->black[2]=0x8000;else edit->transparent[which==2?8:2]=0x8000;
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    const VoxelEdge *e=MarshBeamTopAt(g,cx*16,cy*16);assert(e);
    if(which==2) {
      assert(e->surface_texture&&e->top_owned_art);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      for(unsigned x=0;x<16;++x) {
        uint16_t colors[4];const unsigned n=MarshBeamTopPrefix(s,words,role,0,x,colors);
        /* Check the actual edge's complete native columns. */
        if(x>=e->x-cx*16&&x<e->x-cx*16+e->length)MarshBeamTopColumn(art,x,colors,n);
      }
    } else assert(!e->surface_texture&&!e->top_owned_art);
    assert(ActionVoxel_CellGeometry(g,cx+1,cy,&after)&&!memcmp(&neighbour,&after,sizeof(after)));
  }
  DioramaLayerOrder_ClearRoom(&terrain);terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=terrain.voxel.cells;
  *cell=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,cx,cy);
  assert(a&&a->surface_sample_size[1]==16&&a->surface_derived[1]&&a->surface_native_x[1]);
  for(unsigned x=0;x<16;++x) {
    uint16_t colors[4];const unsigned n=MarshBeamTopPrefix(s,words,role,0,x,colors);
    MarshBeamTopColumn(a->pixels[1],x,colors,n);
  }
  cell->fields|=kDioramaVoxelCell_TopArt;
  cell->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=8};
  memcpy(cell->top_art.words,words,sizeof(words));
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,cx,cy);
  assert(a&&!a->surface_sample_size[1]&&!a->surface_derived[1]&&!a->surface_native_x[1]&&!(a->automatic_art&(1u<<1)));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned word=words[0],ink=RockFixtureWordPixel(s,word,x&7,y&7),at=(((word>>10)&7)*16+ink)*2;
    assert(a->pixels[1][y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
  assert(memcmp(a->pixels[1],expected,sizeof(expected)));
  cell->fields|=kDioramaVoxelCell_Faces;cell->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,cx,cy);assert(a&&!(a->faces&kDioramaVoxelFaceMask_Top));
  assert(ActionVoxel_CellGeometry(g,cx+1,cy,&after)&&!memcmp(&neighbour,&after,sizeof(after)));
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=cx,.y=cy};
  assert(ActionVoxel_Build(g,s,&terrain,settings));SameGeometry(base,g);
  DioramaLayerOrder_ClearRoom(&terrain);ActionVoxel_Release(base);ActionVoxel_Release(g);
}
static void MarshBeamTopFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0201.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);const size_t n=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    unsigned covered[2];int selected[2]={INT_MIN,0};
    for(unsigned shipped=0;shipped<2;++shipped) {
      TombCapTerrain(2,1,region,&terrain);const DioramaRoomOverride *t=shipped?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));covered[shipped]=MarshBeamTopCheck(s,t,g,selected);
      DioramaLayerOrder_ClearRoom(&terrain);
    }
    assert(selected[0]!=INT_MIN);assert(ActionVoxel_Build(g,s,NULL,settings));
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
    int unused[2]={INT_MIN,0};assert(MarshBeamTopCheck(&remapped,NULL,other,unused)==covered[0]);
    SameGeometry(g,other);assert(!memcmp(g->front,other->front,(size_t)g->width*g->height));
    MarshBeamTopEdits(s,selected[0],selected[1]);assert(!memcmp(s,&original,sizeof(original)));
    printf("Marsh beam tops region%u:14families native%u/shipped%u columns; four-row grain, cutout prefixes, native phase/priority, remap, masks and explicit art PASS\n",region,covered[0],covered[1]);
    ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
