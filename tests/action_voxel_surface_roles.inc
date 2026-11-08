/* Native fixture addresses are an oracle for reviewed decoration, never
 * runtime material identity. White waterfall foam, green bamboo and dark
 * rear corners must remain in the native picture without becoming rock. */
static unsigned SurfaceFixtureNonstructuralInk(const ActionRoomScene *s,int x,int y) {
  uint16_t word;uint8_t id;
  assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,&id));
  const unsigned q=(y/8%2)*2+x/8%2;
  if(s->group==3 && s->map==3)return id==0x66?0x8000:0;
  if(s->group!=4 || s->map>3)return 0;
  if(id==0x06 || id==0x33)return 0x0200;
  if(id==0x4e || id==0x4f || id==0xf4 || id==0xf6 || id==0xfc || id==0xfe)return 0x0080;
  if(id==0xb8 || id==0xb9 || id==0xba || id==0xbc || id==0xbd || id==0xbe)return 0x0070;
  if((id==0xc1 && q!=1) || (id==0xc8 && q==1) || (id==0xc9 && q<2))return 0xfe80;
  return 0;
}
static void SurfaceRoleDonors(const ActionVoxelGeometry *g,const ActionRoomScene *native) {
  unsigned count,affected=0,textured=0;
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    affected+=SurfaceFixtureNonstructuralInk(native,e->x,e->y)!=0;
    if(!e->surface_texture)continue;
    ++textured;
    for(unsigned y=0;y<e->top_sample_size;++y)for(unsigned x=0;x<e->top_sample_size;++x) {
      const int wx=e->top_sample_x+x,wy=e->top_sample_y+y;
      const unsigned pixel=FixturePixel(native,wx,wy);
      assert(!(SurfaceFixtureNonstructuralInk(native,wx,wy)&(1u<<pixel)));
    }
  }
  assert(affected && textured);
}
static void SurfaceRoleFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene variant,original;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  const struct {unsigned group,room;} rooms[]={{3,3},{4,1},{4,2},{4,3}};
  for(unsigned region=0;region<3;++region)for(unsigned room=0;room<sizeof(rooms)/sizeof(*rooms);++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/%02x%02x.arscene",directory,region,
        rooms[room].group,rooms[room].room);
    FILE *file=fopen(path,"rb");assert(file);size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    original=snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(original.group,original.map);
    assert(ActionVoxel_Build(g,&snapshot.scene,NULL,settings));
    assert(!memcmp(&original,&snapshot.scene,sizeof(original)));
    SurfaceRoleDonors(g,&original);
    variant=original;Renumber(&variant);
    assert(ActionVoxel_Build(g,&variant,NULL,settings));SurfaceRoleDonors(g,&original);
    if(original.group==4 && original.map==3) {
      int tx,ty;assert(FixtureCell(&original,0x4e,&tx,&ty));
      uint32_t native[256],rear[256],front[256];DesertNativeLow(&original,tx,ty,native);
      const ActionRoomSceneFrameState frame={.brightness=15};
      assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      unsigned foam=0;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        if(FixturePixel(&original,tx*8+x,ty*8+y)==7) {
          assert(front[y*16+x]==native[y*16+x]);++foam;
        }
      assert(foam); /* Native front foam survives the automatic donor guard. */
      static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
      terrain.voxel.cell_count=1;DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=tx/2,.y=ty/2,.fields=kDioramaVoxelCell_TopArt,
          .top_art={.size=8,.set=true}};
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&original,1,tx+(q&1),ty+(q>>1),
          &edit->top_art.words[q],NULL));
      assert(ActionVoxel_Build(g,&original,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,tx/2,ty/2);assert(a);
      assert(!a->surface_sample_size[1] && !(a->automatic_art&(1u<<1)));
      const uint16_t *paint=ActionVoxel_AuthoredArt(g,a->art_id[1]);assert(paint);
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        uint16_t word;assert(ActionRoomScene_LookupTile(&original,1,tx,ty,&word,NULL));
        const unsigned pixel=FixturePixel(&original,tx*8+x%8,ty*8+y%8);
        const unsigned at=(((word>>10)&7)*16+pixel)*2;
        assert(paint[y*16+x]==(pixel?(uint16_t)(0x8000|original.palette[at]|original.palette[at+1]<<8):0));
      }
    }
    ActionVoxel_Release(g);
    printf("surface roles %02x/%02x terrain %u: native decoration retained, structural donors clean, remapping and explicit art passed\n",
        original.group,original.map,region);
  }
}
