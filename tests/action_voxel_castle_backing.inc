/* Include after action_voxel_tomb_caps.inc. This independent oracle uses the
 * displayed native background-brick signature, not its incidental ROM ID.
 * The old backing marker selects a blue window edge and MUST fail here. */
static bool CastleBackingWords(const ActionRoomScene *s, uint16_t words[4]) {
  unsigned matches=0;
  for(unsigned id=0;id<256;++id) {
    uint16_t w[4];unsigned collision=0;
    for(unsigned q=0;q<4;++q) {
      const uint8_t *p=s->bg[0].metatiles+id*8+q*2;
      w[q]=ActionBg_ComposeTilemapWord(p[1]|p[0]<<8,
          kActionRoomSceneTileWordMask,ActionRoomScene_BgAttributes(s,1));
      collision|=!!(p[0]&2)<<q;
    }
    if(TombCapKey(s,w,collision,0)==UINT64_C(0x5f76186e1ba1aff5)) {
      memcpy(words,w,sizeof(w));++matches;
    }
  }
  return matches==1;
}
static uint16_t CastleBackingColor555(const ActionRoomScene *s,
    const uint16_t words[4],int x,int y) {
  const unsigned xx=((unsigned)x+(((unsigned)y&31)>=16?8:0))&15,yy=(unsigned)y&15;
  const unsigned w=words[(yy/8)*2+xx/8];
  const unsigned ink=RockFixtureWordPixel(s,w,xx&7,yy&7);
  assert(ink); /* The chosen background masonry is completely opaque. */
  const unsigned at=(((w>>10)&7)*16+ink)*2;
  return s->palette[at]|s->palette[at+1]<<8;
}
static uint32_t CastleBackingColor32(uint16_t color,const ActionRoomSceneFrameState *f) {
  const unsigned subtract=!f->cgwsel&&(f->cgadsub&0xc1)==0x81?f->fixed_color:0;
  uint32_t rgb=UINT32_C(0xff000000);
  for(unsigned c=0;c<3;++c) {
    int v=((color>>(c*5))&31)-((subtract>>(c*5))&31);if(v<0)v=0;
    rgb|=(uint32_t)((v*8+(v>>2))*f->brightness/15)<<(16-c*8);
  }
  return rgb;
}
static void CastleBackingCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  uint16_t words[4];assert(CastleBackingWords(s,words));
  for(unsigned y=0;y<32;++y)for(unsigned x=0;x<16;++x)
    assert(g->backing_color[y*16+x]==CastleBackingColor555(s,words,x,y));
  /* Front identity, native priority/collision and real source alpha are
   * independent of the donor. Exercise an actual complete opaque ceiling
   * tile when present, including frame brightness and fixed subtraction. */
  bool checked=false;
  for(int y=0;y<g->height/16&&!checked;++y)for(int x=0;x<g->width/16&&!checked;++x) {
    const int wx=g->origin_x+x*16,wy=g->origin_y+y*16;
    if(wx<0||wy<0)continue;
    uint16_t native_words[4];unsigned collision;
    if(!TombCapWords(s,NULL,wx/16,wy/16,native_words,&collision))continue;
    const uint64_t key=TombCapKey(s,native_words,collision,0);
    if(key!=UINT64_C(0x771b974a513ca12e)&&key!=UINT64_C(0x63961125d6c1e304))continue;
    for(unsigned mode=0;mode<3;++mode) {
      const ActionRoomSceneFrameState frame={.brightness=mode==1?7:15,
          .cgadsub=mode==2?0x81:0,.fixed_color=mode==2?0x421:0};
      uint32_t low[256],saved[256],rear[256],front[256];
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        low[py*16+px]=ForestNativeColor(s,wx+px,wy+py,&frame);
      memcpy(saved,low,sizeof(low));
      assert(ActionVoxel_Split(g,wx,wy,16,16,16,&frame,low,16,rear,front));
      assert(!memcmp(low,saved,sizeof(low))&&!memcmp(low,front,sizeof(low)));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        assert(rear[py*16+px]==CastleBackingColor32(CastleBackingColor555(s,words,wx+px,wy+py),&frame));
    }
    checked=true;
  }
  assert(checked);
}
static void CastleBackingFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot fixture;
  static ActionRoomScene immutable,renumbered;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=2;room<=8;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/02%02u.arscene",directory,region,room);
    FILE *f=fopen(path,"rb");assert(f);
    size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    ActionRoomScene *s=&fixture.scene;immutable=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,room);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*r=ActionVoxel_Create();assert(g&&r);
    assert(ActionVoxel_Build(g,s,NULL,settings)&&ActionVoxel_Active(g,2,room));
    CastleBackingCheck(s,g);
    renumbered=*s;Renumber(&renumbered);
    assert(ActionVoxel_Build(r,&renumbered,NULL,settings));
    CastleBackingCheck(&renumbered,r);SameGeometry(g,r);
    assert(g->width==r->width&&g->height==r->height&&
        !memcmp(g->front,r->front,(size_t)g->width*g->height));
    if(room==8) {
      static DioramaRoomOverride terrain;
      TombCapTerrain(2,8,region,&terrain);
      assert(ActionVoxel_Build(r,s,&terrain,settings));
      uint16_t donor[4];assert(CastleBackingWords(s,donor));
      /* The reported blue rail is the exposed rear of authored green
       * ceiling rows, including negative world coordinates. */
      const int rows[]={-6,-2,-1};
      for(unsigned row=0;row<3;++row) {
        uint16_t w[4];unsigned collision;
        assert(TombCapWords(s,&terrain,0,rows[row],w,&collision));
        const ActionRoomSceneFrameState frame={.brightness=15};
        uint32_t low[256],rear[256],front[256];
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned word=w[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
          const unsigned at=(((word>>10)&7)*16+ink)*2;assert(ink);
          low[y*16+x]=CastleBackingColor32(s->palette[at]|s->palette[at+1]<<8,&frame);
        }
        assert(ActionVoxel_Split(r,0,rows[row]*16,16,16,16,&frame,low,16,rear,front));
        assert(!memcmp(low,front,sizeof(low)));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
          assert(rear[y*16+x]==CastleBackingColor32(CastleBackingColor555(s,donor,x,rows[row]*16+y),&frame));
      }
      /* A real, transparent window opening remains empty. */
      const ActionRoomSceneFrameState frame={.brightness=15};
      uint32_t empty[256]={0},rear[256],front[256];
      assert(ActionVoxel_Split(r,8*16,8*16,16,16,16,&frame,empty,16,rear,front));
      for(unsigned i=0;i<256;++i)assert(!rear[i]&&!front[i]);
    }
    /* Opaque black native room filler must remain black in the rear and
     * must not become a solid front or a new extrusion. */
    for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
      for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
        uint16_t w[4];unsigned collision;
        assert(TombCapWords(s,NULL,cx,cy,w,&collision));
        if(TombCapKey(s,w,collision,0)!=UINT64_C(0xabde6e24e3e445e5))continue;
        uint32_t low[256],rear[256],front[256];
        for(unsigned i=0;i<256;++i)low[i]=UINT32_C(0xff000000);
        const ActionRoomSceneFrameState frame={.brightness=15};
        assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
        for(unsigned i=0;i<256;++i)assert(rear[i]==low[i]&&!front[i]);
      }
    assert(!memcmp(s,&immutable,sizeof(immutable)));
    ActionVoxel_Release(g);ActionVoxel_Release(r);
    printf("Castle02/%02u terrain%u: native brick backing, front/collision/priority, black matte, fade/subtract and CHR/definition renumbering preserved\n",room,region);
  }
}
