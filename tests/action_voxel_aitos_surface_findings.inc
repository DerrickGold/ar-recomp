/* A04-RAMP-MAPPING/A04-LAVA-CAPS: exact source rays, not merely a textured
 * flag. Art identities survive CHR/metatile renumbering and editor stamps. */
static unsigned AitosFindingRole(const ActionRoomScene *s,const uint16_t w[4],unsigned collision,unsigned *orientation) {
  static const uint64_t slopes[]={UINT64_C(0x053f6a8ec9540500),UINT64_C(0x9223e06babb1820e),
    UINT64_C(0xa0ed90eb4ba0a159),UINT64_C(0xae36a9383c002bfe),UINT64_C(0xbeade6e9e1c86aa7),
    UINT64_C(0xcdb3bdaf2c77a9f2),UINT64_C(0xecb36bec959efcb7)};
  static const uint64_t feet[]={UINT64_C(0x1f484a1a26f601d0),UINT64_C(0x241696c6f57f22ff),
    UINT64_C(0x8af429da16ff6e27),UINT64_C(0x9dc90cfebc192875),UINT64_C(0xfdb176965bc3cc3d),
    UINT64_C(0x082b9c553e579e91),UINT64_C(0x3403e18c5df5595b),
    UINT64_C(0x6370882a4dc539c6),UINT64_C(0xdecb5803708dcde7)};
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,w,collision,flip);unsigned role=0;
    for(unsigned i=0;i<sizeof(slopes)/sizeof(*slopes);++i)if(key==slopes[i])role=1;
    for(unsigned i=0;i<sizeof(feet)/sizeof(*feet);++i)if(key==feet[i])role=2;
    if(key==UINT64_C(0xe639f96cef86d105)||key==UINT64_C(0x447212ab17d7f93c))role=3;
    if(key==UINT64_C(0x1dfca2cdbf743810)||key==UINT64_C(0x4c7fc75f685114e3))role=4;
    if(key==UINT64_C(0xfcd63670ed8a0a20))role=5;
    if(role){if(orientation)*orientation=flip;return role;}
  }
  return 0;
}
static unsigned AitosFindingSource(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *color,unsigned *attrs,unsigned *flip) {
  uint16_t w[4];unsigned collision;
  if(!TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),w,&collision))return 0;
  const unsigned q=((y&15)/8)*2+(x&15)/8,word=w[q],ink=RockFixtureWordPixel(s,word,x&7,y&7);
  if(!ink)return 0;
  const unsigned at=(((word>>10)&7)*16+ink)*2;
  if(color)*color=(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8);
  if(attrs)*attrs=word&0x3c00;
  unsigned role=AitosFindingRole(s,w,collision,flip);
  if(!role&&CavernSurfaceRole(s,w,collision))role=6;
  return role;
}
static bool AitosFindingHorizontalRock(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  static const uint64_t keys[]={UINT64_C(0x3696a52f0ee6375d),UINT64_C(0x5cd4a29a48756824),
    UINT64_C(0xaff945b74fb849d8),UINT64_C(0xf423238f83d717f8)};
  for(unsigned flip=0;flip<4;++flip)for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)
    if(TombCapKey(s,words,collision,flip)==keys[i])return true;
  return false;
}
static void AitosFindingDonorPixels(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,const VoxelEdge *e,unsigned reference) {
  assert(e->surface_texture==kVoxelSurfaceTexturePatch&&!e->surface_derived&&e->top_owned_art&&e->top_sample_size==16);
  assert(!(e->top_sample_x&15)&&!(e->top_sample_y&15));
  uint16_t words[4];unsigned collision;
  assert(TombCapWords(s,terrain,e->top_sample_x/16,e->top_sample_y/16,words,&collision));
  assert(AitosFindingHorizontalRock(s,words,collision));
  const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned word=words[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
    const unsigned at=(((word>>10)&7)*16+ink)*2;
    assert((word&0x3c00)==reference&&ink&&SurfaceFixtureStaticWord(s,word));
    assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
}
static void AitosFindingCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,unsigned totals[5]) {
  memset(totals,0,5*sizeof(*totals));unsigned n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  const int dx[4]={0,-1,0,1},dy[4]={1,0,-1,0};
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=&edges[i];if(e->far_rank!=2)continue;
    for(int j=0;j<e->length;++j) {
      const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0);
      unsigned flip=0,reference=0;uint16_t first=0;
      const unsigned role=AitosFindingSource(s,terrain,x,y,&first,&reference,&flip);
      const unsigned span=e->fold_length?e->fold_length:e->length;
      const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForEdge(g,e);
      const bool ramp_return=curve && curve->contact_count==8;
      const bool receiver=(role==1&&(!(e->side&1)||((e->side&1)&&(span<=2||ramp_return)))) ||
          (role==2&&(!(e->side&1)||ramp_return)) || (role==3&&e->side==((flip&2)?2:0)) || (role==4&&e->side==2);
      if(!receiver)continue;
      ++totals[role-1];
      if(role<=2&&e->side==2)++totals[4];
      if(role<=2) {
        if(e->top_sample_size==16)AitosFindingDonorPixels(s,terrain,g,e,reference);
        else {
          /* Reviewed support faces may cross the native snow/rock palette
           * boundary, where no compatible horizontal16px donor exists.
           * Require the exact opaque, matching-palette source quadrant. */
          assert(role==2 && e->surface_texture==kVoxelSurfaceTexturePatch &&
              !e->surface_derived && e->top_owned_art && e->top_sample_size==8);
          const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
          assert(e->top_sample_x==cx*16+((x&15)/8)*8 &&
              e->top_sample_y==cy*16+((y&15)/8)*8);
          uint16_t words[4];unsigned collision;assert(TombCapWords(s,terrain,cx,cy,words,&collision));
          const unsigned q=((y&15)/8)*2+(x&15)/8;
          assert((words[q]&0x3c00)==reference && SurfaceFixtureStaticWord(s,words[q]));
          const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
          TombOrnamentCrop(s,words,cx,cy,e->top_sample_x,e->top_sample_y,art);
          for(unsigned p=0;p<64;++p)assert(art[p]&0x8000);
        }
        if((e->side&1)&&span<=2)assert(e->surface_top_material); /* Short walking risers share top X phase. */
        continue;
      }
      if(e->surface_texture!=kVoxelSurfaceTextureNativeInward||!e->surface_derived||!e->top_owned_art) {
        fprintf(stderr,"aitos finding missing %02u/%02u role%u at%d,%d side%u span%u texture%u\n",
          s->group,s->map,role,x,y,e->side,span,e->surface_texture);assert(false);
      }
      assert(e->top_sample_size==16&&!!e->high_priority==!!(reference&0x2000));
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      uint16_t native[16];unsigned length=0;
      for(int d=0;d<16;++d) {
        const int xx=x+dx[e->side]*d,yy=y+dy[e->side]*d;
        if(role==3) {
          const int row=(flip&2)?15-(yy&15):(yy&15);
          if((int)floorf(yy/16.f)!=(int)floorf(y/16.f)||row>=7)break;
        }
        uint16_t color=0;unsigned attrs=0;
        const unsigned source=AitosFindingSource(s,terrain,xx,yy,&color,&attrs,NULL);
        if(!source||attrs!=reference)break;
        if(role<=2) {
          if(source!=1&&source!=2)break;
          const unsigned kind=g->front[(yy-g->origin_y)*g->width+xx-g->origin_x]&kVoxelKindMask;
          if(source==1&&kind!=kVoxelSolid)break;
        } else if(role==3&&source!=3)break;
        else if(role==4&&source!=4&&source!=6)break;
        native[length++]=color;
      }
      assert(length&&native[0]==first);
      if(role==3)assert(length<=7);
      for(unsigned d=0;d<16;++d) {
        const unsigned k=role==3?(unsigned)floorf(d*(length-1)/15.f+.5f):(d<length?d:length-1);
        const unsigned along=(unsigned)((e->side&1)?y:x)&15;
        const unsigned at=!e->side?d*16+along:e->side==1?along*16+15-d:
            e->side==2?(15-d)*16+along:along*16+d;
        if(art[at]!=native[k])fprintf(stderr,"aitos finding ink %02u/%02u role%u at%d,%d side%u depth%u got%04x expected%04x length%u\n",
          s->group,s->map,role,x,y,e->side,d,art[at],native[k],length);
        assert(art[at]==native[k]);
      }
    }
  }
}
static const VoxelEdge *AitosFindingTopAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)if(!e[i].side&&e[i].y==y&&x>=e[i].x&&x<e[i].x+e[i].length)return &e[i];
  return NULL;
}
static void AitosRampDonorEdits(const ActionRoomScene *s,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;static ActionRoomScene changed;
  DioramaTileStamp stamps[2]={{.x=-3,.y=-2},{.x=-1,.y=-2}};
  bool found[2]={false,false};unsigned collisions[2]={0};
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
      unsigned which=2;
      if(!found[0]&&AitosFindingRole(s,words,collision,NULL)==1)which=0;
      if(!found[1]&&AitosFindingHorizontalRock(s,words,collision))which=1;
      if(which==2)continue;
      uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&stamps[which].metatile));
      memcpy(stamps[which].words,words,sizeof(words));collisions[which]=collision;found[which]=true;
    }
  assert(found[0]&&found[1]&&collisions[1]==15);
  const DioramaTileStamp original=stamps[1];
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,1);
  for(unsigned variant=0;variant<8;++variant) {
    memset(&terrain,0,sizeof(terrain));stamps[1]=original;changed=*s;
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-3,.y0=-2,
      .x1=0,.y1=-1,.cells=stamps,.count=2};
    const ActionRoomScene *source=s;
    if(variant==1||variant==2) {
      terrain.pixel_layers[0].count=1;DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
      *p=(DioramaPixelEdit){.by_cell=true,.x=-1,.y=-2};
      if(variant==1)p->transparent[8]=1u<<7;else p->black[8]=1u<<7;
    } else if(variant==3)stamps[1].blank=true;
    else if(variant==4||variant==5)for(unsigned q=0;q<4;++q)
      stamps[1].words[q]^=variant==4?0x2000:0x0400;
    else if(variant==6||variant==7) {
      /* Move only the donor CHRs into a distinct low bank. Its semantic
       * picture survives addresses, then unknown ink/animation must reject it. */
      for(unsigned q=0;q<4;++q) {
        const unsigned chr=original.words[q]&1023;
        memcpy(changed.characters+q*32,s->characters+chr*32,32);
        stamps[1].words[q]=(original.words[q]&~1023u)|q;
        for(unsigned r=0;r<4;++r)assert((stamps[0].words[r]&1023)>=4);
      }
      source=&changed;
      if(variant==6)changed.characters[0]^=0x80;
      else {changed.have_video_profile=true;changed.video_profile[23]=0x12;changed.video_profile[24]=1;
        assert(ActionRoomScene_HasCharacterAnimation(&changed));}
    }
    assert(ActionVoxel_Build(g,source,&terrain,settings));unsigned n,receivers=0;
    const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
    for(unsigned i=0;i<n;++i) {
      const VoxelEdge *e=&edges[i];
      if(e->x<-48||e->x>=-32||e->y<-32||e->y>=-16||e->far_rank!=2||
          ((e->side&1)&&(e->fold_length?e->fold_length:e->length)>2))continue;
      ++receivers;
      if(!variant) {
        assert(e->top_sample_x==-16&&e->top_sample_y==-32);
        uint16_t words[4];unsigned collision;assert(TombCapWords(source,&terrain,-3,-2,words,&collision));
        const unsigned q=(((unsigned)e->y&15)/8)*2+((unsigned)e->x&15)/8;
        AitosFindingDonorPixels(source,&terrain,g,e,words[q]&0x3c00);
      } else {
        /* No qualified horizontal donor remains. The existing underside
         * fallback may still own a safe8px crop of the receiver itself;
         * it must never reuse the rejected donor or invent another16px cap. */
        if(e->top_owned_art) {
          assert(e->top_sample_size==8&&!e->surface_derived);
          assert(e->top_sample_x>=-48&&e->top_sample_x+8<=-32&&
              e->top_sample_y>=-32&&e->top_sample_y+8<=-16);
          const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);
          TombOrnamentCrop(source,stamps[0].words,-3,-2,e->top_sample_x,e->top_sample_y,art);
        }
      }
    }
    assert(receivers);
  }
}
static void AitosFindingEdits(const ActionRoomScene *s,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;
  static ActionRoomScene reflected;
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  /* Native floating cap at33,37: a pixel edit in its inward source invalidates
   * that ray; explicit artwork wins and clearing the edit restores the cap. */
  const int cx=33,cy=37;uint16_t words[4];unsigned collision;uint8_t id;
  assert(TombCapWords(s,NULL,cx,cy,words,&collision));
  assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&words[0],&id));
  assert(AitosFindingRole(s,words,collision,NULL)==3);
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_TopArt};
  cell->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};memcpy(cell->top_art.words,words,sizeof(words));
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,cx,cy);
  assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_derived[1]);
  for(unsigned black=0;black<2;++black) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[0];*edit=(DioramaPixelEdit){.by_cell=true,.x=cx,.y=cy};
    if(black)edit->black[4]=1u<<(15-8);else edit->transparent[4]=1u<<(15-8);
    assert(ActionVoxel_Build(g,s,&terrain,settings));
    const VoxelEdge *edge=AitosFindingTopAt(g,cx*16+8,cy*16);
    assert(edge&&edge->surface_texture!=kVoxelSurfaceTextureNativeInward);
  }
  /* Both mirrors keep the stone/glow boundary attached to the original art.
   * Negative isolated stamps also rule out room-coordinate special cases. */
  for(unsigned flip=0;flip<4;++flip) {
    DioramaTileStamp stamp={.x=-2,.y=-3,.metatile=id};
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1)?0x4000:0)^((flip&2)?0x8000:0);
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){
      .set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    const ActionRoomScene *source=s;
    if(flip&2) {
      /* A stamp that flips artwork alone deliberately keeps gameplay
       * collision. It must not impersonate a fully reflected semantic cap. */
      assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned count;
      const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
      for(unsigned i=0;i<count;++i)assert(edges[i].surface_texture!=kVoxelSurfaceTextureNativeInward);
      reflected=*s;
      for(unsigned q=0;q<4;++q)reflected.bg[0].metatiles[id*8+q*2]=
          (reflected.bg[0].metatiles[id*8+q*2]&~2u)|((collision>>(q^flip)&1u)<<1);
      source=&reflected;
    }
    assert(ActionVoxel_Build(g,source,&terrain,settings));unsigned totals[5];AitosFindingCheck(source,&terrain,g,totals);
    assert(totals[2]==16);
  }
  memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(g,s,&terrain,settings));
  unsigned totals[5];AitosFindingCheck(s,&terrain,g,totals);assert(totals[2]);
}
static void AitosSurfaceFindingsFixtures(const char *directory) {
  if(!directory)return;
  static unsigned char bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene remapped;static DioramaRoomOverride terrain;
  const unsigned rooms[][2]={{4,1},{4,4},{4,5},{4,6},{4,7},{7,5}};
  for(unsigned region=0;region<3;++region)for(unsigned room=0;room<sizeof(rooms)/sizeof(*rooms);++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,rooms[room][0],rooms[room][1]);
    FILE *f=fopen(path,"rb");assert(f);size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    for(unsigned shipped=0;shipped<2;++shipped) {
      TombCapTerrain(s->group,s->map,region,&terrain);const DioramaRoomOverride *t=shipped?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));unsigned totals[5];AitosFindingCheck(s,t,g,totals);
      assert(totals[0]+totals[1]+totals[2]+totals[3]);
      /* The bounded ramp underside now returns to the rear plane, so it has
       * no rectangular bottom face. Its owned tapered seams are checked by
       * the same source oracle above. */
      if(!room)assert(!totals[4] && g->column_curve_count==40);
      printf("aitos finding %02u/%02u region%u shipped%u ramp%u foot%u cap%u underside%u ramp_bottom%u geometry%016llx front%016llx\n",
        s->group,s->map,region,shipped,totals[0],totals[1],totals[2],totals[3],totals[4],
        (unsigned long long)TombCapGeometryHash(g),(unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height));
      if(!shipped) {
        remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));unsigned remap_totals[5];
        AitosFindingCheck(&remapped,NULL,other,remap_totals);assert(!memcmp(totals,remap_totals,sizeof(totals)));
        assert(TombCapGeometryHash(other)==TombCapGeometryHash(g));
        assert(other->width==g->width&&other->height==g->height&&!memcmp(other->front,g->front,(size_t)g->width*g->height));
      }
    }
    if(!room)AitosRampDonorEdits(s,other);
    if(room==1)AitosFindingEdits(s,other);
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
  DioramaLayerOrder_ClearRoom(&terrain);
}
