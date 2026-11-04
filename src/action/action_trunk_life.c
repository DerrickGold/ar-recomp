#include "action_trunk_life.h"
#include <stdlib.h>
#include <string.h>

void ActionTrunk_Capture(ActionTrunkObserver *state,ActionSceneEffectFrame *frame,
    const ActionTrunkAssets *assets,DioramaTrunkSettings settings,unsigned clock,bool particles) {
  frame->trunk=settings;
  memset(frame->trunk_bark,0,sizeof(frame->trunk_bark));
  if(!assets||!settings.enabled) {memset(state,0,sizeof(*state));return;}
  const unsigned delta=(uint16_t)(clock-state->clock);
  const bool reset=!state->valid||state->room!=assets->room||state->revision!=assets->revision||delta>4;
  if(reset){state->count=0;memset(state->events,0,sizeof(state->events));}
  state->room=assets->room;state->revision=assets->revision;state->clock=clock;state->valid=true;
  if(!delta&&!reset){memcpy(frame->trunk_bark,state->events,sizeof(frame->trunk_bark));return;}
  ActionTrunkLandingTrack next[kActionEffectActorMax];unsigned count=0;
  for(unsigned i=0;i<frame->actor_count&&count<kActionEffectActorMax;++i) {
    const ActionEffectActor *a=&frame->actors[i];if(!a->visible||!a->body_radius)continue;
    ActionTrunkLandingTrack n={a->generation,a->address,a->x,a->y,a->feet_y,0,0};
    for(unsigned j=0;j<state->count&&!reset;++j) {
      const ActionTrunkLandingTrack *old=&state->tracks[j];
      if(old->generation!=n.generation||old->address!=n.address)continue;
      const int dy=n.y-old->y,dx=n.x-old->x;
      if(abs(dx)>(int)delta*16||abs(dy)>(int)delta*24||abs((n.feet-old->feet)-dy)>2)break;
      n.fall=dy<-2?0:old->fall+(dy>0?dy:0);n.impact=dy<-2?0:old->impact;
      if(dy>(int)(n.impact*delta))n.impact=dy/(int)delta;
      const int bottom=ActionTrunk_BranchBottom(assets,n.x,n.feet);
      if(bottom>=0&&a->vy==0) {
        if(settings.bark>0&&particles&&n.fall>=8&&n.impact>=2) {
          unsigned free_slot=8;bool depleted=false;
          for(unsigned k=0;k<8;++k) {
            const ActionTrunkBarkEvent *ev=&state->events[k];const unsigned age=(uint16_t)(clock-ev->born);
            if(!ev->valid||age>300)free_slot=k;
            else if(fabsf(ev->x-n.x)<28&&fabsf(ev->y-bottom)<16)depleted=true;
          }
          if(!depleted&&free_slot<8) {
            state->events[free_slot]=(ActionTrunkBarkEvent){n.x,bottom+1,clock,
                a->generation^(clock*71u),true};
          }
        }
        n.fall=n.impact=0;
      }
      break;
    }
    next[count++]=n;
  }
  memcpy(state->tracks,next,count*sizeof(*next));state->count=count;
  memcpy(frame->trunk_bark,state->events,sizeof(frame->trunk_bark));
}
