/* Include after TombCapWords/Key/GeometryHash and surface fixture helpers.
 * Read-only native snapshots provide the source. Baseline mode saves geometry,
 * front ownership and ALL Top art before this proposed surface-only change. */
#include "temple-native-key-arrays.inc"
static unsigned TempleNativeRole(const ActionRoomScene *s,int x,int y,
    uint16_t *word,unsigned *ink) {
  uint16_t words[4];unsigned collision;
  const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
  if(!TombCapWords(s,NULL,cx,cy,words,&collision))return 0;
  unsigned role=0;
  for(unsigned flip=0;flip<4&&!role;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    for(unsigned i=0;i<sizeof(kTempleNativeColumnKeys)/sizeof(*kTempleNativeColumnKeys);++i)
      if(key==kTempleNativeColumnKeys[i])role=1;
    for(unsigned i=0;i<sizeof(kTempleNativeLedgeKeys)/sizeof(*kTempleNativeLedgeKeys);++i)
      if(key==kTempleNativeLedgeKeys[i])role=2;
  }
  const unsigned px=x-cx*16,py=y-cy*16;
  *word=words[(py/8)*2+px/8];*ink=RockFixtureWordPixel(s,*word,px&7,py&7);
  return role;
}
static uint64_t TempleNativeTopHash(const ActionVoxelGeometry *g) {
  uint64_t sum=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=&g->edges[i];if(e->side)continue;
    assert(e->surface_texture!=2);
    const uint16_t *art=e->top_owned_art?ActionVoxel_AuthoredArt(g,e->top_owned_art-1):NULL;
    const uint64_t ink=art?ForestBackingHash(art,256*sizeof(*art)):0;
    for(int p=0;p<e->length;++p) {
      const uint64_t values[]={(uint64_t)(e->x+p),(uint64_t)e->y,e->near_rank,e->far_rank,
          e->high_priority,e->colliding,e->rear_trim,e->textured_top,e->surface_texture,
          (uint64_t)e->top_sample_x,(uint64_t)e->top_sample_y,e->top_sample_size,ink};
      sum+=ForestBackingHash(values,sizeof(values));
    }
  }
  return sum;
}
static unsigned TempleNativeSurfaceCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned owned=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=&g->edges[i];
    if(!e->side){assert(e->surface_texture!=2);continue;}
    uint16_t reference;unsigned ink;
    const unsigned role=TempleNativeRole(s,e->x,e->y,&reference,&ink);
    const bool eligible=s->group==5&&s->map>=4&&s->map<=7&&role&&
        (role==1?e->far_rank==1:e->far_rank==2)&&
        (role==1||e->side==2||(e->fold_length?e->fold_length:e->length)<=3);
    if(!eligible){assert(e->surface_texture!=2);continue;}
    /* Explicit preassigned native artwork remains authoritative. The114
     * formerly automatic8px underside pixels in05/06–07 join this same
     * inward profile instead of retaining a conflicting depth phase. */
    if(!e->surface_texture&&e->top_sample_size)continue;
    assert(e->surface_texture==2&&e->surface_derived&&e->top_sample_size==16&&e->top_owned_art);
    assert(!e->surface_top_material&&!e->contour_skin);
    assert(e->top_sample_x==(int)floorf(e->x/16.f)*16);
    assert(e->top_sample_y==(int)floorf(e->y/16.f)*16);
    if(role==1)assert(e->high_priority&&!e->colliding);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    static const int dx[]={0,-1,0,1},dy[]={1,0,-1,0};
    for(int along=0;along<e->length;++along) {
      const int x=e->x+((e->side&1)?0:along),y=e->y+((e->side&1)?along:0);
      uint16_t colors[16],first_word;unsigned first_ink;
      assert(TempleNativeRole(s,x,y,&first_word,&first_ink)==role&&first_ink&&first_ink!=13);
      unsigned count=0;
      for(unsigned d=0;d<16;++d) {
        uint16_t word;unsigned p;
        if(TempleNativeRole(s,x+dx[e->side]*(int)d,y+dy[e->side]*(int)d,&word,&p)!=role||
            !p||p==13||((word^first_word)&0x3c00))break;
        assert(SurfaceFixtureStaticWord(s,word));
        const unsigned at=(((word>>10)&7)*16+p)*2;
        colors[count++]=0x8000|s->palette[at]|s->palette[at+1]<<8;
      }
      assert(count);
      const unsigned phase=((e->side&1)?y:x)&15;
      for(unsigned d=0;d<16;++d) {
        const unsigned across=(e->side==1||e->side==2)?15-d:d;
        const unsigned at=(e->side&1)?phase*16+across:across*16+phase;
        assert(art[at]==colors[d<count?d:count-1]);
      }
      ++owned;
    }
  }
  return owned;
}
static void TempleNativeSurfaceFixtures(const char *directory) {
  if(!directory||!*directory)return;
  const char *write_path=getenv("AR_VOXEL_TEMPLE_NATIVE_SURFACE_WRITE_BASELINE");
  const char *read_path=getenv("AR_VOXEL_TEMPLE_NATIVE_SURFACE_BASELINE");
  assert(write_path||read_path);
  FILE *baseline=fopen(write_path?write_path:read_path,write_path?"w":"r");assert(baseline);
  static ActionSceneSnapshot fixture;static ActionRoomScene original,renumbered;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  const unsigned groups[]={5,5,5,5,5,7},rooms[]={4,5,6,7,8,6};
  for(unsigned region=0;region<3;++region)for(unsigned c=0;c<6;++c) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02x%02x.arscene",directory,region,groups[c],rooms[c]);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&fixture));
    ActionRoomScene *s=&fixture.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*copy=ActionVoxel_Create();assert(g&&copy);
    assert(ActionVoxel_Build(g,s,NULL,settings));
    const uint64_t geometry=TombCapGeometryHash(g),front=ForestBackingHash(g->front,(size_t)g->width*g->height),
        tops=TempleNativeTopHash(g);
    if(write_path)fprintf(baseline,"%u %u %u %016llx %016llx %016llx\n",region,groups[c],rooms[c],
        (unsigned long long)geometry,(unsigned long long)front,(unsigned long long)tops);
    else {
      unsigned rr,gg,mm;unsigned long long gg_hash,ff_hash,tt_hash;
      assert(fscanf(baseline,"%u %u %u %llx %llx %llx",&rr,&gg,&mm,&gg_hash,&ff_hash,&tt_hash)==6);
      assert(rr==region&&gg==groups[c]&&mm==rooms[c]);
      assert(geometry==gg_hash&&front==ff_hash&&tops==tt_hash);
      const unsigned count=TempleNativeSurfaceCheck(s,g);
      assert(c<4?count>0:count==0);
      renumbered=*s;Renumber(&renumbered);
      assert(ActionVoxel_Build(copy,&renumbered,NULL,settings));SameGeometry(g,copy);
      assert(TempleNativeSurfaceCheck(&renumbered,copy)==count);
      assert(!memcmp(g->front,copy->front,(size_t)g->width*g->height));
    }
    assert(!memcmp(s,&original,sizeof(original)));
    ActionVoxel_Release(g);ActionVoxel_Release(copy);
    printf("Temple native side source %02u/%02u region%u: %s\n",groups[c],rooms[c],region,
        write_path?"original geometry/front/Top baseline recorded":"boundary ink, complete inward profile, native geometry/front/Top and renumbering preserved");
  }
  assert(!ferror(baseline));fclose(baseline);
}
