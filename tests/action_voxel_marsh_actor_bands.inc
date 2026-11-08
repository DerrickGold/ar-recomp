/* Reviewed pass-through beams retain every physical contour and native actor
 * priority, including their walking tops and matte-carved grain seams. */
static const VoxelEdge *MarshActorBandEdge(const ActionVoxelGeometry *g,int x,int y,unsigned side) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=edges+i;
    if(e->side!=side)continue;
    if(side&1) {if(e->x==x && e->y<=y && y<e->y+e->length)return e;}
    else if(e->y==y && e->x<=x && x<e->x+e->length)return e;
  }
  return NULL;
}
static void MarshActorBandCheck(const ActionVoxelGeometry *g,int x,int y,unsigned side,bool native_priority) {
  const VoxelEdge *e=MarshActorBandEdge(g,x,y,side);
  if(!e)fprintf(stderr,"missing marsh actor-band edge at (%d,%d), side%u\n",x,y,side);
  assert(e);
  assert(!e->high_priority && e->colliding && e->far_rank==2);
  assert(!!e->native_actor_priority==native_priority);
  const float depth=ActionVoxel_Depth(g),contact=ActionVoxel_PlayRecess(g)-.01f;
  VoxelEdgeSpan front,rear;
  assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,contact,&front)==!native_priority);
  assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,contact,depth,&rear));
  assert(rear.near_depth==(native_priority?0:contact) && rear.far_depth==depth);
  if(!native_priority)assert(front.near_depth==0 && front.far_depth==contact);
}
static void MarshActorBandGeometry(const ActionVoxelGeometry *g) {
  const float depth=ActionVoxel_Depth(g),contact=ActionVoxel_PlayRecess(g)-.01f;
  unsigned count,low=0,high=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=edges+i;VoxelEdge original=*e;original.native_actor_priority=0;
    VoxelEdgeSpan a,b;
    /* The priority tag must not move or remove the full physical interval. */
    const bool have_a=ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,depth,&a);
    const bool have_b=ActionVoxel_ResolveEdgeSpan(g,&original,kVoxelEdgeNative,0,depth,&b);
    assert(have_a==have_b);
    if(have_a)assert(a.near_depth==b.near_depth && a.far_depth==b.far_depth);
    if(!e->native_actor_priority)continue;
    assert(e->colliding && e->far_rank==2);
    if(e->high_priority) {
      ++high;
      const bool before=ActionVoxel_ResolveEdgeSpan(g,&original,kVoxelEdgeNative,0,contact,&a);
      assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,contact,&b)==before);
      if(before)assert(a.near_depth==b.near_depth && a.far_depth==b.far_depth);
    } else {
      ++low;
      assert(!ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,contact,&a));
    }
  }
  assert(low && high);
  /* Native-low earth still owns normal contact caps outside the beam policy. */
  const VoxelEdge *earth=MarshActorBandEdge(g,176,448,0);
  assert(earth && !earth->native_actor_priority && earth->colliding);
}
static void MarshActorBandFixture(const ActionRoomScene *s) {
  ActionVoxelGeometry *g=ActionVoxel_Create(),*changed=ActionVoxel_Create();assert(g && changed);
  static ActionRoomScene saved;saved=*s;
  assert(ActionVoxel_Build(g,s,NULL,ActionVoxel_SettingsForRoom(2,1)));
  assert(!memcmp(s,&saved,sizeof(saved)));
  MarshActorBandGeometry(g);
  for(int top=368;top<=464;top+=48) {
    /* The bottom platform reuses a hanging-grass variant at x3848. Its
     * matching plain two-rail beam is two cells farther right. */
    const int x=top==464?3880:3848;
    MarshActorBandCheck(g,x,top,0,true);
    MarshActorBandCheck(g,x,top+5,0,true);
    MarshActorBandCheck(g,x,top+3,2,true);
    MarshActorBandCheck(g,x,top+6,2,true);
    /* Black seams remain carved and the original solid pixels remain solid. */
    for(int y=0;y<8;++y) {
      const unsigned pixel=g->front[(size_t)(top+y-g->origin_y)*g->width+x-g->origin_x];
      assert(pixel==(y==4 || y==7?0:kVoxelSolid|kVoxelColliding));
    }
  }
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *paint=terrain.pixel_layers[0].edits;
  *paint=(DioramaPixelEdit){.by_cell=true,.x=240,.y=26};paint->transparent[0]=0x8000;
  assert(ActionVoxel_Build(changed,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  /* Erasing the first texel changes the contour, but never its actor order. */
  MarshActorBandCheck(changed,3840,417,0,true);
  MarshActorBandCheck(changed,3841,416,0,true);
  MarshActorBandCheck(changed,3840,421,0,true);
  DioramaLayerOrder_ClearRoom(&terrain);
  static VoxelGroupCell members[kVoxelSemanticGroupMaxCells];VoxelSemanticGroup group;
  assert(ActionVoxel_SemanticGroup(g,240,26,&group,members,kVoxelSemanticGroupMaxCells)==kActionVoxelGroup_Complete);
  assert(group.count<=sizeof(terrain.voxel.cells)/sizeof(*terrain.voxel.cells));
  terrain.voxel.cell_count=group.count;
  for(unsigned i=0;i<group.count;++i)
    terrain.voxel.cells[i]=(DioramaVoxelCell){.x=members[i].x,.y=members[i].y,.fields=kDioramaVoxelCell_Transform,
        .transform={.px=3840,.py=416,.pz=.1f,.dx=4,.dy=2,.dz=.01f,.sx=1,.sy=1,.sz=.8f}};
  assert(ActionVoxel_Build(changed,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(changed,240,26);
  assert(a && a->native_transform);
  assert(a->native_actor_priority[0] && a->native_actor_priority[1]);
  MarshActorBandCheck(changed,3848,416,0,true);
  MarshActorBandCheck(changed,3848,421,0,true);
  DioramaLayerOrder_ClearRoom(&terrain);
  /* Resizing and painting replace the contour renderer, not native priority. */
  terrain.voxel.cell_count=2;
  DioramaVoxelCell *edit=terrain.voxel.cells;
  *edit=(DioramaVoxelCell){.x=240,.y=26,.fields=kDioramaVoxelCell_Width,.width=12};
  terrain.voxel.cells[1]=(DioramaVoxelCell){.x=11,.y=28,.fields=kDioramaVoxelCell_Width,.width=12};
  for(unsigned painted=0;painted<2;++painted) {
    if(painted) {
      edit->fields|=kDioramaVoxelCell_TopArt;
      edit->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=8};
      for(unsigned q=0;q<4;++q)
        assert(ActionRoomScene_LookupTile(s,1,480+(q&1),52+(q>>1),&edit->top_art.words[q],NULL));
    }
    assert(ActionVoxel_Build(changed,s,&terrain,ActionVoxel_SettingsForRoom(2,1)));
    a=ActionVoxel_AuthoredCellAt(changed,240,26);assert(a && a->surface && a->width==12);
    assert(a->native_actor_priority[0] && a->native_actor_priority[1]);
    assert(!a->priority[0] && !a->priority[1]);
    const VoxelAuthoredCell *earth=ActionVoxel_AuthoredCellAt(changed,11,28);assert(earth && earth->surface);
    for(unsigned q=0;q<4;++q)assert(!earth->native_actor_priority[q]);
  }
  DioramaLayerOrder_ClearRoom(&terrain);
  ActionVoxel_Release(g);ActionVoxel_Release(changed);
}
static void MarshActorBandFixtures(const char *directory) {
  if(!directory || !*directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0201.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&fixture));
    MarshActorBandFixture(&fixture.scene);
    remapped=fixture.scene;Renumber(&remapped);MarshActorBandFixture(&remapped);
    printf("marsh actor bands terrain %u: native-priority beam tops and grain\n",region);
  }
}
