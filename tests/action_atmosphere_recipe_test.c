#include "action/action_effect_render.h"
#include "action/action_effect_recipes.h"
#include "action/action_environment_exposure.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;




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

static void Atmosphere(unsigned room,unsigned x,unsigned y,unsigned ticks) {
  memset(&frame,0,sizeof(frame));
  const unsigned kinds[]={kActionEffect_TempleDust,kActionEffect_TowerWindowLight,kActionEffect_CaveAmbientLight,kActionEffect_TempleGrit,kActionEffect_TempleGroundMist};
  const unsigned layers[]={kActionEffectRenderLayer_WorldOverlay,kActionEffectRenderLayer_ForegroundLight,kActionEffectRenderLayer_ForegroundLight,kActionEffectRenderLayer_WorldDust,kActionEffectRenderLayer_Bg1Mist};
  const unsigned dimensions[][2]={{2048,1280},{1024,1792},{512,256}};
  for(unsigned i=0;i<5;++i) {
    if((i==1&&room!=4)||(i>=2&&room==4))continue;
    ActionEffectInstance e={
      .generation=0xC2000000u|(room<<8)|kinds[i],.pulse_generation=0xD2000000u|(room<<8)|kinds[i],
      .world_x=(int16_t)(x+128),.world_y=(int16_t)((int)y-160),.environment_room=room,
      .age_ticks=ticks,.phase_ticks=ticks,.pulse_ticks=ticks,
      .kind=kinds[i],.phase=kActionEffectPhase_CaveEnvironment,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=layers[i],.projection_plane=kActionEffectProjectionPlane_Bg1,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,0,384,544}},
      .clip_rect={-(float)(x+128),160-(float)y,dimensions[room-2][0]-(float)(x+128),dimensions[room-2][1]+160-(float)y},
    };
    if(i==4) {
      const unsigned left=room==2?880:512,right=room==2?1760:960,floor=room==2?1120:1664;
      e.generation=0xC3000000u|(room<<16)|left;e.pulse_generation=0xD3000000u|(room<<16)|left;
      e.world_x=left;e.world_y=floor;
      e.geometry.data.rect=(ActionEffectLocalRect){0,-26,right-left,0};e.clip_rect=e.geometry.data.rect;
    }
    frame.decorations[frame.decoration_count++]=e;
  }
  frame.decoration_visible_count=frame.decoration_count;
}
static ActionEffectRecipes recipes,previous;
static char definition[16384];
static uint8_t map[8*7*256],water_map[8*5*256];
static void Put(uint8_t *bytes,unsigned pages,int x,int y,uint8_t tile) {
  bytes[((y/256)*pages+x/256)*256+((y/16)%16)*16+(x/16)%16]=tile;
}
static ActionEnvironmentScene Scene(unsigned preset,unsigned group,unsigned room,unsigned x,unsigned y,unsigned ticks) {
  memset(map,0,sizeof(map));memset(water_map,0,sizeof(water_map));
  const unsigned dimensions[][4]={{2048,1280,2048,1280},{1024,1792,256,512},{512,256,256,256}};
  const unsigned witnesses[][6]={{326,352,184,420,432,185},{896,128,57,448,1664,38},{80,80,8,96,192,38}};
  const unsigned *d=dimensions[preset-2],*w=witnesses[preset-2],pages=d[0]/256;
  Put(map,pages,w[0],w[1],w[2]);Put(map,pages,w[3],w[4],w[5]);
  if(preset==2){Put(water_map,8,0,896,1);Put(water_map,8,720,0,2);}
  const unsigned left=preset==2?880:512,right=preset==2?1760:960,floor=preset==2?1120:1664;
  if(preset<4)for(unsigned ix=left;ix<right;ix+=16)Put(map,pages,ix,floor,254);
  ActionEnvironmentScene scene={.group=group,.room=room,.clock=ticks,
    .camera_x={(int)x,(int)x},.camera_y={(int)y,(int)y},
    .maps={{map,sizeof(map),d[0],d[1],pages},{water_map,sizeof(water_map),d[2],d[3],d[2]/256}}};
  scene.collision[254]=15;return scene;
}
static void Load(unsigned preset,unsigned group,unsigned room) {
  const ActionAtmosphereField *p=ActionAtmosphereField_Bundled(preset);assert(p);
  const int prefix=snprintf(definition,sizeof(definition),"[effects]\nversion=1\n[field:%02x:%02x:0:atmosphere-field:c200%02x00]\n",group,room,preset);
  assert(ActionAtmosphereField_Write(p,definition+prefix,sizeof(definition)-(size_t)prefix));
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,definition,strlen(definition),&line));
  assert(recipes.atmosphere_field_count==1&&recipes.records[0].atmosphere_field==1);
}
int main(void) {
 Load(2,5,3);ActionEnvironmentScene exposure_scene=Scene(2,5,3,900,950,60);
 recipes.atmosphere_fields[0].Dimming[0]=.7f;recipes.atmosphere_fields[0].Receivers[0]=6;
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,3,0,60,&exposure_scene,&frame);
 assert(ActionEnvironment_NativeBg1Dimming(&frame,5,3)==.7f&&ActionEnvironment_Bg1Dimming(&frame,5,3)==0);
 const ArRenderRectF ramp=ActionEnvironment_Bg1DimmingRamp(&frame,5,3);assert(ramp.x==800&&ramp.y==640&&ramp.w==320&&ramp.h==448);

 const unsigned cameras[][2]={{0,0},{96,200},{300,320},{650,620},{950,170},{1450,880},{500,1450}};
 const unsigned ticks[]={0,37,54,72,255,511,1023,65535};
 const uint32_t oracle[]={0x161e87d6u,0x628197c5u};
 for(unsigned view=0;view<2;++view) {
   uint32_t hash=2166136261u;
   for(unsigned room=2;room<=4;++room) {
    Load(room,1,room);
    assert(ActionEffectRecipes_NeedsBgMask(&recipes,1,room,0,0)==(room<4));
    assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,room,0,1));
    assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,room,1,0));
    for(unsigned c=0;c<7;++c)for(unsigned t=0;t<8;++t)for(unsigned g=1;g<4;++g) {
     Atmosphere(room,cameras[c][0],cameras[c][1],ticks[t]);
     const uint32_t original=Digest(view,g&1,g&2);
     ActionEnvironmentScene scene=Scene(room,1,room,cameras[c][0],cameras[c][1],ticks[t]);
     memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,1,room,0,scene.clock,&scene,&frame);
     assert(frame.atmosphere_field_valid);
     const uint32_t reconstructed=Digest(view,g&1,g&2);assert(original==reconstructed);
     hash=HashWord(hash,reconstructed);
    }
   }
   printf("atmosphere pre-migration oracle view %u: %08x\n",view,hash);assert(hash==oracle[view]);
 }
 /* Export/reload into another room: no room-number visual dispatch. */
 for(unsigned preset=2;preset<=4;++preset) {
   Load(preset,1,preset);ActionEnvironmentScene scene=Scene(preset,1,preset,300,320,37);
   memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,1,preset,0,37,&scene,&frame);
   const uint32_t native=Digest(0,true,true);
   Load(preset,5,1);scene=Scene(preset,5,1,300,320,37);
   memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
   assert(Digest(0,true,true)==native);
   const char *names[]={"cave","temple","tower"};char path[128];
   snprintf(path,sizeof(path),"assets/effects/%s-atmosphere-field.ini",names[preset-2]);
   FILE *file=fopen(path,"rb");assert(file);size_t n=fread(definition,1,sizeof(definition)-1,file);assert(!ferror(file));fclose(file);definition[n]=0;
   unsigned line;assert(ActionEffectRecipes_Parse(&recipes,definition,n,&line));
   assert(!memcmp(&recipes.atmosphere_fields[0],ActionAtmosphereField_Bundled(preset),sizeof(ActionAtmosphereField)));
 }
 Load(3,5,1);ActionEnvironmentScene scene=Scene(3,5,1,600,1400,37);
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 const uint32_t original=Digest(0,true,true);const ActionSceneEffectFrame retained=frame;previous=recipes;
 ActionAtmosphereField edited=recipes.atmosphere_fields[0];edited.AmbientColor[0]=.1f;edited.MistDensity[0]=.5f;
 const int prefix=snprintf(definition,sizeof(definition),"[effects]\nversion=1\n[field:05:01:0:atmosphere-field:c2000300]\n");
 assert(ActionAtmosphereField_Write(&edited,definition+prefix,sizeof(definition)-(size_t)prefix));
 unsigned line;assert(ActionEffectRecipes_Parse(&recipes,definition,strlen(definition),&line));
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);assert(Digest(0,true,true)!=original);
 edited=previous.atmosphere_fields[0];edited.Components[0]=0;
 assert(ActionAtmosphereField_Write(&edited,definition+prefix,sizeof(definition)-(size_t)prefix));
 assert(ActionEffectRecipes_Parse(&recipes,definition,strlen(definition),&line));
 ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);assert(!frame.decoration_count&&frame.atmosphere_field_valid);
 recipes=previous;assert(ActionAtmosphereField_Write(&recipes.atmosphere_fields[0],definition+prefix,sizeof(definition)-(size_t)prefix));
 const char *bad[]={"ambient-color=nan 1 1\n","unknown=1\n","counts=9 1 1 1 1\n"};
 for(unsigned i=0;i<3;++i) {
   char text[17000];snprintf(text,sizeof(text),"%s%s",definition,bad[i]);
   assert(!ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));assert(!memcmp(&recipes,&previous,sizeof(recipes)));
 }
 const char *incomplete="[effects]\nversion=1\n[field:05:01:0:atmosphere-field:1]\n";
 assert(!ActionEffectRecipes_Parse(&recipes,incomplete,strlen(incomplete),&line));assert(!memcmp(&recipes,&previous,sizeof(recipes)));
 edited=previous.atmosphere_fields[0];edited.Ambient1[2]=0;assert(!ActionAtmosphereField_Valid(&edited));
 edited=previous.atmosphere_fields[0];edited.DustGrid[2]=26;assert(!ActionAtmosphereField_Valid(&edited));
 edited=previous.atmosphere_fields[0];edited.GritTiming[0]=17;assert(!ActionAtmosphereField_Valid(&edited));
 edited=previous.atmosphere_fields[0];edited.FloorArea[2]=2048;assert(!ActionAtmosphereField_Valid(&edited));
 /* Floor edits move the mist down through non-colliding spike artwork. */
 for(unsigned x=512;x<960;x+=16){Put(map,4,x,1664,253);Put(map,4,x,1680,254);}
 ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(frame.decorations[frame.decoration_count-1].kind==kActionEffect_TempleGroundMist);
 assert(frame.decorations[frame.decoration_count-1].world_y==1680);
 /* Required artwork missing: no partial atmosphere leaks into transitions. */
 Put(map,4,896,128,0);ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
 assert(!frame.decoration_count&&!frame.atmosphere_field_valid);
 frame=retained;assert(Digest(0,true,true)==original);
 return 0;
}
