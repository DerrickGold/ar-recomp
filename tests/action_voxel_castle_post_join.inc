/* Optional regional native fixtures for the reviewed lower-post joins.
 * IDs locate reviewed artwork in these fixtures only;
 * the production change uses two room-qualified artwork fingerprints. */
static void CastlePostDepth(const ActionVoxelGeometry *g,int x,int y,
    float front,float back,unsigned kind,bool colliding) {
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,x,y,&shape));
  assert(!shape.authored);
  for(unsigned q=0;q<4;++q) {
    assert(fabsf(shape.front[q]-front)<.000001f);
    assert(fabsf(shape.back[q]-back)<.000001f);
  }
  for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx) {
    const unsigned pixel=g->front[(y*16+(int)yy-g->origin_y)*g->width+x*16+xx-g->origin_x];
    assert((pixel&kVoxelKindMask)==kind && (pixel&kVoxelHighPriority));
    assert(!!(pixel&kVoxelColliding)==colliding && !(pixel&(kVoxelRearTrim|kVoxelRearCopy)));
  }
}
static void CastlePostSource(const ActionRoomScene *s,const ActionVoxelGeometry *g,int x,int y) {
  uint32_t native[256],before[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,native);
  memcpy(before,native,sizeof(before));
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,native,16,rear,front));
  assert(!memcmp(before,native,sizeof(before)));
  /* Native high artwork is captured separately. Supplying the same ink to
   * the low splitter exercises the existing CastleForeground pass-through,
   * which must match adjoining CC/CD rather than invent a new rear policy. */
  uint32_t trim_rear[256],trim_front[256];
  assert(ActionVoxel_Split(g,55*16,56*16,16,16,16,&frame,native,16,trim_rear,trim_front));
  for(unsigned p=0;p<256;++p)assert(front[p]==native[p] &&
      front[p]==trim_front[p] && rear[p]==trim_rear[p]);
  const uint32_t empty[256]={0};
  assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,empty,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!front[p]&&!rear[p]);
  for(unsigned q=0;q<4;++q) {
    uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,&id));
    assert((word&0x2000) && !(s->bg[0].metatiles[id*8+q*2]&2));
  }
}
static void CastlePostEditor(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;
  const int x=55,y=57;VoxelCellGeometry neighbor,after;
  assert(ActionVoxel_CellGeometry(base,x+1,y,&neighbor));
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=x,.y=y,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,
      .front=.04f,.back=.16f,.width=10};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_CellGeometry(edited,x,y,&after) && after.authored && after.width==10);
  for(unsigned q=0;q<4;++q)assert(after.front[q]==.04f && after.back[q]==.16f);
  assert(ActionVoxel_CellGeometry(edited,x+1,y,&after));assert(!memcmp(&neighbor,&after,sizeof(after)));

  *cell=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Faces,
      .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);
  assert(a && a->native_front_hidden && !a->surface);SameGeometry(base,edited);
  cell->faces=kDioramaVoxelFaceMask_All;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
  assert(!ActionVoxel_AuthoredCellAt(edited,x,y));

  *cell=(DioramaVoxelCell){.x=x,.y=y,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Transform,
      .front=.04f,.back=.16f,
      .transform={.px=x*16,.py=y*16,.pz=0,.dx=11,.dy=-7,.dz=.03f,.sx=1.25f,.sy=.75f,.sz=1}};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  a=ActionVoxel_AuthoredCellAt(edited,x,y);assert(a && a->surface);
  float tx,ty,tz;ActionVoxel_TransformPoint(&a->transform,x*16+8,y*16+8,.04f,&tx,&ty,&tz);
  assert(tx==x*16+21 && ty==y*16-1 && fabsf(tz-.07f)<.000001f);
  assert(ActionVoxel_CellGeometry(edited,x+1,y,&after));assert(!memcmp(&neighbor,&after,sizeof(after)));
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);

  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=x,.y=y};pixel->transparent[3]=0x1000;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(!edited->front[(y*16+3-edited->origin_y)*edited->width+x*16+3-edited->origin_x]);
  uint32_t native[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,native);native[3*16+3]=0;
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(edited,x*16,y*16,16,16,16,&frame,native,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(front[p]==native[p] && rear[p]==native[p]);
  assert(ActionVoxel_CellGeometry(edited,x+1,y,&after));assert(!memcmp(&neighbor,&after,sizeof(after)));
  memset(pixel->transparent,0,sizeof(pixel->transparent));pixel->black[3]=0x1000;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));native[3*16+3]=0xff000000;
  assert(ActionVoxel_Split(edited,x*16,y*16,16,16,16,&frame,native,16,rear,front));
  assert(front[3*16+3]==0xff000000 && rear[3*16+3]==0xff000000);

  /* A native stamp remains fingerprint based, including mirrored artwork at
   * a negative map origin. Native collision still comes from its definition. */
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-2,.y=-3};
    for(unsigned q=0;q<4;++q) {
      assert(ActionRoomScene_LookupTile(s,1,x*2+((q&1)^flip),y*2+(q>>1),&stamp.words[q],&stamp.metatile));
      if(flip)stamp.words[q]^=0x4000;
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,
        .x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    CastlePostDepth(edited,-2,-3,0,.0288f,kVoxelForeground,false);
  }
}
static void CastlePostJoinFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;
  static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0205.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,5);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    assert(fabsf(ActionVoxel_Depth(base)-.24f)<.000001f);
    /* Exact independent established trim: the new rods must meet these
     * CC/CD rows at the same resolved depth, rather than just share an enum. */
    CastlePostDepth(base,55,56,0,.0288f,kVoxelForeground,false);
    CastlePostDepth(base,56,56,0,.0288f,kVoxelForeground,false);
    VoxelCellGeometry trim;assert(ActionVoxel_CellGeometry(base,55,56,&trim));
    unsigned cells=0,edges_seen=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(id!=0xd4 && id!=0xd5)continue;
        assert((x==55||x==56||x==95||x==96)&&y>=56&&y<=60);
        CastlePostDepth(base,x,y,0,.0288f,kVoxelForeground,false);CastlePostSource(s,base,x,y);
        VoxelCellGeometry body;assert(ActionVoxel_CellGeometry(base,x,y,&body));
        assert(!memcmp(trim.front,body.front,sizeof(trim.front))&&!memcmp(trim.back,body.back,sizeof(trim.back)));
        ++cells;
        unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(base,&count);
        for(unsigned i=0;i<count;++i) {
          const VoxelEdge *edge=&edges[i];
          if(!(edge->side&1)||edge->x<(int)x*16||edge->x>=(int)(x+1)*16||
              edge->y+edge->length<=(int)y*16||edge->y>=(int)(y+1)*16||edge->far_rank!=1)continue;
          assert(edge->high_priority&&!edge->colliding&&!edge->rear_trim);
          const int sample_y=edge->y>(int)y*16?edge->y:(int)y*16;
          VoxelEdgeSpan span;
          assert(ActionVoxel_ResolveEdgeSpanAtY(base,edge,kVoxelEdgeNative,0,.24f,sample_y,&span));
          assert(span.near_depth==0&&fabsf(span.far_depth-.0288f)<.000001f);++edges_seen;
        }
      }
    assert(cells==10&&edges_seen);
    /* FE is exactly the same displayed source as D4 but genuinely colliding
     * in an upper ceiling context. Its different fingerprint stays full depth. */
    uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,214,8,&word,&id)&&id==0xfe);
    CastlePostDepth(base,107,4,0,.24f,kVoxelSolid,true);
    remapped=*s;Renumber(&remapped);
    assert(ActionVoxel_Build(edited,&remapped,NULL,settings));SameGeometry(base,edited);
    assert(!memcmp(base->front,edited->front,(size_t)base->width*base->height));
    CastlePostEditor(s,base,edited,settings);
    remapped=*s;remapped.map=4;
    assert(ActionVoxel_Build(edited,&remapped,NULL,ActionVoxel_SettingsForRoom(2,4)));
    VoxelCellGeometry outside;assert(ActionVoxel_CellGeometry(edited,55,57,&outside));
    for(unsigned q=0;q<4;++q)assert(fabsf(outside.front[q]-.24f)<.000001f&&fabsf(outside.back[q]-.24f)<.000001f);
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Castle02/05 terrain%u: ten reviewed post bodies joinCC/CD at0..0.0288; nativehigh/collision/art, FEceiling, renumber, editor masks/transforms and mirrorednegative stamps preserved\n",region);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
