#include "action_effect_manifest.h"
#include "action_effect_recipes.h"
#include "action_effect_capture.h"
#include "app/settings.h"
#include "actraiser/actraiser_action_bg.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "actraiser_game.h"
#include "snesrecomp/support/utf8_fs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
static ActionEffectRecipes s_recipes;
static ActionSceneEffectFrame s_receiver_frame;
static ActionReceiverLighting s_receiver_lighting;
bool ActionEffectManifest_HasAuthored(unsigned group,unsigned room) {
  for(unsigned i=0;i<s_recipes.count;++i) {
    const ActionEffectRecipe *r=&s_recipes.records[i];
    if(r->group==group&&r->room==room&&r->terrain==ActRaiserActionBg_TerrainProfile()&&r->emitter&&r->enabled)return true;
  }
  return false;
}
bool ActionEffectManifest_NeedsBg1Mask(unsigned group,unsigned room) {
  if(ActionEffectManifest_HasAuthored(group,room))return true;
  for(unsigned i=0;i<s_recipes.count;++i) {
    const ActionEffectRecipe *r=&s_recipes.records[i];
    if(r->group==group&&r->room==room&&r->terrain==ActRaiserActionBg_TerrainProfile()&&r->enabled&&
        r->tuning.light_receivers_set&&(r->tuning.light_receivers&kActionReceiver_Scenery))return true;
  }
  return false;
}
const ActionReceiverLighting *ActionEffectManifest_PrepareReceivers(unsigned group,unsigned room,const uint8_t *ram,size_t size) {
  bool needed=false;
  for(unsigned i=0;i<s_recipes.count;++i) {
    const ActionEffectRecipe *r=&s_recipes.records[i];
    if(r->group==group&&r->room==room&&r->terrain==ActRaiserActionBg_TerrainProfile()&&r->enabled&&
        ((r->tuning.light_receivers_set&&(r->tuning.light_receivers&6))||
         ((r->tuning.dim_receivers_set||r->kind==kActionEffect_AuthoredExposure)&&(r->tuning.dim_receivers&6))))needed=true;
  }
  if(!needed||!ram||size<kActRaiserWramSize)return NULL;
  const uint16_t clock=ActionEffectCapture_VisualClock();
  ActionEnvironmentScene scene;
  if(!ActionEnvironmentScene_FromWram(&scene,ram,size,clock))return NULL;
  ActionEffectCapture_PeekScene(&s_receiver_frame);
  ActionEffectRecipes_Apply(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),clock,&scene,&s_receiver_frame);
  return ActionEffectReceivers_Prepare(&s_receiver_frame,group,room,scene.camera_x[0],scene.camera_y[0],
      scene.camera_x[1],scene.camera_y[1],g_settings.action_effect_lighting,&s_receiver_lighting)?&s_receiver_lighting:NULL;
}
bool ActionEffectManifest_Load(void) {
  char path[kHostPathCapacity];
  UserDataFile(path,sizeof(path),"action-effects.ini");
  FILE *f = sr_fopen(path,"rb");
  if (!f) {
    if (errno == ENOENT) { s_recipes = (ActionEffectRecipes){0}; return true; }
    fprintf(stderr,"[action-effects] cannot read %s; retaining previous recipes\n",path);
    return false;
  }
  char *text = malloc(kActionEffectRecipeMaxBytes + 1);
  if (!text) { fclose(f); return false; }
  const size_t size = fread(text,1,kActionEffectRecipeMaxBytes + 1,f);
  bool ok = !ferror(f); fclose(f);
  unsigned line = 0;
  ok = ok && ActionEffectRecipes_Parse(&s_recipes,text,size,&line);
  free(text);
  if (!ok) fprintf(stderr,"[action-effects] rejected %s at line %u; retaining previous recipes\n",path,line);
  else fprintf(stderr,"[action-effects] loaded %u source overrides from %s\n",s_recipes.count,path);
  return ok;
}
void ActionEffectManifest_Apply(unsigned group, unsigned room, uint16_t clock, const uint8_t *ram, size_t size, ActionSceneEffectFrame *frame) {
  if (!s_recipes.count) return;
  ActionEnvironmentScene scene;
  const bool valid=ActionEnvironmentScene_FromWram(&scene,ram,size,clock);
  ActionEffectRecipes_Apply(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),clock,valid?&scene:NULL,frame);
}
