/* A reviewed rock support is structural when elevated, but the complete
 * connected shaft is rear-supported when it ends on native walkable sand.
 * Material fingerprints, collision and native ink prove ownership; tile IDs,
 * screen position and palette RGB are never eligibility inputs. */
static bool DesertSupportMaterial(const CastleMaterial *m) {
  static const uint64_t keys[]={UINT64_C(0x54514daec1abcef0),UINT64_C(0x8411c7075a7251e8),
      UINT64_C(0xb5e5ed801cf90280),UINT64_C(0xe63cc5b4a7d7283c)};
  if(m->collision)return false;
  for(unsigned q=0;q<4;++q)if(m->kind[q]!=kVoxelSolid || (m->words[q]&0x2000))return false;
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=MaterialKey(m,flip);
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(key==keys[i])return true;
  }
  return false;
}
static bool DesertWalkableSand(const ActionVoxelGeometry *c,const CastleMaterial *m,int cx,int cy) {
  if(m->native_surface_family!=kVoxelNativeSurfaceDesertSandLive ||
      (m->collision&3)!=3 || (m->flags&kVoxelSpikeMaterial))return false;
  const int x=cx*16-c->origin_x,y=cy*16-c->origin_y;
  if(x<0 || y<0 || x+16>c->width || y>=c->height)return false;
  for(unsigned px=0;px<16;++px) {
    const unsigned role=c->front[(size_t)y*c->width+x+px];
    if((role&kVoxelKindMask)!=kVoxelSolid || !(role&kVoxelColliding))return false;
  }
  return true;
}
static bool BuildDesertGroundedColumns(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    const uint16_t *tiles,size_t tile_pitch) {
  if(c->profile!=kActionVoxelProfile_KassandoraDesert)return true;
  const int width=c->width/16,height=c->height/16,x0=c->origin_x/16,y0=c->origin_y/16;
  const size_t cells=(size_t)width*height;
  uint8_t *roles=calloc(cells,1);size_t *queue=malloc(cells*sizeof(*queue));
  if(!roles || !queue){free(roles);free(queue);return false;}
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    CastleMaterial m;uint8_t id;
    if(!LookupMaterial(s,stamps,native,x+x0,y+y0,&m,&id,tiles,tile_pitch))continue;
    if(DesertSupportMaterial(&m) && CastleColumnOccupied(c,x+x0,y+y0))roles[(size_t)y*width+x]=1;
    else if(DesertWalkableSand(c,&m,x+x0,y+y0))roles[(size_t)y*width+x]=2;
  }
  bool ok=true;
  for(size_t start=0;ok && start<cells;++start) {
    if(roles[start]!=1)continue;
    size_t count=1;queue[0]=start;roles[start]=3;bool grounded=true;int top=height;
    for(size_t i=0;i<count;++i) {
      const size_t at=queue[i];const int x=at%width,y=at/width;
      if(y<top)top=y;
      /* Every lower boundary must meet actual solid native sand. An empty
       * page edge, pointed noncolliding base, spike or wall cannot qualify. */
      if(y+1>=height)grounded=false;
      else if(roles[at+width]!=1 && roles[at+width]!=3 && roles[at+width]!=2)grounded=false;
      const int nx[]={x-1,x+1,x,x},ny[]={y,y,y-1,y+1};
      for(unsigned d=0;d<4;++d)if(nx[d]>=0 && ny[d]>=0 && nx[d]<width && ny[d]<height) {
        const size_t next=(size_t)ny[d]*width+nx[d];
        if(roles[next]==1){roles[next]=3;queue[count++]=next;}
      }
    }
    if(!grounded)continue;
    if(!c->column_cells) {
      c->column_cells=calloc((size_t)c->width/8,c->height/8);
      if(!c->column_cells){ok=false;break;}
    }
    /* A single native character row meets the full cap and then leaves the
     * lower shaft recessed. Even the shortest32px column has24px of clear
     * rear-supported body; there is no abrupt ledge at the cap underside. */
    const VoxelTrunkCurve curve={.top_y=(short)((top+y0)*16),.base_y=(short)((top+y0)*16+8)};
    unsigned owner=0;
    while(owner<c->column_curve_count && c->column_curves[owner].top_y!=curve.top_y)++owner;
    if(owner==c->column_curve_count) {
      if(owner==kVoxelMaxCastleColumns){ok=false;break;}
      c->column_curves[c->column_curve_count++]=curve;
    }
    for(size_t i=0;i<count;++i) {
      const int x=queue[i]%width,y=queue[i]/width;
      for(unsigned q=0;q<4;++q)c->column_cells[(size_t)(y*2+(q>>1))*(c->width/8)+x*2+(q&1)]=(uint8_t)(owner+1);
    }
  }
  free(roles);free(queue);return ok;
}
