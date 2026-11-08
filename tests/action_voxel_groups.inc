static int GroupCellCompare(const void *aa,const void *bb) {
  const VoxelGroupCell *a=aa,*b=bb;
  return a->y!=b->y?(a->y>b->y?1:-1):a->x!=b->x?(a->x>b->x?1:-1):0;
}
static void SortedSemanticGroup(const ActionVoxelGeometry *c,int x,int y,
    VoxelSemanticGroup *group,VoxelGroupCell *cells) {
  assert(ActionVoxel_SemanticGroup(c,x,y,group,cells,kVoxelSemanticGroupMaxCells)==kActionVoxelGroup_Complete);
  assert(group->count && group->label);
  qsort(cells,group->count,sizeof(*cells),GroupCellCompare);
}
static void SemanticGroupFixture(const char *path) {
  if(!path)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  static VoxelGroupCell original[kVoxelSemanticGroupMaxCells],other[kVoxelSemanticGroupMaxCells];
  VoxelSemanticGroup expected={0};
  for(unsigned variant=0;variant<2;++variant) {
    scene=snapshot.scene;if(variant)Renumber(&scene);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene.group,scene.map);
    assert(ActionVoxel_Build(base,&scene,NULL,settings));
    VoxelSemanticGroup group;
    SortedSemanticGroup(base,134,21,&group,other);
    assert(group.kind==kVoxelSemantic_Stump && group.x==134 && group.y==21 &&
        group.width==2 && group.height==4 && group.count==8);
    if(!variant){expected=group;memcpy(original,other,group.count*sizeof(*other));}
    else assert(group.count==expected.count && !memcmp(original,other,group.count*sizeof(*other)));
    /* Any visible part resolves to the same whole profile. The adjoining
     * ground and tree never enter this exact member set. */
    for(unsigned i=0;i<expected.count;++i) {
      SortedSemanticGroup(base,original[i].x,original[i].y,&group,other);
      assert(group.count==expected.count && !memcmp(original,other,group.count*sizeof(*other)));
      assert(!ActionVoxel_TransformUnsupportedReason(base,original[i].x,original[i].y));
    }
    assert(ActionVoxel_SemanticGroup(base,134,25,&group,other,kVoxelSemanticGroupMaxCells)!=kActionVoxelGroup_Invalid);
    assert(group.kind!=kVoxelSemantic_Stump);
    /* A bounded selection reports the failure; count/bounds are never a
     * truncated object that the UI could accidentally move or resize. */
    assert(ActionVoxel_SemanticGroup(base,134,21,&group,other,1)==kActionVoxelGroup_TooLarge);
    assert(!group.count && !group.width && !group.height);
    assert(ActionVoxel_SemanticGroup(base,INT_MIN,INT_MAX,&group,other,16)==kActionVoxelGroup_Empty);
    assert(ActionVoxel_SemanticGroup(base,134,21,&group,other,0)==kActionVoxelGroup_Invalid);
    unsigned edges;const VoxelEdge *edge=ActionVoxel_Edges(base,&edges);bool checked_floor=false;
    int floor_x=0,floor_y=0;
    for(unsigned i=0;i<edges;++i)if(!edge[i].side && edge[i].colliding && edge[i].far_rank==2) {
      const int fx=(int)floorf(edge[i].x/16.f),fy=(int)floorf(edge[i].y/16.f);
      const ActionVoxelGroupStatus status=ActionVoxel_SemanticGroup(base,fx,fy,&group,other,kVoxelSemanticGroupMaxCells);
      if(group.kind!=kVoxelSemantic_Platform)continue;
      assert(status==kActionVoxelGroup_Ambiguous);
      assert(!group.count && !group.width && !group.height);
      if(!variant)printf("semantic floor: seed%d,%d status=%u members=%u bounds=%d,%d %dx%d\n",
          fx,fy,status,group.count,group.x,group.y,group.width,group.height);
      floor_x=fx;floor_y=fy;
      checked_floor=true;break;
    }
    assert(checked_floor);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=134,.y=21,
        .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Width|kDioramaVoxelCell_TopArt,
        .front=-.1f,.width=9,.top_art=AuthoredTestArt(33)};
    assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    SortedSemanticGroup(edited,134,21,&group,other);
    assert(group.count==expected.count && !memcmp(original,other,group.count*sizeof(*other)));
    /* A reset over part of an edited rectangle retains the native group too. */
    terrain.voxel.cells[0].w=2;terrain.voxel.cells[0].h=4;terrain.voxel.cell_count=2;
    terrain.voxel.cells[1]=(DioramaVoxelCell){.x=134,.y=21};
    assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    SortedSemanticGroup(edited,135,24,&group,other);
    assert(group.count==expected.count && !memcmp(original,other,group.count*sizeof(*other)));
    terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=134,.y=21,
        .fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_Fill};
    assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    assert(ActionVoxel_TransformUnsupportedReason(edited,134,21));
    assert(!ActionVoxel_TransformUnsupportedReason(edited,135,21));
    SortedSemanticGroup(base,126,17,&group,other);
    assert(group.kind==kVoxelSemantic_TreeWood && group.count>3);
    bool branch_tip=false,branch_join=false,trunk=false;
    for(unsigned i=0;i<group.count;++i) {
      branch_tip|=other[i].x==126 && other[i].y==17;
      branch_join|=other[i].x==128 && other[i].y==17;
      trunk|=other[i].x==129 && other[i].y==17;
      assert(!(other[i].x>=134 && other[i].x<=135 && other[i].y>=21 && other[i].y<=24));
    }
    assert(branch_tip && branch_join && trunk);
    if(!variant)printf("semantic tree wood: seed126,17 members=%u bounds=%d,%d %dx%d; stump134,21 members=8\n",
        group.count,group.x,group.y,group.width,group.height);
    unsigned patches;const VoxelPatch *p=ActionVoxel_Patches(base,&patches);bool checked_hazard=false;
    for(unsigned i=0;i<patches;++i)if(p[i].kind==kVoxelPatch_Thorns || p[i].kind==kVoxelPatch_Spikes) {
      assert(ActionVoxel_TransformUnsupportedReason(base,p[i].x/16,p[i].y/16));checked_hazard=true;break;
    }
    assert(checked_hazard);
    /* The same ground artwork, explicitly isolated by edited map bounds,
     * is a complete finite platform. Negative world coordinates must not
     * change ownership or turn the continuous-ground refusal into a blanket
     * ban on structural selection. */
    DioramaTileStamp stamps[4]={0};
    for(unsigned i=0;i<4;++i) {
      stamps[i].x=-2+(i&1);stamps[i].y=-2+(i>>1);
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,
          floor_x*2+(q&1),floor_y*2+(q>>1),&stamps[i].words[q],&stamps[i].metatile));
    }
    memset(&terrain,0,sizeof(terrain));
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-2,.x1=0,.y1=0,
        .cells=stamps,.count=4};
    assert(ActionVoxel_Build(edited,&scene,&terrain,settings));
    SortedSemanticGroup(edited,-2,-2,&group,other);
    assert(group.kind==kVoxelSemantic_Platform && group.count==4 && group.x==-2 && group.y==-2 &&
        group.width==2 && group.height==2);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
  puts("semantic groups: whole stump/tree wood, native remapping, edited members, compact reset, bounded selection and capability checks passed");
}
