/* Exact crown source oracle, independent of generated family tables. */
static bool HeadCrownKey(const ActionRoomScene *s,const uint16_t w[4],unsigned collision) {
  const uint64_t keys[]={UINT64_C(0xeea8ad16d0e49899),UINT64_C(0x5085dd49ab8259d4),UINT64_C(0x93125d713d44ff8c),UINT64_C(0x119aafd6ec752f01)};
  for(unsigned flip=0;flip<2;++flip)for(unsigned i=0;i<4;++i)
    if(TombCapKey(s,w,collision,flip)==keys[i])return true;
  return false;
}
static const VoxelEdge *HeadTopAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned n;const VoxelEdge *e=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)if(!e[i].side&&e[i].y==y&&e[i].x<=x&&e[i].x+e[i].length>x)return &e[i];
  return NULL;
}
static uint64_t HeadColumnHash(const ActionVoxelGeometry *g,int x,int y) {
  const VoxelEdge *e=HeadTopAt(g,x,y);assert(e&&e->surface_texture==kVoxelSurfaceTextureNativeInward&&e->top_owned_art);
  const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);uint16_t col[16];
  for(unsigned d=0;d<16;++d)col[d]=art[d*16+(x&15)];return ForestBackingHash(col,sizeof(col));
}
static unsigned HeadCrownCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,const ActionVoxelGeometry *g,bool candidate,unsigned *thin) {
  unsigned n,total=0;*thin=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i) {
    const VoxelEdge *e=&edges[i];
    for(int j=0;j<e->length;++j) {
      const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0),cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
      uint16_t w[4];unsigned collision;if(!TombCapWords(s,terrain,cx,cy,w,&collision)||!HeadCrownKey(s,w,collision))continue;
      if(e->side){assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;}
      ++total;uint16_t native[16];unsigned length=0;
      /* Crown block ends before a separate eye/fire cell, even same palette. */
      for(int yy=y;yy<(cy+1)*16&&length<16;++yy) {
        const unsigned q=((yy&15)/8)*2+(x&15)/8,word=w[q],ink=RockFixtureWordPixel(s,word,x&7,yy&7);
        if(!ink)break;
        assert(ink<=7&&(word&0x3c00)==0x1000);const unsigned at=(((word>>10)&7)*16+ink)*2;
        native[length++]=(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8);
      }
      assert(length);if(length==2)++*thin;
      if(!candidate){assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;}
      assert(e->surface_texture==kVoxelSurfaceTextureNativeInward&&e->surface_derived&&e->top_owned_art&&e->top_sample_size==16);
      assert(e->near_rank==0&&e->far_rank==2&&!e->high_priority);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      assert(art[x&15]==native[0]&&art[15*16+(x&15)]==native[length-1]);
      for(unsigned d=0;d<16;++d) {
        /* Independent nearest sample at the normalized physical depth. */
        unsigned k=(unsigned)floorf((float)d*(length-1)/15.f+.5f);
        assert(art[d*16+(x&15)]==native[k]);
        if(length==2)assert(art[d*16+(x&15)]==native[d<8?0:1]);
      }
    }
  }
  return total;
}
static uint64_t HeadOtherArtHash(const ActionRoomScene *s,const DioramaRoomOverride *terrain,const ActionVoxelGeometry *g) {
  uint64_t hash=0;unsigned n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);
  for(unsigned i=0;i<n;++i)for(int j=0;j<edges[i].length;++j) {
    const VoxelEdge *e=&edges[i];const int x=e->x+((e->side&1)?0:j),y=e->y+((e->side&1)?j:0);
    uint16_t words[4];unsigned collision;
    if(!e->side&&TombCapWords(s,terrain,(int)floorf(x/16.f),(int)floorf(y/16.f),words,&collision)&&HeadCrownKey(s,words,collision))continue;
    const uint32_t v[]={x,y,e->side,e->near_rank,e->far_rank,e->high_priority,e->colliding,e->surface_texture,e->top_sample_x,e->top_sample_y,e->top_sample_size,e->surface_derived};
    hash+=ForestBackingHash(v,sizeof(v));
    if(e->top_owned_art){const uint16_t *a=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(a);hash+=ForestBackingHash(a,512);}
  }
  return hash;
}
static void HeadCrownEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,ActionVoxelGeometry *g) {
  static DioramaRoomOverride terrain;const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,6);
  const uint64_t neighbor=HeadColumnHash(base,1424,656),thin=HeadColumnHash(base,1409,656);
  for(unsigned black=0;black<2;++black) {
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];*p=(DioramaPixelEdit){.by_cell=true,.x=88,.y=41};
    if(black)p->black[1]=1u<<(15-1);else p->transparent[1]=1u<<(15-1);
    assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelEdge *e=HeadTopAt(g,1409,656);assert(e&&e->surface_texture!=kVoxelSurfaceTextureNativeInward);
    assert(HeadColumnHash(g,1424,656)==neighbor);
  }
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *c=&terrain.voxel.cells[0];*c=(DioramaVoxelCell){.x=88,.y=41,.fields=kDioramaVoxelCell_TopArt};c->top_art=(DioramaVoxelArt){.set=true,.bg=0,.size=16};unsigned collision;
  assert(TombCapWords(s,NULL,88,41,c->top_art.words,&collision));assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,88,41);
  assert(a&&a->surface&&!(a->automatic_art&(1u<<1))&&!a->surface_derived[1]);
  memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(g,s,&terrain,settings));assert(HeadColumnHash(g,1409,656)==thin&&HeadColumnHash(g,1424,656)==neighbor);
  /* Changed unknown crown art cannot inherit the exact family through an ID. */
  static ActionRoomScene unknown;unknown=*s;uint16_t uw[4];
  assert(TombCapWords(s,NULL,88,41,uw,&collision));unknown.characters[(uw[0]&1023)*32+2]^=0x40;
  assert(ActionVoxel_Build(g,&unknown,NULL,settings));const VoxelEdge *changed=HeadTopAt(g,1409,656);
  assert(changed&&changed->surface_texture!=kVoxelSurfaceTextureNativeInward);
  /* Exact native H-reflection on an isolated negative stamp. No implicit V ceiling opt-in. */
  uint16_t original[4];uint8_t id;assert(TombCapWords(s,NULL,88,41,original,&collision));assert(ActionRoomScene_LookupTile(s,1,176,82,&original[0],&id));
  for(unsigned flip=0;flip<2;++flip) {
    DioramaTileStamp stamp={.x=-2,.y=-3,.metatile=id};for(unsigned q=0;q<4;++q)stamp.words[q]=original[q^flip]^(flip?0x4000:0);
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned tips;assert(HeadCrownCheck(s,&terrain,g,true,&tips)>0&&tips==1);
  }
}
static void HeadCrownFixtures(const char *directory,bool candidate) {
  static unsigned char bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;static ActionRoomScene remapped;static DioramaRoomOverride terrain;
  const unsigned rooms[3][2]={{4,6},{4,4},{7,5}};
  for(unsigned region=0;region<3;++region)for(unsigned room=0;room<3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,rooms[room][0],rooms[room][1]);FILE*f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
    for(unsigned shipped=0;shipped<(room?1u:2u);++shipped) {
      TombCapTerrain(s->group,s->map,region,&terrain);const DioramaRoomOverride*t=shipped?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));unsigned thin;unsigned count=HeadCrownCheck(s,t,g,candidate,&thin);assert(count==(room?0u:165u));assert(thin==(room?0u:5u));
      printf("HEAD %02u%02u region%u shipped%u top%u thin%u geometry%016llx front%016llx otherart%016llx\n",s->group,s->map,region,shipped,count,thin,(unsigned long long)TombCapGeometryHash(g),(unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height),(unsigned long long)HeadOtherArtHash(s,t,g));
      if(!room&&!shipped){remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));unsigned t2;assert(HeadCrownCheck(&remapped,NULL,other,candidate,&t2)==count&&t2==thin);assert(TombCapGeometryHash(other)==TombCapGeometryHash(g));assert(!memcmp(other->front,g->front,(size_t)g->width*g->height));if(candidate)HeadCrownEdits(s,g,other);}
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(other);ActionVoxel_Release(g);
  }
}
