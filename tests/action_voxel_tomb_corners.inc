/* Independent native silhouette: only the outer triangular matte is empty.
 * The darker stone border (indices12/13) and all other black art stay opaque. */
static unsigned TombCornerFlip(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    if(key==UINT64_C(0x698ebbf41a607ec1))return flip;
  }
  return 4;
}
static bool TombCornerPixel(unsigned flip,unsigned x,unsigned y) {
  static const uint16_t rows[]={0,0,1,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191};
  return !!(rows[flip&2?15-y:y]&(1u<<(flip&1?x:15-x)));
}
static void TombCornerCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    int x,int y,const uint16_t words[4],unsigned collision) {
  const unsigned flip=TombCornerFlip(s,words,collision);assert(flip<4);unsigned removed=0;
  for(unsigned q=0;q<4;++q) {
    uint8_t rows[8]={0};ActionVoxel_CutoutRows(g,x*2+(q&1),y*2+(q>>1),rows);
    for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
      const unsigned mx=(q&1)*8+px,my=(q>>1)*8+py,ink=RockFixtureWordPixel(s,words[q],px,py);
      const bool expected=TombCornerPixel(flip,mx,my),cutout=!!(rows[py]&(1u<<(7-px)));
      assert(ink&&cutout==expected);removed+=cutout;
      const unsigned role=g->front[(size_t)(y*16+my-g->origin_y)*g->width+x*16+mx-g->origin_x];
      if(expected){assert(ink==15&&!role);continue;}
      assert(role); /* Includes every dark border and native solid source bit. */
      assert(!!(role&kVoxelHighPriority)==!!(words[q]&0x2000));
      assert(!!(role&kVoxelColliding)==!!(collision&(1u<<q)));
    }
  }
  assert(removed==92);
}
static void TombCornerEdits(const ActionRoomScene *s,int x,int y,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;static ActionRoomScene reflected;
  uint16_t words[4];unsigned collision;uint8_t id;
  assert(TombCapWords(s,NULL,x,y,words,&collision));uint16_t provenance;
  assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&provenance,&id));
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));reflected=*s;
    for(unsigned q=0;q<4;++q)reflected.bg[0].metatiles[id*8+q*2]=
        (s->bg[0].metatiles[id*8+q*2]&~2u)|((collision>>(q^flip)&1u)<<1);
    DioramaTileStamp stamp={.x=-2,.y=-1,.metatile=id};
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1?0x4000:0)|(flip&2?0x8000:0));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-3,.y0=-2,.x1=0,.y1=1,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,&reflected,&terrain,ActionVoxel_SettingsForRoom(s->group,s->map)));
    unsigned sc=0;for(unsigned q=0;q<4;++q)sc|=((collision>>(q^flip))&1u)<<q;
    TombCornerCheck(&reflected,g,-2,-1,stamp.words,sc);
  }
  /* An explicit black pixel restores the authored silhouette at that point. */
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[0];*edit=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};
  unsigned restore_x=0,restore_y=0;const unsigned flip=TombCornerFlip(s,words,collision);
  while(restore_y<16&&!TombCornerPixel(flip,restore_x,restore_y))if(++restore_x==16){restore_x=0;++restore_y;}
  assert(restore_y<16);edit->black[restore_y]=1u<<(15-restore_x);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(s->group,s->map)));
  uint8_t rows[8]={0};ActionVoxel_CutoutRows(g,x*2+restore_x/8,y*2+restore_y/8,rows);
  assert(!(rows[restore_y&7]&(1u<<(7-(restore_x&7)))));
  assert(g->front[(size_t)(y*16+restore_y-g->origin_y)*g->width+x*16+restore_x-g->origin_x]);
}
static void TombCornerFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=3;room<=6;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/03%02x.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);assert(ActionVoxel_Build(g,s,NULL,settings));
    unsigned found=0;int sample_x=-1,sample_y=-1;
    for(int y=0;y<(int)ActionRoomScene_TileHeight(s,1)/2;++y)for(int x=0;x<(int)ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
      if(TombCapKey(s,words,collision,0)==UINT64_C(0xee8b2ff901056847)) {
        /* The same drawing's structural variant already recesses its black
         * triangle. Preserve that backing, including native collision bits. */
        for(unsigned q=0;q<4;++q) {
          uint8_t rows[8]={0};ActionVoxel_CutoutRows(g,x*2+(q&1),y*2+(q>>1),rows);
          for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
            if(RockFixtureWordPixel(s,words[q],px,py)!=15)continue;
            assert(!(rows[py]&(1u<<(7-px))));
            const unsigned role=g->front[(size_t)(y*16+(q>>1)*8+py-g->origin_y)*g->width+x*16+(q&1)*8+px-g->origin_x];
            assert((role&kVoxelKindMask)==kVoxelRearOnly);
          }
        }
      }
      if(TombCornerFlip(s,words,collision)==4)continue;
      TombCornerCheck(s,g,x,y,words,collision);++found;sample_x=x;sample_y=y;
    }
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));SameGeometry(g,other);
    if(found)TombCornerEdits(s,sample_x,sample_y,other);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);ActionVoxel_Release(other);
    fprintf(stderr,"Tomb corners 03/%02x region%u: %u exact triangular silhouettes, native black/detail preserved\n",room,region,found);
  }
}
