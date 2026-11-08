/* BP-STAIR-TOP-01 / BP-SKULL-POST-JOIN-01. Match native art contracts,
 * then check complete exposed treads and complete connected skull heads.
 * Include after the tomb-cap and marsh-audit fixture helpers. */
static unsigned BloodpoolFindingRole(const ActionRoomScene *s,const uint16_t words[4],
    unsigned collision,bool skull) {
  static const uint64_t stairs[]={UINT64_C(0x807986aefda61dcb),UINT64_C(0x90316357de70539d),
      UINT64_C(0xabe6832ddfc68343),UINT64_C(0x96c02d91736bfd6b),UINT64_C(0xa1dc23748bb6ad31)};
  static const uint64_t heads[]={UINT64_C(0x093d2e096ae8f6d9),UINT64_C(0x4ab74649ce9b3b95),
      UINT64_C(0x3af0a0552ba2cea9),UINT64_C(0xc565c121b9e9b727),UINT64_C(0xb8e27da4fc59b969),
      UINT64_C(0x031740aeed28b2c3),UINT64_C(0x6f2690f1b8170ef3),UINT64_C(0x51ac0a04bc2b03d7)};
  const uint64_t *keys=skull?heads:stairs;const unsigned n=skull?8:5;
  for(unsigned flip=0;flip<4;++flip)for(unsigned i=0;i<n;++i)
    if(TombCapKey(s,words,collision,flip)==keys[i])return i+1;
  return 0;
}
static bool BloodpoolStairDonor(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  static const uint64_t keys[]={UINT64_C(0x11b404b77de193ab),UINT64_C(0x63961125d6c1e304),
      UINT64_C(0x771b974a513ca12e),UINT64_C(0xf0c039e0334f7623)};
  for(unsigned flip=0;flip<4;++flip)for(unsigned i=0;i<4;++i)
    if(TombCapKey(s,words,collision,flip)==keys[i])return true;
  return false;
}
static unsigned BloodpoolStairCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,unsigned *roles) {
  unsigned count,covered=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=edges+i;if(e->side||e->far_rank!=2)continue;
    /* Inspect every native cell touched by an edge; checking only its start
     * missed the landing return inside a merged24px boundary. */
    for(unsigned p=0;p<(unsigned)e->length;++p) {
      const int cx=(int)floorf((e->x+p)/16.f),cy=(int)floorf(e->y/16.f);
      uint16_t source[4];unsigned collision;
      assert(TombCapWords(s,terrain,cx,cy,source,&collision));
      const unsigned role=BloodpoolFindingRole(s,source,collision,false);if(!role)continue;
      *roles|=1u<<(role-1);++covered;
      const unsigned q=((e->y-cy*16)/8)*2+(e->x+p-cx*16)/8;
      assert(e->surface_texture==kVoxelSurfaceTexturePatch&&e->top_owned_art&&e->top_sample_size==16);
      assert(!e->contour_skin&&!e->surface_top_material&&!e->rear_fold_top&&!e->rear_fold_bottom);
      assert(!!e->high_priority==!!(source[q]&0x2000));
      assert(!!e->colliding==!!(collision&(1u<<q)));
      const int dx=e->top_sample_x/16,dy=e->top_sample_y/16;
      uint16_t donor[4];unsigned dc;assert(TombCapWords(s,terrain,dx,dy,donor,&dc));
      assert(BloodpoolStairDonor(s,donor,dc)&&abs(dx-cx)+abs(dy-cy)<=8);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned word=donor[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
        const unsigned at=(((word>>10)&7)*16+ink)*2;
        assert(ink&&!((source[q]^word)&0x3c00));
        assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
      }
    }
  }
  assert(covered==(s->map==3?96u:512u));return covered;
}
static unsigned BloodpoolSkullCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g) {
  unsigned cells=0,roles=0;
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t words[4];unsigned collision;if(!TombCapWords(s,terrain,x,y,words,&collision))continue;
      const unsigned role=BloodpoolFindingRole(s,words,collision,true);if(!role)continue;
      ++cells;roles|=1u<<(role-1);assert(!collision);
      VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,x,y,&shape));assert(!shape.authored&&shape.width==16);
      for(unsigned q=0;q<4;++q) {
        assert(!(words[q]&0x2000));
        unsigned ink=0;
        for(unsigned yy=0;yy<8;++yy)for(unsigned xx=0;xx<8;++xx) {
          const unsigned native=RockFixtureWordPixel(s,words[q],xx,yy);
          const int wx=x*16+(q&1)*8+xx,wy=y*16+(q>>1)*8+yy;
          const unsigned pixel=g->front[(wy-g->origin_y)*g->width+wx-g->origin_x];
          /* The two lower outer skull cells each have three empty native
           * quadrants. They must remain empty, not become rear solid squares. */
          assert(pixel==(native?kVoxelForeground:0));ink+=!!native;
        }
        const float expected_front=ink?.15f:.20f;
        if(fabsf(shape.front[q]-expected_front)>=.000001f||fabsf(shape.back[q]-.20f)>=.000001f)
          fprintf(stderr,"skull cell%u,%u q%u ink%u actual%.6f..%.6f expected%.6f..0.20\n",
              x,y,q,ink,shape.front[q],shape.back[q],expected_front);
        assert(fabsf(shape.front[q]-expected_front)<.000001f&&fabsf(shape.back[q]-.20f)<.000001f);
        assert(shape.front[q]>ActionVoxel_PlayRecess(g));
      }
    }
  assert(cells==24&&roles==0xff);
  const int joins[3][2]={{138,17},{241,20},{253,20}};
  for(unsigned i=0;i<3;++i) {
    const int x=joins[i][0],y=joins[i][1];VoxelCellGeometry head,post;
    /* Complete head footprint: five cells across the horns, three below.
     * The two outer horns must not detach under either camera yaw. */
    for(int row=-1;row<=0;++row)for(int col=row?-2:-1;col<=(row?2:1);++col) {
      uint16_t words[4];unsigned collision;
      assert(TombCapWords(s,terrain,x+col,y+row,words,&collision));
      assert(BloodpoolFindingRole(s,words,collision,true));
    }
    assert(ActionVoxel_CellGeometry(g,x,y,&head)&&ActionVoxel_CellGeometry(g,x,y+1,&post));
    for(unsigned q=0;q<2;++q)
      assert(head.front[q+2]==post.front[q]&&head.back[q+2]==post.back[q]);
  }
  /* Established mixed-priority thin posts and colliding decks remain intact. */
  MarshAuditPlacement(g,s);return cells;
}
static void BloodpoolSkullEditor(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited) {
  static DioramaRoomOverride terrain;DioramaLayerOrder_ClearRoom(&terrain);
  VoxelCellGeometry neighbour,after;assert(ActionVoxel_CellGeometry(base,139,17,&neighbour));
  terrain.voxel.cell_count=1;
  terrain.voxel.cells[0]=(DioramaVoxelCell){.x=138,.y=17,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,
      .front=.04f,.back=.16f,.width=10};
  assert(ActionVoxel_Build(edited,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  assert(ActionVoxel_CellGeometry(edited,138,17,&after)&&after.authored&&after.width==10);
  for(unsigned q=0;q<4;++q)assert(after.front[q]==.04f&&after.back[q]==.16f);
  assert(ActionVoxel_CellGeometry(edited,139,17,&after)&&!memcmp(&neighbour,&after,sizeof(after)));
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=138,.y=17};
  assert(ActionVoxel_Build(edited,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));SameGeometry(base,edited);
  DioramaLayerOrder_ClearRoom(&terrain);
}
static void BloodpoolSurfaceFindingFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene original,remapped;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    unsigned stair_roles=0;
    for(unsigned room=1;room<=5;room+=2) {
      char path[1024];snprintf(path,sizeof(path),"%s/%u/02%02u.arscene",directory,region,room);
      FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(bytes,1,sizeof(bytes),f);
      assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));
      const ActionRoomScene *s=&snapshot.scene;original=*s;
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,room);
      ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
      unsigned covered[2];
      for(unsigned shipped=0;shipped<2;++shipped) {
        TombCapTerrain(2,room,region,&terrain);const DioramaRoomOverride *t=shipped?&terrain:NULL;
        assert(ActionVoxel_Build(g,s,t,settings));
        covered[shipped]=room==1?BloodpoolSkullCheck(s,t,g):BloodpoolStairCheck(s,t,g,&stair_roles);
        printf("Bloodpool finding02/%02u region%u %s: covered%u mask%016llx geometry%016llx\n",
            room,region,shipped?"shipped":"native",covered[shipped],
            (unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height),
            (unsigned long long)TombCapGeometryHash(g));
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      assert(covered[0]==covered[1]);assert(ActionVoxel_Build(g,s,NULL,settings));
      remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
      const unsigned renamed=room==1?BloodpoolSkullCheck(&remapped,NULL,other):BloodpoolStairCheck(&remapped,NULL,other,&stair_roles);
      assert(renamed==covered[0]);SameGeometry(g,other);
      assert(!memcmp(g->front,other->front,(size_t)g->width*g->height));
      if(room==1)BloodpoolSkullEditor(s,g,other);
      assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(g);ActionVoxel_Release(other);
    }
    assert(stair_roles==0x1f);
  }
}
