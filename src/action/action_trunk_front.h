#ifndef AR_ACTION_TRUNK_FRONT_H
#define AR_ACTION_TRUNK_FRONT_H
#include "action_trunk.h"
#include <string.h>

/* Build-time subdivision metadata. Pure mask processing also lets renderer
 * fixtures publish synthetic assets without linking the ROM decoder. */
static inline void ActionTrunk_PrepareSideFronts(ActionTrunkAssets *a) {
  if(!a)return;
  memset(a->side_front_cells,0,sizeof(a->side_front_cells));
  if(!a->side_clearance||a->side_width>kActionTrunkSideWidth||a->height>kActionTrunkMaxHeight)return;
  const unsigned pitch=kActionTrunkSideWidth/8;
  for(unsigned cy=0;cy<a->height/8;++cy)for(unsigned cx=0;cx<a->side_width/8;++cx) {
    bool body=false;
    for(unsigned y=cy*8;y<cy*8+8;++y)
      body|=!!(a->side_masks[kActionTrunkSideSolid][y*kActionTrunkSideMaskPitch+cx]&
          ~a->side_masks[kActionTrunkSidePriority][y*kActionTrunkSideMaskPitch+cx]);
    if(!body)continue;
    const uint8_t *g=a->side_clearance_grid+cy*(pitch+1)+cx;
    const bool varying=g[0]!=g[1]||g[0]!=g[pitch+1]||g[0]!=g[pitch+2];
    if(!varying&&!g[0])continue;
    bool pixels=varying||a->side_detail_cells[cy*pitch+cx];
    for(int y=(int)cy*8-7;!pixels&&y<(int)cy*8+8;++y)
      if(y>=0&&(unsigned)y<a->height)pixels=a->side_contacts[y*kActionTrunkSideMaskPitch+cx]!=0;
    a->side_front_cells[cy*pitch+cx]=pixels?kActionTrunkFrontPixels:kActionTrunkFrontRecessed;
  }
}
#endif
