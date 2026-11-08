/* Lower trunks are native-art components, anchored to their own grounded
 * mixed root. Eight-pixel ownership is compiled once; camera motion and live
 * palette colours never choose a tree or change its curve. */
static bool BuildForestTrunkCurves(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_FillmoreForest)return true;
  const int width=c->width/8,height=c->height/8,total=width*height;
  enum { bark=1,root=2,branch=4,seen_root=8,occupied=16 };
  unsigned char *types=calloc((size_t)total,1),*halo=calloc((size_t)total,1);
  int *queue=malloc((size_t)total*sizeof(*queue));
  c->trunk_cells=calloc((size_t)total,1);
  if(!types || !halo || !queue || !c->trunk_cells) {
    free(types);free(halo);free(queue);return false;
  }
  for(int cy=0;cy<height/2;++cy)for(int cx=0;cx<width/2;++cx) {
    CastleMaterial material;uint8_t provenance;
    if(!LookupMaterial(s,stamps,native,cx+c->origin_x/16,cy+c->origin_y/16,
                       &material,&provenance,tiles,tile_pitch))continue;
    for(unsigned q=0;q<4;++q) {
      const int tx=cx*2+(q&1),ty=cy*2+(q>>1),at=ty*width+tx;
      /* Native opacity, not faded RGB, keeps neighbouring art out of the
       * filter halo even when the old Thin contour path is black. */
      if(material.kind[q] && material.kind[q]!=kVoxelRearOnly && material.kind[q]!=kVoxelFlatParapet)
        for(unsigned pixel=0;pixel<64;++pixel) {
          const unsigned index=material.pixels[q][pixel];
          if(index && !(material.rear_colors[q]&(1u<<index)))types[at]|=occupied;
        }
      if(material.flags&kVoxelBranchMaterial) {types[at]|=branch;continue;}
      if(!(material.flags&(kVoxelTreeBarkMaterial|kVoxelTreeRootMaterial)))continue;
      if(material.collision || material.kind[q]!=kVoxelForeground)continue;
      bool wood=false;
      for(int y=0;y<8;++y)for(int x=0;x<8;++x)
        wood|=(c->front[(size_t)(ty*8+y)*c->width+tx*8+x]&kVoxelKindMask)==kVoxelForeground;
      if(wood)types[at]|=bark|((material.flags&kVoxelTreeRootMaterial)?root:0);
    }
  }
  const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
  for(int seed=0;seed<total;++seed) {
    if(!(types[seed]&root) || (types[seed]&seen_root))continue;
    int count=1;queue[0]=seed;types[seed]|=seen_root;
    int base_y=c->origin_y;bool supported=false;
    /* Join only the mixed-root wood, never a nearby decorative tree. */
    for(int head=0;head<count;++head) {
      const int at=queue[head],cx=at%width,cy=at/width;
      for(int y=0;y<8;++y)for(int x=0;x<8;++x)
        if((c->front[(size_t)(cy*8+y)*c->width+cx*8+x]&kVoxelKindMask)==kVoxelForeground)
          base_y=base_y>c->origin_y+cy*8+y+1?base_y:c->origin_y+cy*8+y+1;
      /* The lower half of a mixed root can contain plants only. Its support
       * is the native ground below the complete16px root, not the next bark
       * character; otherwise valid roots disappear from this contract. */
      const int support_y=(cy/2+1)*16;
      for(int sy=support_y;sy<=support_y+16 && sy+16<=c->height;sy+=16) {
        bool rear_border=true,solid=false;
        for(int y=0;y<16;++y)for(int x=0;x<8;++x) {
          const unsigned pixel=c->front[(size_t)(sy+y)*c->width+cx*8+x];
          solid|=(pixel&kVoxelColliding)!=0;
          const unsigned kind=pixel&kVoxelKindMask;
          rear_border&=!kind || kind==kVoxelRearOnly;
        }
        supported|=solid;
        if(solid || !rear_border)break;
      }
      for(unsigned d=0;d<4;++d) {
        const int nx=cx+dx[d],ny=cy+dy[d];
        if(nx<0 || ny<0 || nx>=width || ny>=height)continue;
        const int next=ny*width+nx;
        if((types[next]&root) && !(types[next]&seen_root)) {
          types[next]|=seen_root;queue[count++]=next;
        }
      }
    }
    if(!supported)continue;
    if(c->trunk_curve_count==kVoxelMaxTrunkCurves) {
      free(types);free(halo);free(queue);return false;
    }
    const unsigned owner=++c->trunk_curve_count;
    int top_y=base_y-64>c->origin_y?base_y-64:c->origin_y;
    /* The upper untouched shaft owns the cell above this boundary. Begin
     * on the next world8px edge, retaining at most64px of bend and an exact
     * flat-to-curved join even when the root silhouette ends mid-cell. */
    top_y=c->origin_y+((top_y-c->origin_y+7)/8)*8;
    for(int i=0;i<count;++i)c->trunk_cells[queue[i]]=owner;
    /* A bounded native shaft component supplies the bend. A nearby branch
     * terminates it at that branch's existing contact, rather than moving the
     * platform, its rope or a native object attachment. */
    for(int head=0;head<count;++head) {
      const int at=queue[head],cx=at%width,cy=at/width;
      for(unsigned d=0;d<4;++d) {
        const int nx=cx+dx[d],ny=cy+dy[d];
        if(nx<0 || ny<0 || nx>=width || ny>=height)continue;
        const int next=ny*width+nx,cell_y=c->origin_y+ny*8;
        if((types[next]&branch) && cell_y+8<=base_y && cell_y+8>top_y)
          top_y=cell_y+8;
        if(!(types[next]&bark) || c->trunk_cells[next] || cell_y+8<=base_y-64 || cell_y>=base_y)continue;
        c->trunk_cells[next]=owner;queue[count++]=next;
      }
    }
    for(int i=0;i<count;++i)
      if(c->origin_y+(queue[i]/width+1)*8<=top_y || top_y>=base_y)c->trunk_cells[queue[i]]=0;
    if(top_y>=base_y) {--c->trunk_curve_count;continue;}
    c->trunk_curves[owner-1]=(VoxelTrunkCurve){.top_y=(short)top_y,.base_y=(short)base_y};
  }
  /* Carry only the empty filter footprint. Do not propagate through the halo
   * or assign a curve to an occupied branch, rope, stump, ground or canopy. */
  for(int at=0;at<total;++at)if(c->trunk_cells[at]) {
    const int cx=at%width,cy=at/width;
    for(int ny=cy-1;ny<=cy+1;++ny)for(int nx=cx-1;nx<=cx+1;++nx) {
      if(nx<0 || ny<0 || nx>=width || ny>=height)continue;
      const int next=ny*width+nx;
      if(!c->trunk_cells[next] && !halo[next] && !(types[next]&occupied))halo[next]=c->trunk_cells[at];
    }
  }
  for(int at=0;at<total;++at)if(halo[at])c->trunk_cells[at]=halo[at];
  free(types);free(halo);free(queue);
  return true;
}
