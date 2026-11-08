/* Editor selection is a bounded query over immutable source roles. It never
 * changes geometry, follows animated RGB, or guesses object IDs from tiles. */
static VoxelSemanticKind SemanticKindAt(const ActionVoxelGeometry *c,int x,int y) {
  const unsigned role=c->semantic_cells?c->semantic_cells[(size_t)y*(c->width/16)+x]:0;
  if(role)return (VoxelSemanticKind)role;
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(c,x+c->origin_x/16,y+c->origin_y/16);
  return a && a->surface?kVoxelSemantic_Authored:kVoxelSemantic_None;
}
static const char *SemanticLabel(VoxelSemanticKind kind) {
  static const char *const labels[]={"No classified object","Connected rear scenery",
      "Connected foliage","Connected scenery","Connected platform or ground",
      "Water surface","Connected hazard","Gate","Column","Branch","Tree wood",
      "Stump","Connected authored surface"};
  return kind<=kVoxelSemantic_Authored?labels[kind]:labels[0];
}
static bool SemanticCompatible(VoxelSemanticKind a,VoxelSemanticKind b) {
  return a==b || ((a==kVoxelSemantic_Branch || a==kVoxelSemantic_TreeWood) &&
      (b==kVoxelSemantic_Branch || b==kVoxelSemantic_TreeWood));
}
ActionVoxelGroupStatus ActionVoxel_SemanticGroup(const ActionVoxelGeometry *c,
    int x,int y,VoxelSemanticGroup *out,VoxelGroupCell *cells,unsigned capacity) {
  if(!out)return kActionVoxelGroup_Invalid;
  *out=(VoxelSemanticGroup){0};
  if(!c || !c->ready || !cells || !capacity || c->width<=0 || c->height<=0)
    return kActionVoxelGroup_Invalid;
  const int width=c->width/16,height=c->height/16,ox=c->origin_x/16,oy=c->origin_y/16;
  /* Compare before subtraction so an arbitrary editor query cannot overflow. */
  if(x<ox || y<oy || x>=ox+width || y>=oy+height)return kActionVoxelGroup_Empty;
  const size_t total=(size_t)width*height;
  if(total>kVoxelAuthoredCellMax)return kActionVoxelGroup_Invalid;
  const VoxelSemanticKind seed=SemanticKindAt(c,x-ox,y-oy);
  out->kind=seed;out->label=SemanticLabel(seed);
  if(seed==kVoxelSemantic_None)return kActionVoxelGroup_Empty;
  unsigned char *seen=calloc(total,1);if(!seen)return kActionVoxelGroup_Invalid;
  if(capacity>kVoxelSemanticGroupMaxCells)capacity=kVoxelSemanticGroupMaxCells;
  unsigned count=1;cells[0]=(VoxelGroupCell){x,y};seen[(size_t)(y-oy)*width+x-ox]=1;
  int x0=x,y0=y,x1=x+1,y1=y+1;
  const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
  for(unsigned head=0;head<count;++head) {
    const int cx=cells[head].x-ox,cy=cells[head].y-oy;
    if(SemanticKindAt(c,cx,cy)==kVoxelSemantic_TreeWood)out->kind=kVoxelSemantic_TreeWood;
    for(unsigned side=0;side<4;++side) {
      const int nx=cx+dx[side],ny=cy+dy[side];
      if(nx<0 || ny<0 || nx>=width || ny>=height)continue;
      const size_t at=(size_t)ny*width+nx;
      if(seen[at])continue;
      seen[at]=1;
      if(!SemanticCompatible(seed,SemanticKindAt(c,nx,ny)))continue;
      if(count==capacity) {
        out->label=SemanticLabel(out->kind);free(seen);return kActionVoxelGroup_TooLarge;
      }
      cells[count++]=(VoxelGroupCell){nx+ox,ny+oy};
      if(nx+ox<x0)x0=nx+ox;if(ny+oy<y0)y0=ny+oy;
      if(nx+ox+1>x1)x1=nx+ox+1;if(ny+oy+1>y1)y1=ny+oy+1;
      /* One continuous terrain body can run beneath many apparent floors.
       * There is no native object boundary at an arbitrary editor cut, and
       * its newly exposed interior walls do not exist. Refuse that ambiguous
       * selection instead of selecting the level or inventing a truncation. */
      if(seed==kVoxelSemantic_Platform &&
          (x1-x0>kVoxelSemanticPlatformMaxSpan || y1-y0>kVoxelSemanticPlatformMaxSpan)) {
        free(seen);return kActionVoxelGroup_Ambiguous;
      }
    }
  }
  free(seen);out->label=SemanticLabel(out->kind);
  out->x=x0;out->y=y0;out->width=x1-x0;out->height=y1-y0;out->count=count;
  return kActionVoxelGroup_Complete;
}
static bool SemanticRectIntersects(int x,int y,int px,int py,int width,int height) {
  return x<px+width && x+16>px && y<py+height && y+16>py;
}
const char *ActionVoxel_TransformUnsupportedReason(const ActionVoxelGeometry *c,int x,int y) {
  if(!c || !c->ready || x<c->origin_x/16 || y<c->origin_y/16 ||
      x>=(c->origin_x+c->width)/16 || y>=(c->origin_y+c->height)/16)
    return "Select a cell inside the active Voxel room.";
  const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(c,x,y);
  if(authored && authored->surface)return NULL;
  if(authored && !authored->native_front_hidden && !authored->native_transform_requested)
    return "Reset backing-only overrides before moving or resizing this native structural group.";
  x*=16;y*=16;
  for(unsigned i=0;i<c->gate_count;++i) {
    const VoxelGate *p=&c->gates[i];
    if(SemanticRectIntersects(x,y,p->x,p->y,p->width?p->width:8,p->height))
      return "Native gates cannot be moved or resized as structural groups yet.";
  }
  for(unsigned i=0;i<c->rear_trim_count;++i) {
    const VoxelRearTrim *p=&c->rear_trims[i];
    if(SemanticRectIntersects(x,y,p->x,p->y,p->width,16))
      return "Native rear trim cannot be moved or resized as a structural group yet.";
  }
  for(unsigned i=0;i<c->patch_count;++i) {
    const VoxelPatch *p=&c->patches[i];
    if(p->kind==kVoxelPatch_BranchRear ||
        (p->kind==kVoxelPatch_SideProfile && c->profile==kActionVoxelProfile_FillmoreForest))continue;
    if(SemanticRectIntersects(x,y,p->x,p->y,p->width,p->height))
      return "Crossed scenery and hazards cannot be moved or resized as structural groups yet.";
  }
  for(unsigned i=0;i<c->water_surface_count;++i) {
    const VoxelWaterSurface *p=&c->water_surfaces[i];
    if(!p->background && SemanticRectIntersects(x,y,p->x,p->y,p->width,16))
      return "Water surfaces cannot be moved or resized as structural groups yet.";
  }
  for(unsigned i=0;i<c->waterfall_count;++i) {
    const VoxelWaterfall *p=&c->waterfalls[i];
    if(SemanticRectIntersects(x,y,p->x,p->y,p->width,p->height))
      return "Waterfalls cannot be moved or resized as structural groups yet.";
  }
  if(c->front)for(int yy=0;yy<16;++yy)for(int xx=0;xx<16;++xx) {
    const unsigned kind=c->front[(size_t)(y+yy-c->origin_y)*c->width+x+xx-c->origin_x]&kVoxelKindMask;
    if(kind==kVoxelSolid || kind==kVoxelForeground || kind==kVoxelThin)return NULL;
  }
  return "This cell has no native structural surface to move or resize.";
}
