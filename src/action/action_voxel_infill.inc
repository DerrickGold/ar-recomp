/* Compile shared fill ownership once, before the ordinary pixel pass. A
 * column under a flat cap cannot become a projecting pier in a connected
 * ramp wall. Visible rear roots veto the whole component; authored holes
 * still break only their own column's cap inheritance. */
static bool BuildInfillOwnership(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t *tiles,size_t tile_pitch) {
  bool possible=false;
  for(unsigned i=0;i<256&&!possible;++i)possible=(native[i].flags&kVoxelInfillMaterial)!=0;
  for(unsigned i=0;stamps&&i<stamps->count&&!possible;++i) {
    const DioramaTileStamp *stamp=&stamps->cells[i];CastleMaterial material;uint8_t id;
    possible=LookupMaterial(s,stamps,native,stamp->x,stamp->y,&material,&id,tiles,tile_pitch)&&
        (material.flags&kVoxelInfillMaterial);
  }
  if(!possible)return true;
  enum { PendingFront=254, PendingRear=255 };
  const unsigned width=c->width/16,height=c->height/16;
  const int x0=c->origin_x/16,y0=c->origin_y/16;
  unsigned char *roles=c->semantic_cells;
  size_t candidates=0;
  for(unsigned x=0;x<width;++x) {
    bool front=false;
    for(unsigned y=0;y<height;++y) {
      const int cx=x0+(int)x,cy=y0+(int)y;
      CastleMaterial material;uint8_t id;
      if(!LookupMaterial(s,stamps,native,cx,cy,&material,&id,tiles,tile_pitch)) {
        front=false;continue;
      }
      const DioramaPixelEdit *edit=terrain?
          DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,id):NULL;
      if(!(material.flags&(kVoxelInfillMaterial|kVoxelTerrainBodyMaterial))||
          !MaterialVisible(&material,edit)){front=false;continue;}
      if(material.flags&kVoxelInfillMaterial) {
        roles[(size_t)y*width+x]=front?PendingFront:PendingRear;++candidates;
      } else front=(material.flags&kVoxelTerrainBodyMaterial)!=0;
    }
  }
  if(!candidates)return true;
  size_t *queue=malloc(candidates*sizeof(*queue));
  if(!queue)return false;
  for(size_t start=0;start<(size_t)width*height;++start) {
    if(roles[start]<PendingFront)continue;
    size_t count=1;queue[0]=start;
    roles[start]=roles[start]==PendingFront?kVoxelSemantic_Platform:kVoxelSemantic_Rear;
    bool rear_root=false;
    for(size_t at=0;at<count;++at) {
      const size_t cell=queue[at];
      const unsigned x=cell%width,y=cell/width;
      if(!y||!roles[cell-width]) {
        CastleMaterial above;uint8_t id;
        if(LookupMaterial(s,stamps,native,x0+(int)x,y0+(int)y-1,&above,&id,tiles,tile_pitch)) {
          const DioramaPixelEdit *edit=terrain?
              DioramaLayerOrder_PixelEditAt(terrain,0,x0+(int)x,y0+(int)y-1,id):NULL;
          rear_root|=!(above.flags&kVoxelTerrainBodyMaterial)&&MaterialVisible(&above,edit);
        }
      }
      size_t neighbors[4];unsigned n=0;
      if(x)neighbors[n++]=cell-1;
      if(x+1<width)neighbors[n++]=cell+1;
      if(y)neighbors[n++]=cell-width;
      if(y+1<height)neighbors[n++]=cell+width;
      for(unsigned i=0;i<n;++i) {
        const size_t next=neighbors[i];
        if(roles[next]<PendingFront)continue;
        roles[next]=roles[next]==PendingFront?kVoxelSemantic_Platform:kVoxelSemantic_Rear;
        queue[count++]=next;
      }
    }
    if(rear_root)for(size_t i=0;i<count;++i)roles[queue[i]]=kVoxelSemantic_Rear;
  }
  free(queue);
  return true;
}
