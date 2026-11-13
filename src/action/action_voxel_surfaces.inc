/* Full native artwork for exposed structural faces. This optional pass runs
 * only when immutable room geometry is compiled. A rejected match retains the
 * old contour sampling; it never changes the native silhouette or priority. */
enum { kVoxelAutomaticSurfaceArtMax=1024 };
typedef struct VoxelSurfaceBuild {
  ActionVoxelGeometry *geometry;
  const ActionRoomScene *scene;
  const DioramaRoomOverride *terrain;
  const DioramaStampLayerOverride *stamps;
  const CastleMaterial *native;
  const uint16_t *palette,*tiles;
  size_t pitch;
  uint32_t *slots;
  bool used[kVoxelAuthoredArtMax];
  unsigned initial_art;
  bool native_surfaces;
  uint8_t *top_donors;
  int32_t *top_donor_cache; /* Per source quadrant: zero unresolved, -1 absent. */
} VoxelSurfaceBuild;
typedef struct VoxelSurfacePatch {
  int x,y,size;
  bool derived,native_x;
  uint16_t pixels[256];
} VoxelSurfacePatch;
typedef struct VoxelSurfaceCandidate {
  VoxelEdge edge;
  VoxelSurfacePatch patch;
  bool patch_available;
} VoxelSurfaceCandidate;
static bool SurfaceStaticWord(const ActionRoomScene *s,unsigned word) {
  const size_t start=(word&1023)*32;
  /* Extra characters have separate dependency tracking for explicit donors.
   * Automatic art must not introduce an untracked extra-bank dependency. */
  if(start>=sizeof(s->characters))return false;
  if(!ActionRoomScene_HasCharacterAnimation(s))return true;
  const size_t target=(size_t)ActionRoomScene_CharacterAnimationTarget(s)*2;
  const size_t end=target+ActionRoomScene_CharacterAnimationStride(s);
  return start+32<=target || start>=end;
}
static bool SurfaceStructural(const CastleMaterial *m,unsigned q) {
  const unsigned excluded=kVoxelWaterSurfaceMaterial|kVoxelOpaqueSurfaceMaterial|
      kVoxelGrowthMaterial|kVoxelSpikeMaterial|
      kVoxelPixelRecessMaterial|kVoxelBranchMaterial;
  return m->kind[q]==kVoxelSolid && !(m->flags&excluded);
}
static bool SurfaceMaterial(VoxelSurfaceBuild *b,int x,int y,CastleMaterial *m,
    const DioramaPixelEdit **edit) {
  uint8_t metatile;
  if(!LookupMaterial(b->scene,b->stamps,b->native,x/16,y/16,m,&metatile,b->tiles,b->pitch))return false;
  *edit=DioramaLayerOrder_PixelEditAt(b->terrain,0,x/16,y/16,metatile);
  return true;
}

static bool SurfaceAtRoomEdge(const ActionVoxelGeometry *c,const VoxelEdge *edge) {
  const int x=edge->x-c->origin_x,y=edge->y-c->origin_y;
  return edge->side==0 ? y==0 : edge->side==1 ? x==c->width-1 :
         edge->side==2 ? y==c->height-1 : x==0;
}

/* Sample the matching depth rank beside the cap in its source quadrant.
 * Using an opaque colour avoids exposing the rear sheet through edge UVs. */
static uint16_t SurfaceRoomEdgeColor(const ActionVoxelGeometry *c,const VoxelEdge *edge,
    const CastleMaterial *material,const DioramaPixelEdit *edit,
    const uint16_t palette[128],int x,int y,int cx,int cy) {
  const int tx=(x-cx)&8,ty=(y-cy)&8;
  unsigned red=0,green=0,blue=0,count=0;
  for(int py=ty;py<ty+8;++py)for(int px=tx;px<tx+8;++px) {
    const size_t at=(size_t)(cy+py-c->origin_y)*c->width+cx+px-c->origin_x;
    if(DepthRank(c,at)!=edge->far_rank)continue;
    const unsigned ink=SceneryBackingColor(material,edit,palette,px,py);
    red+=ink&31;
    green+=(ink>>5)&31;
    blue+=(ink>>10)&31;
    ++count;
  }
  if(!count)return 0;
  return 0x8000|((red+count/2)/count)|(((green+count/2)/count)<<5)|
      (((blue+count/2)/count)<<10);
}

static void SurfaceCloseRoomEdges(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],const uint16_t *tiles,size_t pitch) {
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *edge=&c->edges[i];
    if(edge->near_rank || !SurfaceAtRoomEdge(c,edge))continue;
    const VoxelAuthoredCell *owner=AuthoredPixelOwner(c,edge->x,edge->y);
    if(owner && owner->native_transform)continue;
    const int x=edge->x+((edge->side&1)?0:edge->length/2);
    const int y=edge->y+((edge->side&1)?edge->length/2:0);
    /* Room origins are aligned to metatiles, including negative stamps. */
    const int cx=c->origin_x+(x-c->origin_x)/16*16;
    const int cy=c->origin_y+(y-c->origin_y)/16*16;
    CastleMaterial material;
    uint8_t id;
    if(!LookupMaterial(s,stamps,native,cx/16,cy/16,&material,&id,tiles,pitch))continue;
    const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(terrain,0,cx/16,cy/16,id);
    edge->room_edge_color=SurfaceRoomEdgeColor(c,edge,&material,edit,palette,x,y,cx,cy);
  }
}
#include "action_voxel_native_surface.inc"
/* An actual edge owns a finite fragment. Follow its original native pixels
 * inward, not a first-opaque silhouette guess: lower shelves inside the same
 * tile are independent source contours. Geometry/priority remain untouched. */
#include "action_voxel_sand_live.inc"

static bool SurfaceNativeEdgePatch(VoxelSurfaceBuild *b,const VoxelEdge *edge,
    VoxelSurfacePatch *patch) {
  if(!b->native_surfaces || !edge || edge->side>3 || edge->length<=0)return false;
  const int cx=(int)floorf(edge->x/16.f)*16,cy=(int)floorf(edge->y/16.f)*16;
  CastleMaterial owner;const DioramaPixelEdit *owner_edit;
  if(!SurfaceMaterial(b,cx,cy,&owner,&owner_edit) ||
      owner.native_surface_family==kVoxelNativeSurfaceDesertSandLive ||
      !(owner.native_surface_faces&(1u<<edge->side)))return false;
  const unsigned full_span=edge->fold_length?edge->fold_length:edge->length;
  if((edge->side&1) && owner.native_surface_side_limit &&
      full_span>owner.native_surface_side_limit)return false;
  const unsigned first=(unsigned)((edge->side&1)?edge->y-cy:edge->x-cx);
  const unsigned length=(unsigned)edge->length;
  if(first>=16 || length>16-first)return false;
  /* These are closed, reviewed native material families, not user sampler
   * parameters. Crystal keeps eight complete native rows and rejects a
   * masonry/matte tip; the other families retain a real terminal edge ink. */
  const bool crown=owner.native_surface_family==kVoxelNativeSurfaceHeadCrown;
  const bool lava_cap=owner.native_surface_family==kVoxelNativeSurfaceLavaStoneCap;
  if(lava_cap && (edge->side&1))return false;
  const bool hold=owner.native_surface_family!=kVoxelNativeSurfaceIceCrystal;
  const unsigned extent=hold?16:8;
  static const int inward_x[4]={0,-1,0,1},inward_y[4]={1,0,-1,0};
  VoxelNativeSurfaceColumn columns[16]={0};
  bool ice_terminal[16]={0},has_ice_terminal=false,crown_terminal[16]={0};
  bool lava_terminal[16]={0};
  for(unsigned along=0;along<length;++along) {
    VoxelNativeSurfaceColumn *column=&columns[first+along];
    unsigned reference_word=0,reference_kind=0;
    for(unsigned depth=0;depth<extent;++depth) {
      const int x=edge->x+((edge->side&1)?0:(int)along)+inward_x[edge->side]*(int)depth;
      const int y=edge->y+((edge->side&1)?(int)along:0)+inward_y[edge->side]*(int)depth;
      const int ox=(int)floorf(x/16.f)*16,oy=(int)floorf(y/16.f)*16;
      /* A crown owns only its native16px block. The next row is the
       * separately carved eye/fire, even when it shares palette4. */
      if(crown && (ox!=cx || oy!=cy)) {crown_terminal[first+along]=true;break;}
      /* These native floating stones contain bright lava below their brown
       * crown. Only the reviewed seven-row stone band may become a walking
       * face; retain its actual alpha contour and mirrored source orientation. */
      if(lava_cap) {
        const int row=(owner.native_surface_flip&2)?15-(y-cy):y-cy;
        if(ox!=cx || oy!=cy || row<0 || row>=7) {
          lava_terminal[first+along]=true;break;
        }
      }
      CastleMaterial m;const DioramaPixelEdit *edit;
      if(!SurfaceMaterial(b,ox,oy,&m,&edit))break;
      const bool ice_tip=!hold && m.native_ice_tip_source;
      if(m.native_surface_family!=owner.native_surface_family && !ice_tip)break;
      const unsigned px=(unsigned)(x-ox),py=(unsigned)(y-oy),q=(py/8)*2+px/8;
      const unsigned word=m.words[q],bit=1u<<(15-px);
      /* Edits are authoritative. Do not turn erased/painted source into an
       * invented opaque continuation or read a moved source at its old spot. */
      if((edit && ((edit->black[py]|edit->transparent[py])&bit)) ||
          (m.cutouts[py]&bit) || ActionVoxel_AuthoredSurfaceAt(b->geometry,x,y))return false;
      if(!SurfaceStaticWord(b->scene,word))return false;
      const unsigned ink=AuthoredPixel(b->scene,word,px&7,py&7),kind=m.kind[q];
      if(!depth) {reference_word=word;reference_kind=kind;}
      if((kind!=kVoxelSolid && kind!=kVoxelForeground && kind!=kVoxelThin) ||
          kind!=reference_kind || ((word^reference_word)&0x3c00))break;
      if(ice_tip && ink) {
        const VoxelIceTipSourceProfile *tip=&kVoxelIceTipSources[m.native_ice_tip_source-1];
        const unsigned tx=(m.native_ice_tip_flip&1)?15-px:px;
        const unsigned ty=(m.native_ice_tip_flip&2)?15-py:py;
        if(!(tip->ice_rows[ty]&(1u<<(15-tx)))) {
          ice_terminal[first+along]=has_ice_terminal=true;break;
        }
      }
      if(crown && !ink) {crown_terminal[first+along]=true;break;}
      if(!ink || (m.rear_rows[py]&bit) ||
          ((m.rear_colors[q]|m.face_colors[q]|m.gate_colors[q])&(1u<<ink)))break;
      column->colors[column->length++]=0x8000|b->palette[((word>>10)&7)*16+ink];
    }
  }
  if(lava_cap) {
    if(!NativeSurfacePackPrefix(columns,lava_terminal,first,length,edge->side,7,patch->pixels))return false;
  } else if(crown) {
    if(!NativeSurfacePackPrefix(columns,crown_terminal,first,length,edge->side,16,patch->pixels))return false;
  } else if(has_ice_terminal) {
    if(!NativeSurfacePackIceTip(columns,ice_terminal,first,length,edge->side,patch->pixels))return false;
  } else if(!NativeSurfacePack(columns,first,length,edge->side,extent,hold,patch->pixels))return false;
  patch->x=cx;patch->y=cy;patch->size=16;patch->derived=true;patch->native_x=false;return true;
}
/* Staged caller-only addition, inserted after SurfaceNativeEdgePatch.
 * A one/two-pixel ice stair remains an upper-surface skin. Its horizontal
 * inward side sample is not the adjoining walking surface's source profile.
 * Require an actual existing top edge, then reuse that exact source column.
 * This runs only during immutable geometry compilation, never per frame. */
static bool SurfaceIceTopRiserPatch(VoxelSurfaceBuild *b,const VoxelEdge *edge,
    VoxelSurfacePatch *patch) {
  if(!b->native_surfaces || !(edge->side&1) || !edge->soft_surface ||
      (edge->fold_length?edge->fold_length:edge->length)>2)return false;
  CastleMaterial owner;const DioramaPixelEdit *edit;
  const int cx=(int)floorf(edge->x/16.f)*16,cy=(int)floorf(edge->y/16.f)*16;
  if(!SurfaceMaterial(b,cx,cy,&owner,&edit) ||
      owner.native_surface_family!=kVoxelNativeSurfaceIceCrystal ||
      !(owner.native_surface_faces&1))return false;
  for(unsigned i=0;i<b->geometry->edge_count;++i) {
    const VoxelEdge *top=&b->geometry->edges[i];
    if(top->side || top->y!=edge->y || edge->x<top->x ||
        edge->x>=top->x+top->length || top->near_rank!=edge->near_rank ||
        top->far_rank!=edge->far_rank || top->high_priority!=edge->high_priority ||
        top->colliding!=edge->colliding || top->rear_mounted!=edge->rear_mounted ||
        top->soft_surface!=edge->soft_surface)continue;
    if(top->top_sample_size)return false; /* Explicit donor remains authoritative. */
    VoxelEdge anchor=*top;
    anchor.x=edge->x;anchor.length=1;
    anchor.fold_origin_y=0;anchor.fold_length=0;
    return SurfaceNativeEdgePatch(b,&anchor,patch);
  }
  return false;
}

/* The crop is a representable native8/16px square, not synthesized ink.
 * Require every pixel to survive the reviewed matte and authored pixel edits. */
static bool SurfaceOpaquePatchCore(VoxelSurfaceBuild *b,const CastleMaterial *m,
    const DioramaPixelEdit *edit,int ox,int oy,int size,bool rear_turf,VoxelSurfacePatch *patch) {
  for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
    const unsigned mx=(ox+x)&15,my=(oy+y)&15,q=(my/8)*2+mx/8;
    const unsigned word=m->words[q],bit=1u<<(15-mx);
    const bool reviewed_turf=rear_turf && (m->top_surface&kVoxelTopSurface_TurfDonor8) &&
        m->kind[q]==kVoxelRearOnly;
    if((!SurfaceStructural(m,q) && !reviewed_turf) || !SurfaceStaticWord(b->scene,word))return false;
    const bool black=edit && (edit->black[my]&bit);
    const unsigned p=AuthoredPixel(b->scene,word,mx&7,my&7);
    /* Mixed native cells may contain rear plants, a face-only foam/cloud
     * fringe, or a gate alongside solid rock. Opaque pixels do not make that
     * decoration a structural surface donor. Keep the same per-pixel role
     * boundary as geometry classification; explicit authored art still wins. */
    if((m->rear_rows[my]&bit) ||
        ((m->rear_colors[q]|m->face_colors[q]|m->gate_colors[q])&(1u<<p)))return false;
    /* Picker provenance is native words plus a crop. An authored black mask
     * cannot be encoded there, so never advertise it as a native donor. */
    if(black || !p || (m->cutouts[my]&bit) ||
        (edit && (edit->transparent[my]&bit)))return false;
    patch->pixels[y*16+x]=0x8000|b->palette[((word>>10)&7)*16+p];
  }
  for(int y=0;y<16;++y)for(int x=0;x<16;++x)
    patch->pixels[y*16+x]=patch->pixels[(y%size)*16+x%size];
  patch->x=ox;patch->y=oy;patch->size=size;patch->derived=false;patch->native_x=false;return true;
}
static bool SurfaceOpaquePatch(VoxelSurfaceBuild *b,const CastleMaterial *m,
    const DioramaPixelEdit *edit,int ox,int oy,int size,VoxelSurfacePatch *patch) {
  return SurfaceOpaquePatchCore(b,m,edit,ox,oy,size,false,patch);
}
static bool SurfaceSameBand(const CastleMaterial *m,unsigned source,unsigned candidate) {
  /* Stay in the same native material and8px height band for vertical faces.
   * Palette/priority/role must also match. Grass cannot borrow the wood below. */
  return SurfaceStructural(m,candidate) && m->kind[source]==m->kind[candidate] &&
      !((m->words[source]^m->words[candidate])&0x3c00) &&
      m->rear_colors[source]==m->rear_colors[candidate] &&
      m->face_colors[source]==m->face_colors[candidate] &&
      m->gate_colors[source]==m->gate_colors[candidate];
}
static void SurfaceBuildTopDonors(VoxelSurfaceBuild *b) {
  bool any=false;
  const unsigned donor_flags=kVoxelTopSurface_Donor16|kVoxelTopSurface_TurfDonor8|
      kVoxelTopSurface_FamilyDonor16;
  for(unsigned i=0;i<256;++i)any|=!!(b->native[i].top_surface&donor_flags);
  if(!any && b->stamps)for(unsigned i=0;i<b->stamps->count && !any;++i) {
    CastleMaterial m;const DioramaPixelEdit *edit;
    any=SurfaceMaterial(b,b->stamps->cells[i].x*16,b->stamps->cells[i].y*16,&m,&edit) &&
        (m.top_surface&donor_flags);
  }
  if(!any)return;
  const ActionVoxelGeometry *c=b->geometry;
  const int columns=c->width/16,rows=c->height/16;
  b->top_donors=calloc((size_t)columns*rows,1);
  b->top_donor_cache=calloc((size_t)columns*rows*4,sizeof(*b->top_donor_cache));
  if(!b->top_donors || !b->top_donor_cache) {
    free(b->top_donors);free(b->top_donor_cache);b->top_donors=NULL;b->top_donor_cache=NULL;return;
  }
  /* Compile the reviewed donor index once; no camera/frame search. Stamps,
   * pixel masks, animation and palette remain normal geometry dependencies. */
  for(int row=0;row<rows;++row)for(int col=0;col<columns;++col) {
    CastleMaterial m;const DioramaPixelEdit *edit;VoxelSurfacePatch patch;
    const int x=c->origin_x+col*16,y=c->origin_y+row*16;
    if(!SurfaceMaterial(b,x,y,&m,&edit))continue;
    if((m.top_surface&(kVoxelTopSurface_Donor16|kVoxelTopSurface_FamilyDonor16)) &&
        SurfaceOpaquePatch(b,&m,edit,x,y,16,&patch))
      b->top_donors[(size_t)row*columns+col]=(m.top_surface&kVoxelTopSurface_FamilyDonor16)?1+m.top_family:1;
    else if((m.top_surface&kVoxelTopSurface_TurfDonor8) &&
        SurfaceOpaquePatchCore(b,&m,edit,x,y+8,8,true,&patch) &&
        SurfaceOpaquePatchCore(b,&m,edit,x+8,y+8,8,true,&patch))
      b->top_donors[(size_t)row*columns+col]=1+m.top_family;
  }
}
static bool SurfaceNearbyTop(VoxelSurfaceBuild *b,const CastleMaterial *source,
    unsigned q,int x,int y,VoxelSurfacePatch *patch) {
  if(!b->top_donors || !b->top_donor_cache)return false;
  const ActionVoxelGeometry *c=b->geometry;const int columns=c->width/16,rows=c->height/16;
  const int cx=(x-c->origin_x)/16,cy=(y-c->origin_y)/16;
  if(cx<0 || cy<0 || cx>=columns || cy>=rows)return false;
  int32_t *cached=&b->top_donor_cache[((size_t)cy*columns+cx)*4+q];
  if(*cached<0)return false;
  const bool turf=!!(source->top_surface&kVoxelTopSurface_TurfBand8);
  const bool family=!!(source->top_surface&kVoxelTopSurface_FamilyCap16);
  const unsigned donor_kind=(turf||family)?1+source->top_family:1;
  /* Masonry stays within eight cells. Isolated forest stairs may be eighteen
   * cells from an identical reviewed horizontal turf band; their separate
   * twenty-four-cell bound never admits another family or a colour match. */
  for(int radius=0;radius<=(family?40:turf?24:8);++radius)for(int dy=-radius;dy<=radius;++dy) {
    const int dx=radius-abs(dy);
    for(unsigned half=0;half<(dx?2u:1u);++half) {
      int tx=cx+(half?dx:-dx),ty=cy+dy;
      if(*cached){tx=(*cached-1)%columns;ty=(*cached-1)/columns;}
      if(tx<0 || ty<0 || tx>=columns || ty>=rows)continue;
      const size_t at=(size_t)ty*columns+tx;
      if(b->top_donors[at]!=donor_kind)continue;
      CastleMaterial donor;const DioramaPixelEdit *edit;
      const int ox=c->origin_x+tx*16,oy=c->origin_y+ty*16;
      bool compatible=SurfaceMaterial(b,ox,oy,&donor,&edit);
      if(turf)compatible=compatible && !((source->words[q]^donor.words[2+(q&1)])&0x3c00);
      else for(unsigned quadrant=0;compatible && quadrant<4;++quadrant)
        compatible=!((source->words[q]^donor.words[quadrant])&0x3c00);
      const bool opaque=compatible && (turf?
          SurfaceOpaquePatchCore(b,&donor,edit,ox+(q&1)*8,oy+8,8,true,patch):
          SurfaceOpaquePatch(b,&donor,edit,ox,oy,16,patch));
      if(opaque) {
        *cached=(int32_t)at+1;return true;
      }
      if(*cached){*cached=-1;return false;}
    }
  }
  *cached=-1;return false;
}
/* Diagonal cliff artwork depicts the vertical rock wall below a thin native
 * walking rim. Projecting those vertical cracks inward shears them into long
 * stripes. Use the already reviewed horizontal rock family for both exposed
 * cap orientations, including the small undersides visible from below.
 * The actual solid contact/body is the receiver; unrelated rear rock remains
 * untouched. */
static bool SurfaceMountainCapPatch(VoxelSurfaceBuild *b,const VoxelEdge *edge,
    VoxelSurfacePatch *patch) {
  const ActionVoxelGeometry *g=b->geometry;
  const bool ramp_return=edge->column_curve && edge->column_curve<=g->column_curve_count &&
      g->column_curves[edge->column_curve-1].contact_count==8;
  if(g->profile!=kActionVoxelProfile_AitosMountains || edge->far_rank!=2 ||
      ((edge->side&1)&&(edge->fold_length?edge->fold_length:edge->length)>2 && !ramp_return))return false;
  const int cx=(int)floorf(edge->x/16.f)*16,cy=(int)floorf(edge->y/16.f)*16;
  CastleMaterial source;const DioramaPixelEdit *edit;
  if(!SurfaceMaterial(b,cx,cy,&source,&edit) ||
      (!source.native_slope&&!source.native_slope_foot&&!source.native_slope_support) ||
      !(source.top_surface&kVoxelTopSurface_FamilyCap16))return false;
  for(int along=0;along<edge->length;++along) {
    const int x=edge->x+((edge->side&1)?0:along),y=edge->y+((edge->side&1)?along:0);
    if(x<g->origin_x || y<g->origin_y || x>=g->origin_x+g->width || y>=g->origin_y+g->height ||
        (g->front[(y-g->origin_y)*g->width+x-g->origin_x]&kVoxelKindMask)!=kVoxelSolid ||
        ActionVoxel_AuthoredSurfaceAt(g,x,y))return false;
  }
  const unsigned q=((edge->y-cy)/8)*2+(edge->x-cx)/8;
  if(!SurfaceStaticWord(b->scene,source.words[q]))return false;
  if(SurfaceNearbyTop(b,&source,q,cx,cy,patch))return true;
  /* Some reviewed supports carry a native snow-to-rock palette transition.
   * With no matching horizontal donor, keep the actual opaque8px rock
   * quadrant. Only the already validated solid receiver gains this sampling
   * permission; unrelated occurrences of the rear artwork stay untouched. */
  if((source.native_slope_foot || source.native_slope_support) && source.kind[q]==kVoxelRearOnly) {
    source.kind[q]=kVoxelSolid;
    return SurfaceOpaquePatch(b,&source,edit,cx+(q&1)*8,cy+(q>>1)*8,8,patch);
  }
  return false;
}
/* Reviewed platform bands traverse their own native upper rows once. Castle
 * stone owns five rows; marsh beams own four before the rail's dark seam.
 * Preserve the full16px source phase at end/middle joints without copying
 * lower brackets, posts or repeating the rim across the walking surface. */
static bool SurfaceOwnTopBandPatch(VoxelSurfaceBuild *b,const CastleMaterial *m,
    const DioramaPixelEdit *edit,int ox,int oy,unsigned rows,VoxelSurfacePatch *patch) {
  if(rows!=4 && rows!=5)return false;
  uint16_t band[5][16];
  unsigned lengths[16]={0};
  if(edit)for(unsigned y=0;y<rows;++y)
    if(edit->transparent[y] || edit->black[y])return false;
  for(unsigned x=0;x<16;++x)for(unsigned y=0;y<rows;++y) {
    const unsigned q=x/8,word=m->words[q],bit=1u<<(15-x);
    if(!SurfaceStructural(m,q) || !SurfaceStaticWord(b->scene,word))return false;
    const unsigned ink=AuthoredPixel(b->scene,word,x&7,y);
    /* Mixed beam/post artwork has native narrow gaps. Keep a missing source
     * column transparent; a visible column owns only its real upper prefix.
     * Never widen a post or borrow a neighbour's grain to hide its cutout. */
    if(!ink || (m->cutouts[y]&bit)) {if(rows==4)break;return false;}
    if((m->rear_rows[y]&bit) ||
        ((m->rear_colors[q]|m->face_colors[q]|m->gate_colors[q])&(1u<<ink)))return false;
    band[y][x]=0x8000|b->palette[((word>>10)&7)*16+ink];
    ++lengths[x];
  }
  /* Endpoint-centred nearest resampling preserves each native ink exactly;
   * no blended/new colour, wrapping, donor search or per-frame work. */
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    patch->pixels[y*16+x]=lengths[x]?band[(y*(lengths[x]-1)+7)/15][x]:0;
  patch->x=ox;patch->y=oy;patch->size=16;patch->derived=true;patch->native_x=rows==4;return true;
}

#include "action_voxel_snow_crest.inc"
static bool SurfaceSnowCrestPatch(VoxelSurfaceBuild *b,const CastleMaterial *source,
    int cx,int cy,VoxelSurfacePatch *patch) {
  if(source->contour_surface!=kVoxelContourSkinSnowCrest)return false;
  SummitCrestColumn columns[16]={0};uint16_t used=0;
  for(int x=0;x<16;++x) {
    int top=0;
    while(top<16 && !source->pixels[(top/8)*2+x/8][(top&7)*8+(x&7)])++top;
    if(top==16)continue;
    const unsigned reference=source->words[(top/8)*2+x/8];
    for(int row=0;row<16;++row) {
      const int wy=cy+top+row,oy=(int)floorf(wy/16.f)*16;
      CastleMaterial m;const DioramaPixelEdit *edit;
      if(!SurfaceMaterial(b,cx,oy,&m,&edit) ||
          (m.contour_surface!=kVoxelContourSkinSnowCrest &&
           m.contour_surface!=kVoxelContourSkinSnowBody))return false;
      /* Native alpha can terminate a thin tip; authored alpha must fall back
       * instead of being converted into an opaque automatic source profile. */
      if(edit)for(unsigned y=0;y<16;++y)
        if(edit->black[y] || edit->transparent[y])return false;
      if(ActionVoxel_AuthoredSurfaceAt(b->geometry,cx+x,wy))return false;
      const unsigned my=(unsigned)(wy-oy),q=(my/8)*2+x/8,word=m.words[q];
      if(!SurfaceStructural(&m,q) || !SurfaceStaticWord(b->scene,word) ||
          ((word^reference)&0x3c00) || (m.cutouts[my]&(1u<<(15-x))))return false;
      const unsigned pixel=AuthoredPixel(b->scene,word,x&7,my&7);
      if(!pixel)break;
      if((m.rear_rows[my]&(1u<<(15-x))) ||
          ((m.rear_colors[q]|m.face_colors[q]|m.gate_colors[q])&(1u<<pixel)))return false;
      columns[x].colors[columns[x].length++]=0x8000|b->palette[((word>>10)&7)*16+pixel];
    }
    if(!columns[x].length)return false;
    used|=(uint16_t)(1u<<x);
  }
  if(!SummitCrest_Build(columns,used,patch->pixels))return false;
  patch->x=cx;patch->y=cy;patch->size=16;patch->derived=true;patch->native_x=false;return true;
}

static bool SurfaceSelectPatch(VoxelSurfaceBuild *b,int x,int y,bool top,
    VoxelSurfacePatch *patch) {
  VoxelSurfaceStats *stats=&b->geometry->surface_stats;
  const int cx=(int)floorf(x/16.f)*16,cy=(int)floorf(y/16.f)*16;
  CastleMaterial source;const DioramaPixelEdit *edit;
  if(!SurfaceMaterial(b,cx,cy,&source,&edit)) {++stats->semantic_fallbacks;return false;}
  const unsigned q=((y-cy)/8)*2+(x-cx)/8;
  if(!SurfaceStructural(&source,q)) {++stats->semantic_fallbacks;return false;}
  if(!SurfaceStaticWord(b->scene,source.words[q])) {++stats->unsafe_source_fallbacks;return false;}
  if(top && (source.top_surface&(kVoxelTopSurface_OwnTopBand4|kVoxelTopSurface_OwnTopBand5))) {
    const bool beam=!!(source.top_surface&kVoxelTopSurface_OwnTopBand4);
    /* Grain/rail contours inside a beam are not additional walking caps. */
    if((!beam || y==cy) && SurfaceOwnTopBandPatch(b,&source,edit,cx,cy,beam?4:5,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && source.contour_surface==kVoxelContourSkinSnowCrest) {
    if(SurfaceSnowCrestPatch(b,&source,cx,cy,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && source.contour_surface && source.contour_surface<=kVoxelContourSkinBottom) {
    if(SurfaceNearbyTop(b,&source,q,cx,cy,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && (source.top_surface&kVoxelTopSurface_SoilBand8)) {
    if(SurfaceOpaquePatch(b,&source,edit,cx+(q&1)*8,cy,8,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && (source.top_surface&kVoxelTopSurface_TurfBand8)) {
    /* Native-word crop provenance cannot freeze authored source masks. Keep
     * those edited fronts on their existing fallback until explicitly painted. */
    if(edit)for(unsigned row=0;row<16;++row)
      if(edit->black[row] || edit->transparent[row]){++stats->opaque_fallbacks;return false;}
    if(SurfaceNearbyTop(b,&source,q,cx,cy,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && (source.top_surface&(kVoxelTopSurface_Own16|kVoxelTopSurface_FamilyDonor16))) {
    if(SurfaceOpaquePatch(b,&source,edit,cx,cy,16,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  if(top && (source.top_surface&(kVoxelTopSurface_MasonryCap16|kVoxelTopSurface_FamilyCap16))) {
    if(SurfaceNearbyTop(b,&source,q,cx,cy,patch))return true;
    ++stats->opaque_fallbacks;return false;
  }
  /* Reviewed complete-stone and masonry-cap contracts were handled above.
   * Other stone rims retain their upper-row contour instead of blindly
   * spreading a front bevel/shadow band over the floor. Reviewed turf/rock
   * tops and internal bark ledges can use a safe band-local patch; explicit
   * cut-grain samples have already bypassed this automatic pass. */
  if(top && !(source.flags&(kVoxelTexturedTopMaterial|kVoxelSideProfileMaterial))) {
    ++stats->semantic_fallbacks;return false;
  }
  /* Search the source8px height band first. Even a top must not borrow wood
   * below its grass fringe merely because the lower square is more opaque. */
  const unsigned alternate=q^1,candidates[]={q,alternate};
  /* Native left/right top quadrants preserve a16px-wide grass motif. Do not
   * repeat the same middle8px strip on both halves of its walking surface. */
  if(top && SurfaceOpaquePatch(b,&source,edit,cx+(q&1)*8,cy+(q>>1)*8,8,patch))return true;
  /* Prefer the interior of a compatible native band over its outlined edge.
   * This is still an exact native crop, and can never mix grass with wood
   * in the next8px row or cross a palette/priority/material boundary. */
  if(SurfaceSameBand(&source,q,alternate) &&
      SurfaceOpaquePatch(b,&source,edit,cx+4,cy+(q>>1)*8,8,patch))return true;
  bool found=false;unsigned detail=0;
  for(unsigned i=0;i<2;++i) {
    const unsigned donor=candidates[i];
    VoxelSurfacePatch candidate;
    if(!SurfaceSameBand(&source,q,donor) || !SurfaceOpaquePatch(b,&source,edit,
        cx+(donor&1)*8,cy+(donor>>1)*8,8,&candidate))continue;
    unsigned candidate_detail=0;
    for(unsigned p=1;p<64;++p)candidate_detail+=source.pixels[donor][p]!=source.pixels[donor][0];
    /* Only leave an opaque source crop when it is an entirely flat shadow
     * strip and its own material band contains native texture detail. This
     * uses palette indices, so a lighting/palette fade cannot change donors. */
    if(!found || (!detail && candidate_detail)){*patch=candidate;detail=candidate_detail;found=true;}
    if(detail)return true;
  }
  if(found)return true;
  /* Rounded outer edges may leave neither aligned quadrant opaque even
   * though their shared interior has a clean8px square. Try only bounded
   * crops inside this same16px source row, never cross the material band. */
  if(SurfaceSameBand(&source,q,alternate)) {
    static const unsigned crop_x[]={4,3,5,2,6,1,7};
    for(unsigned i=0;i<sizeof(crop_x)/sizeof(*crop_x);++i)
      if(SurfaceOpaquePatch(b,&source,edit,cx+crop_x[i],cy+(q>>1)*8,8,patch))return true;
  }
  /* A bounded neighbouring repeat may repair an authored transparency hole.
   * Exact displayed native fingerprint includes palette/priority/collision;
   * broad roles such as Platform, or a similar colour, are never enough. */
  const uint64_t key=MaterialKey(&source,0);
  static const int offsets[4][2]={{-16,0},{16,0},{0,-16},{0,16}};
  for(unsigned i=0;i<4;++i) {
    CastleMaterial donor;const DioramaPixelEdit *donor_edit;
    const int dx=cx+offsets[i][0],dy=cy+offsets[i][1];
    if(!SurfaceMaterial(b,dx,dy,&donor,&donor_edit) || MaterialKey(&donor,0)!=key ||
        donor.flags!=source.flags || memcmp(donor.kind,source.kind,sizeof(donor.kind)))continue;
    if(SurfaceOpaquePatch(b,&donor,donor_edit,dx+(q&1)*8,dy+(q>>1)*8,8,patch))return true;
  }
  ++stats->opaque_fallbacks;return false;
}
/* A uniform native backing bottom already samples the same ink everywhere.
 * Turning a room-wide matte into hundreds of identical texture pieces adds
 * no detail and can starve real cliff/beam faces. Qualify native indices, not
 * displayed RGB, so palette fades cannot change the geometry partition. */
static bool SurfaceFlatNativeBottom(VoxelSurfaceBuild *b,const VoxelEdge *edge) {
  if(b->geometry->profile!=kActionVoxelProfile_BloodpoolMarsh ||
      edge->side!=2 || (edge->y&15)<3)return false;
  const int cx=(int)floorf(edge->x/16.f)*16,cy=(int)floorf(edge->y/16.f)*16;
  CastleMaterial m;const DioramaPixelEdit *edit;
  if(!SurfaceMaterial(b,cx,cy,&m,&edit))return false;
  const unsigned ink=m.pixels[0][0];if(!ink)return false;
  for(unsigned q=0;q<4;++q) {
    if(!SurfaceStructural(&m,q) || !SurfaceStaticWord(b->scene,m.words[q]) ||
        ((m.words[q]^m.words[0])&0x3c00) || m.rear_colors[q] || m.face_colors[q] || m.gate_colors[q])return false;
    for(unsigned p=0;p<64;++p)if(m.pixels[q][p]!=ink)return false;
  }
  for(unsigned row=0;row<16;++row)
    if(m.cutouts[row] || m.rear_rows[row] || (edit && (edit->black[row] || edit->transparent[row])))return false;
  return true;
}
static bool SurfaceIntern(VoxelSurfaceBuild *b,const VoxelSurfacePatch *patch,bool pin,unsigned *id) {
  ActionVoxelGeometry *c=b->geometry;
  uint64_t hash=UINT64_C(14695981039346656037);
  for(unsigned i=0;i<256;++i)hash=(hash^patch->pixels[i])*UINT64_C(1099511628211);
  unsigned slot=(unsigned)hash&(kAuthoredHashSlots-1);
  while(b->slots[slot]) {
    const unsigned candidate=b->slots[slot]-1;
    if(!memcmp(c->authored_art[candidate],patch->pixels,sizeof(patch->pixels))) {
      *id=candidate;
      if(!pin || b->used[*id])return true;
      break;
    }
    slot=(slot+1)&(kAuthoredHashSlots-1);
  }
  if((pin && c->authored_cap_art_count>=kVoxelAutomaticSurfaceArtMax) ||
      (!b->slots[slot] && (c->authored_art_count==kVoxelAuthoredArtMax ||
       c->authored_art_count-b->initial_art>=kVoxelAutomaticSurfaceArtMax))) {
    ++c->surface_stats.art_budget_fallbacks;return false;
  }
  const unsigned error=c->build_error;
  if(!InternAuthoredArt(c,b->slots,patch->pixels,id) || (pin && !KeepOwnedArt(c,b->used,*id))) {
    c->build_error=error;++c->surface_stats.art_budget_fallbacks;return false;
  }
  return true;
}
static void SurfacePublishPointers(ActionVoxelGeometry *c) {
  for(unsigned i=0;i<c->authored_count;++i)for(unsigned face=0;face<7;++face)
    c->authored[i].pixels[face]=c->authored_art[c->authored[i].art_id[face]];
}
/* A reviewed profile may contain face-only foam around its structural rock.
 * Its side keeps the original native rock pixels and silhouette, never the
 * foam that belongs on the front. Compile masked cells once into the same
 * immutable atlas as caps; camera motion only projects these cached cells. */
static void SurfaceProfileArt(VoxelSurfaceBuild *b,VoxelPatch *profile) {
  ActionVoxelGeometry *c=b->geometry;
  if(profile->kind!=kVoxelPatch_SideProfile)return;
  const int x0=(int)floorf(profile->x/16.f)*16,y0=(int)floorf(profile->y/16.f)*16;
  const int x1=(int)ceilf((profile->x+profile->width)/16.f)*16;
  const int y1=(int)ceilf((profile->y+profile->height)/16.f)*16;
  const unsigned cells=(unsigned)((x1-x0)/16)*((y1-y0)/16);
  if(!cells || cells>kVoxelAutomaticSurfaceArtMax-c->profile_surface_count)return;
  bool mixed=false;
  for(int y=y0;y<y1;y+=16)for(int x=x0;x<x1;x+=16) {
    CastleMaterial m;const DioramaPixelEdit *edit;
    if(!SurfaceMaterial(b,x,y,&m,&edit) ||
        !(m.top_surface&kVoxelTopSurface_FamilyCap16) || !(m.flags&kVoxelSideProfileMaterial))return;
    for(unsigned q=0;q<4;++q) {
      if(!SurfaceStaticWord(b->scene,m.words[q]))return;
      mixed|=(m.rear_colors[q]|m.face_colors[q]|m.gate_colors[q])!=0;
    }
  }
  if(!mixed)return;
  const unsigned first=c->profile_surface_count,needed=first+cells;
  if(needed>c->profile_surface_capacity) {
    VoxelProfileSurface *next=realloc(c->profile_surfaces,needed*sizeof(*next));
    if(!next)return;
    c->profile_surfaces=next;c->profile_surface_capacity=needed;
  }
  unsigned count=0;
  for(int y=y0;y<y1;y+=16)for(int x=x0;x<x1;x+=16) {
    /* Authored geometry owns only its source cell. Omitting that cell keeps
     * its side/picking with the editor while neighboring native sides retain
     * their clean masks; one edit must not restore foam on the whole object. */
    const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(c,x/16,y/16);
    if(authored && authored->surface)continue;
    CastleMaterial m;const DioramaPixelEdit *edit;VoxelSurfacePatch patch={.x=x,.y=y,.size=16};
    if(!SurfaceMaterial(b,x,y,&m,&edit))return;
    for(int dy=0;dy<16;++dy)for(int dx=0;dx<16;++dx) {
      const int wx=x+dx,wy=y+dy;
      if(wx<profile->x || wx>=profile->x+profile->width || wy<profile->y || wy>=profile->y+profile->height ||
          wx<c->origin_x || wx>=c->origin_x+c->width || wy<c->origin_y || wy>=c->origin_y+c->height)continue;
      const unsigned native=c->front[(size_t)(wy-c->origin_y)*c->width+wx-c->origin_x];
      if((native&kVoxelKindMask)!=kVoxelSolid || !!(native&kVoxelHighPriority)!=profile->high_priority)continue;
      const unsigned q=(dy/8)*2+dx/8,word=m.words[q],p=AuthoredPixel(b->scene,word,dx&7,dy&7);
      const bool black=edit && (edit->black[dy]&(1u<<(15-dx)));
      patch.pixels[dy*16+dx]=0x8000|(black?0:b->palette[((word>>10)&7)*16+p]);
    }
    unsigned id;if(!SurfaceIntern(b,&patch,true,&id))return;
    c->profile_surfaces[first+count++]=(VoxelProfileSurface){x,y,id};
  }
  profile->surface_first=first;profile->surface_count=count;profile->surface_compiled=1;
  c->profile_surface_count=first+count;
}
/* A native snow stair's short exterior closer shares the actual adjacent
 * walking cap. Resolve after automatic selection so neither a second crop
 * search nor a foreign donor can change that cap's source/phase. This is an
 * artwork-only compilation pass; the complete original span still excludes
 * short fragments of tall walls. Edited cells keep their existing treatment. */
static bool SurfaceSnowPhaseNativeCell(VoxelSurfaceBuild *b,int x,int y) {
  const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(b->geometry,cx,cy);
  if(a && (a->surface || a->native_transform))return false;
  CastleMaterial material;const DioramaPixelEdit *edit;
  if(!SurfaceMaterial(b,cx*16,cy*16,&material,&edit))return false;
  if(edit)for(unsigned y=0;y<16;++y)
    if(edit->black[y] || edit->transparent[y])return false;
  return true;
}
/* Effect support is independent of cap texture selection. A raw native
 * contour and an owned crop can support the same physical Snow bank. */
static bool SurfaceSnowClosingSource(VoxelSurfaceBuild *b,int x,int y,
    bool slope,unsigned *word) {
  if(!SurfaceSnowPhaseNativeCell(b,x,y))return false;
  const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
  CastleMaterial m;const DioramaPixelEdit *edit;
  if(!SurfaceMaterial(b,cx*16,cy*16,&m,&edit))return false;
  if(slope) {
    if(!(m.native_slope&&(m.collision==6||m.collision==9))&&!m.native_slope_foot)return false;
  } else {
    bool reviewed=false;
    for(unsigned flip=0;flip<4&&!reviewed;++flip)
      reviewed=FindMaterial(b->scene->group,b->scene->map,MaterialKey(&m,flip))!=NULL;
    if(!reviewed)return false;
  }
  const unsigned q=(unsigned)(y-cy*16)/8*2+(unsigned)(x-cx*16)/8;
  if(!SurfaceStaticWord(b->scene,m.words[q]))return false;
  *word=m.words[q];return true;
}
static void SurfaceSnowMarkClosingRiser(VoxelSurfaceBuild *b,VoxelEdge *t,
    const VoxelEdge *r,unsigned native_word,bool require_material) {
  ActionVoxelGeometry *c=b->geometry;
  if(r->rear_fold_top||r->rear_fold_bottom||r->trunk_curve||
      r->column_curve||r->neighbor_column_curve||t->rear_fold_top||
      t->rear_fold_bottom||t->trunk_curve||t->column_curve||t->neighbor_column_curve)return;
  for(unsigned k=0;k<c->edge_count;++k) {
    const VoxelEdge *lower=c->edges+k;
    if(lower->side||lower->y!=r->y+r->length||
        (r->side==1?lower->x!=r->x+1:lower->x+lower->length!=r->x)||
        lower->near_rank!=t->near_rank||lower->far_rank!=t->far_rank||
        lower->high_priority!=t->high_priority||lower->soft_surface!=t->soft_surface||
        lower->rear_mounted!=t->rear_mounted||lower->rear_fold_top||lower->rear_fold_bottom||
        lower->trunk_curve||lower->column_curve||lower->neighbor_column_curve||
        !SurfaceSnowPhaseNativeCell(b,lower->x,lower->y))continue;
    if(require_material) {
      unsigned lower_word;
      const int lx=r->side==1?lower->x:lower->x+lower->length-1;
      if(!SurfaceSnowClosingSource(b,lx,lower->y,false,&lower_word)||
          ((native_word^lower_word)&0x3c00))continue;
    }
    VoxelEdge a=*t,z=*lower,side=*r;VoxelEdgeSpan as,zs,ss;
    a.high_priority=z.high_priority=side.high_priority=0;
    a.colliding=z.colliding=side.colliding=0;
    if(!ActionVoxel_ResolveEdgeSpan(c,&a,kVoxelEdgeNative,-INFINITY,INFINITY,&as)||
        !ActionVoxel_ResolveEdgeSpan(c,&z,kVoxelEdgeNative,-INFINITY,INFINITY,&zs)||
        !ActionVoxel_ResolveEdgeSpan(c,&side,kVoxelEdgeNative,-INFINITY,INFINITY,&ss)||
        fabsf(as.near_depth-zs.near_depth)>.000001f||fabsf(as.far_depth-zs.far_depth)>.000001f||
        fabsf(as.near_depth-ss.near_depth)>.000001f||fabsf(as.far_depth-ss.far_depth)>.000001f)continue;
    t->snow_closing_riser[r->side==1]=(unsigned char)r->length;break;
  }
}
static void SurfaceSnowCapRiserPhase(VoxelSurfaceBuild *b) {
  ActionVoxelGeometry *c=b->geometry;
  if(c->profile!=kActionVoxelProfile_NorthwallSnow || c->active_room!=1)return;
  for(unsigned i=0;i<c->edge_count;++i) {
    VoxelEdge *r=c->edges+i;
    if(!(r->side&1) || !r->soft_surface || r->far_rank!=2 || r->high_priority ||
        (r->fold_length?r->fold_length:r->length)>2 ||
        r->surface_texture!=kVoxelSurfaceTexturePatch || r->top_sample_size!=8 ||
        !r->top_owned_art || r->surface_derived || r->surface_top_material ||
        r->contour_skin || !SurfaceSnowPhaseNativeCell(b,r->x,r->y))continue;
    for(unsigned j=0;j<c->edge_count;++j) {
      VoxelEdge *t=c->edges+j;
      if(t->side || t->y!=r->y ||
          (r->side==1?t->x+t->length!=r->x+1:t->x!=r->x) ||
          t->near_rank!=r->near_rank || t->far_rank!=r->far_rank ||
          t->high_priority!=r->high_priority || t->colliding!=r->colliding ||
          t->rear_mounted!=r->rear_mounted || t->soft_surface!=r->soft_surface ||
          t->surface_texture!=kVoxelSurfaceTexturePatch || t->top_sample_size!=8 ||
          !t->top_owned_art || t->surface_derived || t->contour_skin ||
          !SurfaceSnowPhaseNativeCell(b,t->x,t->y))continue;
      r->top_sample_x=t->top_sample_x;r->top_sample_y=t->top_sample_y;
      r->top_owned_art=t->top_owned_art;r->surface_top_material=1;
      SurfaceSnowMarkClosingRiser(b,t,r,0,false);
      break;
    }
  }
}
/* The old artwork-phase pass intentionally excludes raw source sampling.
 * Do not import that unrelated restriction into receiver-owned snow cover. */
static void SurfaceSnowNativeClosingRisers(VoxelSurfaceBuild *b) {
  ActionVoxelGeometry *c=b->geometry;
  if(c->profile!=kActionVoxelProfile_NorthwallSnow||c->active_room!=1)return;
  for(unsigned i=0;i<c->edge_count;++i) {
    const VoxelEdge *r=c->edges+i;unsigned rw;
    if(!(r->side&1)||!r->soft_surface||r->far_rank!=2||r->high_priority||
        !r->length||(r->fold_length?r->fold_length:r->length)>2||
        !SurfaceSnowClosingSource(b,r->x,r->y,true,&rw))continue;
    for(unsigned j=0;j<c->edge_count;++j) {
      VoxelEdge *t=c->edges+j;unsigned tw;
      if(t->side||t->y!=r->y||
          (r->side==1?t->x+t->length!=r->x+1:t->x!=r->x)||
          t->near_rank!=r->near_rank||t->far_rank!=r->far_rank||
          t->high_priority!=r->high_priority||t->rear_mounted!=r->rear_mounted||
          t->soft_surface!=r->soft_surface||
          !SurfaceSnowClosingSource(b,r->side==1?t->x+t->length-1:t->x,t->y,true,&tw)||
          ((rw^tw)&0x3c00))continue;
      SurfaceSnowMarkClosingRiser(b,t,r,rw,true);break;
    }
  }
}
static bool BuildAutomaticSurfaceArt(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],const uint16_t *tiles,size_t pitch) {
  VoxelSurfaceBuild *b=calloc(1,sizeof(*b));VoxelEdge *edges=malloc(sizeof(c->edges));
  if(!b || !edges){free(b);free(edges);return true;}
  *b=(VoxelSurfaceBuild){.geometry=c,.scene=s,.terrain=terrain,.stamps=stamps,
      .native=native,.palette=palette,.tiles=tiles,.pitch=pitch,.initial_art=c->authored_art_count};
  for(unsigned i=0;i<256;++i)b->native_surfaces|=native[i].native_surface_family!=0;
  if(!b->native_surfaces && stamps)for(unsigned i=0;i<stamps->count;++i) {
    CastleMaterial m;const DioramaPixelEdit *edit;
    b->native_surfaces|=SurfaceMaterial(b,stamps->cells[i].x*16,stamps->cells[i].y*16,&m,&edit) &&
        m.native_surface_family!=0;
  }
  b->slots=calloc(kAuthoredHashSlots,sizeof(*b->slots));
  if(!b->slots){free(b);free(edges);return true;}
  for(unsigned id=0;id<c->authored_art_count;++id) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(unsigned p=0;p<256;++p)hash=(hash^c->authored_art[id][p])*UINT64_C(1099511628211);
    unsigned slot=(unsigned)hash&(kAuthoredHashSlots-1);
    while(b->slots[slot])slot=(slot+1)&(kAuthoredHashSlots-1);
    b->slots[slot]=id+1;
  }
  for(unsigned i=0;i<c->authored_cap_art_count;++i)b->used[c->authored_cap_art[i]]=true;
  SurfaceBuildTopDonors(b);
  c->surface_stats.edges_before=c->edge_count;
  unsigned count=0,candidate_capacity=0;
  VoxelSurfaceCandidate *candidates=NULL;
  for(unsigned i=0;i<c->edge_count;++i) {
    const VoxelEdge original=c->edges[i];
    /* A reviewed explicit sample (notably stump cut-grain) stays untouched.
     * textured_top only requested the former6px contour inset: replacing
     * that raw sampling with a verified full square preserves its fold. */
    if((original.far_rank!=2 && !(b->native_surfaces && original.far_rank==1)) ||
        original.top_sample_size || original.room_edge_color) {edges[count++]=original;continue;}
    const int origin=(original.side&1)?original.y:original.x;
    const unsigned bands=((origin&7)+original.length+7)/8;
    if(bands>candidate_capacity) {
      VoxelSurfaceCandidate *grown=realloc(candidates,bands*sizeof(*candidates));
      if(!grown){edges[count++]=original;continue;}
      candidates=grown;candidate_capacity=bands;
    }
    unsigned selected=0;bool textured=false;
    bool top_material=false;
    if((original.side&1) && original.soft_surface &&
        (original.fold_length?original.fold_length:original.length)<=2) {
      CastleMaterial material;const DioramaPixelEdit *edit;
      const int cx=(int)floorf(original.x/16.f)*16,cy=(int)floorf(original.y/16.f)*16;
      top_material=SurfaceMaterial(b,cx,cy,&material,&edit) &&
          (material.top_surface&kVoxelTopSurface_FamilyCap16);
    }
    for(int start=0;start<original.length;) {
      VoxelEdge edge=original;edge.x+=(edge.side&1)?0:start;edge.y+=(edge.side&1)?start:0;
      int length=8-((origin+start)&7);
      if(length>original.length-start)length=original.length-start;
      edge.length=length;
      if((edge.side&1) && length!=original.length && !edge.fold_length) {
        edge.fold_origin_y=original.y;edge.fold_length=original.length;
      }
      unsigned contour_skin=0;
      if(c->profile==kActionVoxelProfile_KassandoraTomb ||
          c->profile==kActionVoxelProfile_NorthwallSnow || c->profile==kActionVoxelProfile_DeathHeim) {
        CastleMaterial material;const DioramaPixelEdit *edit;
        const int cx=(int)floorf(edge.x/16.f)*16,cy=(int)floorf(edge.y/16.f)*16;
        /* Native boundaries can merge across material cells. Each fragment
         * owns its artwork independently, while a tall wall's short remainder
         * must still use the original physical span for riser eligibility. */
        if(SurfaceMaterial(b,cx,cy,&material,&edit) && material.contour_surface &&
            material.contour_surface!=kVoxelContourSkinSnowBody) {
          const bool snow=material.contour_surface==kVoxelContourSkinSnowCrest;
          const unsigned side=material.contour_surface==kVoxelContourSkinBottom?2:0;
          const unsigned full_length=original.fold_length?original.fold_length:original.length;
          if(edge.side==side || ((edge.side&1) && full_length<=(snow?2u:3u)))
            contour_skin=material.contour_surface;
        }
      }
      ++c->surface_stats.native_bands;
      if(SurfaceSandLiveEdge(b,&edge)) {
        candidates[selected++]=(VoxelSurfaceCandidate){.edge=edge};
        textured=true;start+=length;continue;
      }
      VoxelSurfacePatch patch;
      const bool mountain_cap=SurfaceMountainCapPatch(b,&edge,&patch);
      const bool ice_top_riser=top_material && SurfaceIceTopRiserPatch(b,&edge,&patch);
      const bool inward=!mountain_cap && (ice_top_riser || SurfaceNativeEdgePatch(b,&edge,&patch));
      bool found=mountain_cap || inward || (edge.far_rank==2 &&
          SurfaceSelectPatch(b,edge.x,edge.y,!edge.side || top_material || contour_skin,&patch));
      if(found && !inward && SurfaceFlatNativeBottom(b,&edge))found=false;
      if(found) {
        edge.surface_texture=inward?kVoxelSurfaceTextureNativeInward:kVoxelSurfaceTexturePatch;edge.top_sample_x=patch.x;edge.top_sample_y=patch.y;
        edge.top_sample_size=patch.size;edge.surface_derived=patch.derived;edge.surface_native_x=patch.native_x;
        edge.surface_top_material=ice_top_riser?1:inward?0:top_material;edge.contour_skin=inward?0:(uint8_t)contour_skin;
        VoxelSurfaceCandidate *previous=selected?candidates+selected-1:NULL;
        /* The marsh beam's reviewed cap owns one complete16px native band.
         * Its two8px source words are a continuous UV span, so identical
         * halves do not need separate geometry. Other surface families keep
         * their established8px atlas partitioning. */
        const bool same_beam=previous && patch.native_x && previous->edge.surface_native_x &&
            !edge.side && !inward && patch.derived && patch.size==16 &&
            previous->patch_available && previous->edge.x+previous->edge.length==edge.x &&
            previous->patch.x==patch.x && previous->patch.y==patch.y &&
            previous->patch.size==patch.size && previous->patch.derived==patch.derived &&
            !memcmp(previous->patch.pixels,patch.pixels,sizeof(patch.pixels));
        if(same_beam)previous->edge.length+=length;
        else candidates[selected++]=(VoxelSurfaceCandidate){.edge=edge,.patch=patch,.patch_available=true};
        textured=true;
      } else if(selected && !candidates[selected-1].edge.surface_texture) {
        /* A partial native contour may have one usable8px band followed by a
         * long untextured tail. Those unchanged pieces still share the same
         * original boundary and fold; retaining every8px split consumes the
         * room budget without adding artwork or geometry. */
        candidates[selected-1].edge.length+=length;
      } else {
        candidates[selected++]=(VoxelSurfaceCandidate){.edge=edge};
      }
      start+=length;
    }
    if(!textured){edges[count++]=original;continue;}
    /* Reserve every untouched original, but budget only pieces that will
     * actually be emitted. Sample first so a harmless fallback-only run cannot
     * starve later native faces. Never pin artwork for a rejected split. */
    if(count+selected+c->edge_count-i-1>kVoxelMaxEdges) {
      ++c->surface_stats.edge_budget_fallbacks;edges[count++]=original;continue;
    }
    const unsigned begin=count;bool published=false;
    for(unsigned j=0;j<selected;++j) {
      VoxelEdge edge=candidates[j].edge;
      if(candidates[j].patch_available) {
        unsigned id;
        if(SurfaceIntern(b,&candidates[j].patch,true,&id))edge.top_owned_art=id+1;
        else {
          edge.surface_texture=0;edge.top_sample_x=original.top_sample_x;edge.top_sample_y=original.top_sample_y;
          edge.top_sample_size=original.top_sample_size;edge.top_owned_art=original.top_owned_art;
          edge.surface_derived=original.surface_derived;edge.surface_native_x=original.surface_native_x;
          edge.surface_top_material=original.surface_top_material;
          edge.contour_skin=original.contour_skin;
        }
      }
      if(edge.surface_texture){++c->surface_stats.native_textured_bands;published=true;}
      edges[count++]=edge;
    }
    if(!published){count=begin;edges[count++]=original;}
  }
  free(candidates);
  memcpy(c->edges,edges,count*sizeof(*edges));c->edge_count=count;
  SurfaceSnowCapRiserPhase(b);
  SurfaceSnowNativeClosingRisers(b);
  memset(c->pass_counts,0,sizeof(c->pass_counts));
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&c->edges[i];
    c->pass_edges[e->high_priority?1:0][c->pass_counts[e->high_priority?1:0]++]=i;
    if(e->rear_trim)c->pass_edges[2][c->pass_counts[2]++]=i;
    if(e->high_priority && e->colliding && !e->side && e->far_rank>=2)
      c->pass_edges[3][c->pass_counts[3]++]=i;
  }
  for(unsigned i=0;i<c->authored_count;++i) {
    VoxelAuthoredCell *a=&c->authored[i];
    if(!a->surface || !a->native_front_art)continue;
    CastleMaterial m;const DioramaPixelEdit *edit;
    if(!SurfaceMaterial(b,a->x,a->y,&m,&edit))continue;
    if(m.contour_surface==kVoxelContourSkinSnowCrest)continue;
    for(unsigned face=1;face<7;++face) {
      if(face==3 || !(a->automatic_art&(1u<<face)))continue;
      ++c->surface_stats.authored_faces;
      VoxelSurfacePatch patch;unsigned id;bool found=false;
      /* A side spans both8px bands: keep the complete16px source rather than
       * spreading its grass over the wood below. Caps choose their own band. */
      if(face==1 || face==6)found=SurfaceSelectPatch(b,a->x,a->y+(face==6?15:0),
          face==1 || (face==6 && m.contour_surface==kVoxelContourSkinBottom),&patch);
      else found=SurfaceOpaquePatch(b,&m,edit,a->x,a->y,16,&patch);
      if(!found || !SurfaceIntern(b,&patch,false,&id))continue;
      a->art_id[face]=id;a->automatic_art&=~(1u<<face);
      a->surface_sample_x[face]=patch.x;a->surface_sample_y[face]=patch.y;
      a->surface_sample_size[face]=patch.size;a->surface_derived[face]=patch.derived;
      a->surface_native_x[face]=patch.native_x;
      ++c->surface_stats.authored_textured_faces;
    }
  }
  for(unsigned i=0;i<c->patch_count;++i)SurfaceProfileArt(b,&c->patches[i]);
  SurfacePublishPointers(c);
  c->surface_stats.edges_after=count;c->surface_stats.unique_patches=c->authored_art_count-b->initial_art;
  free(b->top_donors);free(b->top_donor_cache);free(b->slots);free(b);free(edges);return true;
}
