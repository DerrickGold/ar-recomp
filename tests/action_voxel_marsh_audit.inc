/* Authored coordinates select visible native contracts; every assertion is
 * repeated after metatile and CHR renumbering. No runtime tile IDs are used. */
static void MarshAuditRecess(const ActionVoxelGeometry *g,const ActionRoomScene *s,
    int x,int y,bool high,float recess) {
  uint16_t word;
  assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,NULL));
  assert(!!(word&0x2000)==high);
  assert(fabsf(ActionVoxel_FrontRecess(g,x/8,y/8)-recess)<.00001f);
}

static void MarshAuditPlacement(const ActionVoxelGeometry *g,const ActionRoomScene *s) {
  const float rear=.15f;
  assert(fabsf(ActionVoxel_SceneryRecess(g)-rear)<.00001f);
  assert(fabsf(ActionVoxel_PlayRecess(g)-.065f)<.00001f);
  /* One native metatile carries both front and rear posts. Preserve its
   * independent priorities while clearing the actor's walking depth. */
  MarshAuditRecess(g,s,1984,256,true,0);
  MarshAuditRecess(g,s,1992,256,false,rear);
  MarshAuditRecess(g,s,1984,272,true,0);
  MarshAuditRecess(g,s,1992,272,false,rear);
  MarshAuditRecess(g,s,1904,256,true,0);
  MarshAuditRecess(g,s,1912,256,false,rear);
  /* The fence and cross braces were all low priority in the native scene. */
  MarshAuditRecess(g,s,2000,352,false,rear);
  MarshAuditRecess(g,s,2008,352,false,rear);
  MarshAuditRecess(g,s,2016,368,false,rear);
  MarshAuditRecess(g,s,1920,256,false,rear);
  MarshAuditRecess(g,s,1936,272,false,rear);
  /* The gray skull-pole collar is timber assembly artwork, despite its
   * stone-colored palette. Crown, collar and shaft must share the rear plane. */
  MarshAuditRecess(g,s,736,224,false,rear);
  MarshAuditRecess(g,s,744,240,false,rear);
  MarshAuditRecess(g,s,744,256,false,rear);
  MarshAuditRecess(g,s,1456,192,false,rear);
  MarshAuditRecess(g,s,1464,208,false,rear);
  MarshAuditRecess(g,s,1464,224,false,rear);
  /* At the post/deck intersection, the actual colliding top stays full depth.
   * Only the noncolliding low-priority post below it moves to the back. */
  MarshAuditRecess(g,s,1984,288,true,0);
  MarshAuditRecess(g,s,1992,288,false,0);
  MarshAuditRecess(g,s,1984,296,true,0);
  MarshAuditRecess(g,s,1992,296,false,rear);
  unsigned count,beam_caps=0,front_posts=0,rear_posts=0;
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->x<1984 || e->x>=2000)continue;
    if(e->y>=288 && e->y<296 && e->far_rank==2 && e->side==0) {
      VoxelEdgeSpan span;
      assert(e->colliding && !e->rear_mounted);
      assert(ActionVoxel_ResolveEdgeSpan(g,e,e->high_priority?kVoxelEdgeHighDeckBehind:
          kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
      assert(span.near_depth<=ActionVoxel_PlayRecess(g) &&
          span.far_depth>ActionVoxel_PlayRecess(g));
      ++beam_caps;
    }
    if(e->y>=296 && e->y<336 && e->far_rank==1) {
      assert(!e->colliding);
      VoxelEdgeSpan span;
      assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
      if(e->high_priority) {
        assert(e->thin_trim && !e->rear_mounted);
        assert(fabsf(span.near_depth)<.00001f && fabsf(span.far_depth-.02f)<.00001f);
        ++front_posts;
      } else {
        assert(!e->thin_trim && e->rear_mounted);
        assert(fabsf(span.near_depth-.15f)<.00001f && fabsf(span.far_depth-.20f)<.00001f);
        ++rear_posts;
      }
    }
  }
  assert(beam_caps && front_posts && rear_posts);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->thin_trim)assert(e->high_priority && !e->colliding && e->far_rank==1 && !e->rear_mounted);
    if(e->colliding || (e->x<176 && e->y>=368 && e->y<448))assert(!e->thin_trim);
  }
}

static void MarshAuditFold(const ActionVoxelGeometry *g) {
  /* The two sloping rock feet also contain a painted earth band. It joins
   * the adjacent mixed rock/ground, flat and rounded ground donors. */
  const int donor_columns[]={112,128,144,160,176,864,880};
  enum { donors=sizeof(donor_columns)/sizeof(donor_columns[0]) };
  unsigned found[donors]={0},grass=0,rocks=0,undersides=0,bridge=0;
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side==0 && e->y==448 && e->far_rank==2) {
      for(unsigned j=0;j<donors;++j)if(e->x>=donor_columns[j] && e->x<donor_columns[j]+16) {
        assert(e->rear_fold_top==8 && e->colliding && !e->high_priority);
        assert(e->x+e->length<=donor_columns[j]+16);
        const float contact=ActionVoxel_PlayRecess(g)-.01f;
        assert(ActionVoxel_RearFoldOffset(e->rear_fold_top,contact,contact,ActionVoxel_Depth(g))==0);
        assert(ActionVoxel_RearFoldOffset(e->rear_fold_top,ActionVoxel_Depth(g),contact,ActionVoxel_Depth(g))==8);
        ++found[j];
      }
      if(e->x<112) {assert(!e->rear_fold_top);++grass;}
    }
    if(e->far_rank==1 && e->x<128 && e->y>=368 && e->y<448) {
      assert(!e->rear_fold_top && !e->rear_fold_bottom);++rocks;
    }
    if(e->side==2) {assert(!e->rear_fold_top && !e->rear_fold_bottom);++undersides;}
    if(e->high_priority && e->colliding && e->side==0) {
      assert(!e->rear_fold_top && !e->rear_fold_bottom);++bridge;
    }
  }
  for(unsigned j=0;j<donors;++j)assert(found[j]);
  assert(grass && rocks && undersides && bridge);
}

static uint32_t MarshAuditNativeColor(const ActionRoomScene *s,int x,int y) {
  uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,NULL));
  const unsigned index=((word>>10)&7)*16+FixturePixel(s,x,y);
  const unsigned color=s->palette[index*2]|s->palette[index*2+1]<<8;
  unsigned rgb=0;
  for(unsigned channel=0;channel<3;++channel) {
    const unsigned v=(color>>(channel*5))&31;
    rgb|=((v<<3)|(v>>2))<<(16-channel*8);
  }
  return 0xff000000u|rgb;
}
static void MarshAuditSlopingRockGround(const ActionVoxelGeometry *g,const ActionRoomScene *s) {
  const int columns[]={112,144};
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256];
  for(unsigned i=0;i<256;++i)low[i]=0xff030507;
  for(unsigned c=0;c<2;++c) {
    const int wx=columns[c],wy=432;
    assert(ActionVoxel_Split(g,wx,wy,16,16,16,&frame,low,16,rear,front));
    unsigned ground=0,shadow=0;
    for(unsigned y=8;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned p=FixturePixel(s,wx+x,wy+y),i=y*16+x;
      assert(p); /* This lower strip is authored opaque ground/shadow. */
      if((0x1fc2u>>p)&1u) {
        assert(rear[i]==low[i] && !front[i]);++ground;
      } else {
        assert(p==14 && front[i]==low[i]);
        assert(rear[i]==MarshAuditNativeColor(s,160+x,432+y));++shadow;
      }
    }
    assert(ground && shadow);
    /* No horizontal shift or rear placement is applied to the rock itself. */
    for(unsigned q=0;q<4;++q)
      assert(ActionVoxel_FrontRecess(g,wx/8+(q&1),wy/8+(q>>1))==0);
  }
}

static size_t MarshAuditMapCell(const ActionRoomScene *s,unsigned x,unsigned y) {
  return ((y/16)*s->bg[0].pages_wide+x/16)*256+(y%16)*16+x%16;
}
static void MarshAuditRockBackingEdits(const ActionRoomScene *s) {
  static DioramaRoomOverride terrain;
  static ActionRoomScene changed;
  memset(&terrain,0,sizeof(terrain));
  DioramaPixelLayerOverride *layer=&terrain.pixel_layers[0];layer->count=1;
  DioramaPixelEdit *edit=&layer->edits[0];
  *edit=(DioramaPixelEdit){.by_cell=true,.x=144/16,.y=432/16};
  assert(FixturePixel(s,144,440)==14);
  const ActionRoomSceneFrameState frame={.brightness=15};
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  uint32_t low[256],rear[256],front[256];
  for(unsigned i=0;i<256;++i)low[i]=0xff030507;
  /* Target transparency is a real hole; painting it black is explicit art.
   * Neither may be replaced by the inferred ground donor. */
  edit->transparent[8]=0x8000;low[128]=0;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,144,432,16,16,16,&frame,low,16,rear,front));
  assert(!rear[128] && !front[128]);
  edit->transparent[8]=0;edit->black[8]=0x8000;low[128]=0xff000000;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,144,432,16,16,16,&frame,low,16,rear,front));
  assert(rear[128]==0xff000000 && front[128]==low[128]);
  memset(&terrain,0,sizeof(terrain));layer->count=2;low[128]=0xff030507;
  for(unsigned i=0;i<2;++i) {
    layer->edits[i]=(DioramaPixelEdit){.by_cell=true,.x=10+(int)i,.y=27};
    for(unsigned y=0;y<16;++y)layer->edits[i].transparent[y]=0xffff;
  }
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,144,432,16,16,16,&frame,low,16,rear,front));
  assert(!rear[128] && front[128]==low[128]);
  /* No compatible donor, or no uninterrupted colliding earth below it:
   * both leave the source shadow intact and decline invented backing. */
  const uint8_t empty=s->bg[0].map[MarshAuditMapCell(s,12,27)];
  for(unsigned unsupported=0;unsupported<2;++unsupported) {
    changed=*s;
    for(unsigned cx=10;cx<=11;++cx)
      changed.bg[0].map[MarshAuditMapCell(s,cx,unsupported?28:27)]=empty;
    assert(ActionVoxel_Build(g,&changed,NULL,settings));
    assert(ActionVoxel_Split(g,144,432,16,16,16,&frame,low,16,rear,front));
    assert(!rear[128] && front[128]==low[128]);
  }
  ActionVoxel_Release(g);
}

static void MarshAuditWetPosts(const ActionVoxelGeometry *g,const ActionRoomScene *s) {
  /* Coordinates select five reviewed mixed materials, independent of CHR
   * numbering. Each row must select exactly its low-priority wood ink. */
  static const struct {int x,y;uint16_t colors[4];} samples[]={
    {1904,480,{0,0x003e,0,0}}, {592,480,{0x0066,0x006c,0,0}},
    {224,480,{0,0x003e,0,0}}, {240,480,{0x000e,0,0,0}},
    {1664,480,{0x003e,0x000e,0,0}}
  };
  for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i)
    for(unsigned q=0;q<4;++q) {
      const int x=samples[i].x+(q&1)*8,y=samples[i].y+(q>>1)*8;
      const uint8_t *rows=ActionVoxel_PixelRecessRows(g,x/8,y/8);
      uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,NULL));
      assert(!!rows==!!samples[i].colors[q]);
      assert(ActionVoxel_FrontRecess(g,x/8,y/8)==0);
      if(!rows)continue;
      assert(!(word&0x2000));
      for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
        const unsigned p=FixturePixel(s,x+px,y+py);
        assert(!!(rows[py]&(1u<<(7-px)))==!!(samples[i].colors[q]&(1u<<p)));
        if(p>=8)assert(!(rows[py]&(1u<<(7-px))));
      }
    }
  /* Shoreline matte also uses index 1. Sharing that ink must not create a
   * post mask: the contract is reviewed material identity, never color. */
  assert(!ActionVoxel_PixelRecessRows(g,864/8,480/8));
  assert(!ActionVoxel_PixelRecessRows(g,864/8+1,480/8));
  unsigned cells=0,pixels=0;
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1);++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1);++x) {
      const uint8_t *rows=ActionVoxel_PixelRecessRows(g,x,y);
      if(!rows)continue;
      uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x,y,&word,NULL));
      assert(!(word&0x2000));++cells;
      for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px)
        if(rows[py]&(1u<<(7-px))) {
          const unsigned p=FixturePixel(s,x*8+px,y*8+py);
          assert(p && p<8);++pixels;
        }
    }
  assert(cells==63 && cells<=kVoxelMaxPixelRecessCells && pixels);
  assert(!ActionVoxel_PixelRecessRows(g,INT_MIN,INT_MAX));
  assert(!ActionVoxel_PixelRecessRows(NULL,0,0));
  printf("marsh mixed post placement: %u cells, %u wood pixels; water/high priority unchanged\n",cells,pixels);
}

static void MarshAuditWetPostEdits(const ActionRoomScene *s) {
  static DioramaRoomOverride terrain;
  memset(&terrain,0,sizeof(terrain));
  DioramaPixelLayerOverride *layer=&terrain.pixel_layers[0];layer->count=1;
  DioramaPixelEdit *edit=&layer->edits[0];
  *edit=(DioramaPixelEdit){.by_cell=true,.x=1904/16,.y=480/16};
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_PixelRecessRows(g,1912/8,480/8));
  /* Neither explicit transparent pixels nor a newly painted black face
   * inherit the original wood's placement. Unedited adjacent posts do. */
  for(unsigned y=0;y<8;++y)edit->transparent[y]=0x00ff;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(!ActionVoxel_PixelRecessRows(g,1912/8,480/8));
  assert(ActionVoxel_PixelRecessRows(g,1944/8,480/8));
  memset(edit->transparent,0,sizeof(edit->transparent));
  for(unsigned y=0;y<8;++y)edit->black[y]=0x00ff;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(!ActionVoxel_PixelRecessRows(g,1912/8,480/8));
  assert(ActionVoxel_PixelRecessRows(g,1944/8,480/8));
  ActionVoxel_Release(g);
}

static void MarshAuditFrontPostEdits(const ActionRoomScene *s) {
  static DioramaRoomOverride terrain;
  memset(&terrain,0,sizeof(terrain));
  DioramaPixelLayerOverride *layer=&terrain.pixel_layers[0];layer->count=1;
  DioramaPixelEdit *edit=&layer->edits[0];
  *edit=(DioramaPixelEdit){.by_cell=true,.x=1984/16,.y=256/16};
  /* Erase only the high-priority half of a real mixed post. Its low-priority
   * half must keep the rear placement, with no stray thin front contour. */
  for(unsigned y=0;y<16;++y)edit->transparent[y]=0xff00;
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  MarshAuditRecess(g,s,1992,256,false,.15f);
  unsigned count,rear=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(!(e->side&1) || e->y>=272 || e->y+e->length<=256)continue;
    if(e->x>=1984 && e->x<1992)assert(!e->thin_trim);
    if(e->x>=1992 && e->x<2000 && e->far_rank==1 && !e->high_priority) {
      assert(e->rear_mounted && !e->thin_trim);++rear;
    }
  }
  assert(rear);
  ActionVoxel_Release(g);
}

static unsigned MarshAuditFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0201-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    assert(ActionVoxel_Build(a,&fixture.scene,NULL,ActionVoxel_SettingsForRoom(2,1)));
    MarshAuditPlacement(a,&fixture.scene);MarshAuditFold(a);MarshRockJoins(a);
    MarshAuditSlopingRockGround(a,&fixture.scene);
    MarshAuditRockBackingEdits(&fixture.scene);
    MarshAuditWetPosts(a,&fixture.scene);MarshAuditWetPostEdits(&fixture.scene);
    MarshAuditFrontPostEdits(&fixture.scene);
    remapped=fixture.scene;Renumber(&remapped);
    assert(ActionVoxel_Build(b,&remapped,NULL,ActionVoxel_SettingsForRoom(2,1)));
    SameGeometry(a,b);MarshAuditPlacement(b,&remapped);MarshAuditFold(b);MarshRockJoins(b);
    MarshAuditSlopingRockGround(b,&remapped);
    MarshAuditRockBackingEdits(&remapped);
    MarshAuditWetPosts(b,&remapped);MarshAuditWetPostEdits(&remapped);
    MarshAuditFrontPostEdits(&remapped);
    for(unsigned y=0;y<ActionRoomScene_TileHeight(&remapped,1);++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(&remapped,1);++x) {
        const uint8_t *ar=ActionVoxel_PixelRecessRows(a,x,y),*br=ActionVoxel_PixelRecessRows(b,x,y);
        assert(!!ar==!!br);if(ar)assert(!memcmp(ar,br,8));
      }
    ActionVoxel_Release(a);ActionVoxel_Release(b);
    printf("marsh audit terrain %u: native-priority posts, rear fences and intact beam contact\n",region);
  }
  return 3;
}
