/* Optional ROM-derived fixtures exercise the published Death Heim materials.
 * Include after the shared SameGeometry/Renumber helpers. */
static unsigned DeathHeimFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for (unsigned room=1;room<=8;++room) for (unsigned region=0;region<3;++region) {
    char path[1024];
    snprintf(path,sizeof(path),"%s/070%u-%u.arscene",directory,room,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t bytes=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,bytes,&fixture));
    const ActionRoomScene *scene=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(7,room);
    assert(ActionVoxel_Build(a,scene,NULL,settings) && ActionVoxel_Active(a,7,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_DeathHeim);
    unsigned contours;
    ActionVoxel_Edges(a,&contours);
    /* The final battle has invisible collision over a moving starfield,
     * not physical star-shaped terrain. Every other arena has a real deck. */
    assert(room==8 ? contours==0 : contours>0);
    if(room==8)assert(ActionVoxel_PlayRecess(a)==0);
    assert(contours<kVoxelMaxEdges);
    variant=*scene;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*scene;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    /* Local caps never fabricate a distant opaque backdrop in empty space. */
    uint32_t rear[32*32],front[32*32];
    const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(a,0,0,32,32,32,&frame,NULL,32,rear,front));
    for(unsigned i=0;i<32*32;++i)assert(!rear[i]&&!front[i]);
    if(room==8) {
      uint32_t sky[32*32];
      for(unsigned i=0;i<32*32;++i)sky[i]=0xff102030u+i;
      assert(ActionVoxel_Split(a,0,0,32,32,32,&frame,sky,32,rear,front));
      assert(!memcmp(sky,rear,sizeof(sky)));
      for(unsigned i=0;i<32*32;++i)assert(!front[i]);
    }
    printf("Death Heim %u terrain %u: %u contours; native-art remap/fade invariance\n",room,region,contours);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 24;
}
