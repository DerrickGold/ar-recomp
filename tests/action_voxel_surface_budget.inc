/* These are the exposed source pixels that lost optional artwork when
 * fallback-only8px subdivisions exhausted the room's edge budget. */
static const VoxelEdge *SurfaceBudgetLeftAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)if(edges[i].side==3 && edges[i].x==x &&
      edges[i].y<=y && y<edges[i].y+edges[i].length)return edges+i;
  return NULL;
}
static void SurfaceBudgetCrop(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const uint16_t *art,int sx,int sy,unsigned size,const DioramaPixelEdit *paint) {
  assert(art && size==8);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const int wx=sx+x%size,wy=sy+y%size;
    uint16_t words[4];unsigned collision;
    assert(TombCapWords(s,terrain,wx/16,wy/16,words,&collision));
    const uint16_t word=words[((wy&15)/8)*2+(wx&15)/8];
    const unsigned ink=RockFixtureWordPixel(s,word,wx&7,wy&7),index=(((word>>10)&7)*16+ink)*2;
    assert(ink && SurfaceFixtureStaticWord(s,word));
    assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[index]|s->palette[index+1]<<8));
    if(paint && wx/16==paint->x && wy/16==paint->y)
      assert(!((paint->black[wy&15]|paint->transparent[wy&15])&(1u<<(15-(wx&15)))));
  }
}
static void SurfaceBudgetTextured(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,int x,int y,bool textured,const DioramaPixelEdit *paint) {
  const VoxelEdge *e=SurfaceBudgetLeftAt(g,x,y);assert(e);
  assert(e->far_rank==2 && !!e->surface_texture==textured);
  if(!textured){assert(!e->top_sample_size && !e->top_owned_art);return;}
  assert(e->surface_texture==kVoxelSurfaceTexturePatch && !e->surface_derived && e->top_owned_art);
  SurfaceBudgetCrop(s,terrain,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),
      e->top_sample_x,e->top_sample_y,e->top_sample_size,paint);
}
static void SurfaceBudgetCoverage(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool stamped_fill) {
  static const int cliffs[][3]={{2545,471,6},{2546,462,6},{2848,449,12},
      {2849,471,6},{2850,462,6},{2688,512,16},{2912,512,16}};
  static const int logs[][2]={{2932,343},{3108,327},{3108,407},{3332,375},
      {3604,375},{3668,327},{3732,279}};
  const VoxelSurfaceStats *stats=ActionVoxel_SurfaceStats(g);assert(stats);
  assert(!stats->edge_budget_fallbacks && !stats->art_budget_fallbacks);
  assert(stats->edges_after==g->edge_count && g->edge_count<=kVoxelMaxEdges);
  for(unsigned i=0;i<sizeof(cliffs)/sizeof(*cliffs);++i) {
    /* These two uniform fills are shipped US/JP stamps below the finite
     * native map. Their real source words must be resolved from the override;
     * they intentionally do not exist in raw rooms or the EU override. */
    if(i>=5 && !stamped_fill) {
      assert(!SurfaceBudgetLeftAt(g,cliffs[i][0],cliffs[i][1]));continue;
    }
    for(int p=0;p<cliffs[i][2];++p)
      SurfaceBudgetTextured(s,terrain,g,cliffs[i][0],cliffs[i][1]+p,true,NULL);
  }
  for(unsigned i=0;i<sizeof(logs)/sizeof(*logs);++i)
    for(int p=0;p<4;++p)SurfaceBudgetTextured(s,terrain,g,logs[i][0],logs[i][1]+p,!p,NULL);
}
static void SurfaceBudgetEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base) {
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *paint=terrain.pixel_layers[0].edits;
  *paint=(DioramaPixelEdit){.by_cell=true,.x=159,.y=29};
  /* Every possible local8px donor includes one of these two interior pixels.
   * A matching neighbour may rescue it, but no donor may ignore the paint. */
  paint->black[0]=paint->black[8]=0x0180;
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  for(int y=471;y<477;++y) {
    const VoxelEdge *e=SurfaceBudgetLeftAt(g,2545,y);assert(e);
    if(e->surface_texture)SurfaceBudgetTextured(s,&terrain,g,2545,y,true,paint);
  }
  const VoxelEdge *before=SurfaceBudgetLeftAt(base,2848,449),*after=SurfaceBudgetLeftAt(g,2848,449);
  assert(before && after && before->top_owned_art && after->top_owned_art);
  assert(!memcmp(ActionVoxel_AuthoredArt(base,before->top_owned_art-1),
      ActionVoxel_AuthoredArt(g,after->top_owned_art-1),256*sizeof(uint16_t)));
  DioramaLayerOrder_ClearRoom(&terrain);
  terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=terrain.voxel.cells;
  *cell=(DioramaVoxelCell){.x=159,.y=29,.fields=kDioramaVoxelCell_Width|kDioramaVoxelCell_LeftArt,.width=12};
  cell->left_art=(DioramaVoxelArt){.bg=1,.x=4,.y=8,.size=8,.set=true};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,318+(q&1),58+(q>>1),
      cell->left_art.words+q,NULL));
  assert(ActionVoxel_Build(g,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,159,29);assert(a && a->surface);
  assert(!(a->automatic_art&(1u<<4)) && !a->surface_sample_size[4]);
  SurfaceBudgetCrop(s,&terrain,a->pixels[4],2548,472,8,NULL);
  DioramaLayerOrder_ClearRoom(&terrain);
  ActionVoxel_Release(g);
}
static void SurfaceBudgetFixtures(const char *directory) {
  if(!directory || !*directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0201.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&fixture));
    ActionVoxelGeometry *base=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(base && other);
    const ActionRoomScene *s=&fixture.scene;
    const uint64_t source=ForestBackingHash(s,sizeof(*s));
    assert(ActionVoxel_Build(base,s,NULL,ActionVoxel_SettingsForRoom(2,1)));
    assert(ForestBackingHash(s,sizeof(*s))==source);
    SurfaceBudgetCoverage(s,NULL,base,false);SurfaceBudgetEdits(s,base);
    remapped=*s;Renumber(&remapped);
    assert(ActionVoxel_Build(other,&remapped,NULL,ActionVoxel_SettingsForRoom(2,1)));
    SurfaceBudgetCoverage(&remapped,NULL,other,false);
    assert(TombCapGeometryHash(base)==TombCapGeometryHash(other));
    assert(MarshRockBackingHash(base)==MarshRockBackingHash(other));
    assert(!memcmp(base->front,other->front,(size_t)base->width*base->height));
    static DioramaRoomOverride shipped;TombCapTerrain(2,1,region,&shipped);
    assert(ActionVoxel_Build(other,s,&shipped,ActionVoxel_SettingsForRoom(2,1)));
    SurfaceBudgetCoverage(s,&shipped,other,region!=1);
    DioramaLayerOrder_ClearRoom(&shipped);
    ActionVoxel_Release(base);ActionVoxel_Release(other);
    printf("surface budget terrain %u: recovered cliffs/log lips, safe fallback tails and edit precedence\n",region);
  }
}
