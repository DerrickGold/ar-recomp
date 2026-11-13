/* Immutable native-art geometry shared by the game, editor and compositor. */
#include "action_voxel_internal.h"
#include <stdlib.h>

const DioramaVoxelEdits *ActionVoxel_Edits(const ActionVoxelGeometry *c) {
  return c ? &c->edits : NULL;
}
uint16_t *ActionVoxel_RecordOwners(const DioramaVoxelEdits *edits,int x,int y,int w,int h) {
  if(!edits || edits->cell_count>kDioramaVoxelCellMax || w<=0 || h<=0 ||
      (size_t)w*h>kVoxelAuthoredCellMax)return NULL;
  uint16_t *owners=calloc((size_t)w*h,sizeof(*owners));if(!owners)return NULL;
  for(unsigned i=0;i<edits->cell_count;++i) {
    const DioramaVoxelCell *cell=&edits->cells[i];
    const int x0=cell->x>x?cell->x:x,y0=cell->y>y?cell->y:y;
    const int right=cell->x+(int)DioramaVoxelCell_Width(cell),bottom=cell->y+(int)DioramaVoxelCell_Height(cell);
    const int x1=right<x+w?right:x+w,y1=bottom<y+h?bottom:y+h;
    for(int yy=y0;yy<y1;++yy)for(int xx=x0;xx<x1;++xx)owners[(size_t)(yy-y)*w+xx-x]=i+1;
  }
  return owners;
}
bool ActionVoxel_HasObjectEdits(const ActionVoxelGeometry *c) {
  return c && c->edits.object_count;
}
const DioramaVoxelObject *ActionVoxel_ObjectEdit(const ActionVoxelGeometry *c,
    uint16_t source, uint16_t parent, uint16_t animation) {
  return DioramaVoxelEdits_Object(ActionVoxel_Edits(c), source, parent, animation);
}
#include "diorama/diorama_capture_blend.h"
#include "snes_bgr555.h"
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static atomic_uint_fast64_t s_next_revision = 1;
ActionVoxelProfile ActionVoxel_ProfileForRoom(unsigned group, unsigned room) {
  if (group==1 && room==1) return kActionVoxelProfile_FillmoreForest;
  if (group==1 && room==2) return kActionVoxelProfile_FillmoreCave;
  if (group==1 && room==3) return kActionVoxelProfile_FillmoreTemple;
  if (group==1 && room==4) return kActionVoxelProfile_FillmoreTower;
  if (group==2 && room==1) return kActionVoxelProfile_BloodpoolMarsh;
  if (group==3 && room>=1 && room<=2) return kActionVoxelProfile_KassandoraDesert;
  if (group==3 && room>=3 && room<=6) return kActionVoxelProfile_KassandoraTomb;
  if (group==4 && room>=1 && room<=3) return kActionVoxelProfile_AitosMountains;
  if (group==4 && room>=4 && room<=7) return kActionVoxelProfile_AitosCaverns;
  if (group==5 && room>=1 && room<=3) return kActionVoxelProfile_MarahnaJungle;
  if (group==5 && room>=4 && room<=8) return kActionVoxelProfile_MarahnaTemple;
  if (group==6 && (room==1 || room==5 || room==8)) return kActionVoxelProfile_NorthwallSnow;
  if (group==6 && room>=2 && room<=4) return kActionVoxelProfile_NorthwallIce;
  /* The established curved enclosure owns Northwall 06:06–07. */
  if (group==7 && room>=1 && room<=8) return kActionVoxelProfile_DeathHeim;
  return group == 2 && room >= 2 && room <= 8
      ? kActionVoxelProfile_Castle : kActionVoxelProfile_None;
}
ActionVoxelSettings ActionVoxel_SettingsForRoom(unsigned group, unsigned room) {
  /* The final arena's collision is invisible; keep its native celestial
   * artwork behind the actors without inventing a floor or recessing them. */
  if (group==7 && room==8)
    return (ActionVoxelSettings){.depth=.20f,.play_recess=0};
  const ActionVoxelProfile profile=ActionVoxel_ProfileForRoom(group,room);
  if (profile==kActionVoxelProfile_NorthwallSnow || (group==7 && (room==1 || room==7)))
    return (ActionVoxelSettings){.depth=.20f,.play_recess=.065f};
  switch(profile) {
    case kActionVoxelProfile_NorthwallIce:
    case kActionVoxelProfile_FillmoreCave:
      return (ActionVoxelSettings){.depth=.22f,.play_recess=.075f};
    case kActionVoxelProfile_BloodpoolMarsh:
    case kActionVoxelProfile_KassandoraDesert:
    case kActionVoxelProfile_AitosMountains:
    case kActionVoxelProfile_MarahnaJungle:
      return (ActionVoxelSettings){.depth=.20f,.play_recess=.065f};
    case kActionVoxelProfile_FillmoreForest:
      return (ActionVoxelSettings){.depth=.18f,.play_recess=.06f};
    default:
      return ActionVoxel_DefaultSettings();
  }
}
/* Per-profile depth policy. Rear-mounted architecture and the deeper trim
 * class are fixed by the material profile, never by room geometry. */
typedef enum VoxelSceneryRecessMode {
  kVoxelSceneryRecess_None,      /* Scenery stays on the native contact plane. */
  kVoxelSceneryRecess_RearFace,  /* Scenery projects from the rear wall. */
  kVoxelSceneryRecess_Contact    /* Thin forest scenery follows actor contact. */
} VoxelSceneryRecessMode;
typedef struct VoxelProfilePolicy {
  bool rear_mounted, deep_trim;
  unsigned char scenery_recess;
} VoxelProfilePolicy;
static const VoxelProfilePolicy kVoxelProfilePolicies[] = {
  [kActionVoxelProfile_None] = {0},
  [kActionVoxelProfile_Castle] = {0},
  [kActionVoxelProfile_FillmoreForest] = {.scenery_recess=kVoxelSceneryRecess_Contact},
  [kActionVoxelProfile_FillmoreCave] = {0},
  [kActionVoxelProfile_FillmoreTemple] = {true,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_FillmoreTower] = {true,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_BloodpoolMarsh] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_KassandoraDesert] = {false,true,kVoxelSceneryRecess_None},
  [kActionVoxelProfile_KassandoraTomb] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_AitosMountains] = {false,true,kVoxelSceneryRecess_None},
  [kActionVoxelProfile_AitosCaverns] = {false,true,kVoxelSceneryRecess_None},
  [kActionVoxelProfile_MarahnaJungle] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_MarahnaTemple] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_NorthwallSnow] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_NorthwallIce] = {false,true,kVoxelSceneryRecess_RearFace},
  [kActionVoxelProfile_DeathHeim] = {false,true,kVoxelSceneryRecess_RearFace},
};
_Static_assert(sizeof(kVoxelProfilePolicies)/sizeof(*kVoxelProfilePolicies)==kActionVoxelProfile_DeathHeim+1,
               "every voxel profile has a depth policy");
static const VoxelProfilePolicy *ProfilePolicy(const ActionVoxelGeometry *c) {
  const unsigned profile=(unsigned)ActionVoxel_Profile(c);
  return &kVoxelProfilePolicies[profile<=kActionVoxelProfile_DeathHeim ? profile : 0];
}
ActionVoxelProfile ActionVoxel_Profile(const ActionVoxelGeometry *geometry) {
  return geometry ? geometry->profile : kActionVoxelProfile_None;
}
const VoxelTerminalMatte *ActionVoxel_TerminalMatte(const ActionVoxelGeometry *c) {
  return c&&c->ready&&(c->terminal_matte.bottom_opaque_black||c->terminal_matte.right_opaque_black)
      ? &c->terminal_matte:NULL;
}
const VoxelJungleTerminal *ActionVoxel_JungleTerminal(const ActionVoxelGeometry *c) {
  return c&&c->ready?c->jungle_terminal:NULL;
}
bool ActionVoxel_ValidBounds(int x0, int y0, int x1, int y1) {
  if (x0 < INT16_MIN || y0 < INT16_MIN || x1 > INT16_MAX || y1 > INT16_MAX ||
      x1 <= x0 || y1 <= y0) return false;
  const unsigned width = (unsigned)(x1 - x0), height = (unsigned)(y1 - y0);
  return width <= 8192 && height <= 8192 && (size_t)width * height <= 8 * 1024 * 1024;
}
ActionVoxelGeometry *ActionVoxel_Create(void) {
  ActionVoxelGeometry *c = calloc(1, sizeof(*c));
  if (c) {
    atomic_init(&c->references, 1);
    c->revision = atomic_fetch_add_explicit(&s_next_revision, 1, memory_order_relaxed);
  }
  return c;
}
uint64_t ActionVoxel_Revision(const ActionVoxelGeometry *geometry) {
  return geometry ? geometry->revision : 0;
}
const ActionVoxelGeometry *ActionVoxel_Retain(const ActionVoxelGeometry *geometry) {
  ActionVoxelGeometry *c = (ActionVoxelGeometry *)geometry;
  if (c) atomic_fetch_add_explicit(&c->references, 1, memory_order_relaxed);
  return geometry;
}
void ActionVoxel_Release(const ActionVoxelGeometry *geometry) {
  ActionVoxelGeometry *c = (ActionVoxelGeometry *)geometry;
  if (c && atomic_fetch_sub_explicit(&c->references, 1, memory_order_acq_rel) == 1) {
    free(c->front);
    free(c->semantic_cells);
    free(c->native_priority_decks);
    free(c->cutouts);
    free(c->branch_pixels);
    free(c->front_recess);
    free(c->raised_scenery_cells);
    free(c->thin_trim_cells);
    free(c->pixel_recess_cells);
    free(c->trunk_cells);
    free(c->column_cells);
    free(c->rear_relief);
    free(c->soft_surfaces);
    free(c->rear_fills);
    free(c->jungle_terminal);
    free(c->authored);
    free(c->authored_at);
    free(c->authored_art);
    free(c->authored_cap_art);
    free(c->authored_contours);
    free(c->profile_surfaces);
    free(c);
  }
}
bool ActionVoxel_HasCutouts(const ActionVoxelGeometry *c) {
  return c && c->ready && c->cutouts;
}
bool ActionVoxel_CutoutRows(const ActionVoxelGeometry *c, int tile_x, int tile_y,
                            uint8_t rows[8]) {
  if (!rows || !ActionVoxel_HasCutouts(c)) return false;
  /* Bounds are 16px aligned; reject huge coordinates before multiplying. */
  const int64_t x = (int64_t)tile_x - c->origin_x / 8;
  const int64_t y = (int64_t)tile_y - c->origin_y / 8;
  if (x < 0 || y < 0 || x >= c->width / 8 || y >= c->height / 8) return false;
  const size_t pitch = c->width / 8;
  const uint8_t *source = c->cutouts + (size_t)y * 8 * pitch + (size_t)x;
  unsigned any = 0;
  for (unsigned row = 0; row < 8; ++row) {
    const unsigned mask = source[row * pitch];
    rows[row] |= mask;
    any |= mask;
  }
  return any != 0;
}
float ActionVoxel_Depth(const ActionVoxelGeometry *c) { return c ? c->settings.depth : 0; }
float ActionVoxel_PlayRecess(const ActionVoxelGeometry *c) {
  return c ? c->settings.play_recess : 0;
}
bool ActionVoxel_RearMountedScenery(const ActionVoxelGeometry *c) {
  return ProfilePolicy(c)->rear_mounted;
}
float ActionVoxel_SceneryRecess(const ActionVoxelGeometry *c) {
  switch(ProfilePolicy(c)->scenery_recess) {
    case kVoxelSceneryRecess_RearFace:
      return ActionVoxel_Depth(c)-ActionVoxel_TrimDepth(c);
    case kVoxelSceneryRecess_Contact:
      /* OBJ2's native depth is .01 ahead of BG1. Match that contact surface
       * without changing object positions, priority or the trunk's thickness. */
      return fmaxf(0,ActionVoxel_PlayRecess(c)-.01f);
    default:
      if(c && c->material_rear_mounted)return ActionVoxel_RearFaceDepth(c);
      return c && c->contact_scenery ? fmaxf(0,ActionVoxel_PlayRecess(c)-.01f) : 0;
  }
}
float ActionVoxel_FrontRecess(const ActionVoxelGeometry *c,int tile_x,int tile_y) {
  if(!c || !c->ready || !c->front_recess)return 0;
  const int64_t x=(int64_t)tile_x-c->origin_x/8,y=(int64_t)tile_y-c->origin_y/8;
  if(x<0 || y<0 || x>=c->width/8 || y>=c->height/8)return 0;
  const size_t at=(size_t)y*(c->width/8)+(size_t)x;
  return ActionVoxel_SceneryFront(c,c->front_recess[at],
      c->raised_scenery_cells && c->raised_scenery_cells[at]);
}
const uint8_t *ActionVoxel_PixelRecessRows(const ActionVoxelGeometry *c,int tile_x,int tile_y) {
  if(!c || !c->ready || !c->pixel_recess_cells)return NULL;
  const int64_t x=(int64_t)tile_x-c->origin_x/8,y=(int64_t)tile_y-c->origin_y/8;
  if(x<0 || y<0 || x>=c->width/8 || y>=c->height/8)return NULL;
  const unsigned owner=c->pixel_recess_cells[(size_t)y*(c->width/8)+(size_t)x];
  return owner ? c->pixel_recess_rows[owner-1] : NULL;
}
void ActionVoxel_GateInkRows(const ActionVoxelGeometry *c,int tile_x,int tile_y,
    bool high_priority,uint8_t rows[8]) {
  memset(rows,0,8);
  if(!c || !c->ready || !c->front)return;
  const int64_t x=(int64_t)tile_x*8-c->origin_x,y=(int64_t)tile_y*8-c->origin_y;
  if(x<0 || y<0 || x+8>c->width || y+8>c->height)return;
  const unsigned tag=kVoxelGate|(high_priority?kVoxelHighPriority:0);
  const uint8_t *cell=c->front+(size_t)y*c->width+x;
  for(unsigned py=0;py<8;++py)
    for(unsigned px=0;px<8;++px)
      if((cell[py*c->width+px]&(kVoxelKindMask|kVoxelHighPriority))==tag)
        rows[py]|=1u<<(7-px);
}
bool ActionVoxel_SnowReceiverRun(const ActionVoxelGeometry *c,int world_x,int world_y,int *bottom) {
  if(!bottom||!c||!c->ready||c->profile!=kActionVoxelProfile_NorthwallSnow||
      !c->front||!c->soft_surfaces)return false;
  const int64_t x=(int64_t)world_x-c->origin_x,y=(int64_t)world_y-c->origin_y;
  if(x<0||y<0||x>=c->width||y>=c->height||
      !c->soft_surfaces[(size_t)(y/16)*(c->width/16)+(size_t)x/16])return false;
  const unsigned pixel=c->front[(size_t)y*c->width+(size_t)x];
  if((pixel&kVoxelKindMask)!=kVoxelSolid)return false;
  if(y>0&&(c->front[(size_t)(y-1)*c->width+(size_t)x]&kVoxelKindMask)==kVoxelSolid)return false;
  const unsigned material=pixel&(kVoxelKindMask|kVoxelHighPriority);
  int end=(int)y,limit=((int)y/16+1)*16;
  if(limit>y+8)limit=(int)y+8;
  if(limit>c->height)limit=c->height;
  while(end<limit&&(c->front[(size_t)end*c->width+(size_t)x]&
      (kVoxelKindMask|kVoxelHighPriority))==material)++end;
  if(end==y)return false;
  *bottom=c->origin_y+end;return true;
}
const VoxelAuthoredCell *ActionVoxel_AuthoredCells(const ActionVoxelGeometry *c, unsigned *count) {
  if (count) *count = c ? c->authored_count : 0;
  return c ? c->authored : NULL;
}
const VoxelAuthoredCell *ActionVoxel_AuthoredCellAt(const ActionVoxelGeometry *c,int x,int y) {
  if(!c || !c->authored_count || !c->authored_at)return NULL;
  x-=c->origin_x/16;y-=c->origin_y/16;
  if(x<0 || y<0 || x>=c->width/16 || y>=c->height/16)return NULL;
  const unsigned at=c->authored_at[(size_t)y*(c->width/16)+x];
  return at && at<=c->authored_count?&c->authored[at-1]:NULL;
}
unsigned ActionVoxel_AuthoredArtCount(const ActionVoxelGeometry *c) {
  return c?c->authored_art_count:0;
}
const VoxelSurfaceStats *ActionVoxel_SurfaceStats(const ActionVoxelGeometry *c) {
  return c ? &c->surface_stats : NULL;
}
const uint16_t *ActionVoxel_AuthoredCapArt(const ActionVoxelGeometry *c,unsigned *count) {
  if(count)*count=c?c->authored_cap_art_count:0;
  return c?c->authored_cap_art:NULL;
}
const uint16_t *ActionVoxel_AuthoredArt(const ActionVoxelGeometry *c,unsigned id) {
  return c && id<c->authored_art_count?c->authored_art[id]:NULL;
}
const char *ActionVoxel_BuildError(const ActionVoxelGeometry *c) {
  return c && c->build_error==1?"Voxel edit exceeds the 32,768-cell room limit":
      c && c->build_error==2?"Voxel edit exceeds 8,192 unique native artwork squares; reduce unique face artwork":
      c && c->build_error==3?"Voxel geometry allocation failed":
      c && c->build_error==4?"Voxel shape or face settings are invalid":
      c && c->build_error==5?"Voxel edit exceeds 262,144 silhouette edges; simplify transparent face artwork":
      c && c->build_error==9?"Native move/resize requires the complete classified object (at most 4,096 cells) with one shared transform":
      c && c->build_error==10?"Reset backing-only overrides before moving or resizing this native structural group":
      c && c->build_error==11?"Native gates, water, rear trim and crossed scenery do not support structural move/resize yet":"Voxel geometry could not be compiled";
}
void ActionVoxel_AuthoredDepths(const VoxelAuthoredCell *a,unsigned q,float y,float *front,float *back) {
  if(!a || q>=4 || !front || !back)return;
  float t=a->shape_h?fmaxf(0,fminf(1,(y-a->shape_y)/a->shape_h)):0;
  if(a->shape==kDioramaVoxelShape_Box)t=0;
  else if(a->shape==kDioramaVoxelShape_Curve)t=t*t*(3-2*t);
  *front=a->front[q]+(a->front_end[q]-a->front[q])*t;
  *back=fmaxf(*front,a->back[q]+(a->back_end[q]-a->back[q])*t);
}
void ActionVoxel_BendOffset(const DioramaVoxelBend *bend,float source_x,float source_y,
    float *dx,float *dy,float *dz) {
  if(!dx || !dy || !dz)return;
  *dx=*dy=*dz=0;
  if(!bend || (bend->axis!=kDioramaVoxelBend_X && bend->axis!=kDioramaVoxelBend_Y) ||
      bend->anchor==bend->tip)return;
  const float source=bend->axis==kDioramaVoxelBend_X?source_x:source_y;
  if(!isfinite(source))return;
  float t=fmaxf(0,fminf(1,(source-bend->anchor)/(bend->tip-bend->anchor)));
  t=t*t*(3-2*t);
  *dx=bend->dx*t;*dy=bend->dy*t;*dz=bend->dz*t;
}
void ActionVoxel_AuthoredBend(const VoxelAuthoredCell *a,float source_x,float source_y,
    float *dx,float *dy,float *dz) {
  ActionVoxel_BendOffset(a?&a->bend:NULL,source_x,source_y,dx,dy,dz);
}
bool ActionVoxel_TransformValid(const DioramaVoxelTransform *t) {
  return t && t->px>=-8192 && t->px<=8192 && t->py>=-8192 && t->py<=8192 &&
      t->pz>=-2 && t->pz<=2 && t->dx>=-256 && t->dx<=256 && t->dy>=-256 && t->dy<=256 &&
      t->dz>=-1.2f && t->dz<=1.2f && t->sx>=.125f && t->sx<=4 &&
      t->sy>=.125f && t->sy<=4 && t->sz>=.125f && t->sz<=4;
}
void ActionVoxel_TransformPoint(const DioramaVoxelTransform *t,float x,float y,float z,
    float *out_x,float *out_y,float *out_z) {
  if(!out_x || !out_y || !out_z)return;
  if(ActionVoxel_TransformValid(t)) {
    x=(t->sx==1?x:t->px+(x-t->px)*t->sx)+t->dx;
    y=(t->sy==1?y:t->py+(y-t->py)*t->sy)+t->dy;
    z=(t->sz==1?z:t->pz+(z-t->pz)*t->sz)+t->dz;
  }
  *out_x=x;*out_y=y;*out_z=z;
}
const VoxelAuthoredContour *ActionVoxel_AuthoredContours(const ActionVoxelGeometry *c,
    const VoxelAuthoredCell *a,unsigned *count) {
  if(count)*count=c && a?a->contour_count:0;
  return c && a && a->contour_count?c->authored_contours+a->contour_start:NULL;
}
bool ActionVoxel_AuthoredContourExposed(const ActionVoxelGeometry *c,const VoxelAuthoredCell *a,
    int x,int y,unsigned side) {
  if(!a || x<0 || y<0 || x>=16 || y>=16 || side>=4 || !(a->pixels[0][y*16+x]&0x8000))return false;
  const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};int nx=x+dx[side],ny=y+dy[side];
  const VoxelAuthoredCell *b=a;
  if(nx<0 || ny<0 || nx>=16 || ny>=16) {
    b=ActionVoxel_AuthoredCellAt(c,a->x/16+(nx<0?-1:nx>=16),a->y/16+(ny<0?-1:ny>=16));
    if(!b || !b->surface || !(b->faces&~kDioramaVoxelFaceMask_Back))return true;
    nx=(nx+16)%16;ny=(ny+16)%16;
  }
  if(!(b->pixels[0][ny*16+nx]&0x8000))return true;
  const unsigned q=(y/8)*2+x/8,nq=(ny/8)*2+nx/8;
  for(unsigned end=0;end<2;++end) {
    const float sx=a->x+x+(side==1)+(!(side&1)?end:0);
    const float sy=a->y+y+(side==2)+((side&1)?end:0);
    float adx,ady,adz,bdx,bdy,bdz;
    ActionVoxel_AuthoredBend(a,sx,sy,&adx,&ady,&adz);
    ActionVoxel_AuthoredBend(b,sx,sy,&bdx,&bdy,&bdz);
    const float ax=a->x+8+a->offset_x+(sx-a->x-8)*a->width/16+adx;
    const float bx=b->x+8+b->offset_x+(sx-b->x-8)*b->width/16+bdx;
    float af,ab,bf,bb;ActionVoxel_AuthoredDepths(a,q,sy,&af,&ab);ActionVoxel_AuthoredDepths(b,nq,sy,&bf,&bb);
    float atx,aty,atz,btx,bty,btz,unused;
    ActionVoxel_TransformPoint(&a->transform,ax,sy+ady,af+adz,&atx,&aty,&atz);
    ActionVoxel_TransformPoint(&b->transform,bx,sy+bdy,bf+bdz,&btx,&bty,&btz);
    if(fabsf(atx-btx)>.00001f || fabsf(aty-bty)>.00001f || btz>atz+.00001f)return true;
    ActionVoxel_TransformPoint(&a->transform,ax,sy+ady,ab+adz,&unused,&unused,&atz);
    ActionVoxel_TransformPoint(&b->transform,bx,sy+bdy,bb+bdz,&unused,&unused,&btz);
    if(btz<atz-.00001f)return true;
  }
  return false;
}
bool ActionVoxel_AuthoredSurfaceAt(const ActionVoxelGeometry *c,int x,int y) {
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(c,(int)floorf(x/16.f),(int)floorf(y/16.f));
  return a && a->surface;
}
bool ActionVoxel_HasHiddenNativeFronts(const ActionVoxelGeometry *c) {
  return c && c->hidden_native_front_count;
}
bool ActionVoxel_HasNativeTransforms(const ActionVoxelGeometry *c) {
  return c && c->native_transform_count;
}
bool ActionVoxel_HasTransformedBacks(const ActionVoxelGeometry *c) {
  return c && c->transformed_rear_count;
}
#include "action_voxel_groups.inc"
bool ActionVoxel_CellOwnsArtwork(const DioramaVoxelCell *cell) {
  if(!cell)return false;
  uint32_t fields=DioramaVoxelCell_EffectiveFields(cell) &
      ~(cell->faces==kDioramaVoxelFaceMask_All?kDioramaVoxelCell_Faces:0u);
  if(ActionVoxel_CellKeepsNativeIdentity(cell))fields&=~kDioramaVoxelCell_Transform;
  return fields && !(fields==kDioramaVoxelCell_Faces &&
      cell->faces==(kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front));
}
bool ActionVoxel_CellUsesNativeTransform(const DioramaVoxelCell *cell) {
  const uint32_t fields=DioramaVoxelCell_EffectiveFields(cell);
  return (fields&kDioramaVoxelCell_Transform) &&
      !(fields&~(kDioramaVoxelCell_Transform|kDioramaVoxelCell_Faces));
}
bool ActionVoxel_CellKeepsNativeIdentity(const DioramaVoxelCell *cell) {
  if(!ActionVoxel_CellUsesNativeTransform(cell) || !ActionVoxel_TransformValid(&cell->transform))return false;
  const DioramaVoxelTransform *t=&cell->transform;
  const unsigned faces=cell->fields&kDioramaVoxelCell_Faces?cell->faces:kDioramaVoxelFaceMask_All;
  /* Other visibility masks still require the native owned-face renderer. */
  return t->dx==0 && t->dy==0 && t->dz==0 && t->sx==1 && t->sy==1 && t->sz==1 &&
      (faces==kDioramaVoxelFaceMask_All ||
       faces==(kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front));
}
bool ActionVoxel_CellGeometry(const ActionVoxelGeometry *c,int x,int y,VoxelCellGeometry *out) {
  if (!c || !c->ready || !out || x<c->origin_x/16 || y<c->origin_y/16 ||
      x>=(c->origin_x+c->width)/16 || y>=(c->origin_y+c->height)/16) return false;
  *out=(VoxelCellGeometry){.width=16,.faces=kDioramaVoxelFaceMask_All};
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(c,x,y);
  if(a) {
    memcpy(out->front,a->front,sizeof(out->front));memcpy(out->back,a->back,sizeof(out->back));
    out->width=a->width;out->offset_x=a->offset_x;out->authored=true;
    out->faces=a->faces;out->shape=a->shape;out->shape_y=a->shape_y;out->shape_h=a->shape_h;
    out->front_end=a->front_end[0];out->back_end=a->back_end[0];out->bend=a->bend;out->transform=a->transform;return true;
  }
  for (unsigned q=0;q<4;++q) {
    const int tx=x*2+(q&1),ty=y*2+(q>>1),mx=tx*8-c->origin_x,my=ty*8-c->origin_y;
    unsigned kind=0;bool branch=false;
    for(int yy=0;yy<8;++yy)for(int xx=0;xx<8;++xx) {
      const size_t at=(size_t)(my+yy)*c->width+mx+xx;
      const unsigned k=c->front[at]&kVoxelKindMask;
      if(k==kVoxelSolid || !kind)kind=k;
      branch|=c->branch_pixels && (c->branch_pixels[at>>3]&(1u<<(at&7)));
    }
    const bool modeled=kind && kind!=kVoxelRearOnly && kind!=kVoxelFlatParapet;
    out->front[q]=modeled?ActionVoxel_FrontRecess(c,tx,ty):ActionVoxel_Depth(c);
    out->back[q]=ActionVoxel_Depth(c);
    if(kind==kVoxelForeground || kind==kVoxelThin || kind==kVoxelFoliage) {
      const bool thin=c->thin_trim_cells && c->thin_trim_cells[(size_t)(my/8)*(c->width/8)+mx/8];
      const bool raised=c->raised_scenery_cells && c->raised_scenery_cells[(size_t)(my/8)*(c->width/8)+mx/8];
      out->back[q]=ActionVoxel_SceneryBack(c,out->front[q],thin,raised);
    }
    if(branch)out->back[q]=ActionVoxel_BranchBackDepth(c);
    const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForCell(c,tx,ty);
    if(curve) {
      VoxelTrunkCurve section;ActionVoxel_ColumnSection(curve,tx*8+4,&section);
      out->front[q]=ActionVoxel_ColumnFrontDepth(c,&section,ty*8+4);
    }
    curve=ActionVoxel_TrunkCurveForCell(c,tx,ty);
    if(curve) {out->front[q]=ActionVoxel_TrunkFrontDepth(c,curve,ty*8+4);
      out->back[q]=ActionVoxel_TrunkBackDepth(c,curve,ty*8+4);}
  }
  out->front_end=out->back_end=out->back[0];
  return true;
}
float ActionVoxel_TrimBackDepth(const ActionVoxelGeometry *c) {
  return ActionVoxel_SceneryRecess(c)+ActionVoxel_TrimDepth(c);
}
float ActionVoxel_TrimDepth(const ActionVoxelGeometry *c) {
  return ActionVoxel_Depth(c)*(ProfilePolicy(c)->deep_trim ? .25f : .12f);
}
float ActionVoxel_ClosureInset(const ActionVoxelGeometry *c) {
  return ActionVoxel_Depth(c)*.16f;
}
float ActionVoxel_BranchDepth(const ActionVoxelGeometry *c) {
  return fminf(ActionVoxel_Depth(c), fmaxf(ActionVoxel_TrimDepth(c),
                                         ActionVoxel_PlayRecess(c) + .015f));
}
float ActionVoxel_BranchBackDepth(const ActionVoxelGeometry *c) {
  return ActionVoxel_SceneryRecess(c)+ActionVoxel_BranchDepth(c);
}
float ActionVoxel_RearFaceDepth(const ActionVoxelGeometry *c) {
  return ActionVoxel_Depth(c) - ActionVoxel_TrimDepth(c);
}
const VoxelTrunkCurve *ActionVoxel_TrunkCurveForCell(const ActionVoxelGeometry *c,int tile_x,int tile_y) {
  if(!c || !c->ready || !c->trunk_cells)return NULL;
  const int64_t x=(int64_t)tile_x-c->origin_x/8,y=(int64_t)tile_y-c->origin_y/8;
  if(x<0 || y<0 || x>=c->width/8 || y>=c->height/8)return NULL;
  const unsigned owner=c->trunk_cells[(size_t)y*(c->width/8)+(size_t)x];
  return owner && owner<=c->trunk_curve_count?&c->trunk_curves[owner-1]:NULL;
}
const VoxelTrunkCurve *ActionVoxel_TrunkCurveForEdge(const ActionVoxelGeometry *c,const VoxelEdge *edge) {
  return c && c->ready && edge && edge->trunk_curve && edge->trunk_curve<=c->trunk_curve_count
      ? &c->trunk_curves[edge->trunk_curve-1]:NULL;
}
static float TrunkCurveWeight(const VoxelTrunkCurve *curve,float world_y) {
  if(!curve || curve->base_y<=curve->top_y || !isfinite(world_y))return 0;
  const float t=fmaxf(0,fminf(1,(world_y-curve->top_y)/(curve->base_y-curve->top_y)));
  return t*t*(3-2*t);
}
float ActionVoxel_TrunkFrontDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y) {
  const float contact=ActionVoxel_SceneryRecess(c);
  return contact+(ActionVoxel_Depth(c)-contact)*TrunkCurveWeight(curve,world_y);
}
float ActionVoxel_TrunkBackDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y) {
  return ActionVoxel_TrunkFrontDepth(c,curve,world_y)+
      ActionVoxel_TrimDepth(c)*(1-TrunkCurveWeight(curve,world_y));
}
const VoxelRearTrim *ActionVoxel_RearTrims(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->rear_trim_count : 0;
  return c ? c->rear_trims : NULL;
}
const VoxelGate *ActionVoxel_Gates(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->gate_count : 0;
  return c ? c->gates : NULL;
}
const VoxelPatch *ActionVoxel_Patches(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->patch_count : 0;
  return c ? c->patches : NULL;
}
const VoxelProfileSurface *ActionVoxel_ProfileSurfaces(const ActionVoxelGeometry *c,unsigned *count) {
  *count=c?c->profile_surface_count:0;
  return c?c->profile_surfaces:NULL;
}
const VoxelWaterSurface *ActionVoxel_WaterSurfaces(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->water_surface_count : 0;
  return c ? c->water_surfaces : NULL;
}
bool ActionVoxel_HasRearRelief(const ActionVoxelGeometry *c) { return c && c->rear_relief; }
const VoxelWaterfall *ActionVoxel_Waterfalls(const ActionVoxelGeometry *c,unsigned *count) {
  *count=c?c->waterfall_count:0;
  return c?c->waterfalls:NULL;
}
static float ReliefCell(const ActionVoxelGeometry *c,int x,int y) {
  const int width=(c->width+15)/16,height=(c->height+15)/16;
  return x>=0 && y>=0 && x<width && y<height ? c->rear_relief[y*width+x]/255.f : 0;
}
float ActionVoxel_RearRelief(const ActionVoxelGeometry *c,float world_x,float world_y) {
  if(!ActionVoxel_HasRearRelief(c) || !isfinite(world_x) || !isfinite(world_y) ||
      world_x<c->origin_x-16 || world_y<c->origin_y-16 ||
      world_x>c->origin_x+c->width+16 || world_y>c->origin_y+c->height+16) return 0;
  const float x=(world_x-c->origin_x)/16-.5f,y=(world_y-c->origin_y)/16-.5f;
  const int ix=(int)floorf(x),iy=(int)floorf(y);
  const float tx=x-ix,ty=y-iy,sx=tx*tx*(3-2*tx),sy=ty*ty*(3-2*ty);
  const float a=ReliefCell(c,ix,iy)*(1-sx)+ReliefCell(c,ix+1,iy)*sx;
  const float b=ReliefCell(c,ix,iy+1)*(1-sx)+ReliefCell(c,ix+1,iy+1)*sx;
  return .075f*(a*(1-sy)+b*sy);
}
bool ActionVoxel_Active(const ActionVoxelGeometry *c, unsigned group, unsigned room) {
  return c && c->ready && group == c->active_group && room == c->active_room &&
      c->profile != kActionVoxelProfile_None && ActionVoxel_Depth(c) > 0;
}
const VoxelEdge *ActionVoxel_Edges(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->edge_count : 0;
  return c ? c->edges : NULL;
}
const VoxelLintelReturn *ActionVoxel_LintelReturns(const ActionVoxelGeometry *c, unsigned *count) {
  *count = c ? c->lintel_return_count : 0;
  return c ? c->lintel_returns : NULL;
}
static bool HighDeck(const VoxelEdge *e) {
  return e->high_priority && e->colliding && e->side == 0 && e->far_rank >= 2;
}
bool ActionVoxel_HasHighDecks(const ActionVoxelGeometry *c) {
  return c && c->high_deck_count != 0;
}
float ActionVoxel_EdgeRecess(const ActionVoxelGeometry *c,const VoxelEdge *e) {
  return ActionVoxel_SceneryFront(c,!(c && c->mixed_scenery && !e->rear_mounted),e->raised_scenery);
}
bool ActionVoxel_HasColumns(const ActionVoxelGeometry *c) {
  return c && c->ready && c->column_curve_count;
}
const VoxelTrunkCurve *ActionVoxel_ColumnCurveForCell(const ActionVoxelGeometry *c,int tile_x,int tile_y) {
  if(!ActionVoxel_HasColumns(c) || !c->column_cells)return NULL;
  const int64_t x=(int64_t)tile_x-c->origin_x/8,y=(int64_t)tile_y-c->origin_y/8;
  if(x<0 || y<0 || x>=c->width/8 || y>=c->height/8)return NULL;
  const unsigned owner=c->column_cells[(size_t)y*(c->width/8)+(size_t)x];
  return owner && owner<=c->column_curve_count?&c->column_curves[owner-1]:NULL;
}
const VoxelTrunkCurve *ActionVoxel_ColumnCurveForEdge(const ActionVoxelGeometry *c,const VoxelEdge *e) {
  return ActionVoxel_HasColumns(c) && e && e->column_curve && e->column_curve<=c->column_curve_count
      ? &c->column_curves[e->column_curve-1]:NULL;
}
void ActionVoxel_ColumnSection(const VoxelTrunkCurve *curve,float world_x,VoxelTrunkCurve *section) {
  if(!section)return;
  *section=curve?*curve:(VoxelTrunkCurve){0};
  if(!curve || curve->contact_count!=8 || !isfinite(world_x))return;
  int column=(int)floorf((world_x-curve->contact_x)*.5f);
  if(column<0)column=0;else if(column>7)column=7;
  const short contact=curve->contact[column];
  section->top_y=contact+4;section->base_y=contact+16;
  section->contact_x=0;section->contact_count=0;
  memset(section->contact,0,sizeof(section->contact));
}
float ActionVoxel_ColumnFrontDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y) {
  /* Only grounded reviewed desert shafts receive a column owner. Preserve
   * their full cap join with one native8px underside taper, then clear the actor
   * contact (.01 ahead of PlayRecess) by .025. Elevated shafts have no owner. */
  const float weight=curve && curve->linear && curve->base_y>curve->top_y && isfinite(world_y)
      ? fmaxf(0,fminf(1,(world_y-curve->top_y)/(curve->base_y-curve->top_y)))
      : TrunkCurveWeight(curve,world_y);
  if(c && c->profile==kActionVoxelProfile_NorthwallSnow && curve) {
    const float contact=fmaxf(0,ActionVoxel_PlayRecess(c)-.01f);
    return contact+(ActionVoxel_SceneryRecess(c)-contact)*weight;
  }
  if(c && c->profile==kActionVoxelProfile_KassandoraDesert && curve)
    return fminf(ActionVoxel_Depth(c),ActionVoxel_PlayRecess(c)+.015f)*weight;
  return ActionVoxel_Depth(c)*weight;
}
bool ActionVoxel_ResolveEdgeSpanAtY(const ActionVoxelGeometry *c, const VoxelEdge *e,
                                     VoxelEdgePass pass, float near_depth, float far_depth,
                                     float world_y,VoxelEdgeSpan *out) {
  const float depth = ActionVoxel_Depth(c),
      depths[] = {0, ActionVoxel_TrimBackDepth(c), depth, ActionVoxel_BranchBackDepth(c)};
  const float actor_depth = fmaxf(0, ActionVoxel_PlayRecess(c) - .01f);
  const bool deck = HighDeck(e);
  if (pass == kVoxelEdgeRearTrim && !e->rear_trim) return false;
  if (pass == kVoxelEdgeHighDeckBehind) {
    if (!deck) return false;
    near_depth = fmaxf(near_depth, actor_depth);
  } else if (pass == kVoxelEdgeNative && deck) {
    /* Only the near part of a high-source walking surface belongs in the
     * foreground slot. Its extension behind actors is submitted earlier. */
    far_depth = fminf(far_depth, actor_depth);
  }
  /* Preserve native-low scenery and reviewed pass-through beams wholly behind
   * actors. Their exposed caps must not cut through a passing actor. */
  const bool behind_actors = !e->high_priority &&
      (!e->colliding || e->far_rank == 1 || e->native_actor_priority);
  if (behind_actors && far_depth < depth) return false;
  /* A column mounted against the rear wall covers the back of a ledge.
   * Its exposed cap is in front of the column, unlike the castle's forward
   * parapet whose cap continues behind it. Do not discard that walking top
   * merely because both materials end at the same rear depth. */
  const bool column_cap = e->near_rank==1 && e->far_rank==2 &&
      ActionVoxel_RearMountedScenery(c);
  float exposed_near=column_cap ? 0 : depths[e->near_rank];
  float exposed_far=column_cap ? ActionVoxel_SceneryRecess(c) : depths[e->far_rank];
  if(c->mixed_scenery || e->column_curve || e->neighbor_column_curve) {
    /* Subtract the neighbour's physical interval, not just its thickness.
     * A full deck exposes its front beside a rear column, and its back beside
     * a front jamb. Disjoint trim leaves both silhouettes exposed. */
    exposed_near=ActionVoxel_SceneryFront(c,e->rear_mounted,e->raised_scenery);
    if(e->column_curve && e->column_curve<=c->column_curve_count) {
      VoxelTrunkCurve section;
      ActionVoxel_ColumnSection(&c->column_curves[e->column_curve-1],e->x+.5f,&section);
      exposed_near=ActionVoxel_ColumnFrontDepth(c,&section,world_y);
    }
    exposed_far=e->far_rank==1?ActionVoxel_SceneryBack(c,exposed_near,e->thin_trim,e->raised_scenery):depths[e->far_rank];
    float neighbor_near=ActionVoxel_SceneryFront(c,e->neighbor_rear_mounted,e->neighbor_raised_scenery);
    if(e->neighbor_column_curve && e->neighbor_column_curve<=c->column_curve_count) {
      VoxelTrunkCurve section;
      ActionVoxel_ColumnSection(&c->column_curves[e->neighbor_column_curve-1],
          e->x+.5f+(e->side==1?1:e->side==3?-1:0),&section);
      neighbor_near=ActionVoxel_ColumnFrontDepth(c,&section,world_y);
    }
    const float neighbor_far=e->near_rank==1?ActionVoxel_SceneryBack(c,neighbor_near,e->neighbor_thin_trim,
        e->neighbor_raised_scenery):depths[e->near_rank];
    if(neighbor_far>exposed_near && neighbor_near<exposed_far) {
      const bool ramp=(e->column_curve && e->column_curve<=c->column_curve_count &&
          c->column_curves[e->column_curve-1].contact_count==8) ||
          (e->neighbor_column_curve && e->neighbor_column_curve<=c->column_curve_count &&
           c->column_curves[e->neighbor_column_curve-1].contact_count==8);
      /* An emerging ramp seam collapses at its owner's front, not at the
       * back of the fully covering neighbour. The projected endpoint must
       * meet the next visible slice instead of drawing a diagonal wedge. */
      if(ramp && neighbor_near<=exposed_near && neighbor_far>=exposed_far)
        exposed_far=exposed_near;
      else if(neighbor_near<=exposed_near)exposed_near=neighbor_far;
      else exposed_far=neighbor_near;
    }
  }
  out->near_depth = behind_actors ? exposed_near : fmaxf(near_depth, exposed_near);
  if(e->far_rank==1 || e->far_rank==3)
    out->near_depth=fmaxf(out->near_depth,ActionVoxel_EdgeRecess(c,e));
  out->far_depth = fminf(far_depth, exposed_far);
  return out->far_depth > out->near_depth;
}
bool ActionVoxel_ResolveEdgeSpan(const ActionVoxelGeometry *c,const VoxelEdge *e,
    VoxelEdgePass pass,float near_depth,float far_depth,VoxelEdgeSpan *out) {
  const float y=e->y+(e->side==2);
  if(!(e->side&1) || (!e->column_curve && !e->neighbor_column_curve))
    return ActionVoxel_ResolveEdgeSpanAtY(c,e,pass,near_depth,far_depth,y,out);
  const unsigned owners[]={e->column_curve,e->neighbor_column_curve};
  bool ramp=false;
  for(unsigned i=0;i<2;++i)ramp|=owners[i] && owners[i]<=c->column_curve_count &&
      c->column_curves[owners[i]-1].contact_count==8;
  if(ramp) {
    /* A thin seam may exist only near one end of a tapered span. Start,
     * midpoint and end alone can all lie outside an actor-clipped interval.
     * Linear ramp sections change only at their lip/base and clip crossings;
     * their endpoints plus intervening midpoints give the complete union. */
    float knots[12]={y,y+e->length};unsigned count=2;
    const float depth=ActionVoxel_Depth(c);
    const float limits[]={near_depth,far_depth,fmaxf(0,ActionVoxel_PlayRecess(c)-.01f)};
    for(unsigned i=0;i<2;++i)if(owners[i] && owners[i]<=c->column_curve_count) {
      VoxelTrunkCurve section;
      ActionVoxel_ColumnSection(&c->column_curves[owners[i]-1],
          e->x+.5f+(i?(e->side==1?1:e->side==3?-1:0):0),&section);
      const float endpoints[]={section.top_y,section.base_y};
      for(unsigned k=0;k<2;++k)
        if(endpoints[k]>y && endpoints[k]<y+e->length)knots[count++]=endpoints[k];
      if(section.linear && depth>0)for(unsigned k=0;k<3;++k) {
        if(!isfinite(limits[k]) || limits[k]<=0 || limits[k]>=depth)continue;
        const float crossing=section.top_y+(section.base_y-section.top_y)*limits[k]/depth;
        if(crossing>y && crossing<y+e->length)knots[count++]=crossing;
      }
    }
    for(unsigned i=1;i<count;++i) {
      const float value=knots[i];unsigned j=i;
      while(j && knots[j-1]>value){knots[j]=knots[j-1];--j;}knots[j]=value;
    }
    bool any=false,bounds=false;
    for(unsigned i=0;i<count*2-1;++i) {
      const float at=(i&1)?(knots[i/2]+knots[i/2+1])*.5f:knots[i/2];
      VoxelEdgeSpan sample={NAN,NAN};
      any|=ActionVoxel_ResolveEdgeSpanAtY(c,e,pass,near_depth,far_depth,at,&sample);
      /* Collapsed limiting endpoints bound a visible adjacent interval,
       * but never make an otherwise empty edge visible on their own. */
      if(!isfinite(sample.near_depth) || !isfinite(sample.far_depth) ||
          sample.far_depth<sample.near_depth)continue;
      if(!bounds)*out=sample;
      else {out->near_depth=fminf(out->near_depth,sample.near_depth);out->far_depth=fmaxf(out->far_depth,sample.far_depth);}
      bounds=true;
    }
    return any;
  }
  bool any=false;
  for(unsigned i=0;i<3;++i) {
    VoxelEdgeSpan sample;
    if(!ActionVoxel_ResolveEdgeSpanAtY(c,e,pass,near_depth,far_depth,y+e->length*i*.5f,&sample))continue;
    if(!any)*out=sample;
    else {out->near_depth=fminf(out->near_depth,sample.near_depth);out->far_depth=fmaxf(out->far_depth,sample.far_depth);}
    any=true;
  }
  return any;
}
static unsigned BackingSubtract(const ActionRoomSceneFrameState *f) {
  return
      DioramaCaptureBlend_LayerUsesFixedColorSubtract(f->cgwsel, f->cgadsub, f->fixed_color, 1)
          ? f->fixed_color
          : 0;
}
static uint32_t BackingColor(unsigned color, unsigned subtract, unsigned brightness) {
  unsigned rgb = 0;
  for (unsigned channel = 0; channel < 3; ++channel) {
    int value = ((color >> (channel * 5)) & 31) - ((subtract >> (channel * 5)) & 31);
    rgb |= (unsigned)ExpandColor5(value > 0 ? value : 0, brightness) << (16 - channel * 8);
  }
  return 0xff000000u | rgb;
}
uint32_t ActionVoxel_AuthoredColor(unsigned color,const ActionRoomSceneFrameState *frame) {
  return color&0x8000 ? BackingColor(color,frame?BackingSubtract(frame):0,frame?frame->brightness:15) : 0;
}
uint32_t ActionVoxel_WaterSurfaceColor(const VoxelWaterSurface *surface, unsigned background,
    const ActionRoomSceneFrameState *frame) {
  if (!surface || background > 1 || !(surface->uniform_color[0] & 0x8000) ||
      !(surface->uniform_color[1] & 0x8000)) return 0;
  /* BG2 is a separate main-screen term. Do not reuse AuthoredColor's BG1-only
   * fixed-color eligibility; full-add remains the compositor's existing job. */
  const unsigned subtract = frame && DioramaCaptureBlend_LayerUsesFixedColorSubtract(
      frame->cgwsel, frame->cgadsub, frame->fixed_color, background + 1)
      ? frame->fixed_color : 0;
  return BackingColor(surface->uniform_color[background], subtract,
      frame ? frame->brightness : 15);
}
static void BuildBacking(const ActionVoxelGeometry *c, const ActionRoomSceneFrameState *f, uint32_t backing[512]) {
  const unsigned subtract = BackingSubtract(f);
  for (unsigned i = 0; i < 16 * 32; ++i) {
    backing[i] = BackingColor(c->backing_color[i], subtract, f->brightness);
  }
}
typedef struct VoxelSplitKeep {
  uint32_t rear, front;
} VoxelSplitKeep;
_Static_assert(kVoxelKindMask==7 && kVoxelRearCopy==128 && kVoxelSolid==1 &&
    kVoxelThin==2 && kVoxelFoliage==3 && kVoxelForeground==4 && kVoxelGate==5 &&
    kVoxelFlatParapet==6 && kVoxelRearOnly==7, "direct split material layout");
/* Simple rooms already have their final ownership in front[]. The other
 * material flags do not change this split. Keep full words, including RGB in
 * transparent pixels, exactly as the general path does. */
#define VOXEL_SPLIT_LOW_ROW \
  {UINT32_MAX,0}, {UINT32_MAX,UINT32_MAX}, {0,UINT32_MAX}, {0,UINT32_MAX}, \
  {0,UINT32_MAX}, {UINT32_MAX,UINT32_MAX}, {UINT32_MAX,0}, {UINT32_MAX,0}
#define VOXEL_SPLIT_COPY_ROW \
  {UINT32_MAX,0}, {UINT32_MAX,UINT32_MAX}, {UINT32_MAX,UINT32_MAX}, {UINT32_MAX,UINT32_MAX}, \
  {UINT32_MAX,UINT32_MAX}, {UINT32_MAX,UINT32_MAX}, {UINT32_MAX,0}, {UINT32_MAX,0}
static const VoxelSplitKeep kDirectSplitKeep[256] = {
  VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW,
  VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW,
  VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW,
  VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW, VOXEL_SPLIT_LOW_ROW,
  VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW,
  VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW,
  VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW,
  VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW, VOXEL_SPLIT_COPY_ROW,
};
#undef VOXEL_SPLIT_LOW_ROW
#undef VOXEL_SPLIT_COPY_ROW
static void SplitDirect(const ActionVoxelGeometry *c, int ox, int oy, int width, int height,
                        size_t output_pitch, const uint32_t *low, size_t low_pitch,
                        uint32_t *rear, uint32_t *foreground) {
  const int map_width=c->width, map_height=c->height;
  const int64_t map_x=(int64_t)ox-c->origin_x;
  const int64_t begin=-map_x, end=(int64_t)map_width-map_x;
  const int first=begin<0 ? 0 : begin>width ? width : (int)begin;
  const int last=end<0 ? 0 : end>width ? width : (int)end;
  for(int y=0;y<height;++y) {
    const int64_t map_y=(int64_t)oy-c->origin_y+y;
    const uint32_t *source_row=low ? low+(size_t)y*low_pitch : NULL;
    uint32_t *rear_row=rear+(size_t)y*output_pitch;
    uint32_t *front_row=foreground+(size_t)y*output_pitch;
    const bool inside=map_y>=0 && map_y<map_height;
    const int x0=inside ? first : 0, x1=inside ? last : 0;
    for(int x=0;x<x0;++x) {
      const uint32_t source=source_row ? source_row[x] : 0;
      rear_row[x]=source;
      front_row[x]=0;
    }
    if(x0<x1) {
      const unsigned char *mask=c->front+(size_t)map_y*map_width+(size_t)(map_x+x0);
      for(int x=x0;x<x1;++x) {
        const uint32_t source=source_row ? source_row[x] : 0;
        const VoxelSplitKeep keep=kDirectSplitKeep[mask[x-x0]];
        rear_row[x]=source&keep.rear;
        front_row[x]=source&keep.front;
      }
    }
    for(int x=x1;x<width;++x) {
      const uint32_t source=source_row ? source_row[x] : 0;
      rear_row[x]=source;
      front_row[x]=0;
    }
  }
}
bool ActionVoxel_Split(const ActionVoxelGeometry *c, int ox, int oy, int width, int h,
                        size_t output_pitch, const ActionRoomSceneFrameState *frame,
                        const uint32_t *low, size_t low_pitch,
                        uint32_t *rear, uint32_t *foreground) {
  if (!c || !c->ready || !frame || !rear || !foreground || width <= 0 || h <= 0 ||
      output_pitch < (size_t)width || (low && low_pitch < (size_t)width)) return false;
  uint32_t backing[512];
  const bool castle=c->profile==kActionVoxelProfile_Castle;
  if (castle) BuildBacking(c, frame, backing);
  if(!castle && !c->branch_pixels && !c->trunk_cells && !c->column_cells) {
    SplitDirect(c,ox,oy,width,h,output_pitch,low,low_pitch,rear,foreground);
  } else for (int y = 0; y < h; ++y) {
    const int wy = oy + y, my = wy - c->origin_y;
    const unsigned char *mask = my >= 0 && my < c->height ? c->front + my * c->width : NULL;
    const uint32_t *row = backing + ((unsigned)wy & 31) * 16;
    for (int x = 0; x < width; ++x) {
      const size_t at = (size_t)y * output_pitch + x;
      const uint32_t source = low ? low[(size_t)y * low_pitch + x] : 0;
      const int wx = ox + x, mx = wx - c->origin_x;
      const unsigned material = mask && mx >= 0 && mx < c->width ? mask[mx] : 0;
      const unsigned kind = material & kVoxelKindMask;
      const size_t world_at = mask && mx >= 0 && mx < c->width ? (size_t)my * c->width + mx : 0;
      const bool branch = material && c->branch_pixels &&
          (c->branch_pixels[world_at >> 3] & (1u << (world_at & 7)));
      /* Owned curved root bark already reaches the rear vegetation plane.
       * A second bark copy would coincide there; keep only the independently
       * classified rear plant pixels from the mixed native root artwork. */
      const bool curved_bark=kind==kVoxelForeground && (material&kVoxelRearCopy) && c->trunk_cells &&
          c->trunk_cells[(size_t)(my/8)*(c->width/8)+mx/8];
      const unsigned column=kind==kVoxelSolid && c->column_cells
          ? c->column_cells[(size_t)(my/8)*(c->width/8)+mx/8] : 0;
      VoxelTrunkCurve section={0};
      if(column && column<=c->column_curve_count)
        ActionVoxel_ColumnSection(&c->column_curves[column-1],wx+.5f,&section);
      const bool rear_column=column && column<=c->column_curve_count && wy>=section.base_y &&
          c->profile!=kActionVoxelProfile_NorthwallSnow;
      /* Native high priority is an independent source, not a material hint.
       * Never merge it into either low-priority image or condition it on the
       * depth classifier. The ordinary high slot stays above OBJ2/below OBJ3. */
      /* Reviewed flat parapet art is the original 2D rear-battlement hint. The voxel
       * enclosure supplies an actual rear row, so expose the sky here. */
      rear[at] = branch || curved_bark ? 0 : !castle ? (!(material & kVoxelRearCopy) &&
                           (kind==kVoxelThin || kind==kVoxelFoliage || kind==kVoxelForeground) ? 0 : source)
                 : kind == kVoxelFlatParapet ? 0
                 : kind == kVoxelSolid     ? row[(unsigned)wx & 15]
                 : kind == kVoxelThin || kind == kVoxelGate || kind == kVoxelFoliage
                     ? 0xff000000u
                     : source;
      foreground[at] = kind && kind != kVoxelFlatParapet && kind != kVoxelRearOnly ? source : 0;
      /* A fully recessed shaft's native face already covers this rear pixel.
       * Keep that face in its native priority slot and remove only the opaque
       * backing underneath it. Two coplanar copies otherwise fight in depth;
       * genuinely transparent source pixels still retain their backing. */
      if(rear_column && (source>>24)==255)rear[at]=0;
    }
  }
  /* Moving a native solid owns its copied rear artwork too. Clear only that
   * semantic copy before independent world backing repairs are applied. */
  const int transformed_x0=(int)floorf(ox/16.f),transformed_y0=(int)floorf(oy/16.f);
  const int transformed_x1=(int)ceilf((ox+width)/16.f),transformed_y1=(int)ceilf((oy+h)/16.f);
  for(int cy=transformed_y0;c->transformed_rear_count && cy<transformed_y1;++cy)
    for(int cx=transformed_x0;cx<transformed_x1;++cx) {
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(c,cx,cy);
      if(!a || !ActionVoxel_TransformValid(&a->transform))continue;
      const int x0=a->x-ox,y0=a->y-oy;
      for(int y=y0<0?-y0:0;y<16 && y0+y<h;++y)
        for(int x=x0<0?-x0:0;x<16 && x0+x<width;++x)
          if(a->native_rear_rows[y]&(1u<<(15-x)))rear[(size_t)(y0+y)*output_pitch+x0+x]=0;
    }
  /* Sparse, world-anchored fills are compiled from nearby compatible native
   * artwork. They stay valid when the donor is outside the capture rectangle. */
  const unsigned subtract = BackingSubtract(frame);
  for (unsigned i = 0; i < c->rear_fill_count; ++i) {
    const VoxelRearFill *fill = &c->rear_fills[i];
    const int x0 = fill->x - ox, y0 = fill->y - oy;
    if (x0 >= width || y0 >= h || x0 + 16 <= 0 || y0 + 16 <= 0) continue;
    for (int y = y0 < 0 ? -y0 : 0; y < 16 && y0 + y < h; ++y)
      for (int x = x0 < 0 ? -x0 : 0; x < 16 && x0 + x < width; ++x) {
        const unsigned color = fill->colors[y * 16 + x];
        if(color & 0x8000) {
          const size_t at=(size_t)(y0+y)*output_pitch+x0+x;
          const uint32_t replacement=BackingColor(color,subtract,frame->brightness);
          if(!fill->preserve_existing || !(rear[at]>>24))rear[at]=replacement;
          if(fill->replace_front)foreground[at]=replacement;
        }
      }
  }
  const int authored_x0=(int)floorf(ox/16.f),authored_y0=(int)floorf(oy/16.f);
  const int authored_x1=(int)ceilf((ox+width)/16.f),authored_y1=(int)ceilf((oy+h)/16.f);
  for(int cy=authored_y0;c->authored_count && cy<authored_y1;++cy)
    for(int cx=authored_x0;cx<authored_x1;++cx) {
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(c,cx,cy);
    if(!a || a->native_front_hidden || (a->native_transform_requested && !a->native_transform))continue;
    const int x0=a->x-ox,y0=a->y-oy;
    if(x0>=width || y0>=h || x0+16<=0 || y0+16<=0)continue;
    for(int y=y0<0?-y0:0;y<16 && y0+y<h;++y)
      for(int x=x0<0?-x0:0;x<16 && x0+x<width;++x) {
        const size_t at=(size_t)(y0+y)*output_pitch+x0+x;
        if(!(a->native_rear_rows[y]&(1u<<(15-x))))rear[at]=ActionVoxel_AuthoredColor(a->pixels[3][y*16+x],frame);
        if(a->surface)foreground[at]=0;
      }
  }
  return true;
}
const uint16_t *ActionVoxel_EdgesForPass(const ActionVoxelGeometry *c, VoxelEdgePass pass,
                                             bool high, unsigned *count) {
  const unsigned list = pass == kVoxelEdgeRearTrim         ? 2
                        : pass == kVoxelEdgeHighDeckBehind ? 3
                        : high                              ? 1
                                                            : 0;
  *count = c ? c->pass_counts[list] : 0;
  return c ? c->pass_edges[list] : NULL;
}
