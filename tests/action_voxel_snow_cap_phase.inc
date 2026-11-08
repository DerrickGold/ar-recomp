/* Include after TombCapWords/Terrain/GeometryHash and ForestBackingHash.
 * Existing SnowFloorFixtures remains the independent native-contact/hazard
 * oracle; this fixture tests only actual selected cap/riser artwork. */
static const VoxelEdge *SnowPhaseEdge(const ActionVoxelGeometry *g,int x,int y,unsigned side) {
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    if(e->x==x&&e->y==y&&e->side==side&&e->far_rank==2)return e;
  }
  return NULL;
}
static const VoxelEdge *SnowPhaseCap(const ActionVoxelGeometry *g,const VoxelEdge *r) {
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    if(e->side||e->y!=r->y)continue;
    if(r->side==1?e->x+e->length==r->x+1:e->x==r->x)return e;
  }
  return NULL;
}
static uint64_t SnowPhaseStableArt(const ActionVoxelGeometry *g) {
  uint64_t hash=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    /* Every walking top, underside, nonsoft side and original long wall
     * retains the exact chosen source and pixels. Short soft sides alone
     * are allowed to adopt their adjoining cap. */
    if((e->side&1)&&e->soft_surface&&(e->fold_length?e->fold_length:e->length)<=2)continue;
    const int fields[]={e->x,e->y,e->length,e->side,e->top_sample_x,e->top_sample_y,
        e->top_sample_size,e->surface_texture,e->surface_derived,e->surface_top_material,e->contour_skin};
    hash+=ForestBackingHash(fields,sizeof(fields));
    if(e->top_owned_art)hash+=ForestBackingHash(ActionVoxel_AuthoredArt(g,e->top_owned_art-1),512);
  }
  return hash;
}
static void SnowPhasePair(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,const VoxelEdge *r) {
  assert(r);
  const VoxelEdge *t=SnowPhaseCap(g,r);assert(t);
  assert(r->surface_top_material&&r->top_owned_art==t->top_owned_art);
  assert(r->top_sample_size==8&&r->top_sample_x==t->top_sample_x&&r->top_sample_y==t->top_sample_y);
  assert(t->surface_texture==kVoxelSurfaceTexturePatch&&!t->surface_derived&&!t->contour_skin);
  assert(r->soft_surface&&r->length<=2&&r->near_rank==t->near_rank&&r->far_rank==t->far_rank);
  assert(r->colliding==t->colliding&&r->high_priority==t->high_priority);
  /* The old and new full-depth intervals are still the same actual surface. */
  VoxelEdgeSpan ts,rs;
  assert(ActionVoxel_ResolveEdgeSpan(g,t,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&ts));
  assert(ActionVoxel_ResolveEdgeSpan(g,r,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&rs));
  if(fabsf(ts.near_depth-rs.near_depth)>=.000001f||fabsf(ts.far_depth-rs.far_depth)>=.000001f)
    fprintf(stderr,"SNOW PHASE SPAN top %d,%d len%d %.6f..%.6f neighborRear%u riser %d,%d side%u len%d %.6f..%.6f neighborRear%u\n",t->x,t->y,t->length,ts.near_depth,ts.far_depth,t->neighbor_rear_mounted,r->x,r->y,r->side,r->length,rs.near_depth,rs.far_depth,r->neighbor_rear_mounted);
  assert(fabsf(ts.near_depth-rs.near_depth)<.000001f&&fabsf(ts.far_depth-rs.far_depth)<.000001f);
  /* An unchanged native8 crop, repeated by the existing atlas contract;
   * palette similarity or a different snow donor cannot satisfy this. */
  const uint16_t *art=ActionVoxel_AuthoredArt(g,r->top_owned_art-1);assert(art);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const int wx=t->top_sample_x+x%8,wy=t->top_sample_y+y%8;
    const int cx=(int)floorf(wx/16.f),cy=(int)floorf(wy/16.f);
    uint16_t words[4];unsigned collision;assert(TombCapWords(s,terrain,cx,cy,words,&collision));
    const unsigned nx=wx-cx*16,ny=wy-cy*16,w=words[(ny/8)*2+nx/8];
    const unsigned ink=RockFixtureWordPixel(s,w,nx&7,ny&7),at=(((w>>10)&7)*16+ink)*2;
    assert(ink&&art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
}
static unsigned SnowPhaseCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool witnesses) {
  unsigned changed=0;
  for(unsigned i=0;i<g->edge_count;++i)if(g->edges[i].surface_top_material) {
    assert(g->edges[i].side&1);SnowPhasePair(s,terrain,g,g->edges+i);++changed;
  }
  if(witnesses) {
    const VoxelEdge *a=SnowPhaseEdge(g,1319,664,1),*b=SnowPhaseEdge(g,1327,672,1);
    assert(a&&b);SnowPhasePair(s,terrain,g,a);SnowPhasePair(s,terrain,g,b);
    /* Same-crop rotated seam and differing-crop seam from the native0F/17
     * descending pair. Both must reuse the already chosen walking cap. */
    assert(a->top_sample_x==1312&&a->top_sample_y==664);
    assert(b->top_sample_x==1320&&b->top_sample_y==672);
    assert(changed>2);
  }
  return changed;
}
static void SnowCapPhaseFixtures(const char *directory) {
  if(!directory||!*directory)return;
  const char *write=getenv("AR_SNOW_PHASE_WRITE_BASELINE"),*read=getenv("AR_SNOW_PHASE_BASELINE");
  assert(write||read);FILE *f=fopen(write?write:read,write?"w":"r");assert(f);
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0601.arscene",directory,region);
    FILE *scene=fopen(path,"rb");assert(scene);const size_t size=fread(bytes,1,sizeof(bytes),scene);
    assert(!ferror(scene));fclose(scene);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    ActionRoomScene *s=&snapshot.scene;original=*s;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,1);
    for(unsigned authored=0;authored<2;++authored) {
      TombCapTerrain(6,1,region,&terrain);const DioramaRoomOverride *t=authored?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));
      const uint64_t front=ForestBackingHash(g->front,(size_t)g->width*g->height),
          geometry=TombCapGeometryHash(g),stable=SnowPhaseStableArt(g),
          patches=ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches));
      if(write)fprintf(f,"%u %u %016llx %016llx %016llx %016llx\n",region,authored,
          (unsigned long long)front,(unsigned long long)geometry,(unsigned long long)stable,(unsigned long long)patches);
      else {
        unsigned rr,aa;unsigned long long ff,gg,ss,pp;
        assert(fscanf(f,"%u %u %llx %llx %llx %llx",&rr,&aa,&ff,&gg,&ss,&pp)==6);
        assert(rr==region&&aa==authored&&ff==front&&gg==geometry&&ss==stable&&pp==patches);
        printf("Snow phase region%u authored%u: %u actual cap/riser joins\n",region,authored,SnowPhaseCheck(s,t,g,true));
      }
      DioramaLayerOrder_ClearRoom(&terrain);
    }
    if(!write) {
      assert(ActionVoxel_Build(g,s,NULL,settings));const unsigned count=SnowPhaseCheck(s,NULL,g,true);
      remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
      assert(SnowPhaseCheck(&remapped,NULL,other,true)==count);
      assert(TombCapGeometryHash(g)==TombCapGeometryHash(other));
      for(unsigned transparent=0;transparent<2;++transparent) {
        terrain.pixel_layers[0].count=1;
        DioramaPixelEdit *p=terrain.pixel_layers[0].edits;
        *p=(DioramaPixelEdit){.by_cell=true,.x=82,.y=41};
        if(transparent)p->transparent[0]=0x8000;else p->black[0]=0x8000;
        assert(ActionVoxel_Build(other,s,&terrain,settings));
        const VoxelEdge *edited=SnowPhaseEdge(other,1319,664,1);
        assert(edited&&!edited->surface_top_material);
        SnowPhasePair(s,&terrain,other,SnowPhaseEdge(other,1327,672,1));
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      for(unsigned side=0;side<2;++side) {
        terrain.voxel.cell_count=1;DioramaVoxelCell *p=terrain.voxel.cells;
        *p=(DioramaVoxelCell){.x=82,.y=41,.fields=side?kDioramaVoxelCell_SideArt:kDioramaVoxelCell_TopArt};
        DioramaVoxelArt *a=side?&p->side_art:&p->top_art;a->set=true;a->bg=1;a->size=16;
        unsigned collision;assert(TombCapWords(s,NULL,82,41,a->words,&collision));
        assert(ActionVoxel_Build(other,s,&terrain,settings));
        const VoxelAuthoredCell *owned=ActionVoxel_AuthoredCellAt(other,82,41);
        assert(owned&&owned->surface&&!owned->surface_derived[side?2:1]);
        SnowPhasePair(s,&terrain,other,SnowPhaseEdge(other,1327,672,1));
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=82,.y=41};
      assert(ActionVoxel_Build(other,s,&terrain,settings));
      assert(SnowPhaseCheck(s,&terrain,other,true)==count);
      assert(TombCapGeometryHash(g)==TombCapGeometryHash(other));DioramaLayerOrder_ClearRoom(&terrain);
    }
    assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
  assert(!ferror(f));fclose(f);
}
