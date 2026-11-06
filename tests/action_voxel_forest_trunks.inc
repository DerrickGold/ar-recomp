static unsigned SameTrunkCells(const ActionRoomScene *source,const ActionVoxelGeometry *a,
                               const ActionVoxelGeometry *b) {
  unsigned owned=0;
  for(int y=0;y<(int)ActionRoomScene_TileHeight(source,1);++y)
    for(int x=0;x<(int)ActionRoomScene_TileWidth(source,1);++x) {
      const VoxelTrunkCurve *ca=ActionVoxel_TrunkCurveForCell(a,x,y),*cb=ActionVoxel_TrunkCurveForCell(b,x,y);
      assert(!!ca==!!cb);
      if(ca) {assert(ca->top_y==cb->top_y && ca->base_y==cb->base_y);++owned;}
    }
  return owned;
}
static bool ForestFixedWood(unsigned id) {
  return id==0x24 || id==0x25 || (id>=0x5c && id<=0x67) || (id>=0xbc && id<=0xbf) ||
      id==6 || id==7 || id==14 || id==15 || id==30 || id==31 || id==38 || id==39;
}
static unsigned ForestTrunkContinuity(const ActionRoomScene *source,const ActionVoxelGeometry *a) {
  const int width=ActionRoomScene_TileWidth(source,1),height=ActionRoomScene_TileHeight(source,1);
  unsigned checked=0;
  for(int ty=1;ty+1<height;++ty)for(int tx=1;tx+1<width;++tx) {
    const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(a,tx,ty);if(!curve)continue;
    uint16_t own_word;uint8_t own_id;assert(ActionRoomScene_LookupTile(source,1,tx,ty,&own_word,&own_id));
    if(((own_word>>10)&7)!=5)continue;
    for(unsigned d=0;d<4;++d) {
      const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
      const int nx=tx+dx[d],ny=ty+dy[d];uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(source,1,nx,ny,&word,&id));
      const unsigned q=(ny&1)*2+(nx&1);
      if(((word>>10)&7)!=5 ||
          (source->bg[0].metatiles[id*8+q*2]&2) || ForestFixedWood(id))continue;
      bool joined=false;
      for(int n=0;n<8;++n) {
        const int x=tx*8+(d==0?0:d==1?7:n),y=ty*8+(d==2?0:d==3?7:n);
        const int boundary_y=d==3?y+1:y;
        if(boundary_y<=curve->top_y || boundary_y>=curve->base_y)continue;
        joined|=FixturePixel(source,x,y)>=7 && FixturePixel(source,x+dx[d],y+dy[d])>=7;
      }
      if(joined) {
        const VoxelTrunkCurve *next=ActionVoxel_TrunkCurveForCell(a,nx,ny);
        if(next!=curve)fprintf(stderr,"unjoined native bark cells%d,%d(%02x) -> %d,%d(%02x), curve%d..%d\n",
                              tx,ty,own_id,nx,ny,id,curve->top_y,curve->base_y);
        assert(next==curve);++checked;
      }
    }
  }
  assert(checked);return checked;
}
static void ForestTrunkCurves(const ActionRoomScene *source,const ActionVoxelGeometry *a,
                              const ActionVoxelGeometry *renumbered) {
  const unsigned owned=SameTrunkCells(source,a,renumbered);assert(owned);
  const unsigned shared_joins=ForestTrunkContinuity(source,a);
  assert(!ActionVoxel_TrunkCurveForCell(a,INT_MIN,INT_MAX));
  assert(!ActionVoxel_TrunkCurveForCell(NULL,0,0));
  unsigned count,curved=0,branches=0,stumps=0,roots=0;
  const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForEdge(a,&edges[i]);
    if(edges[i].far_rank==3) {assert(!curve);++branches;}
    if(!curve)continue;
    ++curved;assert(edges[i].far_rank==1 && !edges[i].colliding);
    assert(curve->base_y>curve->top_y && curve->base_y-curve->top_y<=64);
    const float front=ActionVoxel_SceneryRecess(a),back=ActionVoxel_Depth(a);
    assert(ActionVoxel_TrunkFrontDepth(a,curve,curve->top_y)==front);
    assert(ActionVoxel_TrunkBackDepth(a,curve,curve->top_y)==ActionVoxel_TrimBackDepth(a));
    assert(ActionVoxel_TrunkFrontDepth(a,curve,curve->base_y)==back);
    assert(ActionVoxel_TrunkBackDepth(a,curve,curve->base_y)==back);
    float previous=front;
    for(int y=curve->top_y;y<=curve->base_y;++y) {
      const float near=ActionVoxel_TrunkFrontDepth(a,curve,y),far=ActionVoxel_TrunkBackDepth(a,curve,y);
      assert(near>=previous && far>=near && far<=back+.00001f);previous=near;
    }
  }
  assert(curved && branches);
  /* Native map provenance identifies independent reviewed controls in this
   * fixture. Production ownership uses only their decoded material keys. */
  int root_x=-1,root_y=-1;
  for(int y=0;y<(int)ActionRoomScene_TileHeight(source,1);++y)
    for(int x=0;x<(int)ActionRoomScene_TileWidth(source,1);++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(source,1,x,y,&word,&id));
      const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(a,x,y);
      const bool stump=id==6 || id==7 || id==14 || id==15 || id==30 || id==31 || id==38 || id==39;
      const bool rope=id==0xa8 || id==0xa9 || id==0xab || id==0xac || id==0xb1 || id==0xb2;
      if((stump || rope) && FixturePixel(source,x*8,y*8))assert(!curve);
      stumps+=stump;
      if(!(x&1) && !(y&1) && id==0x30 && curve) {
        ++roots;if(root_x<0) {root_x=x;root_y=y;}
      }
    }
  assert(stumps && roots && root_x>=0);
  /* The late leaning tree and tree-head shaft use a different native bark
   * family. Every painted shaft cell within their lower bend shares the
   * root owner: leaving one at the flat plane opens a rectangular gap. */
  for(int anchor_x=1728;anchor_x<=1792;anchor_x+=64) {
    uint16_t word;uint8_t id;
    assert(ActionRoomScene_LookupTile(source,1,anchor_x/8,496/8,&word,&id));
    if(id!=0x30)continue; /* A regional layout may omit this particular tree. */
    const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(a,anchor_x/8,496/8);assert(curve);
    for(int y=448;y<496;y+=8)for(int x=anchor_x-16;x<anchor_x+32;x+=8) {
      assert(ActionRoomScene_LookupTile(source,1,x/8,y/8,&word,&id));
      const bool shaft=(id>=0x9a && id<=0x9f)||(id>=0xa2 && id<=0xa7)||
          id==0xe9||id==0xea||id==0xf1||id==0xf2||id==0xfb||id==0xfc;
      bool painted=false;
      for(int py=0;py<8;++py)for(int px=0;px<8;++px)painted|=FixturePixel(source,x+px,y+py)!=0;
      if(shaft && painted && y+8>curve->top_y)
        assert(ActionVoxel_TrunkCurveForCell(a,x/8,y/8)==curve);
    }
    printf("forest native shaft anchor%d,496: curve%d..%d\n",anchor_x,curve->top_y,curve->base_y);
  }
  static ActionRoomScene fade;fade=*source;memset(fade.palette,0,sizeof(fade.palette));
  ActionVoxelGeometry *edited=ActionVoxel_Create();assert(edited);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);
  assert(ActionVoxel_Build(edited,&fade,NULL,settings));assert(SameTrunkCells(source,a,edited)==owned);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[0].count=2;
  for(unsigned half=0;half<2;++half) {
    DioramaPixelEdit *edit=&terrain.pixel_layers[0].edits[half];
    *edit=(DioramaPixelEdit){.by_cell=true,.x=root_x/2+half,.y=root_y/2};
    for(unsigned y=0;y<16;++y)edit->transparent[y]=0xffff;
  }
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  /* Erasing the native roots cannot leave detached curved shaft ownership. */
  for(int y=root_y-6;y<root_y+2;++y)for(int x=root_x;x<root_x+4;++x)
    assert(!ActionVoxel_TrunkCurveForCell(edited,x,y));
  ActionVoxel_Release(edited);
  printf("forest rooted curves: %u owned/filter cells, %u curved contours, %u roots, %u shared bark joins; branches/stumps/ropes fixed\n",
         owned,curved,roots,shared_joins);
}
