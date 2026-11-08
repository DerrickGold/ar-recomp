/* Native specimen IDs identify reviewed fixture art only. Production policy
 * follows the displayed artwork fingerprint, including stamped/renumbered art. */
static void ColoredCapNativePixels(const ActionRoomScene *scene,
    const uint16_t *pixels,int x,int y) {
  assert(pixels);
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    uint16_t word;assert(ActionRoomScene_LookupTile(scene,1,(x+px)/8,(y+py)/8,&word,NULL));
    const unsigned ink=FixturePixel(scene,x+px,y+py),at=((word>>10)&7)*16+ink;
    assert(ink && pixels[py*16+px]==(uint16_t)(0x8000|scene->palette[at*2]|
        scene->palette[at*2+1]<<8));
  }
}
static uint64_t ForestBackingHash(const void *bytes,size_t count);
static void JungleCapNative8(const ActionRoomScene *scene,const uint16_t *pixels,int x,int y) {
  assert(pixels);
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const int wx=x+px%8,wy=y+py%8;uint16_t word;
    assert(ActionRoomScene_LookupTile(scene,1,wx/8,wy/8,&word,NULL));
    const unsigned ink=FixturePixel(scene,wx,wy),at=((word>>10)&7)*16+ink;
    assert(ink&&pixels[py*16+px]==(uint16_t)(0x8000|scene->palette[at*2]|scene->palette[at*2+1]<<8));
  }
}
static void JungleCapFixtures(const char *directory) {
  /* Frozen immediately before removing the two receiver contracts; records
   * include native geometry, masks, collision/priority and every cell depth. */
  static const uint64_t front_baseline[3][3]={
    {UINT64_C(0xa5a25bc86b85cbb9),UINT64_C(0xa5a25bc86b85cbb9),UINT64_C(0xa5a25bc86b85cbb9)},
    {UINT64_C(0xe78ae83bc1b8ec1b),UINT64_C(0xe78ae83bc1b8ec1b),UINT64_C(0xe78ae83bc1b8ec1b)},
    {UINT64_C(0x1c2ab265ee4ba75f),UINT64_C(0x1c2ab265ee4ba75f),UINT64_C(0x1c2ab265ee4ba75f)}};
  static const uint64_t boundary_baseline[3][3]={
    {UINT64_C(0x58e8efac9b834585),UINT64_C(0x58e8efac9b834585),UINT64_C(0x58e8efac9b834585)},
    {UINT64_C(0x40e2ca9a15170e19),UINT64_C(0x40e2ca9a15170e19),UINT64_C(0x40e2ca9a15170e19)},
    {UINT64_C(0xca4bc2a66c1f1eb0),UINT64_C(0xca4bc2a66c1f1eb0),UINT64_C(0xca4bc2a66c1f1eb0)}};
  static const uint64_t depth_baseline[3][3]={
    {UINT64_C(0x1974f7d20aedc5ba),UINT64_C(0x1974f7d20aedc5ba),UINT64_C(0x1974f7d20aedc5ba)},
    {UINT64_C(0xe6a23d9a8171ca9f),UINT64_C(0xe6a23d9a8171ca9f),UINT64_C(0xe6a23d9a8171ca9f)},
    {UINT64_C(0xade5f1625bec10b3),UINT64_C(0xade5f1625bec10b3),UINT64_C(0xade5f1625bec10b3)}};

  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/05%02u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    for(unsigned renamed=0;renamed<2;++renamed) {
      scene=snapshot.scene;if(renamed)Renumber(&scene);
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene.group,scene.map);
      ActionVoxelGeometry *g=ActionVoxel_Create();assert(g && ActionVoxel_Build(g,&scene,NULL,settings));
      /* Expanded per-pixel edge/depth hashes intentionally ignore atlas IDs
       * and8/16px material partitioning. Baseline captured before restoration. */
      uint64_t boundary_hash=0,depth_hash=0;unsigned boundary_count;
      const VoxelEdge *boundary=ActionVoxel_Edges(g,&boundary_count);
      for(unsigned i=0;i<boundary_count;++i)for(unsigned p=0;p<(unsigned)boundary[i].length;++p) {
        VoxelEdge e=boundary[i];e.x+=(e.side&1)?0:p;e.y+=(e.side&1)?p:0;e.length=1;
        VoxelEdgeSpan span={0};const bool visible=ActionVoxel_ResolveEdgeSpan(g,&e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span);
        unsigned near_bits,far_bits;memcpy(&near_bits,&span.near_depth,4);memcpy(&far_bits,&span.far_depth,4);
        const uint32_t values[]={(uint32_t)e.x,(uint32_t)e.y,e.side,e.near_rank,e.far_rank,
            e.high_priority,e.colliding,e.rear_trim,e.trunk_curve,e.column_curve,visible,near_bits,far_bits};
        boundary_hash+=ForestBackingHash(values,sizeof(values));
      }
      for(int y=g->origin_y/16;y<(g->origin_y+g->height)/16;++y)
        for(int x=g->origin_x/16;x<(g->origin_x+g->width)/16;++x) {
          VoxelCellGeometry c;assert(ActionVoxel_CellGeometry(g,x,y,&c));
          const float values[]={(float)x,(float)y,c.front[0],c.front[1],c.front[2],c.front[3],
              c.back[0],c.back[1],c.back[2],c.back[3],c.width,c.offset_x,
              ActionVoxel_FrontRecess(g,x*2,y*2),ActionVoxel_FrontRecess(g,x*2+1,y*2),
              ActionVoxel_FrontRecess(g,x*2,y*2+1),ActionVoxel_FrontRecess(g,x*2+1,y*2+1),
              ActionVoxel_RearFaceDepth(g)};
          depth_hash+=ForestBackingHash(values,sizeof(values));
        }
      const uint64_t front_hash=ForestBackingHash(g->front,(size_t)g->width*g->height);
      printf("jungle structural05/%02u terrain%u remap%u front%016llx boundary%016llx depth%016llx\n",
          room,region,renamed,(unsigned long long)front_hash,(unsigned long long)boundary_hash,(unsigned long long)depth_hash);
      assert(front_hash==front_baseline[room-1][region]);
      assert(boundary_hash==boundary_baseline[room-1][region]);
      assert(depth_hash==depth_baseline[room-1][region]);
      unsigned count,covered=0,plain=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
      int selected_x=0,selected_y=0,donor_x=0,donor_y=0;
      unsigned donor_families=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id;
        if(e->side||e->far_rank!=2||!ActionRoomScene_LookupTile(&snapshot.scene,1,
            e->x/8,e->y/8,&word,&id))continue;
        const bool ornament=id==8||id==9;
        const bool donor=id==0xb2||id==0xb3||id==0x0c||id==0x26;
        if(!ornament&&!donor)continue;
        assert(e->surface_texture&&e->top_owned_art&&!e->high_priority);
        if(ornament) {
          /* Exact owning quadrant, with original carved divisions. No
           * neighbouring plain/moss donor is accepted by this oracle. */
          assert(e->top_sample_size==8&&e->top_sample_x==(e->x/8)*8&&e->top_sample_y==(e->y/8)*8);
          JungleCapNative8(&scene,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),e->top_sample_x,e->top_sample_y);
          covered+=e->length;selected_x=e->x/16;selected_y=e->y/16;
        } else {
          assert(e->top_sample_size==16&&e->top_sample_x==(e->x/16)*16&&e->top_sample_y==(e->y/16)*16);
          ColoredCapNativePixels(&scene,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),e->top_sample_x,e->top_sample_y);
          plain+=e->length;donor_x=e->top_sample_x;donor_y=e->top_sample_y;
          donor_families|=id==0xb2?1:id==0xb3?2:id==0x0c?4:8;
        }
      }
      static const unsigned expected_ornament[]={2208,1104,112};
      static const unsigned expected_plain[]={688,0,0};
      assert(covered==expected_ornament[room-1]&&plain==expected_plain[room-1]);
      assert(donor_families==(room==1?5u:0u));
      /* Rooms02/03 have no exposed plain donor caps. Check every placed
       * donor family through a real Auto top after a Width edit instead of
       * pretending those native surfaces exist. All four occur inroom01. */
      const unsigned donor_ids[]={0xb2,0xb3,0x0c,0x26};unsigned authored_donors=0;
      for(unsigned d=0;d<4;++d) {
        int tx,ty;if(!FixtureCell(&snapshot.scene,donor_ids[d],&tx,&ty))continue;
        donor_x=tx*8;donor_y=ty*8;memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
        terrain.voxel.cells[0]=(DioramaVoxelCell){.x=tx/2,.y=ty/2,.fields=kDioramaVoxelCell_Width,.width=12};
        assert(ActionVoxel_Build(g,&scene,&terrain,settings));
        const VoxelAuthoredCell *owned=ActionVoxel_AuthoredCellAt(g,tx/2,ty/2);
        assert(owned&&owned->surface_sample_size[1]==16&&owned->surface_sample_x[1]==donor_x&&owned->surface_sample_y[1]==donor_y);
        ColoredCapNativePixels(&scene,owned->pixels[1],donor_x,donor_y);authored_donors|=1u<<d;
      }
      assert(authored_donors==(room==1?15u:room==2?12u:8u));
      assert(ActionVoxel_Build(g,&scene,NULL,settings));
      const int source_x=selected_x*16,source_y=selected_y*16;
      VoxelCellGeometry native_cell;assert(ActionVoxel_CellGeometry(g,selected_x,selected_y,&native_cell));
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=selected_x,.y=selected_y,.fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,selected_x,selected_y);
      assert(a&&a->surface_sample_size[1]==8&&a->surface_sample_x[1]==source_x&&
          a->surface_sample_y[1]==source_y&&!(a->automatic_art&(1u<<1)));
      JungleCapNative8(&scene,a->pixels[1],source_x,source_y);
      uint16_t expected[256],front[256];memcpy(expected,a->pixels[1],sizeof(expected));memcpy(front,a->pixels[0],sizeof(front));
      edit->fields|=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back;edit->front=.03f;edit->back=.17f;
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,selected_x,selected_y);
      assert(a&&a->surface_sample_size[1]==8&&!memcmp(a->pixels[1],expected,sizeof(expected)));
      edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,donor_x/8+(q&1),donor_y/8+(q>>1),&edit->top_art.words[q],NULL));
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,selected_x,selected_y);
      assert(a&&!a->surface_sample_size[1]&&memcmp(a->pixels[1],expected,sizeof(expected))&&!memcmp(a->pixels[0],front,sizeof(front)));
      ColoredCapNativePixels(&scene,a->pixels[1],donor_x,donor_y);
      /* Other native ornament/donor tops still own their original source;
       * the explicit paint changes only this authored receiver. */
      edges=ActionVoxel_Edges(g,&count);unsigned neighbors=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id;
        if(e->side||e->far_rank!=2||(e->x/16==selected_x&&e->y/16==selected_y)||
            !ActionRoomScene_LookupTile(&snapshot.scene,1,e->x/8,e->y/8,&word,&id))continue;
        if(id!=8&&id!=9)continue;
        assert(e->surface_texture&&e->top_owned_art&&e->top_sample_size==8);
        assert(e->top_sample_x==(e->x/8)*8&&e->top_sample_y==(e->y/8)*8);
        JungleCapNative8(&scene,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),e->top_sample_x,e->top_sample_y);++neighbors;
      }
      assert(neighbors);
      edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,selected_x,selected_y);
      assert(a&&!(a->faces&kDioramaVoxelFaceMask_Top));
      *edit=(DioramaVoxelCell){.x=selected_x,.y=selected_y,.fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,selected_x,selected_y);
      assert(a&&a->surface_sample_size[1]==8&&!memcmp(a->pixels[1],expected,sizeof(expected)));
      *edit=(DioramaVoxelCell){.x=selected_x,.y=selected_y};
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));VoxelCellGeometry reset;
      assert(ActionVoxel_CellGeometry(g,selected_x,selected_y,&reset));
      assert(!memcmp(reset.front,native_cell.front,sizeof(reset.front))&&!memcmp(reset.back,native_cell.back,sizeof(reset.back))&&reset.width==native_cell.width&&reset.offset_x==native_cell.offset_x);
      assert(ForestBackingHash(g->front,(size_t)g->width*g->height)==front_hash);
      ActionVoxel_Release(g);
    }
    printf("jungle lintel caps05/%02u terrain%u: own8 carved tops, preserved own16 stone donors, frozen structure, remapping and authored controls passed\n",room,region);
  }
}
static void ColoredCapFixtures(const char *directory) {
  if(!directory)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0204.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    for(unsigned renamed=0;renamed<2;++renamed) {
      scene=snapshot.scene;if(renamed)Renumber(&scene);
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene.group,scene.map);
      ActionVoxelGeometry *g=ActionVoxel_Create();assert(g && ActionVoxel_Build(g,&scene,NULL,settings));
      const int cells[][2]={{1,7},{2,7},{5,7},{25,7},{6,29}};
      for(unsigned cell=0;cell<sizeof(cells)/sizeof(*cells);++cell) {
        const int x=cells[cell][0]*16,y=cells[cell][1]*16;
        VoxelCellGeometry geometry;assert(ActionVoxel_CellGeometry(g,x/16,y/16,&geometry));
        for(unsigned q=0;q<4;++q)assert(geometry.front[q]==0 && geometry.back[q]==settings.depth);
        unsigned count,covered=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
        for(unsigned i=0;i<count;++i) {
          const VoxelEdge *e=&edges[i];
          if(e->side || e->y!=y || e->x<x || e->x>=x+16)continue;
          assert(e->surface_texture && e->top_owned_art && e->top_sample_size==16);
          assert(e->top_sample_x==x && e->top_sample_y==y && e->colliding && !e->high_priority);
          ColoredCapNativePixels(&scene,ActionVoxel_AuthoredArt(g,e->top_owned_art-1),x,y);
          covered+=e->length;
        }
        assert(covered==16);
      }
      /* Authored shape edits keep Auto's complete motif; explicit painting
       * continues to own the top and never acquires automatic provenance. */
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=1,.y=7,.fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,1,7);
      assert(a && a->surface_sample_size[1]==16 && a->surface_sample_x[1]==16 &&
          a->surface_sample_y[1]==112 && !(a->automatic_art&(1u<<1)));
      ColoredCapNativePixels(&scene,a->pixels[1],16,112);
      edit->fields|=kDioramaVoxelCell_TopArt;
      edit->top_art=(DioramaVoxelArt){.size=8,.set=true};
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,2+(q&1),14+(q>>1),
          &edit->top_art.words[q],NULL));
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,1,7);
      assert(a && !a->surface_sample_size[1] && !(a->automatic_art&(1u<<1)));
      ActionVoxel_Release(g);
    }
    printf("colored castle caps terrain %u: complete native16px motifs, continuous halves, native depth/priority, renumbering and explicit art passed\n",region);
  }
  JungleCapFixtures(directory);
}
