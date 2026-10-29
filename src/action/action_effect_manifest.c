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
static ActionSceneryCaptureCache s_scenery_cache;
static ActionSceneEffectFrame s_receiver_frame;
static ActionReceiverLighting s_receiver_lighting;
void ActionEffectManifest_SurfaceFields(unsigned group,unsigned room,const ActionSurfaceField **fields){ActionEffectRecipes_SurfaceFields(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),fields);}
bool ActionEffectManifest_ReplacesRayField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesRayField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesGlowField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesGlowField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesCastleField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesCastleField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesMarshField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesMarshField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesMoonField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesMoonField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesWaterField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesWaterField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_ReplacesAtmosphereField(unsigned group,unsigned room) {
  return ActionEffectRecipes_ReplacesAtmosphereField(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile());
}
bool ActionEffectManifest_NeedsBg1Mask(unsigned group,unsigned room) {
  return ActionEffectRecipes_NeedsBgMask(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),0);
}
bool ActionEffectManifest_NeedsBg2Mask(unsigned group,unsigned room) {
  return ActionEffectRecipes_NeedsBgMask(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),1);
}
const ActionReceiverLighting *ActionEffectManifest_PrepareReceivers(unsigned group,unsigned room,const uint8_t *ram,size_t size,bool projected) {
  if(!ram||size<kActRaiserWramSize)return NULL;
  const uint16_t clock=ActionEffectCapture_VisualClock();
  ActionEnvironmentScene scene;
  if(!ActionEnvironmentScene_FromWram(&scene,ram,size,clock))return NULL;
  ActionEffectCapture_PeekScene(&s_receiver_frame);
  /* Test the resolved scene, not just INI overrides. Native ray defaults,
   * complete fields and individual members all participate in this decision. */
  const bool native_nebula=projected && g_settings.death_heim_final_battle && group==7 && room==8;
  if(!native_nebula && !ActionEffectReceivers_HasActors(&s_receiver_frame,g_settings.action_effect_lighting))return NULL;
  return ActionEffectReceivers_Prepare(&s_receiver_frame,group,room,scene.camera_x[0],scene.camera_y[0],
      scene.camera_x[1],scene.camera_y[1],g_settings.action_effect_lighting&&!projected,&s_receiver_lighting)?&s_receiver_lighting:NULL;
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
  ActionEnvironmentScene scene;
  const bool valid=ActionEnvironmentScene_FromWram(&scene,ram,size,clock);
  if (valid) {
    (void)ActRaiserActionBg_BindEnvironmentScenery(&scene);
    scene.scenery_cache = &s_scenery_cache;
  }
  ActionEffectRecipes_Apply(&s_recipes,group,room,ActRaiserActionBg_TerrainProfile(),clock,valid?&scene:NULL,frame);
}
