/* Complete native ochre cap motifs and the gray stair-entry rim. Fixture
 * identity follows displayed art, so all tests survive metatile/CHR remaps. */
static unsigned CastleFinishRole(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    if(key==UINT64_C(0xde8c7543b911c492)||key==UINT64_C(0xedd5fdd68866c5ae))return 1;
    if(key==UINT64_C(0xa7d7509d4a7fc9a6))return 2;
    if(key==UINT64_C(0x11b404b77de193ab))return 3;
  }
  return 0;
}
static unsigned CastleFinishCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,unsigned totals[2],int selected[2]) {
  unsigned missing=0,count;totals[0]=totals[1]=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];if(e->side||e->far_rank!=2)continue;
    const int cx=(int)floorf(e->x/16.f),cy=(int)floorf(e->y/16.f);
    uint16_t source[4];unsigned collision;if(!TombCapWords(s,terrain,cx,cy,source,&collision))continue;
    const unsigned role=CastleFinishRole(s,source,collision);if(role<1||role>2)continue;
    totals[role-1]+=e->length;
    if(!e->surface_texture||!e->top_owned_art||e->top_sample_size!=16){missing+=e->length;continue;}
    const int dx=e->top_sample_x/16,dy=e->top_sample_y/16;
    uint16_t donor[4];unsigned dc;assert(TombCapWords(s,terrain,dx,dy,donor,&dc));
    if(role==1){assert(dx==cx&&dy==cy);assert(!memcmp(source,donor,sizeof(source)));}
    else {assert(CastleFinishRole(s,donor,dc)==3);assert(abs(dx-cx)+abs(dy-cy)<=8);}
    const unsigned q=((e->y-cy*16)/8)*2+(e->x-cx*16)/8;
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art&&!e->high_priority);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned w=donor[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,w,x&7,y&7),at=(((w>>10)&7)*16+ink)*2;
      assert(ink&&!((source[q]^w)&0x3c00));
      assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
    }
    if(role==1&&selected[0]==INT_MIN){selected[0]=cx;selected[1]=cy;}
  }
  return missing;
}
static void CastleFinishEdits(const ActionRoomScene *s,int x,int y) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&a->surface_sample_size[1]==16&&a->surface_sample_x[1]==x*16&&a->surface_sample_y[1]==y*16);
  uint16_t own[256];memcpy(own,a->pixels[1],sizeof(own));
  edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=8};
  unsigned collision;assert(TombCapWords(s,NULL,x,y,edit->top_art.words,&collision));
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&!a->surface_sample_size[1]&&memcmp(own,a->pixels[1],sizeof(own)));
  edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);assert(a&&!(a->faces&kDioramaVoxelFaceMask_Top));
  *edit=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned totals[2];int selected[2]={INT_MIN,0};
  assert(!CastleFinishCheck(s,&terrain,g,totals,selected));
  terrain.pixel_layers[0].count=1;terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};
  terrain.pixel_layers[0].edits[0].transparent[0]=0x8000;
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned count;const VoxelEdge *e=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)assert(!(e[i].surface_texture&&e[i].top_sample_size==16&&e[i].top_sample_x==x*16&&e[i].top_sample_y==y*16));
  assert((g->front[(y*16-g->origin_y)*g->width+x*16-g->origin_x]&kVoxelKindMask)==0);
  ActionVoxel_Release(g);
}
static void CastleCapFinishFixtures(const char *directory) {
  if(!directory)return;bool complete=true;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene original,remapped;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned index=0;index<8;++index) {
    const unsigned group=index==7?7:2,room=index==7?3:index+2;char path[1024];
    snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,group,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    original=snapshot.scene;const ActionRoomScene *s=&snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*r=ActionVoxel_Create();assert(g&&r);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(group,room);assert(ActionVoxel_Build(g,s,NULL,settings));
    unsigned totals[2];int selected[2]={INT_MIN,0};const unsigned missing=CastleFinishCheck(s,NULL,g,totals,selected);complete&=!missing;
    printf("castle cap finish%02u/%02u terrain%u: native ochre%u gray%u missing16%u mask%016llx geometry%016llx profiles%016llx\n",
        group,room,region,totals[0],totals[1],missing,(unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height),
        (unsigned long long)TombCapGeometryHash(g),(unsigned long long)ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches)));
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(r,&remapped,NULL,settings));SameGeometry(g,r);
    assert(!memcmp(g->front,r->front,(size_t)g->width*g->height));unsigned renamed_totals[2];int none[2]={INT_MIN,0};
    assert(CastleFinishCheck(&remapped,NULL,r,renamed_totals,none)==missing&&!memcmp(totals,renamed_totals,sizeof(totals)));
    TombCapTerrain(group,room,region,&terrain);assert(ActionVoxel_Build(g,s,&terrain,settings));none[0]=INT_MIN;
    const unsigned shipped_missing=CastleFinishCheck(s,&terrain,g,totals,none);complete&=!shipped_missing;
    printf("castle cap finish%02u/%02u terrain%u: shipped ochre%u gray%u missing16%u\n",group,room,region,totals[0],totals[1],shipped_missing);
    if(!missing&&!shipped_missing&&selected[0]!=INT_MIN)CastleFinishEdits(s,selected[0],selected[1]);
    assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(g);ActionVoxel_Release(r);
  }
  DioramaLayerOrder_ClearRoom(&terrain);assert(complete);
}
