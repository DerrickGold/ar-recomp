#ifndef AR_DIORAMA_JUNGLE_TERMINAL_H
#define AR_DIORAMA_JUNGLE_TERMINAL_H
#include "action/action_voxel.h"
#include <math.h>
typedef struct JungleTerminalRect {float x0,y0,x1,y1;} JungleTerminalRect;
/* Sampling identity, not RGB equality, owns a corner. A palette edit must not
 * join two unrelated source intervals merely because their colors coincide. */
static inline bool JungleTerminalSame(const VoxelJungleTerminalRun *a,const VoxelJungleTerminalRun *b) {
  return a->palette_index==b->palette_index&&a->color==b->color&&a->high_priority==b->high_priority;
}
static inline const VoxelJungleTerminalRun *JungleTerminalAt(const VoxelJungleTerminal *s,
    unsigned edge,int at) {
  for(unsigned i=0;i<s->count;++i)if(s->runs[i].edge==edge&&
      at>=s->runs[i].start&&at<s->runs[i].end)return s->runs+i;
  return NULL;
}
/* At most64 native pixels beyond the original finite world. The one-pixel
 * overlap is original qualified ink, never a fill of an internal opening. */
static inline bool JungleTerminalResolve(const VoxelJungleTerminal *s,unsigned index,
    JungleTerminalRect view,JungleTerminalRect *out) {
  if(!s||!out||s->count>kVoxelMaxJungleTerminalRuns||index>=s->count||
      s->left>=s->right||s->top>=s->bottom||
      !isfinite(view.x0)||!isfinite(view.x1)||!isfinite(view.y0)||!isfinite(view.y1)||
      view.x0>=view.x1||view.y0>=view.y1)return false;
  const VoxelJungleTerminalRun *r=s->runs+index;
  if(r->start>=r->end||!(r->color&0x8000)||r->palette_index>=128||
      (r->edge==2?(r->start<s->left||r->end>s->right):(r->start<s->top||r->end>s->bottom)))return false;
  JungleTerminalRect q={0};
  if(r->edge==2) {
    if(view.y1<=s->bottom)return false;
    q=(JungleTerminalRect){r->start,s->bottom-1,r->end,s->bottom+64};
    const VoxelJungleTerminalRun *left=JungleTerminalAt(s,3,s->bottom-1);
    const VoxelJungleTerminalRun *right=JungleTerminalAt(s,1,s->bottom-1);
    if(r->start==s->left&&left&&JungleTerminalSame(r,left))q.x0=s->left-64;
    if(r->end==s->right&&right&&JungleTerminalSame(r,right))q.x1=s->right+64;
  } else if(r->edge==1||r->edge==3) {
    const bool right=r->edge==1;const int boundary=right?s->right:s->left;
    if(right?view.x1<=boundary:view.x0>=boundary)return false;
    q=(JungleTerminalRect){right?boundary-1:boundary-64,r->start,
      right?boundary+64:boundary+1,r->end};
    const VoxelJungleTerminalRun *bottom=JungleTerminalAt(s,2,right?s->right-1:s->left);
    if(view.y1>s->bottom&&bottom&&JungleTerminalSame(r,bottom))q.y1=fminf(q.y1,s->bottom-1);
  } else return false;
  q.x0=fmaxf(q.x0,view.x0);q.x1=fminf(q.x1,view.x1);
  q.y0=fmaxf(q.y0,view.y0);q.y1=fminf(q.y1,view.y1);
  if(q.x0>=q.x1||q.y0>=q.y1)return false;
  *out=q;return true;
}
#endif
