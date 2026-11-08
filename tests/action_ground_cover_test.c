#include "action/action_ground_cover.h"
#include "deterministic_hash.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef ACTION_GROUND_COVER_LEGACY_PARITY
#include "action/action_effect_render.h"
#endif

static void Near(float a,float b) { assert(fabsf(a-b)<.0001f); }
static ActionEffectInstance Example(void) {
  /* Same native path/config as the established GroundSnowCover fixture. */
  return (ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,.generation=77,
      .flags=kActionEffectFlag_Visible,.world_x=128,.world_y=200,
      .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
      .particle_lifetime=240,.pulse_ticks=180,
      .tuning={.active=1,.color=0xd8e6f6,.intensity=2,.reach=1},
      .particle_style={.size_min=.5f,.size_max=1,.color_end=0xfff4df,.seed=621,.spread=1},
      .field_style={.strands=4,.placement=2,.ground_depth=4,.ground_width=8,
          .point_count=4,.points={{-128,0},{0,0},{32,32},{128,32}}},
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-128,-8,128,40}}};
}
static void Preparation(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank b,changed;
  assert(ActionGroundCover_Prepare(&e,&b)&&b.point_count==9);
  const float xs[]={0,32,64,96,128,160,184,208,256};
  for(unsigned i=0;i<9;++i)Near(b.points[i].x,xs[i]);
  for(unsigned i=0;i<5;++i)Near(b.points[i].y,200);
  for(unsigned i=5;i<9;++i)Near(b.points[i].y,232);
  assert(b.content_key);
  e.pulse_ticks=999;e.age_ticks=23;e.velocity_x=8;e.generation=234;
  assert(ActionGroundCover_Prepare(&e,&changed));
  assert(!memcmp(&b,&changed,sizeof(b))); /* No camera/clock/history key. */
  e.particle_style.color_end=0xffffff;
  assert(ActionGroundCover_Prepare(&e,&changed)&&changed.content_key!=b.content_key);
  assert(ActionGroundCover_SameSupport(&b,&changed));
  e.particle_style.seed++;
  assert(ActionGroundCover_Prepare(&e,&changed)&&changed.content_key!=b.content_key);
  assert(ActionGroundCover_SameSupport(&b,&changed));
  e.field_style.points[2].y++;
  assert(ActionGroundCover_Prepare(&e,&changed)&&!ActionGroundCover_SameSupport(&b,&changed));
  e=Example();e.world_x=-128;e.world_y=-200;
  assert(ActionGroundCover_Prepare(&e,&changed));
  for(unsigned i=0;i<b.point_count;++i) {
    Near(changed.points[i].x,b.points[i].x-256);Near(changed.points[i].y,b.points[i].y-400);
    Near(changed.height_noise[i],b.height_noise[i]);
  }
  for(unsigned bad=0;bad<8;++bad) {
    e=Example();
    if(bad==0)e.field_style.ground_width=0;
    if(bad==1)e.field_style.ground_depth=13;
    if(bad==2)e.field_style.points[1].x=e.field_style.points[0].x;
    if(bad==3)e.field_style.points[2].y=NAN;
    if(bad==4)e.projection_plane=kActionEffectProjectionPlane_Bg2;
    if(bad==5)e.render_layer=kActionEffectRenderLayer_Bg1Mist;
    if(bad==6)e.flags=0;
    if(bad==7)e.field_style.wake=NAN;
    memset(&changed,0xff,sizeof(changed));
    assert(!ActionGroundCover_Prepare(&e,&changed));
    const ActionGroundCoverBank empty={0};assert(!memcmp(&changed,&empty,sizeof(empty)));
  }
}
static void ContactAndPressure(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank b;
  assert(ActionGroundCover_Prepare(&e,&b));
  ActionGroundCoverSample quiet,pressed,back,core,side;
  assert(ActionGroundCover_Sample(&b,NULL,80,0,0,&quiet));
  const float a=DeterministicHash_Unit32(621^2*71),z=DeterministicHash_Unit32(621^3*71);
  Near(quiet.settled_height,4*(.82f+.18f*(a+(z-a)*.5f)));
  Near(quiet.height,quiet.settled_height);Near(quiet.native_floor_y,200);
  Near(quiet.alpha,.98f*.78f);Near(quiet.crest_mix,1);
  static ActionSceneEffectFrame frame;
  frame=(ActionSceneEffectFrame){.actor_count=3};
  frame.actors[0]=(ActionEffectActor){.player=1,.visible=1,.body_radius=8,.x=80,.feet_y=200};
  frame.actors[1]=(ActionEffectActor){.visible=1,.body_radius=9,.x=200,.feet_y=232};
  frame.actors[2]=(ActionEffectActor){.visible=1,.body_radius=8,.x=144,.feet_y=219};
  ActionGroundCoverPressure pressure;
  assert(ActionGroundCover_PreparePressure(&b,&frame,&pressure));
  Near(pressure.feet[0].x,80);Near(pressure.feet[1].x,200); /* Stronger enemy wins. */
  assert(ActionGroundCover_Sample(&b,&pressure,80,0,1,&pressed));
  Near(pressed.height,quiet.height*.65f);Near(pressed.compression,1);
  assert(ActionGroundCover_Sample(&b,&pressure,80,1,0,&back));
  Near(back.height,quiet.height);Near(back.alpha,0); /* No pressure outside actual actor band. */
  /* Dedicated core actor lies at the back; side-hall actor lies at the front.
   * The same supplied contact weight must act at either receiving endpoint. */
  assert(ActionGroundCover_Sample(&b,&pressure,80,1,1,&core));
  assert(ActionGroundCover_Sample(&b,&pressure,80,0,1,&side));
  Near(core.height,side.height);Near(core.height,pressed.height);
  assert(ActionGroundCover_Sample(&b,&pressure,80,.5f,1,&core));
  Near(core.height,pressed.height); /* Generic actor contact can lie inside the cap. */
  frame.actors[0].feet_y-=20;frame.actors[1].feet_y-=20;frame.actors[2].visible=0;
  assert(ActionGroundCover_PreparePressure(&b,&frame,&pressure));
  assert(ActionGroundCover_Sample(&b,&pressure,80,0,1,&pressed));
  Near(pressed.height,quiet.height); /* Jumping removes contact. */
  e.field_style.wake=1;assert(ActionGroundCover_Prepare(&e,&b));
  frame.actors[0].wake=(ActionActorWake){.x=80,.y=200,.age=0,.strength=.5f};
  assert(ActionGroundCover_PreparePressure(&b,&frame,&pressure));Near(pressure.feet[0].strength,.2f);
  frame.actors[0].wake.age=kActionActorWakeTicks;
  assert(ActionGroundCover_PreparePressure(&b,&frame,&pressure));Near(pressure.feet[0].strength,0);
  assert(ActionGroundCover_Sample(&b,NULL,144,.5f,0,&quiet));Near(quiet.native_floor_y,216);
  assert(ActionGroundCover_Sample(&b,NULL,0,.5f,0,&quiet));Near(quiet.height,0);Near(quiet.alpha,0);
  assert(!ActionGroundCover_Sample(&b,NULL,-1,.5f,0,&quiet));
  assert(!ActionGroundCover_Sample(&b,NULL,80,.5f,1.1f,&quiet));
  assert(!ActionGroundCover_Sample(&b,NULL,80,NAN,0,&quiet));
}
static void SupportAssociation(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank b;
  assert(ActionGroundCover_Prepare(&e,&b));
  ActionGroundCoverInterval spans[kActionGroundCoverMaxIntervals],reverse[kActionGroundCoverMaxIntervals];
  unsigned count,other;
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){0,200},
      (ActionGroundCoverPoint){128,200},1,spans,kActionGroundCoverMaxIntervals,&count));
  assert(count==4);Near(spans[0].t0,0);Near(spans[3].t1,1);
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){128,200},
      (ActionGroundCoverPoint){0,200},1,reverse,kActionGroundCoverMaxIntervals,&other));
  assert(other==count);
  for(unsigned i=0;i<count;++i){Near(reverse[i].t0,1-spans[count-i-1].t1);Near(reverse[i].t1,1-spans[count-i-1].t0);}
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){128,200},
      (ActionGroundCoverPoint){160,232},0,spans,kActionGroundCoverMaxIntervals,&count));
  assert(count==1);Near(spans[0].t0,0);Near(spans[0].t1,1);
  /* A flat nearby ledge cannot be extended across the rising bank. */
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){128,200},
      (ActionGroundCoverPoint){160,200},1,spans,kActionGroundCoverMaxIntervals,&count));
  assert(count==1);Near(spans[0].t1,1.f/32);
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){0,220},
      (ActionGroundCoverPoint){128,220},1,spans,kActionGroundCoverMaxIntervals,&count));
  assert(count==0); /* Valid empty, not unsupported. */
  const ActionGroundCoverInterval sentinel={99,101};spans[0]=sentinel;
  assert(!ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){0,200},
      (ActionGroundCoverPoint){128,200},1,spans,1,&count));
  assert(count==0&&!memcmp(spans,&sentinel,sizeof(sentinel))); /* No partial output on budget failure. */
  assert(!ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){0,200},
      (ActionGroundCoverPoint){128,200},2,spans,kActionGroundCoverMaxIntervals,&count));
  assert(!ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){0,200},
      (ActionGroundCoverPoint){0,201},1,spans,kActionGroundCoverMaxIntervals,&count));
  e.world_x-=256;e.world_y-=400;assert(ActionGroundCover_Prepare(&e,&b));
  assert(ActionGroundCover_ClipSupport(&b,(ActionGroundCoverPoint){-256,-200},
      (ActionGroundCoverPoint){-128,-200},1,spans,kActionGroundCoverMaxIntervals,&count));
  assert(count==4);Near(spans[0].t0,0);Near(spans[3].t1,1);
}
static void ContactSubdivisions(void) {
  ActionGroundCoverPressure p={.feet={{16,6,1},{48,6,.5f}}};
  float cuts[kActionGroundCoverMaxContactCuts],reverse[kActionGroundCoverMaxContactCuts];
  unsigned n,r;
  assert(ActionGroundCover_ContactCuts(&p,0,64,(ActionGroundCoverInterval){0,1},cuts,8,&n));
  assert(n==8);
  const float expected[]={0,10,16,22,42,48,54,64};
  for(unsigned i=0;i<n;++i)Near(cuts[i]*64,expected[i]);
  assert(ActionGroundCover_ContactCuts(&p,64,0,(ActionGroundCoverInterval){0,1},reverse,8,&r)&&r==n);
  for(unsigned i=0;i<n;++i)Near(reverse[i],1-cuts[n-i-1]);
  /* Two supported pieces separated by a pit remain disjoint even though
   * actor pressure crosses their neighbouring endpoints. */
  assert(ActionGroundCover_ContactCuts(&p,0,64,(ActionGroundCoverInterval){0,.3f},cuts,8,&n));
  for(unsigned i=0;i<n;++i)assert(cuts[i]<=.3f);
  assert(ActionGroundCover_ContactCuts(&p,0,64,(ActionGroundCoverInterval){.7f,1},cuts,8,&n));
  for(unsigned i=0;i<n;++i)assert(cuts[i]>=.7f);
  cuts[0]=91;
  assert(!ActionGroundCover_ContactCuts(&p,0,64,(ActionGroundCoverInterval){0,1},cuts,2,&n));
  assert(n==0&&cuts[0]==91);
  p.feet[1]=p.feet[0];
  assert(ActionGroundCover_ContactCuts(&p,0,64,(ActionGroundCoverInterval){0,1},cuts,8,&n)&&n==5);
  assert(ActionGroundCover_ContactCuts(NULL,0,64,(ActionGroundCoverInterval){0,1},cuts,8,&n)&&n==2);
}
static void PerBankClaims(void) {
  const ActionGroundCoverClaim claims[]={
      ActionGroundCover_ResolveClaim(false,true,true,2),
      ActionGroundCover_ResolveClaim(true,false,true,2),
      ActionGroundCover_ResolveClaim(true,true,true,0),
      ActionGroundCover_ResolveClaim(true,true,true,2),
      ActionGroundCover_ResolveClaim(true,true,false,2)};
  assert(claims[0]==kActionGroundCoverUnsupported&&claims[1]==kActionGroundCoverUnsupported);
  assert(claims[2]==kActionGroundCoverClaimedEmpty&&claims[3]==kActionGroundCoverClaimedGeometry);
  assert(claims[4]==kActionGroundCoverFailure);
  for(unsigned i=0;i<5;++i)assert(ActionGroundCover_SuppressesFlatTop(claims[i])==(i>=2));
}
#ifdef ACTION_GROUND_COVER_LEGACY_PARITY
static bool LegacyProject(void *unused,const ActionEffectInstance *e,float x,float y,ArRenderPointF *out) {
  (void)unused;*out=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;
}
/* Optional link against the actual unchanged action renderer: no copied
 * GroundCover/noise/falloff oracle. Inspect its retained native front/rear
 * vertices under still feet, jumps and aged wakes. */
static void LegacyParity(void) {
  static ActionSceneEffectFrame frame;
  static ActionSceneEffectRenderBatch batch;
  unsigned comparisons=0;
  for(unsigned state=0;state<5;++state) {
    frame=(ActionSceneEffectFrame){.authored_count=1,.actor_count=2};
    frame.authored[0]=Example();frame.authored[0].field_style.wake=1;
    frame.actors[0]=(ActionEffectActor){.player=1,.visible=1,.body_radius=8,.x=80,.feet_y=200};
    frame.actors[1]=(ActionEffectActor){.visible=1,.body_radius=10,.x=200,.feet_y=232};
    if(state==0)frame.actor_count=0;
    if(state>=2)for(unsigned i=0;i<2;++i)frame.actors[i].feet_y-=20;
    if(state>=3)for(unsigned i=0;i<2;++i)frame.actors[i].wake=(ActionActorWake){
        .x=frame.actors[i].x,.y=frame.actors[i].feet_y+20,.strength=.7f,
        .age=state==3?12:kActionActorWakeTicks};
    ActionGroundCoverBank bank;ActionGroundCoverPressure pressure;
    assert(ActionGroundCover_Prepare(&frame.authored[0],&bank));
    assert(ActionGroundCover_PreparePressure(&bank,&frame,&pressure));
    assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Mist,
        false,true,LegacyProject,NULL,NULL,&batch));
    assert(batch.vertex_count>0);
    unsigned front=0,rear=0;
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderVertex2D v=batch.vertices[i];ActionGroundCoverSample s;
      const bool crest=v.color.r==bank.crest.r&&v.color.g==bank.crest.g&&v.color.b==bank.crest.b;
      if(crest) {
        assert(ActionGroundCover_Sample(&bank,&pressure,v.position.x,0,1,&s));
        Near(v.position.y,s.native_floor_y-s.height);Near(v.color.a,s.alpha);++front;++comparisons;
      } else if(v.color.a==0&&v.position.x>bank.points[0].x&&
          v.position.x<bank.points[bank.point_count-1].x) {
        assert(ActionGroundCover_Sample(&bank,&pressure,v.position.x,1,0,&s));
        Near(v.position.y,s.native_floor_y-bank.painted_width-s.settled_height);++rear;++comparisons;
      }
    }
    assert(front&&rear);
  }
  printf("legacy GroundCover parity: %u direct retained vertex comparisons passed\n",comparisons);
}
#endif
int main(void) {
  Preparation();ContactAndPressure();SupportAssociation();ContactSubdivisions();PerBankClaims();
#ifdef ACTION_GROUND_COVER_LEGACY_PARITY
  LegacyParity();
#endif
  puts("action ground cover: preparation, support, pressure, per-bank claims passed");
  return 0;
}
