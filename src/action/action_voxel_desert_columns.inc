/* Reviewed grounded supports share a connected underside/shaft taper.
 * Rear supports end on walkable sand; foreground supports may meet rock and
 * retain their native placement ahead of actors. Elevated supports stay put.
 * Material fingerprints, collision and native ink prove ownership; tile IDs,
 * screen position and palette RGB are never eligibility inputs. */
static bool DesertSupportMaterial(const CastleMaterial *m) {
  static const uint64_t keys[]={UINT64_C(0x54514daec1abcef0),UINT64_C(0x8411c7075a7251e8),
      UINT64_C(0xb5e5ed801cf90280),UINT64_C(0xe63cc5b4a7d7283c),
      UINT64_C(0x32a5cca2ba60a2f4),UINT64_C(0x1fe3bb5b5ad2b200),
      UINT64_C(0x2b405abd0f2d985c),UINT64_C(0x6e853683f98438d8)};
  if(m->collision)return false;
  for(unsigned q=0;q<4;++q)if(m->kind[q]!=kVoxelSolid)return false;
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=MaterialKey(m,flip);
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)if(key==keys[i])return true;
  }
  return false;
}
static unsigned DesertColumnGround(const ActionVoxelGeometry *c,const CastleMaterial *m,int cx,int cy) {
  const bool sand=m->native_surface_family==kVoxelNativeSurfaceDesertSandLive;
  if((m->collision&3)!=3 || (!sand && m->collision!=15) || (m->flags&kVoxelSpikeMaterial))return 0;
  for(unsigned q=0;q<4;++q)if(m->kind[q]!=kVoxelSolid || (m->words[q]&0x2000))return 0;
  const int x=cx*16-c->origin_x,y=cy*16-c->origin_y;
  if(x<0 || y<0 || x+16>c->width || y>=c->height)return 0;
  for(unsigned px=0;px<16;++px) {
    const unsigned role=c->front[(size_t)y*c->width+x+px];
    if((role&kVoxelKindMask)!=kVoxelSolid || !(role&kVoxelColliding))return 0;
  }
  return sand?2:6;
}
static bool DesertShadowOccupied(const ActionVoxelGeometry *c,int cx,int cy) {
  const int x=cx*16-c->origin_x,y=cy*16-c->origin_y;bool any=false;
  for(int py=0;py<16;++py)for(int px=0;px<16;++px) {
    const unsigned ink=c->front[(size_t)(y+py)*c->width+x+px];
    if(!ink)continue;
    if((ink&kVoxelKindMask)!=kVoxelSolid || (ink&(kVoxelColliding|kVoxelHighPriority)))return false;
    any=true;
  }
  return any;
}
static bool BuildDesertGroundedColumns(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
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
    else if(m.collision)roles[(size_t)y*width+x]=DesertColumnGround(c,&m,x+x0,y+y0);
    else if(m.column_shadow && !m.collision && DesertShadowOccupied(c,x+x0,y+y0))roles[(size_t)y*width+x]=4;
  }
  bool ok=true;
  for(size_t start=0;ok && start<cells;++start) {
    if(roles[start]!=1)continue;
    size_t count=1;queue[0]=start;roles[start]=3;
    bool grounded=true,sand_grounded=true,foreground=false;int top=height,shadow_top=height;
    for(size_t i=0;i<count;++i) {
      const size_t at=queue[i];const int x=at%width,y=at/width;
      const bool shaft=roles[at]==3;
      if(shaft && y<top)top=y;
      if(!shaft && y<shadow_top)shadow_top=y;
      /* Foreground shafts can meet a rock bank; rear-supported shafts must
       * end on walkable sand. A page edge, hole or pointed base cannot qualify. */
      if(shaft) {
        CastleMaterial m;uint8_t id;
        if(!LookupMaterial(s,stamps,native,x+x0,y+y0,&m,&id,tiles,tile_pitch)){ok=false;break;}
        for(unsigned q=0;q<4;++q)foreground|=!!(m.words[q]&0x2000);
        if(y+1>=height)grounded=false;
        else {
          const unsigned below=roles[at+width];
          if(below==6)sand_grounded=false;
          else if(below!=1 && below!=3 && below!=2)grounded=false;
        }
      }
      const int nx[]={x-1,x+1,x,x},ny[]={y,y,y-1,y+1};
      for(unsigned d=0;d<4;++d)if(nx[d]>=0 && ny[d]>=0 && nx[d]<width && ny[d]<height) {
        const size_t next=(size_t)ny[d]*width+nx[d];
        if(roles[next]==1 || roles[next]==4){roles[next]=roles[next]==1?3:5;queue[count++]=next;}
      }
    }
    if(!ok)break;
    if(!grounded || (!foreground && !sand_grounded))continue;
    if(!c->column_cells) {
      c->column_cells=calloc((size_t)c->width/8,c->height/8);
      if(!c->column_cells){ok=false;break;}
    }
    /* Turn sharply under the overhanging lip, then ease through the shadow
     * and one shaft row. A source without reviewed shadow art retains its
     * short cap join. The native walking top never moves. */
    const bool shadow=shadow_top<top;
    const VoxelTrunkCurve curve={.top_y=(short)(((shadow?shadow_top:top)+y0)*16),
        .base_y=(short)((top+y0)*16+(shadow?16:8)),.ease_out=shadow,.foreground=foreground};
    unsigned owner=0;
    while(owner<c->column_curve_count && (c->column_curves[owner].top_y!=curve.top_y ||
        c->column_curves[owner].base_y!=curve.base_y ||
        c->column_curves[owner].ease_out!=curve.ease_out ||
        c->column_curves[owner].foreground!=curve.foreground))++owner;
    if(owner==c->column_curve_count) {
      if(owner==kVoxelMaxCastleColumns){ok=false;break;}
      c->column_curves[c->column_curve_count++]=curve;
    }
    for(size_t i=0;i<count;++i) {
      const int x=queue[i]%width,y=queue[i]/width;
      for(unsigned q=0;q<4;++q)c->column_cells[(size_t)(y*2+(q>>1))*(c->width/8)+x*2+(q&1)]=(uint8_t)(owner+1);
      /* A base's native sand fringe joins the walkable dune in front of the
       * rock. Partition only reviewed ink at a proven ground contact; floating
       * pillars and the upper shaft retain their complete structural faces. */
      if(y+1<height && roles[(size_t)(y+1)*width+x]==2) {
        CastleMaterial m;uint8_t id;
        if(!LookupMaterial(s,stamps,native,x+x0,y+y0,&m,&id,tiles,tile_pitch)){ok=false;break;}
        const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,x+x0,y+y0,id):NULL;
        m.flags|=kVoxelPixelRecessMaterial;
        memcpy(m.face_colors,m.column_recess_colors,sizeof(m.face_colors));
        for(unsigned q=0;q<4;++q)
          if(!BuildPixelRecess(c,&m,edit,q,(x+x0)*2+(q&1),(y+y0)*2+(q>>1),0,false)){ok=false;break;}
      }
    }
  }
  free(roles);free(queue);return ok;
}
