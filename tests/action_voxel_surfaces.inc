static bool SurfaceFixtureStaticWord(const ActionRoomScene *s,unsigned word) {
  const size_t start=(word&1023)*32;
  if(start>=sizeof(s->characters))return false;
  if(!ActionRoomScene_HasCharacterAnimation(s))return true;
  const size_t target=(size_t)ActionRoomScene_CharacterAnimationTarget(s)*2;
  return start+32<=target || start>=target+ActionRoomScene_CharacterAnimationStride(s);
}
static void SurfaceFixtureValidate(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  unsigned count,caps;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  const uint16_t *owned=ActionVoxel_AuthoredCapArt(g,&caps);
  assert(count<=kVoxelMaxEdges && caps<=1024);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(ActionVoxel_Profile(g)==kActionVoxelProfile_FillmoreForest) {
      const bool folded=e->rear_fold_top || e->rear_fold_bottom;
      assert(!!e->flat_rear_join==folded);
      if(folded)assert(e->far_rank==2 && e->colliding && !e->high_priority && e->side!=2);
    } else assert(!e->flat_rear_join);
    if(!e->surface_texture)continue;
    if(e->surface_texture==kVoxelSurfaceTextureLiveInward) {
      assert(ActionVoxel_Profile(g)==kActionVoxelProfile_KassandoraDesert);
      assert(e->surface_derived && !e->top_owned_art && e->top_sample_size>=1 && e->top_sample_size<=16 && e->far_rank==2);
      assert(e->soft_surface && (!e->side || ((e->side&1)&&e->surface_top_material&&e->length<=2)));
      assert(e->top_sample_x==e->x && e->top_sample_y==e->y);
      for(unsigned along=0;along<(e->side?1u:e->length);++along) {
        unsigned reference=0;
        for(unsigned depth=0;depth<e->top_sample_size;++depth) {
          const int x=e->x+(int)along,y=e->y+(int)depth;
          uint16_t w;assert(ActionRoomScene_LookupTile(s,1,(int)floorf(x/8.f),(int)floorf(y/8.f),&w,NULL));
          if(!depth)reference=w;
          assert(FixturePixel(s,x,y) && !((w^reference)&0x3c00));
          assert(!!(w&0x2000)==!!e->high_priority);
        }
      }
      continue;
    }
    if(e->surface_texture==kVoxelSurfaceTextureNativeInward) {
      assert(e->surface_derived && e->top_sample_size==16 && e->top_owned_art);
      assert(e->far_rank==1 || e->far_rank==2);
      assert(e->length<=8 && ((e->side&1?e->y:e->x)&7)+e->length<=8);
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      /* The first actual boundary ink is independent of inward continuation
       * policy. Dedicated family fixtures validate every depth sample. */
      const bool top_axis=e->surface_top_material;
      if(top_axis)assert(ActionVoxel_Profile(g)==kActionVoxelProfile_NorthwallIce &&
          (e->side&1) && e->soft_surface &&
          (e->fold_length?e->fold_length:e->length)<=2);
      for(unsigned along=0;along<(top_axis?1u:e->length);++along) {
        const int x=e->x+((e->side&1)?0:(int)along),
            y=e->y+((e->side&1)&&!top_axis?(int)along:0);
        uint16_t w;assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&w,NULL));
        const unsigned ink=FixturePixel(s,x,y),index=(((w>>10)&7)*16+ink)*2;
        const unsigned p=top_axis?x&15:(e->side&1)?y&15:x&15;
        /* Ice tiny risers deliberately retain the actual top anchor's atlas
         * axes. They are not an independent horizontal-inward side sample. */
        const unsigned at=top_axis||e->side==0?p:e->side==1?p*16+15:e->side==2?15*16+p:p*16;
        assert(ink && SurfaceFixtureStaticWord(s,w));
        assert(art[at]==(uint16_t)(0x8000|s->palette[index]|s->palette[index+1]<<8));
      }
      continue;
    }
    const unsigned size=e->top_sample_size;
    assert(e->far_rank==2 && (size==8 || size==16) && e->top_owned_art);
    if(e->surface_native_x) {
      /* A whole native beam band may span both8px words. Its dedicated
       * fixture checks all four source rows and mixed-post cutout columns. */
      assert(ActionVoxel_Profile(g)==kActionVoxelProfile_BloodpoolMarsh && e->surface_derived);
      assert(!e->side && size==16 && e->length<=16 && (e->x&15)+e->length<=16);
      assert(e->top_sample_x==(e->x&~15) && e->top_sample_y==e->y && !(e->y&15));
      const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
      for(unsigned p=0;p<(unsigned)e->length;++p) {
        uint16_t w;assert(ActionRoomScene_LookupTile(s,1,(e->x+p)/8,e->y/8,&w,NULL));
        const unsigned ink=FixturePixel(s,e->x+p,e->y),at=(((w>>10)&7)*16+ink)*2;
        assert(ink && art[(e->x+p)&15]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
      }
      continue;
    }
    if(size==8) {
      assert((e->top_sample_x&15)<=8 && !(e->top_sample_y&7));
      const bool forest_floor=ActionVoxel_Profile(g)==kActionVoxelProfile_FillmoreForest && !e->side;
      assert(forest_floor || (e->top_sample_y&15)==(e->y&8)); /* Other profiles retain their source band. */
    } else assert((!e->side || e->surface_top_material || e->contour_skin) &&
        !(e->top_sample_x&15) && !(e->top_sample_y&15));
    if(e->surface_top_material)assert((e->side&1) && e->soft_surface && e->length<=2);
    if(e->contour_skin) {
      assert(ActionVoxel_Profile(g)==kActionVoxelProfile_KassandoraTomb && size==16);
      assert(e->contour_skin==kVoxelContourSkinTop || e->contour_skin==kVoxelContourSkinBottom);
      const unsigned horizontal=e->contour_skin==kVoxelContourSkinTop?0:2;
      assert(e->side==horizontal || ((e->side&1) && (e->fold_length?e->fold_length:e->length)<=3));
    }
    assert(e->length<=8 && ((e->side&1?e->y:e->x)&7)+e->length<=8);
    if(e->fold_length)assert(e->fold_origin_y<=e->y &&
        e->y+e->length<=e->fold_origin_y+e->fold_length);
    uint16_t source_word,donor_word;
    assert(ActionRoomScene_LookupTile(s,1,e->x/8,e->y/8,&source_word,NULL));
    assert(ActionRoomScene_LookupTile(s,1,e->top_sample_x/8,e->top_sample_y/8,&donor_word,NULL));
    assert(SurfaceFixtureStaticWord(s,source_word) && SurfaceFixtureStaticWord(s,donor_word));
    assert(!!(source_word&0x2000)==e->high_priority && !((source_word^donor_word)&0x3c00));
    bool pinned=false;for(unsigned j=0;j<caps;++j)pinned|=owned[j]+1==e->top_owned_art;
    assert(pinned);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      assert(ActionRoomScene_LookupTile(s,1,(e->top_sample_x+x%size)/8,
          (e->top_sample_y+y%size)/8,&donor_word,NULL));
      assert(SurfaceFixtureStaticWord(s,donor_word) && !((source_word^donor_word)&0x3c00));
      const unsigned p=FixturePixel(s,e->top_sample_x+x%size,e->top_sample_y+y%size);
      const unsigned index=((donor_word>>10)&7)*16+p;
      const uint16_t expected=0x8000|s->palette[index*2]|(s->palette[index*2+1]<<8);
      assert(p && art[y*16+x]==expected);
    }
  }
}
/* Pass exported native room snapshots directly. The parent audit runs this
 * over every action room and regional terrain profile, including both hubs. */
static int SurfaceFixtureInventory(int argc,char **argv) {
  static ActionSceneSnapshot snapshot;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  puts("room,profile,file,native_bands,textured_bands,semantic_fallback,unsafe_source,opaque_fallback,art_budget,edge_budget,edges_before,edges_after,new_art,pinned_art");
  for(int i=2;i<argc;++i) {
    FILE *file=fopen(argv[i],"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    assert(ActionVoxel_Build(g,s,NULL,ActionVoxel_SettingsForRoom(s->group,s->map)));
    SurfaceFixtureValidate(s,g);
    const VoxelSurfaceStats *stats=ActionVoxel_SurfaceStats(g);unsigned caps;ActionVoxel_AuthoredCapArt(g,&caps);
    printf("%02x%02x,%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",s->group,s->map,
        ActionVoxel_Profile(g),argv[i],stats->native_bands,stats->native_textured_bands,
        stats->semantic_fallbacks,stats->unsafe_source_fallbacks,stats->opaque_fallbacks,
        stats->art_budget_fallbacks,stats->edge_budget_fallbacks,stats->edges_before,stats->edges_after,
        stats->unique_patches,caps);
    ActionVoxel_Release(g);
  }
  return 0;
}
static void SurfaceFixtureEdits(const char *path) {
  if(!path)return;
  static ActionSceneSnapshot snapshot;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  FILE *file=fopen(path,"rb");assert(file);size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  ActionRoomScene *s=&snapshot.scene;ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  assert(base && edited && ActionVoxel_Build(base,s,NULL,settings));SurfaceFixtureValidate(s,base);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(base,&count);int cx=0,cy=0;bool found=false;
  for(unsigned i=0;i<count && !found;++i)if(edges[i].surface_texture) {
    cx=edges[i].x/16;cy=edges[i].y/16;bool full=true;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)full&=FixturePixel(s,cx*16+x,cy*16+y)!=0;
    VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(base,cx,cy,&shape));
    for(unsigned q=0;q<4;++q)full&=shape.back[q]>shape.front[q]+.1f;
    found=full;
  }
  assert(found);
  terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,cx,cy);assert(a);
  for(unsigned face=4;face<=5;++face) {
    assert(a->surface_sample_size[face]==16 && !(a->automatic_art&(1u<<face)));
    assert(a->surface_sample_x[face]==cx*16 && a->surface_sample_y[face]==cy*16);
    for(unsigned p=0;p<256;++p)assert(a->pixels[face][p]&0x8000);
  }
  /* Explicit shared art wins over Auto; explicit directional art wins over
   * that shared donor. These crops are never relabelled as automatic. */
  uint16_t words[4];for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,cx*2+(q&1),cy*2+(q>>1),&words[q],NULL));
  edit->fields|=kDioramaVoxelCell_SideArt|kDioramaVoxelCell_LeftArt;
  edit->side_art=(DioramaVoxelArt){.size=8,.set=true};memcpy(edit->side_art.words,words,sizeof(words));
  edit->left_art=edit->side_art;edit->left_art.y=8;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(edited,cx,cy);
  assert(!a->surface_sample_size[4] && !a->surface_sample_size[5]);
  assert(a->art_id[5]==a->art_id[2]);
  /* A negative-coordinate stamped island chooses the same native bands. */
  DioramaTileStamp stamp={.x=-1,.y=-1};memcpy(stamp.words,words,sizeof(words));
  assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&words[0],&stamp.metatile));
  memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){
      .set_bounds=true,.x0=-1,.y0=-1,.x1=0,.y1=0,.cells=&stamp,.count=1};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));edges=ActionVoxel_Edges(edited,&count);unsigned patches=0;
  for(unsigned i=0;i<count;++i)if(edges[i].surface_texture) {
    assert(edges[i].top_sample_x<0 && edges[i].top_sample_y<0);++patches;
  }
  assert(patches);
  /* Native-word eyedropper crops cannot represent authored black masks.
   * An isolated black-painted island therefore retains contour fallback. */
  terrain.pixel_layers[0].count=1;
  terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=-1,.y=-1};
  for(unsigned row=0;row<16;++row)terrain.pixel_layers[0].edits[0].black[row]=0xffff;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));edges=ActionVoxel_Edges(edited,&count);
  for(unsigned i=0;i<count;++i)assert(!edges[i].surface_texture);
  ActionVoxel_Release(base);ActionVoxel_Release(edited);
  puts("automatic face art: opaque native bands, owner metadata, explicit directional/shared precedence and negative stamps passed");
}
static void SurfaceFlatRimFixture(const char *path) {
  if(!path)return;
  static ActionSceneSnapshot snapshot;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  const ActionRoomScene *scene=&snapshot.scene;
  ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene->group,scene->map);
  assert(ActionVoxel_Build(base,scene,NULL,settings) && ActionVoxel_Profile(base)==kActionVoxelProfile_Castle);
  unsigned count,tops=0,stone_tops=0,sides=0;const VoxelEdge *edges=ActionVoxel_Edges(base,&count);int cx=0,cy=0;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *edge=&edges[i];
    if(edge->side==0 && edge->far_rank==2) {
      if(edge->surface_texture){assert(edge->top_sample_size==16);++stone_tops;}
      else {assert(!edge->top_sample_size);cx=edge->x/16;cy=edge->y/16;++tops;}
    }
    sides+=edge->surface_texture && (edge->side&1);
  }
  assert(tops && stone_tops && sides);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(edited,scene,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,cx,cy);assert(a);
  assert(!a->surface_sample_size[1] && (a->automatic_art&(1u<<1)));
  edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.x=4,.size=8,.set=true};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(scene,1,cx*2+(q&1),cy*2+(q>>1),
      &edit->top_art.words[q],NULL));
  assert(ActionVoxel_Build(edited,scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(edited,cx,cy);
  assert(a && !a->surface_sample_size[1] && !(a->automatic_art&(1u<<1)));
  ActionVoxel_Release(base);ActionVoxel_Release(edited);
  puts("automatic face art: unreviewed castle caps retain contour fallback; explicit top art still wins");
}
static const VoxelEdge *SurfaceCaveTopAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)if(!edges[i].side && edges[i].y==y &&
      edges[i].x<=x && edges[i].x+edges[i].length>x)return &edges[i];
  return NULL;
}
static void SurfaceCaveCapitalCheck(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  /* Native fixture locations, independent of the generated policy. Include
   * both isolated row35 capitals: the old fallback merged each into one64px
   * edge, so counting only edge origins silently missed six source cells. */
  static const unsigned starts[][2]={{57,35},{65,35},{57,53},{58,73},
      {102,48},{102,51},{102,54},{102,57},{102,60},{102,63},{102,66},{102,69}};
  unsigned covered=0,distant=0;
  for(unsigned p=0;p<sizeof(starts)/sizeof(*starts);++p)for(unsigned cell=0;cell<4;++cell) {
    const int cx=starts[p][0]+cell,cy=starts[p][1];
    const VoxelEdge *first=SurfaceCaveTopAt(g,cx*16,cy*16);assert(first);
    for(unsigned pixel=0;pixel<16;++pixel) {
      const VoxelEdge *e=SurfaceCaveTopAt(g,cx*16+pixel,cy*16);assert(e);
      assert(e->surface_texture && e->top_owned_art && e->top_sample_size==16);
      assert(e->near_rank==0 && e->far_rank==2 && e->colliding && !e->high_priority);
      assert(e->top_sample_x==first->top_sample_x && e->top_sample_y==first->top_sample_y);
    }
    const unsigned distance=abs(first->top_sample_x/16-cx)+abs(first->top_sample_y/16-cy);
    assert(distance<=40);distant+=distance>8;
    const uint16_t *art=ActionVoxel_AuthoredArt(g,first->top_owned_art-1);assert(art);
    /* Cell77,35 contains the reviewed complete native masonry motif. Every
     * capital must use this artwork, not a merely palette-compatible crop. */
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      uint16_t word;assert(ActionRoomScene_LookupTile(s,1,154+x/8,70+y/8,&word,NULL));
      const unsigned ink=FixturePixel(s,77*16+x,35*16+y),at=(((word>>10)&7)*16+ink)*2;
      assert(ink && art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
    }
    ++covered;
  }
  assert(covered==48 && distant>=6);
}
static void SurfaceCaveCapitalFixtures(const char *directory) {
  if(!directory)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0102.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;scene=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,2);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(base&&other);
    assert(ActionVoxel_Build(base,s,NULL,settings));SurfaceCaveCapitalCheck(s,base);
    assert(!memcmp(s,&scene,sizeof(scene))); /* Compilation never rewrites the native art/map. */
    Renumber(&scene);assert(ActionVoxel_Build(other,&scene,NULL,settings));
    SurfaceCaveCapitalCheck(&scene,other);SameGeometry(base,other);
    const VoxelEdge *neighbor=SurfaceCaveTopAt(base,104*16,63*16);assert(neighbor);
    uint16_t neighbor_art[256];memcpy(neighbor_art,
        ActionVoxel_AuthoredArt(base,neighbor->top_owned_art-1),sizeof(neighbor_art));
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    DioramaVoxelCell *edit=&terrain.voxel.cells[0];
    *edit=(DioramaVoxelCell){.x=102,.y=63,.fields=kDioramaVoxelCell_Width,.width=12};
    assert(ActionVoxel_Build(other,s,&terrain,settings));
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(other,102,63);assert(a);
    assert(a->surface_sample_size[1]==16 && !(a->automatic_art&(1u<<1)));
    assert(!memcmp(a->pixels[1],neighbor_art,sizeof(neighbor_art)));
    /* Deliberately paint the cap's native decorative8px crop. Explicit art
     * wins over the automatic masonry and cannot repaint its neighbor. */
    edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.size=8,.set=true};
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,204+(q&1),126+(q>>1),
        &edit->top_art.words[q],NULL));
    assert(ActionVoxel_Build(other,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(other,102,63);
    assert(a && !a->surface_sample_size[1] && !(a->automatic_art&(1u<<1)));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned ink=FixturePixel(s,102*16+x%8,63*16+y%8),
          at=(((edit->top_art.words[0]>>10)&7)*16+ink)*2;
      assert(a->pixels[1][y*16+x]==(uint16_t)(ink?(0x8000|s->palette[at]|s->palette[at+1]<<8):0));
    }
    neighbor=SurfaceCaveTopAt(other,104*16,63*16);assert(neighbor && neighbor->top_owned_art);
    assert(!memcmp(neighbor_art,ActionVoxel_AuthoredArt(other,neighbor->top_owned_art-1),sizeof(neighbor_art)));
    /* A masked donor cannot remain a source just because the extended
     * family search previously cached it. Rebuild must select intact art. */
    const VoxelEdge *target=SurfaceCaveTopAt(base,102*16,63*16);assert(target);
    const int dx=target->top_sample_x/16,dy=target->top_sample_y/16;
    for(unsigned black=0;black<2;++black) {
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      DioramaPixelEdit *mask=&terrain.pixel_layers[0].edits[0];
      *mask=(DioramaPixelEdit){.by_cell=true,.x=dx,.y=dy};
      for(unsigned y=0;y<16;++y)if(black)mask->black[y]=0xffff;else mask->transparent[y]=0xffff;
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      const VoxelEdge *replacement=SurfaceCaveTopAt(other,102*16,63*16);assert(replacement);
      assert(replacement->surface_texture && replacement->top_sample_size==16 &&
          (replacement->top_sample_x/16!=dx || replacement->top_sample_y/16!=dy));
    }
    memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(other,s,&terrain,settings));
    SameGeometry(base,other);SurfaceCaveCapitalCheck(s,other);
    ActionVoxel_Release(base);ActionVoxel_Release(other);
    printf("cave capital continuity region%u:48 cells/768 pixels, isolated donors, renumbering, width, explicit art, donor masks and reset passed\n",region);
  }
}
static void SurfaceTopMaterialFixtures(const char *directory) {
  SurfaceCaveCapitalFixtures(getenv("AR_VOXEL_CAVE_TOP_FIXTURES"));
  if(!directory)return;
  const struct {const char *file;int x,y;bool own;} cases[]={
      {"0104.arscene",19,12,true},{"0103.arscene",53,60,false},{"0203.arscene",20,35,true},
      {"0203.arscene",24,37,false},{"0102.arscene",55,56,true},{"0102.arscene",57,53,false}};
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene scene;static DioramaRoomOverride terrain;
  for(unsigned test=0;test<sizeof(cases)/sizeof(*cases);++test) {
    char path[1024];snprintf(path,sizeof(path),"%s/%s",directory,cases[test].file);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    for(unsigned renamed=0;renamed<2;++renamed) {
      scene=snapshot.scene;if(renamed)Renumber(&scene);
      ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene.group,scene.map);
      assert(ActionVoxel_Build(g,&scene,NULL,settings));
      unsigned count,matched=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
      int donor_x=0,donor_y=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];
        if(e->side || e->x/16!=cases[test].x || e->y/16!=cases[test].y)continue;
        assert(e->surface_texture && e->top_sample_size==16);
        if(matched)assert(e->top_sample_x==donor_x && e->top_sample_y==donor_y);
        donor_x=e->top_sample_x;donor_y=e->top_sample_y;++matched;
        const uint16_t *pixels=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(pixels);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          uint16_t word;assert(ActionRoomScene_LookupTile(&scene,1,(donor_x+x)/8,(donor_y+y)/8,&word,NULL));
          const unsigned p=FixturePixel(&scene,donor_x+x,donor_y+y),at=((word>>10)&7)*16+p;
          assert(p && pixels[y*16+x]==(uint16_t)(0x8000|scene.palette[at*2]|scene.palette[at*2+1]<<8));
        }
      }
      assert(matched>=2);
      if(cases[test].own)assert(donor_x==cases[test].x*16 && donor_y==cases[test].y*16);
      else assert((donor_x!=cases[test].x*16 || donor_y!=cases[test].y*16) &&
          abs(donor_x/16-cases[test].x)+abs(donor_y/16-cases[test].y)<=8);
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=cases[test].x,.y=cases[test].y,
          .fields=kDioramaVoxelCell_Width,.width=12};
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,cases[test].x,cases[test].y);
      assert(a && a->surface_sample_size[1]==16 && !(a->automatic_art&(1u<<1)));
      assert(a->surface_sample_x[1]==donor_x && a->surface_sample_y[1]==donor_y);
      ActionVoxel_Release(g);
    }
  }
  /* Horizontal rear turf is a different native material from hanging front
   * grass. Preserve each full16px-wide transition as two exact8px bands. */
  char path[1024];snprintf(path,sizeof(path),"%s/0101.arscene",directory);
  FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
  assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
  const struct {int x,y,reference_x,reference_y;bool soil,isolated;} forest[]={
      {144,20,147,17,false,false},{142,22,147,17,false,true},
      {145,20,145,19,false,false},{166,25,157,22,false,false},
      {133,25,133,25,true,false},{136,25,136,25,true,false}};
  for(unsigned renamed=0;renamed<2;++renamed) {
    scene=snapshot.scene;if(renamed)Renumber(&scene);
    ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(scene.group,scene.map);
    assert(ActionVoxel_Build(g,&scene,NULL,settings));SurfaceFixtureValidate(&scene,g);
    unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
    for(unsigned at=0;at<sizeof(forest)/sizeof(*forest);++at) {
      unsigned matched=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];
        if(e->side || e->x/16!=forest[at].x || e->y/16!=forest[at].y)continue;
        assert(e->surface_texture && e->top_sample_size==8 && e->length==8);
        const unsigned distance=abs(e->top_sample_x/16-forest[at].x)+
            abs(e->top_sample_y/16-forest[at].y);
        assert(distance<=24 && (e->top_sample_y&15)==(forest[at].soil?0:8));
        if(forest[at].isolated)assert(distance>8);
        if(forest[at].soil)assert(e->top_sample_x==e->x && e->top_sample_y==e->y);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
          assert(FixturePixel(&scene,e->top_sample_x+x,e->top_sample_y+y)==
              FixturePixel(&scene,forest[at].reference_x*16+(e->x&8)+x,
                  forest[at].reference_y*16+(forest[at].soil?0:8)+y));
        ++matched;
      }
      assert(matched==2);
    }
    /* An authored width retains the same reviewed Auto floor. Explicit art
     * still wins and never claims automatic donor provenance. */
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    DioramaVoxelCell *edit=&terrain.voxel.cells[0];
    *edit=(DioramaVoxelCell){.x=145,.y=20,.fields=kDioramaVoxelCell_Width,.width=12};
    assert(ActionVoxel_Build(g,&scene,&terrain,settings));
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,145,20);assert(a);
    assert(a->surface_sample_size[1]==8 && (a->surface_sample_y[1]&15)==8 &&
        !(a->automatic_art&(1u<<1)));
    edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.size=8,.set=true};
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,290+(q&1),40+(q>>1),
        &edit->top_art.words[q],NULL));
    assert(ActionVoxel_Build(g,&scene,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,145,20);
    assert(a && !a->surface_sample_size[1] && !(a->automatic_art&(1u<<1)));
    /* Horizontal stamps mirror the transition family; vertical stamps keep
     * the former native fallback instead of assuming an upside-down band. */
    DioramaTileStamp stamp={.x=145,.y=20};uint16_t original[4],word;
    assert(ActionRoomScene_LookupTile(&scene,1,290,40,&word,&stamp.metatile));
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(&scene,1,290+(q&1),40+(q>>1),&original[q],NULL));
    for(unsigned flip=1;flip<=2;++flip) {
      memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].cells=&stamp;terrain.stamp_layers[0].count=1;
      for(unsigned q=0;q<4;++q)stamp.words[q]=original[q^flip]^(flip==1?0x4000:0x8000);
      assert(ActionVoxel_Build(g,&scene,&terrain,settings));edges=ActionVoxel_Edges(g,&count);unsigned matched=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];if(e->side || e->x/16!=145 || e->y/16!=20)continue;
        assert(e->surface_texture && e->top_sample_size==8);
        if(flip==1)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
          assert(FixturePixel(&scene,e->top_sample_x+x,e->top_sample_y+y)==
              FixturePixel(&scene,157*16+(e->x&8)+x,22*16+8+y));
        else assert(e->top_sample_y==20*16);
        ++matched;
      }
      assert(matched==2);
    }
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=145,.y=20};
    terrain.pixel_layers[0].edits[0].black[0]=0x8000;
    assert(ActionVoxel_Build(g,&scene,&terrain,settings));edges=ActionVoxel_Edges(g,&count);
    for(unsigned i=0;i<count;++i)if(!edges[i].side && edges[i].x/16==145 && edges[i].y/16==20)
      assert(!edges[i].surface_texture);
    ActionVoxel_Release(g);
  }
  puts("automatic masonry tops: complete16px brick/stone motifs, nearby capital donor, two-half continuity, native remapping and authored Auto passed");
  puts("forest floor art: horizontal native turf families, isolated stair, clean upper soil, stamp flips, mask fallback, explicit art and Auto passed");
}
