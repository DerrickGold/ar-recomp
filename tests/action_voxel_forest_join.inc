#include "action/action_voxel_internal.h"
/* Flattening the walking cap must not duplicate foliage over the original
 * brown rear ground row. Captured native artwork, including a cropped band,
 * remains authoritative under palette effects and explicit editor overrides. */
static void ForestNativeRearRows(const ActionRoomScene *source,const ActionVoxelGeometry *g) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  int x0=-1,y0=-1;
  for(unsigned i=0;i<count && x0<0;++i) {
    const VoxelEdge *e=&edges[i];uint16_t word;uint8_t id;
    if(e->side || !e->flat_rear_join || (e->y&15) ||
        !ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8-2,&word,&id))continue;
    /* Fixture specimens only; production uses semantic RearOnly ownership. */
    if(id!=0x08 && id!=0x09 && id!=0x0a && id!=0x0b && id!=0x0c && id!=0x13)continue;
    int joins[16];ForestJoinColumns(g,e->x&~15,e->y-16,joins);
    bool complete=true;for(unsigned x=0;x<16;++x)complete&=joins[x]==e->y;
    if(complete){x0=e->x&~15;y0=e->y-16;}
  }
  assert(x0>=0 && y0>=0);
  uint32_t low[256],rear[256],front[256];
  for(unsigned phase=0;phase<2;++phase) {
    const ActionRoomSceneFrameState frame={.brightness=phase?7:15,.fixed_color=phase?0x0421:0,.cgadsub=phase?0x81:0};
    for(unsigned i=0;i<256;++i)low[i]=ForestNativeColor(source,x0+i%16,y0+i/16,&frame);
    assert(ActionVoxel_Split(g,x0,y0,16,16,16,&frame,low,16,rear,front));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      assert(!front[y*16+x]);
      assert(rear[y*16+x]==low[y*16+x]);
    }
    assert(ActionVoxel_Split(g,x0,y0+9,16,7,16,&frame,low+9*16,16,rear,front));
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<16;++x)
      assert(rear[y*16+x]==low[(y+9)*16+x]);
  }
  static DioramaRoomOverride edit;memset(&edit,0,sizeof(edit));
  ActionVoxelGeometry *modified=ActionVoxel_Create();assert(modified);
  const ActionRoomSceneFrameState frame={.brightness=15};
  edit.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&edit.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=x0/16,.y=y0/16};
  pixel->transparent[11]=0x8000; /* Preserve a deliberately open destination. */
  pixel->transparent[4]=0x4000;  /* A separate plant edit cannot change ground. */
  assert(ActionVoxel_Build(modified,source,&edit,ActionVoxel_SettingsForRoom(1,1)));
  for(unsigned i=0;i<256;++i)low[i]=0xffa1b2c3;low[11*16]=low[4*16+1]=0;
  assert(ActionVoxel_Split(modified,x0,y0,16,16,16,&frame,low,16,rear,front));
  assert(rear[11*16]==0 && rear[12*16]==low[12*16]);
  for(unsigned y=9;y<16;++y)assert(rear[y*16+1]==low[y*16+1]);
  memset(&edit,0,sizeof(edit));edit.voxel.cell_count=1;
  DioramaVoxelCell *cell=&edit.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=x0/16,.y=y0/16,.fields=kDioramaVoxelCell_Backing|kDioramaVoxelCell_BackArt,
      .backing=kDioramaVoxelBacking_Replace,.backing_art={.set=true,.bg=1,.size=16}};
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(source,1,x0/8+(q&1),(y0+16)/8+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(modified,source,&edit,ActionVoxel_SettingsForRoom(1,1)));
  assert(ActionVoxel_Split(modified,x0,y0,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(rear[y*16+x]==ForestNativeColor(source,x0+x,y0+16+y,&frame));
  ActionVoxel_Release(modified);
  /* Audit every wholly rear-classified plant/ground cell, not just this join.
   * Mixed thorn/root cells have independent backing contracts covered by
   * ForestProfiles and ForestRootVerify; their rear palette membership does
   * not make their entire native tile an original background row. */
  unsigned checked=0;
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(source,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(source,1)/2;++cx) {
      bool relevant=false,mixed=false;
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const size_t at=(size_t)(cy*16+py-g->origin_y)*g->width+cx*16+px-g->origin_x;
        const unsigned kind=g->front[at]&kVoxelKindMask;
        relevant|=kind==kVoxelRearOnly;
        mixed|=kind && kind!=kVoxelRearOnly;
      }
      if(!relevant || mixed)continue;
      for(unsigned p=0;p<256;++p)low[p]=ForestNativeColor(source,cx*16+p%16,cy*16+p/16,&frame);
      assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const size_t at=(size_t)(cy*16+py-g->origin_y)*g->width+cx*16+px-g->origin_x;
        const unsigned p=py*16+px;
        if((g->front[at]&kVoxelKindMask)!=kVoxelRearOnly || !low[p])continue;
        if(rear[p]!=low[p] || front[p])fprintf(stderr,
            "forest native rear mismatch cell%d,%d pixel%u,%u flags%u rear%08x front%08x native%08x\n",
            cx,cy,px,py,g->front[at],rear[p],front[p],low[p]);
        assert(rear[p]==low[p] && !front[p]);++checked;
      }
    }
  assert(checked>10000 && g->rear_fill_count<kVoxelMaxRearFills);
  printf("forest native rear rows: %u original pixels preserved; cropped ground band, palette effects, masks and explicit backing passed\n",checked);
}
