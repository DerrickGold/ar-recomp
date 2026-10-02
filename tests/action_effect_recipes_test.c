#include "action/action_effect_recipes.h"
#include "action/action_floor_support.h"
#include <math.h>
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static ActionEffectRecipes table, before;
static bool Parse(const char *text) { unsigned line; return ActionEffectRecipes_Parse(&table,text,strlen(text),&line); }
static void TestMoteParameters(void) {
  const char *header="[effects]\nversion=1\n[emitter:01:02:0:motes:123]\n";
  char text[512];
  snprintf(text,sizeof(text),"%sheight=120\ncolor=abc123\n",header);
  assert(Parse(text));
  ActionSceneEffectFrame frame={0};
  ActionEffectRecipes_Apply(&table,1,2,0,37,NULL,&frame);
  const ActionEffectParticleStyle *style=&frame.authored[0].particle_style;
  assert(style->active && style->size_min==.35f && style->size_max==.75f);
  assert(style->travel_y==-120 && style->travel_x==0 && style->spread==1 && style->wander==2);
  assert(style->seed==0x123 && style->color_end==0xabc123);
  snprintf(text,sizeof(text),"%ssize-min=1.25\nsize-max=3.5\ntravel-x=-512\ntravel-y=0\n"
      "wander=32\nspread=.25\nseed=4294967295\ncolor-end=00ABfF\n",header);
  assert(Parse(text));
  ActionEffectRecipes_Apply(&table,1,2,0,37,NULL,&frame);
  style=&frame.authored[0].particle_style;
  assert(style->size_min==1.25f && style->size_max==3.5f && style->travel_x==-512 && style->travel_y==0);
  assert(style->wander==32 && style->spread==.25f && style->seed==UINT32_MAX && style->color_end==0xabff);
  assert(frame.authored[0].generation==0x123); /* Pattern seed is not source identity. */
  before=table;
  const char *invalid[]={"size-min=2\nsize-max=1\n","size-min=.09\n","size-max=4.1\n",
      "travel-x=nan\n","travel-y=513\n","wander=-1\n","spread=1.01\n",
      "seed=4294967296\n","seed=-1\n","seed=1.5\n","seed=\n",
      "seed=0\nseed=0\n","color-end=abcdef0\n","color-end=xyzxyz\n"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
    snprintf(text,sizeof(text),"%s%s",header,invalid[i]);
    assert(!Parse(text));assert(!memcmp(&table,&before,sizeof(table)));
  }
  assert(!Parse("[effects]\nversion=1\n[emitter:01:02:0:free-mist:1]\nspread=1\n"));
  assert(!Parse("[effects]\nversion=1\n[source:01:02:0:temple-dust:1]\nseed=1\n"));
  snprintf(text,sizeof(text),"%ssize-min=.1\nsize-max=4\ntravel-x=512\ntravel-y=-512\n"
      "wander=0\nspread=0\nseed=0\ncolor-end=000000\n",header);
  assert(Parse(text));
}
static void TestFloorSupport(void) {
  uint8_t map[3*256]={0};
  ActionEnvironmentScene scene={.group=1,.room=2,
      .maps={{map,sizeof(map),768,256,3}},.collision={[1]=15,[2]=0,[0x54]=3,[4]=5}};
  /* Independent page-major fixtures: transparent spikes/details above a
   * stepped floor, a one-way temple capital, and an unsupported partial tile. */
  map[8*16]=1;map[9*16+1]=1;map[7*16+1]=2;
  map[8*16+2]=0x54;map[8*16+3]=4;
  assert(ActionFloorSupport_Cell(&scene,0,128)==31);
  assert(ActionFloorSupport_Cell(&scene,16,112)==0);
  assert(ActionFloorSupport_Cell(&scene,32,128)==19);
  assert(ActionFloorSupport_Cell(&scene,48,128)==5);
  assert(ActionFloorSupport_Cell(&scene,-1,128)==0);
  ActionEffectFloorField field;
  const ActionEffectLocalRect area={0,64,80,160};
  ActionFloorSupport_Resolve(&scene,&area,26,&field);
  assert(field.count==3);
  assert(field.spans[0].y==128 && field.spans[1].y==144 && field.spans[2].y==128);
  assert(field.spans[1].x0==16 && field.spans[1].x1==32);
  scene.group=2;
  ActionFloorSupport_Resolve(&scene,&area,26,&field);assert(field.count==2);
  scene.group=1;
  map[6*16]=1; /* Exposed floor at 96 is excluded by the painted area's top. */
  const ActionEffectLocalRect low={0,100,16,160};
  ActionFloorSupport_Resolve(&scene,&low,64,&field);
  assert(field.count==1 && field.spans[0].y==128 && field.spans[0].height==16);
  ActionFloorSupport_Resolve(&scene,&low,NAN,&field);assert(!field.count);
  /* Maximum unaligned area, every column at a different height. */
  memset(map,0,sizeof(map));
  for(unsigned x=0;x<33;++x)map[(x/16)*256+(8+(x&1))*16+x%16]=1;
  const ActionEffectLocalRect wide={1,64,513,160};
  ActionFloorSupport_Resolve(&scene,&wide,26,&field);
  assert(field.count==33 && field.spans[0].x0==1 && field.spans[32].x1==513);
  assert(Parse("[effects]\nversion=1\n[emitter:01:02:0:ground-mist:1]\nx=257\ny=112\nwidth=512\nheight=96\nmist-height=26\n"));
  ActionSceneEffectFrame frame={0};
  ActionEffectRecipes_Apply(&table,1,2,0,37,&scene,&frame);
  assert(frame.authored_count==1 && frame.authored_floor[0].count==33);
  assert(frame.authored[0].render_layer==kActionEffectRenderLayer_Bg1Mist);
  memset(map,0,sizeof(map));
  ActionEffectRecipes_Apply(&table,1,2,0,37,&scene,&frame);
  assert(frame.authored_count==1 && !frame.authored_floor[0].count && !frame.authored[0].flags);
  char budget[1024]="[effects]\nversion=1\n";
  for(unsigned i=0;i<3;++i){char line[128];snprintf(line,sizeof(line),"[emitter:01:02:0:ground-mist:%x]\nwidth=512\n",i);strcat(budget,line);}
  before=table;assert(!Parse(budget));assert(!memcmp(&table,&before,sizeof(table)));
  assert(!Parse("[effects]\nversion=1\n[emitter:01:02:0:ground-mist:1]\nmist-height=65\n"));
  assert(!Parse("[effects]\nversion=1\n[emitter:01:02:0:free-mist:1]\nmist-height=26\n"));
}
static void TestFieldsAndReceivers(void) {
  assert(Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-area:1]\nx=8000\ny=8000\nwidth=16000\nheight=16000\nparticles=8\npattern=snow\n"));
  before=table;
  const char *bad[]={"[emitter:01:02:0:particle-area:1]\nparticles=17\n", "[emitter:01:02:0:exposure:1]\nintensity=1.01\n",
    "[emitter:01:02:0:light-fan:1]\nstrands=33\n", "[emitter:01:02:0:light-fan:1]\nangle=nan\n",
    "[emitter:01:02:0:wet-contour:1]\npoints=-32,0 500,0\n", "[emitter:01:02:0:wet-contour:1]\npoints=0,0 0,0\n",
    "[emitter:01:02:0:wet-contour:1]\npoints=0,0\n", "[source:01:02:0:temple-dust:0]\nlight-player=1\n",
    "[emitter:01:02:0:soft-light:1]\nlight-player=2\n", "[emitter:01:02:0:soft-light:1]\nlight-player=1\nlight-player=1\n"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){char text[512];snprintf(text,sizeof(text),"[effects]\nversion=1\n%s",bad[i]);assert(!Parse(text));assert(!memcmp(&table,&before,sizeof(table)));}
  assert(Parse("[effects]\nversion=1\n[source:01:02:0:enemy-fireball:0]\nintensity=.5\ncolor=ff9988\n"));
  ActionSceneEffectFrame frame={.effect_count=2,.visible_count=2};
  for(unsigned i=0;i<2;++i)frame.effects[i]=(ActionEffectInstance){.kind=kActionEffect_EnemyFireball,.generation=i+23,.flags=kActionEffectFlag_Visible};
  ActionEffectRecipes_Apply(&table,1,2,0,37,NULL,&frame);
  assert(frame.effects[0].tuning.intensity==.5f&&frame.effects[1].tuning.color==0xff9988);
}
static void BenchmarkStorage(void) {
  static char text[kActionEffectRecipeMaxBytes];
  size_t size=(size_t)snprintf(text,sizeof(text),"[effects]\nversion=1\n");
  for(unsigned i=0;i<kActionEffectRecipeMax;++i) {
    const int written=snprintf(text+size,sizeof(text)-size,
        "[source:01:02:0:cave-light:%08x]\nenabled=1\nintensity=.75\ncolor=91bedf\n"
        "light-scenery=1\nlight-player=1\nlight-enemies=1\ndim-scenery=1\ndim-player=0\ndim-enemies=1\n",i+1);
    assert(written>0&&(size_t)written<sizeof(text)-size);size+=(size_t)written;
  }
  enum {iterations=500};
  unsigned line=0;const clock_t start=clock();
  for(unsigned i=0;i<iterations;++i)assert(ActionEffectRecipes_Parse(&table,text,size,&line));
  const double ms=(double)(clock()-start)*1000/CLOCKS_PER_SEC/iterations;
  printf("Effects storage: %u records, %zu text bytes, %zu retained table bytes, %.3f ms/parse (%u iterations, CPU clock).\n",
      table.count,size,sizeof(table),ms,iterations);
}
static void TestLayerAnchors(void) {
  const char *anchors[]={"bg1","bg2-point","bg2-raster"};
  const char *depths[]={"background","playfield","foreground"};
  const char *kinds[]={"soft-light","motes","free-mist","particle-area","light-fan","water-surface","drips","waterfall-spray","cloud-bank"};
  char text[512];
  for(unsigned k=0;k<sizeof(kinds)/sizeof(kinds[0]);++k)
    for(unsigned a=0;a<3;++a)for(unsigned d=0;d<3;++d) {
      snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:02:01:0:%s:123]\nx=112\ny=62\nanchor=%s\nplacement=%s\n",kinds[k],anchors[a],depths[d]);
      assert(Parse(text));
      ActionSceneEffectFrame frame={0};
      ActionEffectRecipes_Apply(&table,2,1,0,37,NULL,&frame);
      assert(frame.authored_count==1);
      const ActionEffectInstance *e=&frame.authored[0];
      assert(e->world_x==112&&e->world_y==62);
      assert(e->projection_plane==(a?kActionEffectProjectionPlane_Bg2:kActionEffectProjectionPlane_Bg1));
      assert(!!(e->flags&kActionEffectFlag_StaticAnchor)==(a==1));
      assert(ActionEffectRecipes_NeedsBgMask(&table,2,1,0,1)==(a!=0||d==0));
      assert(ActionEffectRecipes_NeedsBgMask(&table,2,1,0,0)==(a==0||d==1));
      /* Retained sources don't borrow recipe data or follow the BG1 camera. */
      ActionSceneEffectFrame retained=frame;
      assert(Parse("[effects]\nversion=1\n"));
      assert(!memcmp(&frame,&retained,sizeof(frame)));
    }
  const char *invalid[]={"anchor=screen","anchor=bg2-point\nanchor=bg2-point","anchor=BG2", "anchor="};
  before=table;
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:02:01:0:light-fan:123]\n%s\n",invalid[i]);
    assert(!Parse(text));assert(!memcmp(&table,&before,sizeof(table)));
  }
  const char *terrain[]={"ground-mist","exposure","wet-contour"};
  for(unsigned i=0;i<3;++i) {
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:02:01:0:%s:123]\nanchor=bg2-point\n",terrain[i]);
    assert(!Parse(text));assert(!memcmp(&table,&before,sizeof(table)));
  }
}
int main(int argc,char **argv) {
  TestLayerAnchors();
  TestFieldsAndReceivers();
  TestMoteParameters();
  TestFloorSupport();
  const char *valid = "[effects]\nversion=1\n[source:02:03:1:wall-torch:54000101]\n"
    "reach=2.5\nintensity=.75\ncolor=98abFF\nenabled=1\n";
  assert(Parse(valid)); assert(table.count == 1); before = table;
  const char *invalid[] = {
    "[effects]\nversion=2\n", "[effects]\nversion=1\nversion=1\n",
    "[source:02:03:1:wall-torch:54000101]\n",
    "[effects]\nversion=1\n[source:02:03:1:unknown:0]\n",
    "[effects]\nversion=1\n[source:02:03:4:wall-torch:0]\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\nreach=nan\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\nintensity=inf\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\nintensity=-1\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\ncolor=ZZZZZZ\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\nenabled=2\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\nreach=2\nreach=3\n",
    "[effects]\nversion=1\n[source:02:03:1:castle-light:0]\nreach=2\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]\n[source:02:03:1:wall-torch:0]\n",
    "[effects]\nversion=1\n[source:02:03:1:wall-torch:0]garbage\n",
  };
  for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
    assert(!Parse(invalid[i])); assert(!memcmp(&table,&before,sizeof(table)));
  }
  ActionSceneEffectFrame frame = {.decoration_count=2,.decoration_visible_count=2};
  frame.decorations[0] = (ActionEffectInstance){.kind=kActionEffect_WallTorch,
      .generation=0x54000101,.flags=kActionEffectFlag_Visible};
  frame.decorations[1] = (ActionEffectInstance){.kind=kActionEffect_WallTorch,
      .generation=0x54000102,.flags=kActionEffectFlag_Visible};
  ActionEffectRecipes_Apply(&table,2,3,0,123,NULL,&frame);
  assert(!frame.decorations[0].tuning.active);
  ActionEffectRecipes_Apply(&table,2,3,1,123,NULL,&frame);
  assert(frame.decorations[0].tuning.active && frame.decorations[0].tuning.reach==2.5f);
  assert(frame.decorations[0].tuning.color==0x98abff && !frame.decorations[1].tuning.active);
  ActionSceneEffectFrame retained = frame;
  assert(Parse("[effects]\nversion=1\n[source:02:03:1:wall-torch:54000101]\nenabled=0\n"));
  ActionEffectRecipes_Apply(&table,2,3,1,123,NULL,&frame);
  assert(frame.decoration_visible_count==1 && !(frame.decorations[0].flags&kActionEffectFlag_Visible));
  assert(retained.decorations[0].tuning.reach==2.5f && retained.decoration_visible_count==2);
  assert(Parse("[effects]\nversion=1\n"));assert(table.count==0);
  assert(Parse("[effects]\nversion=1\n[emitter:01:02:0:motes:123]\nx=200\ny=500\nparticles=128\nlifetime=180\n"));
  ActionEffectRecipes_Apply(&table,1,2,0,37,NULL,&frame);
  assert(frame.authored_count==1 && frame.authored[0].particle_count==128);
  assert(frame.authored[0].pulse_ticks==37 && frame.authored[0].world_y==500);
  /* Time is supplied by the completed-action clock, not the changing vblank. */
  frame.game_frame=999;
  ActionEffectRecipes_Apply(&table,1,2,0,37,NULL,&frame);
  assert(frame.authored[0].pulse_ticks==37);
  char dense[8192]="[effects]\nversion=1\n";
  for(unsigned i=0;i<17;++i) {
    char line[128];snprintf(line,sizeof(line),"[emitter:01:02:0:soft-light:%08x]\n",i);
    strcat(dense,line);
  }
  before=table;assert(!Parse(dense));assert(!memcmp(&table,&before,sizeof(table)));
  strcpy(dense,"[effects]\nversion=1\n");
  for(unsigned i=0;i<9;++i) {
    char line[128];snprintf(line,sizeof(line),"[emitter:01:02:0:motes:%08x]\nparticles=128\n",i);
    strcat(dense,line);
  }
  assert(!Parse(dense));assert(!memcmp(&table,&before,sizeof(table)));
  puts("Effect recipes: atomic validation, sparse overrides, mote parameters, source identity, terrain scope, retained frames passed.");
  if(argc==2&&!strcmp(argv[1],"--benchmark"))BenchmarkStorage();
  return 0;
}
