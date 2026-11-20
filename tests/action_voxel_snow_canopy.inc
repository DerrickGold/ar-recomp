/* Native artwork is the independent source: geometry must preserve its
 * collision/priority and alpha while the coating joins continuously. */
static unsigned SnowCanopyCheck(const ActionVoxelGeometry *g) {
  unsigned count=0,joins=0;
  for(unsigned i=0;i<g->column_curve_count;++i) {
    const VoxelTrunkCurve *a=g->column_curves+i;if(!a->native_lip)continue;
    assert(a->contact_count==9 && !a->round_width);++count;
    for(unsigned knot=0;knot<9;++knot) {
      VoxelTrunkCurve section;ActionVoxel_ColumnSection(a,a->contact_x+knot*2,&section);
      const float end=a->contact[knot]+16;
      const float top=a->canopy_top[knot];assert(top<end);
      assert(ActionVoxel_ColumnFrontDepth(g,&section,top)==0);
      assert(fabsf(ActionVoxel_ColumnFrontDepth(g,&section,end)-.15f)<.000001f);
      assert(fabsf(ActionVoxel_ColumnFrontDepth(g,&section,(top+end)*.5f)-.075f)<.000001f);
      if(knot && knot<8) {
        VoxelTrunkCurve left,right;
        ActionVoxel_ColumnSection(a,a->contact_x+knot*2-.0001f,&left);
        ActionVoxel_ColumnSection(a,a->contact_x+knot*2+.0001f,&right);
        assert(fabsf(ActionVoxel_ColumnFrontDepth(g,&left,(top+end)*.5f)-
            ActionVoxel_ColumnFrontDepth(g,&right,(top+end)*.5f))<.00001f);
      }
    }
    for(unsigned j=0;j<g->column_curve_count;++j) {
      const VoxelTrunkCurve *b=g->column_curves+j;
      if(!b->native_lip || a->contact_x+16!=b->contact_x || abs(a->contact[8]-b->contact[0])>=16)continue;
      assert(a->contact[8]==b->contact[0] && a->canopy_top[8]==b->canopy_top[0]);++joins;
    }
  }
  unsigned seams=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    if(!(e->side&1) || e->near_rank!=e->far_rank || !e->column_curve || !e->neighbor_column_curve)continue;
    const VoxelTrunkCurve *a=g->column_curves+e->column_curve-1,*b=g->column_curves+e->neighbor_column_curve-1;
    if(!a->native_lip || !b->native_lip)continue;
    const float x=e->x+(e->side==1);
    for(unsigned sample=0;sample<3;++sample) {
      const float y=e->y+e->length*sample*.5f;VoxelTrunkCurve left,right;
      ActionVoxel_ColumnSection(a,x,&left);ActionVoxel_ColumnSection(b,x,&right);
      if(fabsf(ActionVoxel_ColumnFrontDepth(g,&left,y)-ActionVoxel_ColumnFrontDepth(g,&right,y))>.000001f)continue;
      VoxelEdgeSpan span;assert(!ActionVoxel_ResolveEdgeSpanAtY(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),y,&span));++seams;
    }
  }
  assert(count && joins && seams);return count;
}
/* Independent native-art inventory. IDs locate the unmodified fixture;
 * production eligibility is separately verified after ID remapping. */
static void SnowCanopyCoverage(const ActionRoomScene *s,const ActionVoxelGeometry *g) {
  static const unsigned snow[]={0x1e,0x1f,0x26,0x27,0x2f,0x38,0x39,0x3a,0x3b,0x3c,
      0x4a,0x4b,0x4c,0x4f,0x52,0x53,0x54,0x57,0x5a,0x5c,0x5f,0x65,0x66,0x70,0x71,
      0x76,0x80,0x81,0x82,0x83,0x84,0x85,0x8e,0x8f,0x96,0x98,0x99,0x9a,0xa7};
  static const unsigned leaf[]={0x60,0x62,0x64,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,
      0x6e,0x6f,0x72,0x77,0x78,0x79,0x7a,0x7f,0x86,0x87,0x97};
  unsigned expected=0;
  for(unsigned y=1;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t upper,lower;
      assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&lower));
      assert(ActionRoomScene_LookupTile(s,1,x*2,y*2-2,&word,&upper));
      bool a=false,b=false;
      for(unsigned i=0;i<sizeof(snow)/sizeof(*snow);++i)a|=upper==snow[i];
      for(unsigned i=0;i<sizeof(leaf)/sizeof(*leaf);++i)b|=lower==leaf[i];
      if(!a || !b)continue;
      unsigned owners=0;
      for(unsigned i=0;i<g->column_curve_count;++i) {
        const VoxelTrunkCurve *curve=g->column_curves+i;
        if(!curve->native_lip || curve->contact_x!=(int)x*16 || abs(curve->contact[4]+16-(int)y*16)>=32)continue;
        unsigned cells=0;
        for(unsigned row=0;row<(unsigned)g->height/8;++row)for(unsigned dx=0;dx<2;++dx) {
          const size_t at=(size_t)row*(g->width/8)+x*2+dx;
          cells+=g->column_cells[at]==i+1;
        }
        assert(cells);++owners;
      }
      assert(owners==1);++expected;
    }
  assert(expected==38);
  static const unsigned tips[][2]={{3,0},{2,1},{22,18},{23,19},{26,24},{24,19},{25,23},{25,24}};
  for(unsigned i=0;i<sizeof(tips)/sizeof(*tips);++i) {
    unsigned curved=0;
    for(unsigned q=0;q<4;++q) {
      const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForCell(g,tips[i][0]*2+(q&1),tips[i][1]*2+(q>>1));
      curved+=curve && curve->native_lip;
    }
    assert(curved);
  }
  static const unsigned ends[][3]={{24,19,0x57},{25,24,0x4f}};
  for(unsigned i=0;i<2;++i) {
    uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(s,1,ends[i][0]*2,ends[i][1]*2,&word,&id) && id==ends[i][2]);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      assert(!FixturePixel(s,ends[i][0]*16+x,(ends[i][1]+1)*16+y));
  }
  /* The upper contact of every newly curved solid remains at native depth. */
  unsigned tops=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    if(e->side || e->far_rank!=2 || !e->colliding || !e->column_curve)continue;
    const VoxelTrunkCurve *curve=g->column_curves+e->column_curve-1;if(!curve->native_lip)continue;
    for(unsigned px=0;px<=e->length;++px) {
      VoxelTrunkCurve section;ActionVoxel_ColumnSection(curve,e->x+px,&section);
      assert(ActionVoxel_ColumnFrontDepth(g,&section,e->y)<.000001f);++tops;
    }
  }
  assert(tops);
}
static void SnowCanopyFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0605.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&snapshot));const ActionRoomScene *s=&snapshot.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,5);
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a && b);
    assert(ActionVoxel_Build(a,s,NULL,settings));const unsigned count=SnowCanopyCheck(a);
    assert(count==40); /* 38 leaf joins plus both native terminal snow caps. */
    SnowCanopyCoverage(s,a);
    uint8_t *tops=SnowAct2TopCoverage(a);unsigned contacts=0;
    for(unsigned cell=0;cell<sizeof(kSnowAct2SlopeCells)/sizeof(*kSnowAct2SlopeCells);++cell) {
      const int x=kSnowAct2SlopeCells[cell][0],y=kSnowAct2SlopeCells[cell][1];
      const unsigned collision=SnowAct2Collision(s,x,y),left=SnowAct2Collision(s,x-1,y),right=SnowAct2Collision(s,x+1,y);
      for(unsigned px=0;px<16;++px) {
        const int py=SnowAct2Contact(collision,left,right,px);
        const size_t at=SnowAct2At(a,x*16+px,y*16+py);
        assert((a->front[at]&kVoxelKindMask)==kVoxelSolid && tops[at]);++contacts;
      }
    }
    free(tops);assert(contacts==304);
    const VoxelTrunkCurve *target=ActionVoxel_ColumnCurveForCell(a,26,32),
        *neighbor=ActionVoxel_ColumnCurveForCell(a,28,32);
    assert(target && neighbor && target->native_lip && neighbor->native_lip);
    const uint64_t geometry=TombCapGeometryHash(a);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(b,&variant,NULL,settings));
    assert(SnowCanopyCheck(b)==count && TombCapGeometryHash(b)==geometry);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));
    assert(SnowCanopyCheck(b)==count && TombCapGeometryHash(b)==geometry);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=13,.y=16,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.07f,.back=.18f};
    assert(ActionVoxel_Build(b,s,&terrain,settings));
    const VoxelAuthoredCell *edited=ActionVoxel_AuthoredCellAt(b,13,16);assert(edited && edited->surface);
    for(unsigned q=0;q<4;++q)assert(edited->front[q]==.07f && edited->back[q]==.18f);
    assert(!ActionVoxel_ColumnCurveForCell(b,26,32));
    const VoxelTrunkCurve *after=ActionVoxel_ColumnCurveForCell(b,28,32);
    assert(after && after->native_lip && !memcmp(after->contact,neighbor->contact,sizeof(neighbor->contact)) &&
        !memcmp(after->canopy_top,neighbor->canopy_top,sizeof(neighbor->canopy_top)));
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=13,.y=16};
    terrain.pixel_layers[0].edits[0].transparent[4]=1u<<11;
    assert(ActionVoxel_Build(b,s,&terrain,settings));
    after=ActionVoxel_ColumnCurveForCell(b,28,32);
    assert(after && !memcmp(after->contact,neighbor->contact,sizeof(neighbor->contact)) &&
        !memcmp(after->canopy_top,neighbor->canopy_top,sizeof(neighbor->canopy_top)));
    /* Boundary erasure is still a local artwork edit, not a new automatic
     * snow contour for its neighboring native cell. */
    terrain.pixel_layers[0].edits[0].transparent[4]|=1;
    assert(ActionVoxel_Build(b,s,&terrain,settings));
    after=ActionVoxel_ColumnCurveForCell(b,28,32);
    assert(after && !memcmp(after->contact,neighbor->contact,sizeof(neighbor->contact)) &&
        !memcmp(after->canopy_top,neighbor->canopy_top,sizeof(neighbor->canopy_top)));
    uint32_t source[256],rear[256],front[256];DesertNativeLow(s,26,32,source);source[68]=0;
    const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(b,208,256,16,16,16,&frame,source,16,rear,front));assert(!rear[68] && !front[68]);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=24,.y=19,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.07f,.back=.18f};
    assert(ActionVoxel_Build(b,s,&terrain,settings));
    edited=ActionVoxel_AuthoredCellAt(b,24,19);assert(edited && edited->surface);
    for(unsigned q=0;q<4;++q)assert(edited->front[q]==.07f && edited->back[q]==.18f);
    assert(!ActionVoxel_ColumnCurveForCell(b,48,38));
    const VoxelTrunkCurve *upper=ActionVoxel_ColumnCurveForCell(b,48,36);
    assert(upper && upper->native_lip);
    const VoxelTrunkCurve *left_before=ActionVoxel_ColumnCurveForCell(a,46,38),
        *left_after=ActionVoxel_ColumnCurveForCell(b,46,38);
    assert(left_before && left_after && !memcmp(left_before->contact,left_after->contact,sizeof(left_before->contact)) &&
        !memcmp(left_before->canopy_top,left_after->canopy_top,sizeof(left_before->canopy_top)));
    DesertNativeLow(s,48,38,source);
    assert(ActionVoxel_Split(a,384,304,16,16,16,&frame,source,16,rear,front));
    assert(!memcmp(source,front,sizeof(source))); /* Native cap artwork/alpha is unchanged. */
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Northwall snow canopy region%u: %u smooth returns (38 leaf joins + 2 terminal caps), %u native walking contacts; shared fringe knots, source alpha, remapping, fades and local depth/pixel overrides preserved\n",region,count,contacts);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
}
