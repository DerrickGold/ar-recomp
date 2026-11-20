#ifndef AR_ACTION_TRUNK_H
#define AR_ACTION_TRUNK_H
#include "action_room_scene.h"
#include "diorama/diorama_trunk_settings.h"

struct DioramaRoomOverride;
enum { kActionTrunkLeft=320, kActionTrunkWidth=384,
  kActionTrunkMaskPitch=48, kActionTrunkMaxHeight=2048,
  kActionTrunkWallWidth=320, kActionTrunkWallHeight=64,
  kActionTrunkSideWidth=1024, kActionTrunkSideMaskPitch=128 };
enum { kActionTrunkSideSolid, kActionTrunkSideRear,
  kActionTrunkSidePriority, kActionTrunkSideSnow, kActionTrunkSideSnowFace,
  kActionTrunkSideMaskCount };
enum { kActionTrunkFrontPixels=-1, kActionTrunkFrontNative, kActionTrunkFrontRecessed };

/* Built from the loaded ROM and selected terrain, never compiled artwork.
 * Immutable after publication; the owner outlives all frames borrowing it. */
typedef struct ActionTrunkAssets {
  unsigned room, height;
  uint32_t revision;
  uint8_t masks[3][kActionTrunkMaxHeight*kActionTrunkMaskPitch]; /* rim, branch, removed backing */
  uint32_t wall[kActionTrunkWallWidth*kActionTrunkWallHeight];
  uint32_t bark[32*32];
  /* Side passages have separate ownership; the established core masks,
   * lattice and bark retain their exact layout and world coordinates. */
  unsigned side_width;
  uint8_t side_masks[kActionTrunkSideMaskCount]
      [kActionTrunkMaxHeight*kActionTrunkSideMaskPitch];
  uint32_t side_snow[16*16];
  unsigned side_high_pixels;
  /* Native central collision stays at actor contact; the decorative shaft
   * behind it meets the backs of the branch platforms in Voxel mode. */
  bool column_geometry;
  uint8_t column_front[kActionTrunkMaxHeight/8];
  uint8_t column_caps[kActionTrunkMaxHeight*8];
  bool side_clearance;
  uint8_t side_contacts[kActionTrunkMaxHeight*kActionTrunkSideMaskPitch];
  uint8_t side_detail_cells[(kActionTrunkMaxHeight/8)*(kActionTrunkSideWidth/8)];
  /* Native eight-pixel grid vertices: clearance only beside opaque tunnel
   * openings, with a smooth return to full thickness away from their rim. */
  uint8_t side_clearance_grid[(kActionTrunkMaxHeight/8+1)*(kActionTrunkSideWidth/8+1)];
  int8_t side_front_cells[(kActionTrunkMaxHeight/8)*(kActionTrunkSideWidth/8)];
} ActionTrunkAssets;

static inline bool ActionTrunk_ColumnRecess(const ActionTrunkAssets *a,int x,int y) {
  return a&&a->column_geometry&&x>=480&&x<544&&y>=0&&(unsigned)y<a->height&&
      !(a->column_front[y/8]&(1u<<((x-480)/8)));
}

bool ActionTrunk_Build(ActionTrunkAssets *, const ActionRoomScene *,
                       const struct DioramaRoomOverride *);
/* Cached during room construction, before these immutable assets publish. */
static inline int ActionTrunk_SideFrontCell(const ActionTrunkAssets *a,int x,int y) {
  return a&&a->side_clearance&&x>=0&&(unsigned)x<a->side_width&&y>=0&&(unsigned)y<a->height
      ?a->side_front_cells[(y/8)*(kActionTrunkSideWidth/8)+x/8]:kActionTrunkFrontNative;
}
static inline const uint8_t *ActionTrunk_MaskRow(const ActionTrunkAssets *a,int y,unsigned kind) {
  return a&&kind<3&&y>=0&&(unsigned)y<a->height?a->masks[kind]+y*48:NULL;
}
static inline bool ActionTrunk_Solid(const ActionTrunkAssets *a,int x,int y,unsigned kind) {
  const uint8_t *row=ActionTrunk_MaskRow(a,y,kind);
  return row&&x>=320&&x<704&&(row[(x-320)/8]&(1u<<(x&7)));
}
static inline const uint8_t *ActionTrunk_SideMaskRow(const ActionTrunkAssets *a,
    int y,unsigned kind) {
  return a&&kind<kActionTrunkSideMaskCount&&y>=0&&(unsigned)y<a->height
      ?a->side_masks[kind]+y*kActionTrunkSideMaskPitch:NULL;
}
static inline bool ActionTrunk_SideSolid(const ActionTrunkAssets *a,
    int x,int y,unsigned kind) {
  const uint8_t *row=ActionTrunk_SideMaskRow(a,y,kind);
  return row&&x>=0&&(unsigned)x<a->side_width&&
      (x<kActionTrunkLeft||x>=kActionTrunkLeft+kActionTrunkWidth)&&
      (row[x/8]&(1u<<(x&7)));
}
static inline bool ActionTrunk_SideContact(const ActionTrunkAssets *a,int x,int y) {
  return a&&a->side_clearance&&x>=0&&(unsigned)x<a->side_width&&y>=0&&(unsigned)y<a->height&&
      (a->side_contacts[y*kActionTrunkSideMaskPitch+x/8]&(1u<<(x&7)));
}
/* A one-pixel scallop in an icy crown is source trim, not a crack across the
 * whole standing surface. Close only the physical top, without changing RGBA. */
static inline unsigned ActionTrunk_SideShellByte(const ActionTrunkAssets *a,int y,int byte) {
  const uint8_t *solid=ActionTrunk_SideMaskRow(a,y,kActionTrunkSideSolid);
  if(!solid||byte<0||byte>=kActionTrunkSideMaskPitch)return 0;
  unsigned filled=solid[byte];
  if(a->side_clearance&&y+1<(int)a->height) {
    const uint8_t *below=ActionTrunk_SideMaskRow(a,y+1,kActionTrunkSideSnow);
    filled|=a->side_contacts[y*kActionTrunkSideMaskPitch+byte]&below[byte];
  }
  return filled;
}
static inline bool ActionTrunk_SideCrown(const ActionTrunkAssets *a,int x,int y) {
  return a&&x>=0&&(unsigned)x<a->side_width&&y>=0&&(unsigned)y<a->height&&
      !!(ActionTrunk_SideShellByte(a,y,x/8)&(1u<<(x&7)));
}
static inline float ActionTrunk_SideClearance(const ActionTrunkAssets *a,float x,float y) {
  if(!a||!a->side_clearance||x<0||y<0||x>a->side_width||y>a->height)return 0;
  const unsigned pitch=kActionTrunkSideWidth/8+1;
  const int cx=(int)fminf(floorf(x/8),a->side_width/8-1),cy=(int)fminf(floorf(y/8),a->height/8-1);
  if(cx<0||cy<0)return 0;
  float tx=x/8-cx,ty=y/8-cy;tx=tx*tx*(3-2*tx);ty=ty*ty*(3-2*ty);
  const uint8_t *g=a->side_clearance_grid+cy*pitch+cx;
  return ((g[0]*(1-tx)+g[1]*tx)*(1-ty)+(g[pitch]*(1-tx)+g[pitch+1]*tx)*ty)/255.f;
}
/* A four-pixel standing lip returns over four pixels into the recessed body. Owner coordinates
 * keep a real depth step where a one-way shelf meets an overhead solid bank. */
static inline float ActionTrunk_SideFrontFraction(const ActionTrunkAssets *a,int x,int y,float vertex_x,float vertex_y) {
  if(!a||!a->side_clearance||!ActionTrunk_SideSolid(a,x,y,kActionTrunkSideSolid)||
      ActionTrunk_SideSolid(a,x,y,kActionTrunkSidePriority))return 0;
  for(int d=0;d<8;++d)if(ActionTrunk_SideContact(a,x,y-d)) {
    const float t=fminf(1,fmaxf(0,(vertex_y-(y-d)-4)/4));
    return .75f*t*t*(3-2*t)*ActionTrunk_SideClearance(a,vertex_x,vertex_y);
  }
  return .75f*ActionTrunk_SideClearance(a,vertex_x,vertex_y);
}
static inline int ActionTrunk_BranchBottom(const ActionTrunkAssets *a,int x,int feet) {
  if(x<376||x>648||(x>484&&x<540))return -1;
  for(int y=feet-2;y<=feet+3;++y)
    if(ActionTrunk_Solid(a,x,y,1)&&!ActionTrunk_Solid(a,x,y-3,1))
      for(int b=y;b<y+24;++b)
        if(!ActionTrunk_Solid(a,x,b,1)&&!ActionTrunk_Solid(a,x,b+1,1)&&!ActionTrunk_Solid(a,x,b+2,1))return b;
  return -1;
}
void ActionTrunk_Split(const ActionTrunkAssets *, int origin_x, int origin_y,
                       int width, int height, int pitch,
                       const uint32_t *const input[3], const size_t input_pitch[3],
                       uint32_t *const output[3], unsigned brightness);
#endif
