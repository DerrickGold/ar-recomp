#include "action/action_effect_render.h"
#include "action/action_effect_recipes.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static ActionEffectRecipes recipes, previous;
static char definition[16384];
static uint8_t map[8*5*256],water_map[8*5*256];

static bool Project(void *context, const ActionEffectInstance *effect,
                    float x, float y, ArRenderPointF *out) {
  const unsigned view = *(const unsigned *)context;
  *out = (ArRenderPointF){x, y};
  if (view) {
    out->x = (x + effect->world_x) * .75f + y * .125f;
    out->y = (y + effect->world_y) * .625f;
  }
  return true;
}

static uint32_t HashWord(uint32_t hash, uint32_t word) {
  for (unsigned i = 0; i < 4; ++i) {
    hash = (hash ^ (word & 255u)) * 16777619u;
    word >>= 8;
  }
  return hash;
}
static uint32_t HashFloat(uint32_t hash, float value) {
  uint32_t word;
  memcpy(&word, &value, sizeof(word));
  return HashWord(hash, word);
}
static uint32_t Digest(unsigned view, bool lighting, bool particles) {
  uint32_t hash = 2166136261u;
  for (unsigned layer = 0; layer < kActionEffectRenderLayer_Count; ++layer) {
    assert(ActionSceneDecorationRender_Build(&frame, layer, lighting, particles,
                                             Project, NULL, &view, &batch));
    hash = HashWord(hash, layer);
    hash = HashWord(hash, (uint32_t)batch.vertex_count);
    hash = HashWord(hash, (uint32_t)batch.index_count);
    for (int i = 0; i < batch.vertex_count; ++i) {
      const ArRenderVertex2D *v = &batch.vertices[i];
      hash = HashFloat(hash, v->position.x);
      hash = HashFloat(hash, v->position.y);
      hash = HashFloat(hash, v->color.r);
      hash = HashFloat(hash, v->color.g);
      hash = HashFloat(hash, v->color.b);
      hash = HashFloat(hash, v->color.a);
    }
    for (int i = 0; i < batch.index_count; ++i)
      hash = HashWord(hash, (uint32_t)batch.indices[i]);
  }
  return hash;
}

static void Water(unsigned x,unsigned y,unsigned ticks) {
  memset(&frame,0,sizeof(frame));
  const unsigned kinds[]={kActionEffect_CaveWater,kActionEffect_CaveDrips,kActionEffect_CaveMist,kActionEffect_CaveSheen};
  const unsigned layers[]={kActionEffectRenderLayer_Bg2HighPlane,kActionEffectRenderLayer_WorldOverlay,kActionEffectRenderLayer_WorldDust,kActionEffectRenderLayer_Bg1Plane};
  for(unsigned i=0;i<4;++i) {
    frame.decorations[i]=(ActionEffectInstance){
      .generation=0xC2000200u|kinds[i],.pulse_generation=0xD2000200u|kinds[i],
      .world_x=(int16_t)(x+128),.world_y=(int16_t)((int)y-160),.environment_room=2,
      .age_ticks=ticks,.phase_ticks=ticks,.pulse_ticks=ticks,
      .kind=kinds[i],.phase=kActionEffectPhase_CaveEnvironment,
      .source_mask=(i==1||i==3)?255:0,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=layers[i],.projection_plane=(i==0||i==2)?kActionEffectProjectionPlane_Bg2High:kActionEffectProjectionPlane_Bg1,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,0,384,544}},
      .clip_rect={-(float)(x+128),160-(float)y,2048-(float)(x+128),1440-(float)y},
    };
  }
  frame.decoration_count=frame.decoration_visible_count=4;
}
static void Put(uint8_t *bytes,int x,int y,uint8_t tile) {
  bytes[((y/256)*8+x/256)*256+((y/16)%16)*16+(x/16)%16]=tile;
}
static ActionEnvironmentScene Scene(unsigned group,unsigned room,unsigned x,unsigned y,unsigned ticks) {
  memset(map,0,sizeof(map));memset(water_map,0,sizeof(water_map));
  /* Independent source-material fixture, captured before migrating C defaults. */
  const int contacts[][5]={{220,348,448,0x93,0xAF},{326,371,451,0xC0,0x8E},
    {358,371,483,0xC0,0x8E},{420,452,620,0xB8,0x8D},{534,766,896,0x9D,0x62},
    {662,787,896,0xC0,0x6C},{872,844,896,0xC7,0x2F},{888,556,674,0xC7,0x91}};
  for(unsigned i=0;i<8;++i) {Put(map,contacts[i][0],contacts[i][1]-1,contacts[i][3]);Put(map,contacts[i][0],contacts[i][2],contacts[i][4]);}
  Put(map,326,352,0xB8);Put(map,420,432,0xB9);Put(water_map,0,896,1);Put(water_map,720,0,2);
  return (ActionEnvironmentScene){.group=group,.room=room,.clock=ticks,
    .camera_x={(int)x,(int)x},.camera_y={(int)y,(int)y},
    .maps={{map,sizeof(map),2048,1280,8},{water_map,sizeof(water_map),2048,1280,8}}};
}
static void LoadDefinition(unsigned group,unsigned room) {
  const ActionWaterField *field=ActionWaterField_Bundled();assert(field);
  const int n=snprintf(definition,sizeof(definition),"[effects]\nversion=1\n[field:%02x:%02x:0:water-field:c2000200]\nenabled=1\n",group,room);
  assert(n>0&&ActionWaterField_Write(field,definition+n,sizeof(definition)-(size_t)n));
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,definition,strlen(definition),&line));
  assert(recipes.water_field_count==1&&recipes.records[0].water_field==1);
}
int main(int argc,char **argv) {
 const unsigned cameras[][2]={{0,0},{96,200},{300,320},{650,620},{950,170},{1450,880}};
 const unsigned ticks[]={0,37,54,72,255,511,1023,65535};
 const uint32_t oracle[]={0xce3cba6cu,0xbeaec2bbu};
 LoadDefinition(1,2);
 assert(ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,0));
 assert(ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,1));
 recipes.water_fields[0].Components[0]=2;
 assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,0));
 assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,1));
 LoadDefinition(1,2);
 for(unsigned view=0;view<2;++view) {
   uint32_t hash=2166136261u;
   for(unsigned c=0;c<6;++c)for(unsigned t=0;t<8;++t)for(unsigned g=1;g<4;++g) {
     Water(cameras[c][0],cameras[c][1],ticks[t]);
     const uint32_t original=Digest(view,g&1,g&2);
     ActionEnvironmentScene scene=Scene(1,2,cameras[c][0],cameras[c][1],ticks[t]);
     memset(&frame,0,sizeof(frame));
     ActionEffectRecipes_Apply(&recipes,1,2,0,scene.clock,&scene,&frame);
     assert(frame.water_field_valid&&frame.decoration_count==4);
     const uint32_t reconstructed=Digest(view,g&1,g&2);assert(original==reconstructed);
     hash=HashWord(hash,reconstructed);
   }
   printf("water pre-migration oracle view %u: %08x\n",view,hash);assert(hash==oracle[view]);
 }
 LoadDefinition(5,1);ActionEnvironmentScene scene=Scene(5,1,300,320,37);
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 const uint32_t original=Digest(0,true,true);const ActionSceneEffectFrame retained=frame;
 char reload[16384];const int prefix=snprintf(reload,sizeof(reload),"[effects]\nversion=1\n[field:05:01:0:water-field:c2000200]\n");
 assert(ActionWaterField_Write(&recipes.water_fields[0],reload+prefix,sizeof(reload)-(size_t)prefix));
 unsigned line;assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(Digest(0,true,true)==original);previous=recipes;
 ActionWaterField edited=recipes.water_fields[0];edited.SheenLip[3]=.1f;
 assert(ActionWaterField_Write(&edited,reload+prefix,sizeof(reload)-(size_t)prefix));
 assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(Digest(0,true,true)!=original);
 edited=previous.water_fields[0];edited.Components[0]=0;
 assert(ActionWaterField_Write(&edited,reload+prefix,sizeof(reload)-(size_t)prefix));
 assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(!frame.decoration_count&&frame.water_field_valid);
 recipes=previous;
 const char *bad[]={"glint-shape=nan 1 1 1\n","unknown=1\n","counts=4 4 8 4\n"};
 assert(ActionWaterField_Write(&recipes.water_fields[0],reload+prefix,sizeof(reload)-(size_t)prefix));
 for(unsigned i=0;i<3;++i) {
   char text[16384];snprintf(text,sizeof(text),"%s%s",reload,bad[i]);
   assert(!ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));assert(!memcmp(&recipes,&previous,sizeof(recipes)));
 }
 const char *incomplete="[effects]\nversion=1\n[field:05:01:0:water-field:1]\n";
 assert(!ActionEffectRecipes_Parse(&recipes,incomplete,strlen(incomplete),&line));assert(!memcmp(&recipes,&previous,sizeof(recipes)));
 edited=previous.water_fields[0];edited.FallMotion[0]=0;assert(!ActionWaterField_Valid(&edited));
 edited=previous.water_fields[0];edited.GlintMotion[4]=17;assert(!ActionWaterField_Valid(&edited));
 edited=previous.water_fields[0];edited.Pool1[1]=2048;assert(!ActionWaterField_Valid(&edited));
 edited=previous.water_fields[0];edited.Contour1[0]=.5f;assert(!ActionWaterField_Write(&edited,NULL,0));
 /* A missing wet contact suppresses only that linked drop/patch; missing
  * mandatory room artwork suppresses the full field. Retained frames own data. */
 memset(&frame,0,sizeof(frame));Put(map,220,347,0);
 ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(frame.decoration_count==4&&(frame.decorations[1].source_mask&1)==0);
 Put(water_map,0,896,0);ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(!frame.decoration_count&&!frame.water_field_valid);
 frame=retained;assert(Digest(0,true,true)==original);
 if(argc>1&&!strcmp(argv[1],"--bundle")) {
   FILE *file=fopen("assets/effects/cave-water-field.ini","rb");assert(file);
   const size_t n=fread(definition,1,sizeof(definition)-1,file);assert(!ferror(file));fclose(file);definition[n]=0;
   assert(ActionEffectRecipes_Parse(&recipes,definition,n,&line));
   assert(!memcmp(&recipes.water_fields[0],ActionWaterField_Bundled(),sizeof(ActionWaterField)));
 }
 return 0;
}
