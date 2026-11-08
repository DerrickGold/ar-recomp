/* Independent native source/ownership oracle for Castle02/07's terminal void.
 * Include after the TombCapWords/Key/Terrain helpers. Production has no fixed
 * coordinates: these values are the exact reviewed native fixture result. */
static void CastleTerminalMatteCheck(const ActionVoxelGeometry *g) {
  const VoxelTerminalMatte *m=ActionVoxel_TerminalMatte(g);assert(m);
  assert(m->world_right==1024&&m->world_bottom==1024);
  assert(m->bottom_start==464&&m->right_start==400);
  assert(m->bottom_opaque_black&&m->right_opaque_black);
}
static void CastleTerminalMatteFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot fixture;
  static ActionRoomScene original,variant;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0207.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&fixture));
    ActionRoomScene *s=&fixture.scene;original=*s;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(g&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,7);
    assert(ActionVoxel_Build(g,s,NULL,settings));CastleTerminalMatteCheck(g);
    /* Every source pixel establishing the two runs is native opaque black,
     * low priority and noncolliding. No geometry is inferred from emptiness. */
    for(unsigned side=0;side<2;++side)
      for(int cell=side?25:29;cell<64;++cell) {
        const int cx=side?63:cell,cy=side?cell:63;
        uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,cx,cy,w,&collision));
        assert(!collision&&TombCapKey(s,w,collision,0)==UINT64_C(0xabde6e24e3e445e5));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned word=w[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
          const unsigned at=(((word>>10)&7)*16+ink)*2;
          assert(ink&&!(word&0x2000)&&!s->palette[at]&&!s->palette[at+1]);
          assert(!g->front[(cy*16+y)*g->width+cx*16+x]);
        }
      }
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(edited,&variant,NULL,settings));
    CastleTerminalMatteCheck(edited);SameGeometry(g,edited);
    assert(!memcmp(g->front,edited->front,(size_t)g->width*g->height));
    /* A source-driven authored opening at the terminal corner must stop
     * both continuations, and removing the edit must restore the exact run. */
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=63,.y=63};
    terrain.pixel_layers[0].edits[0].transparent[0]=0x8000;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!ActionVoxel_TerminalMatte(edited));
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=63,.y=63,
      .fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!ActionVoxel_TerminalMatte(edited));
    /* An interior boundary edit truncates only the terminal suffix. */
    terrain.voxel.cells[0].x=60;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    const VoxelTerminalMatte *m=ActionVoxel_TerminalMatte(edited);assert(m);
    assert(m->bottom_start==976&&m->right_start==400&&m->bottom_opaque_black&&m->right_opaque_black);
    memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(edited,s,&terrain,settings));
    CastleTerminalMatteCheck(edited);SameGeometry(g,edited);
    /* Palette change disproves native black even when shape/CHR match. */
    variant=*s;uint16_t w[4];unsigned collision;assert(TombCapWords(s,NULL,63,63,w,&collision));
    const unsigned ink=RockFixtureWordPixel(s,w[0],0,0),at=(((w[0]>>10)&7)*16+ink)*2;
    variant.palette[at]=1;assert(ActionVoxel_Build(edited,&variant,NULL,settings));
    assert(!ActionVoxel_TerminalMatte(edited));
    /* The same scenery in a different Castle room does not acquire this
     * level enclosure contract. Native tiles and gameplay state stay const. */
    variant=*s;variant.map=8;assert(ActionVoxel_Build(edited,&variant,NULL,settings));
    assert(!ActionVoxel_TerminalMatte(edited));
    assert(!memcmp(s,&original,sizeof(original)));
    ActionVoxel_Release(g);ActionVoxel_Release(edited);
    printf("Castle02/07 terminal void terrain%u: exact native runs, geometry/source immutability, renumbering, authored opening/reset and palette controls\n",region);
  }
}
