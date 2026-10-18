#include "action/action_effect_manifest.h"
#include "action/action_effect_capture.h"
#include "action/action_environment_scene.h"
#include "actraiser/actraiser_action_bg.h"
#include "app/settings.h"
#include "app/user_data_dir.h"
#include "actraiser_game.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

Settings g_settings;
static uint8_t ram[kActRaiserWramSize];
static ActionSceneEffectFrame observed;
static const char *config_path;
static unsigned peeks;

/* Substitute only the native observation / filesystem boundaries. Recipe
 * resolution, the runtime gate, geometry and palette gains are production. */
uint8_t ActRaiserActionBg_TerrainProfile(void) { return 0; }
bool ActRaiserActionBg_BindEnvironmentScenery(ActionEnvironmentScene *scene) {
  (void)scene; return true;
}
uint16_t ActionEffectCapture_VisualClock(void) { return 37; }
void ActionEffectCapture_PeekScene(ActionSceneEffectFrame *frame) {
  *frame=observed; ++peeks;
  ActionEffectManifest_Apply(ram[kActRaiserWram_MapGroup],ram[kActRaiserWram_CurrentMap],
      37,ram,sizeof(ram),frame);
}
bool ActionEnvironmentScene_FromWram(ActionEnvironmentScene *scene,const uint8_t *data,size_t size,uint16_t clock) {
  assert(data==ram && size==sizeof(ram));
  *scene=(ActionEnvironmentScene){.group=ram[kActRaiserWram_MapGroup],
      .room=ram[kActRaiserWram_CurrentMap],.clock=clock};
  return true;
}
char *UserDataFile(char *buffer,size_t capacity,const char *leaf) {
  assert(!strcmp(leaf,"action-effects.ini"));
  assert(snprintf(buffer,capacity,"%s",config_path)>0);return buffer;
}
static void Configure(const char *text) {
  FILE *file=fopen(config_path,"wb");assert(file);
  assert(fwrite(text,1,strlen(text),file)==strlen(text));assert(!fclose(file));
  assert(ActionEffectManifest_Load());
}
static const ActionReceiverLighting *Prepare(unsigned group,unsigned room) {
  ram[kActRaiserWram_MapGroup]=group;ram[kActRaiserWram_CurrentMap]=room;
  return ActionEffectManifest_PrepareReceivers(group,room,ram,sizeof(ram),false);
}
static void CheckNative(unsigned group) {
  const ActionReceiverLighting *lit=Prepare(group,1);
  assert(lit && lit->player.vertex_count>0 && lit->enemies.vertex_count>0);
  float best=1;
  for(int y=48;y<300;y+=8)for(int x=-128;x<512;x+=8) {
    float player[3],enemy[3],add[3];
    ActionEffectReceivers_Sample(lit,kActionReceiver_Player,x,y,player,add);
    ActionEffectReceivers_Sample(lit,kActionReceiver_Enemies,x,y,enemy,add);
    assert(!memcmp(player,enemy,sizeof(player)));
    for(unsigned c=0;c<3;++c)if(player[c]>best)best=player[c];
  }
  assert(best>1.05f); /* Nonempty geometry must actually illuminate actors. */
  printf("Native %s runtime receivers: maximum palette gain %.3f\n",group==1?"forest":"moon",best);
}
int main(int argc,char **argv) {
  assert(argc==2);config_path=argv[1];g_settings.action_effect_lighting=true;
  Configure("[effects]\nversion=1\n");
  ActionEnvironmentScene scene={.group=1,.room=1,.clock=37};
  ActionRayField forest=*ActionRayField_Bundled();
  memset(forest.dimensions,0,sizeof(forest.dimensions));forest.witness_count=0;
  ActionRayField_Capture(&scene,&observed,&forest,1);
  CheckNative(1); /* No receiver overrides anywhere in the INI. */
  Configure("[effects]\nversion=1\n[source:01:01:0:forest-forward:1]\nlight-player=0\nlight-enemies=0\n");
  assert(!Prepare(1,1));
  Configure("[effects]\nversion=1\n[source:01:01:0:forest-forward:1]\nlight-player=0\nlight-enemies=0\n"
      "[member:01:01:0:forest-forward:1]\nlight-player=1\nlight-enemies=0\n");
  const ActionReceiverLighting *member=Prepare(1,1);
  assert(member && member->player.vertex_count && !member->enemies.vertex_count);
  g_settings.action_effect_lighting=false;assert(!Prepare(1,1));
  Configure("[effects]\nversion=1\n[emitter:01:01:0:exposure:1]\nx=128\ny=160\nwidth=512\nheight=512\n"
      "brightness=.25\ndim-player=1\ndim-enemies=0\n");
  const ActionReceiverLighting *dim=Prepare(1,1);assert(dim && !dim->player.vertex_count);
  float gain[3],add[3];ActionEffectReceivers_Sample(dim,kActionReceiver_Player,128,160,gain,add);
  assert(gain[0]==.25f && gain[1]==.25f && gain[2]==.25f);
  Configure("[effects]\nversion=1\n");g_settings.action_effect_lighting=true;
  observed=(ActionSceneEffectFrame){0};scene.group=2;
  ActionMoonField moon=*ActionMoonField_Bundled();
  memset(moon.Dimensions,0,sizeof(moon.Dimensions));moon.WitnessCount[0]=0;
  ActionMoonField_Capture(&scene,&observed,&moon,2);observed.moonlight.valid=true;
  CheckNative(2);
  observed=(ActionSceneEffectFrame){0};assert(!Prepare(2,1));
  const unsigned before=peeks;
  assert(!ActionEffectManifest_PrepareReceivers(2,1,NULL,0,false));assert(peeks==before);
  assert(!remove(config_path));
  puts("Runtime receiver gate: native defaults, opt-out, member opt-in, lighting toggle and dimming passed.");
  return 0;
}
