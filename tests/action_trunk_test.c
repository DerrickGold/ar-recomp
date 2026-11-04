#include "action/action_trunk_life.h"
#include "support/test_assert.h"
#include <string.h>
#include <stdio.h>

static ActionTrunkAssets assets;
static ActionSceneEffectFrame frame;
static ActionTrunkObserver observer;
static unsigned Events(void) {
  unsigned n=0;for(unsigned i=0;i<8;++i)n+=frame.trunk_bark[i].valid;return n;
}
static void Tick(unsigned clock,int x,int feet,int vy,bool particles) {
  frame.actor_count=1;
  frame.actors[0]=(ActionEffectActor){.generation=1,.address=0x800,.visible=1,
      .body_radius=5,.x=x,.y=feet-16,.feet_y=feet,.vy=vy};
  ActionTrunk_Capture(&observer,&frame,&assets,DioramaTrunk_Default(),clock,particles);
}
int main(void) {
  assets.room=6;assets.height=1024;assets.revision=1;
  for(int y=100;y<107;++y)for(int x=400;x<470;++x)
    assets.masks[1][y*48+(x-320)/8]|=1u<<(x&7);
  Tick(1,440,88,6,true);Tick(2,440,94,6,true);Tick(3,440,100,6,true);
  assert(Events()==0);Tick(4,440,100,0,true);assert(Events()==1);
  const ActionTrunkObserver paused=observer;
  const ActionSceneEffectFrame captured=frame;
  for(int i=0;i<20;++i)Tick(4,440,100,0,true);
  assert(!memcmp(&paused,&observer,sizeof(observer)));
  assert(!memcmp(captured.trunk_bark,frame.trunk_bark,sizeof(frame.trunk_bark)));
  Tick(5,440,88,-6,true);Tick(6,440,94,6,true);Tick(7,440,100,6,true);Tick(8,440,100,0,true);
  assert(Events()==1); /* The same patch remains in cooldown. */
  Tick(9,600,88,6,true);Tick(10,440,100,0,true);assert(Events()==1); /* Warp cannot land. */
  Tick(500,440,88,6,true);assert(Events()==0); /* Discontinuous seek resets history. */
  Tick(501,440,94,6,false);Tick(502,440,100,6,false);Tick(503,440,100,0,false);assert(!Events());
  Tick(504,440,88,-6,true);Tick(505,440,94,6,true);Tick(506,440,100,6,true);
  ++assets.revision;Tick(507,440,100,0,true);assert(!Events());
  memset(&observer,0,sizeof(observer));
  Tick(1,512,88,6,true);Tick(2,512,94,6,true);Tick(3,512,100,6,true);Tick(4,512,100,0,true);
  assert(!Events()); /* The central column never sheds platform flakes. */
  frame.authored_count=1;
  frame.authored[0]=(ActionEffectInstance){.kind=kActionEffect_AuthoredFan,.world_x=350,.world_y=100,
      .field_style={.opening=true},.tuning={.intensity=2}};
  assert(ActionTrunk_Opening(&frame,400,100));frame.trunk.enabled=false;
  assert(!ActionTrunk_Opening(&frame,400,100));
  puts("trunk landing snapshots: pause, cooldown, warp/seek, particles, terrain change and opening gates passed");
}
