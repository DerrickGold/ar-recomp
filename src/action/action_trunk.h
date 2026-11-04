#ifndef AR_ACTION_TRUNK_H
#define AR_ACTION_TRUNK_H
#include "action_room_scene.h"
#include "diorama/diorama_trunk_settings.h"

struct DioramaRoomOverride;
enum { kActionTrunkLeft=320, kActionTrunkWidth=384,
  kActionTrunkMaskPitch=48, kActionTrunkMaxHeight=2048,
  kActionTrunkWallWidth=320, kActionTrunkWallHeight=64 };

/* Built from the loaded ROM and selected terrain, never compiled artwork.
 * Immutable after publication; the owner outlives all frames borrowing it. */
typedef struct ActionTrunkAssets {
  unsigned room, height;
  uint32_t revision;
  uint8_t masks[3][kActionTrunkMaxHeight*kActionTrunkMaskPitch]; /* rim, branch, removed backing */
  uint32_t wall[kActionTrunkWallWidth*kActionTrunkWallHeight];
  uint32_t bark[32*32];
} ActionTrunkAssets;

bool ActionTrunk_Build(ActionTrunkAssets *, const ActionRoomScene *,
                       const struct DioramaRoomOverride *);
static inline const uint8_t *ActionTrunk_MaskRow(const ActionTrunkAssets *a,int y,unsigned kind) {
  return a&&kind<3&&y>=0&&(unsigned)y<a->height?a->masks[kind]+y*48:NULL;
}
static inline bool ActionTrunk_Solid(const ActionTrunkAssets *a,int x,int y,unsigned kind) {
  const uint8_t *row=ActionTrunk_MaskRow(a,y,kind);
  return row&&x>=320&&x<704&&(row[(x-320)/8]&(1u<<(x&7)));
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
