/* ROM-derived fixture checks are optional and remain local. IDs below locate
 * reviewed source art only; the remapping check proves runtime independence. */
static unsigned NorthwallFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  const unsigned rooms[]={1,2,3,4,5,8};
  for(unsigned r=0;r<sizeof(rooms)/sizeof(*rooms);++r)for(unsigned region=0;region<3;++region) {
    const unsigned room=rooms[r];const bool ice=room>=2 && room<=4;
    char path[1024];snprintf(path,sizeof(path),"%s/060%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,room);
    assert(ActionVoxel_Build(a,s,NULL,settings) && ActionVoxel_Active(a,6,room));
    assert(ActionVoxel_Profile(a)==(ice?kActionVoxelProfile_NorthwallIce:kActionVoxelProfile_NorthwallSnow));
    assert(ActionVoxel_Depth(a)==(ice?.22f:.20f));
    assert(ActionVoxel_PlayRecess(a)==(ice?.075f:.065f));
    variant=*s;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
      variant=*s;
      assert(ActionRoomScene_BuildCharacters(s,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    }
    unsigned count,solid=0,rear=0,decks=0;
    const VoxelEdge *edges=ActionVoxel_Edges(a,&count);assert(count && count<kVoxelMaxEdges);
    const float depth=ActionVoxel_Depth(a),actor=ActionVoxel_PlayRecess(a)-.01f;
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];solid+=e->colliding!=0;rear+=e->rear_mounted!=0;
      if(e->high_priority && e->colliding && !e->side && e->far_rank==2) {
        VoxelEdgeSpan front,back;
        if(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeNative,0,depth,&front) &&
            ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeHighDeckBehind,0,depth,&back)) {
          assert(front.far_depth<=actor && back.near_depth>=actor);++decks;
        }
      }
    }
    assert(solid);if(room==1 || room==8)assert(decks);
    if(room==2 || room==3 || room==5){assert(rear);TombIntervals(a);}
    if(ice) {
      assert(ActionVoxel_HasRearRelief(a));
      unsigned water_count,width=0,expected=0;
      const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&water_count);assert(water_count);
      for(size_t i=0;i<s->bg[1].map_size;++i) {
        const unsigned id=s->bg[1].map[i];
        if(id==1 || id==0xb0 || id==0xb1)expected+=16;
      }
      for(unsigned i=0;i<water_count;++i) {
        assert(water[i].background==1 && !water[i].opaque);
        assert(water[i].sample_y==3 && water[i].sample_height==8);width+=water[i].width;
      }
      assert(width==expected);
      DesertMaterial(a,s,0x60,kVoxelSolid);
      DesertMaterial(a,s,0x8a,kVoxelRearOnly); /* Preserve opaque cave interior. */
    } else if(room==1)DesertMaterial(a,s,1,kVoxelSolid);
    else if(room==5)DesertMaterial(a,s,0x5f,kVoxelSolid);
    else DesertMaterial(a,s,0xf3,kVoxelSolid);
    /* The opaque outer exit pillar has no native collision because the native
     * transition owns passage. Its displayed ice still has full depth;
     * its neighbouring narrow jamb remains a separate shallow material. */
    int gate_x,gate_y;
    if(room<=3 && FixtureCell(s,0x7c,&gate_x,&gate_y)) {
      unsigned leaf_faces=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];
        if(e->x<gate_x*8 || e->x>gate_x*8+16 || e->y<gate_y*8 || e->y>gate_y*8+16)continue;
        if(e->high_priority && !e->colliding && e->far_rank==2) {
          VoxelEdgeSpan span;
          assert(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeNative,0,depth,&span));
          assert(span.far_depth==depth);++leaf_faces;
        }
      }
      assert(leaf_faces);
      variant=*s;
      for(unsigned q=0;q<4;++q) {
        uint16_t word;
        assert(ActionRoomScene_LookupTile(s,1,gate_x+(q&1),gate_y+(q>>1),&word,NULL));
        memset(variant.characters+(word&1023)*32,0,32);
      }
      assert(ActionVoxel_Build(b,&variant,NULL,settings) && ActionVoxel_Active(b,6,room));
      unsigned cleared_count;const VoxelEdge *cleared=ActionVoxel_Edges(b,&cleared_count);
      for(unsigned i=0;i<cleared_count;++i) {
        const VoxelEdge *e=&cleared[i];
        if(e->x<gate_x*8 || e->x>gate_x*8+16 || e->y<gate_y*8 || e->y>gate_y*8+16)continue;
        assert(!(e->high_priority && !e->colliding && e->far_rank==2));
      }
    }
    /* Snow, ice and summit profiles must not invent a backdrop behind empty
     * artwork. This also checks the canopy's freestanding border. */
    uint32_t low[64]={0},back[64],front[64];const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(a,-32,-32,8,8,8,&frame,low,8,back,front));
    for(unsigned i=0;i<64;++i)assert(!back[i] && !front[i]);
    variant=*s;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,6,room));
    printf("Northwall %u terrain %u: %u contours, %u rear, %u split walking tops\n",room,region,count,rear,decks);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  assert(!ActionVoxel_Room(6,6) && !ActionVoxel_Room(6,7)); /* Curved trunk owns them. */
  return 18;
}
