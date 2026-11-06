/* A reviewed painted ground band folds onto the island top. Rock relief,
 * grass, open water and the island underside retain their native surfaces.
 * The donor is semantic artwork immediately above reviewed solid earth. */
#include "action_voxel_marsh_rock_backing.inc"
static unsigned MarshFoldAt(const ActionVoxelGeometry *c,const uint8_t *cells,int x,int y) {
  x-=c->origin_x;y-=c->origin_y;
  if(x<0 || y<0 || x>=c->width || y>=c->height || (y&15))return 0;
  return cells[(size_t)(y/16)*(c->width/16)+x/16];
}
static bool BuildMarshFoldEdges(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_BloodpoolMarsh || !c->edge_count)return true;
  const int width=c->width/16,height=c->height/16;
  uint8_t *cells=calloc((size_t)width,height);
  VoxelEdge *edges=malloc(sizeof(c->edges));
  if(!cells || !edges){free(cells);free(edges);return false;}
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    CastleMaterial earth,band;uint8_t id;
    const int cx=x+c->origin_x/16,cy=y+c->origin_y/16;
    if(!LookupMaterial(s,stamps,native,cx,cy,&earth,&id,tiles,tile_pitch) ||
       !(earth.flags&kVoxelTerrainBodyMaterial) ||
       !LookupMaterial(s,stamps,native,cx,cy-1,&band,&id,tiles,tile_pitch) ||
       !band.rear_fold_pixels)continue;
    bool solid=true;
    for(unsigned q=0;q<4;++q)solid &= earth.kind[q]==kVoxelSolid && !(earth.words[q]&0x2000);
    if(solid) {
      cells[(size_t)y*width+x]=band.rear_fold_pixels;
      const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy-1,id):NULL;
      if(!BuildMarshRockGroundBacking(c,s,terrain,stamps,native,&band,cx,cy-1,edit,palette,tiles,tile_pitch)) {
        free(cells);free(edges);return false;
      }
    }
  }
  unsigned count=0;
  for(unsigned i=0;i<c->edge_count;++i) {
    const VoxelEdge original=c->edges[i];
    bool eligible=false;
    if(original.far_rank==2 && original.colliding && original.side!=2) {
      const bool vertical=(original.side&1)!=0;
      for(int at=0;at<original.length+(vertical?1:0) && !eligible;++at)
        eligible=MarshFoldAt(c,cells,original.x+(vertical?0:at),original.y+(vertical?at:0))!=0;
    }
    for(int at=0;at<original.length;) {
      VoxelEdge e=original;
      const bool vertical=(e.side&1)!=0;
      const int position=(vertical?e.y-c->origin_y:e.x-c->origin_x)+at;
      const int remaining=original.length-at;
      const int step=eligible && 16-(position&15)<remaining ? 16-(position&15) : remaining;
      e.x+=vertical?0:at;e.y+=vertical?at:0;e.length=step;
      if(eligible) {
        e.rear_fold_top=MarshFoldAt(c,cells,e.x,e.y);
        if(vertical)e.rear_fold_bottom=MarshFoldAt(c,cells,e.x,e.y+step);
      }
      if(count==kVoxelMaxEdges){free(cells);free(edges);return false;}
      edges[count++]=e;at+=step;
    }
  }
  memcpy(c->edges,edges,count*sizeof(*edges));c->edge_count=count;
  memset(c->pass_counts,0,sizeof(c->pass_counts));c->high_deck_count=0;
  for(unsigned i=0;i<c->patch_count;++i)
    c->high_deck_count+=c->patches[i].high_priority && c->patches[i].kind==kVoxelPatch_BranchRear;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&c->edges[i];
    c->pass_edges[e->high_priority?1:0][c->pass_counts[e->high_priority?1:0]++]=i;
    if(e->rear_trim)c->pass_edges[2][c->pass_counts[2]++]=i;
    if(e->high_priority && e->colliding && e->side==0 && e->far_rank>=2) {
      c->pass_edges[3][c->pass_counts[3]++]=i;++c->high_deck_count;
    }
  }
  free(cells);free(edges);return true;
}
