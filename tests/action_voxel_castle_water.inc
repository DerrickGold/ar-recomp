/* The native BG2 waterline owns the added horizontal moat surface. Geometry
 * must survive chain animation/fades and follow edited water, not emitters. */
static void CastleWaterFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;
  static ActionRoomScene original,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=2;room<=8;++room)
    assert(ActionVoxel_BackgroundWater(kActionVoxelProfile_Castle,room)==(room==5));
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0205.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    size_t bytes=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,bytes,&fixture));original=fixture.scene;
    const ActionRoomScene *source=&fixture.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,5);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    assert(ActionVoxel_Build(base,source,NULL,settings));
    unsigned count;const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(base,&count);
    assert(count==1 && water[0].x==592 && water[0].y==944 && water[0].width==1152);
    assert(water[0].background==1 && water[0].high_priority && water[0].sample_y==3 && water[0].sample_height==8);
    const VoxelWaterSurface expected=water[0];
    for(unsigned phase=0;phase<4;++phase) {
      variant=*source;
      assert(ActionRoomScene_BuildCharacters(source,0,phase,variant.characters,sizeof(variant.characters)));
      size_t offset,span;
      assert(ActionVoxel_AnimationMatches(source,variant.characters,&offset,&span));
      assert(ActionVoxel_Build(edited,&variant,NULL,settings));
      water=ActionVoxel_WaterSurfaces(edited,&count);assert(count==1 && !memcmp(water,&expected,sizeof(expected)));
    }
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(edited,&variant,NULL,settings));
    water=ActionVoxel_WaterSurfaces(edited,&count);assert(count==1 && !memcmp(water,&expected,sizeof(expected)));
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(base,edited);
    static DioramaRoomOverride terrain;
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[1].count=1;
    DioramaPixelEdit *edit=&terrain.pixel_layers[1].edits[0];
    *edit=(DioramaPixelEdit){.by_cell=true,.x=46,.y=59};
    for(unsigned y=0;y<16;++y)edit->transparent[y]=0xffff;
    assert(ActionVoxel_Build(edited,source,&terrain,settings));
    water=ActionVoxel_WaterSurfaces(edited,&count);
    assert(count==2 && water[0].x==592 && water[0].width==144 && water[1].x==752 && water[1].width==992);
    assert(!memcmp(base->front,edited->front,(size_t)base->width*base->height));
    memset(&terrain,0,sizeof(terrain));
    DioramaTileStamp blank={.x=46,.y=59,.blank=true};
    terrain.stamp_layers[1].cells=&blank;terrain.stamp_layers[1].count=1;
    assert(ActionVoxel_Build(edited,source,&terrain,settings));
    water=ActionVoxel_WaterSurfaces(edited,&count);assert(count==2 && water[1].x==752);
    ActionVoxel_WaterSurfaces(base,&count);assert(count==1);
    assert(!memcmp(source,&original,sizeof(original)));
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
    printf("castle moat terrain%u: full native waterline, four cached animation phases, fade/remapping, BG2 edit holes and immutable source passed\n",region);
  }
}
