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
static uint8_t map[9*2*256];

static void LoadDefinition(unsigned group,unsigned room) {
  const ActionRayField *bundled=ActionRayField_Bundled();
  assert(bundled);
  const int prefix=snprintf(definition,sizeof(definition),
      "[effects]\nversion=1\n[field:%02x:%02x:0:ray-field:46000000]\nenabled=1\n",group,room);
  assert(prefix>0);
  assert(ActionRayField_Write(bundled,definition+prefix,sizeof(definition)-(size_t)prefix));
  unsigned line;
  assert(ActionEffectRecipes_Parse(&recipes,definition,strlen(definition),&line));
  assert(recipes.field_count==1&&recipes.records[0].field==1);
}

static ActionEnvironmentScene Scene(unsigned group,unsigned room,unsigned camera,unsigned ticks) {
  /* Independent page-major source art, just the two declared witnesses. */
  map[7*16+9]=0x0f;map[8*16+9]=1;
  return (ActionEnvironmentScene){.group=group,.room=room,.clock=ticks,
      .camera_x={(int)camera*2,0},
      .maps={{.world_width=4096,.world_height=768},
             {map,sizeof(map),2304,512,9}}};
}

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

static void Forest(unsigned camera, unsigned ticks) {
  memset(&frame, 0, sizeof(frame));
  frame.decoration_count = frame.decoration_visible_count = 3;
  frame.decorations[0] = (ActionEffectInstance){
      .generation = 0x46000000u, .pulse_generation = 0x66000000u,
      .world_x = (int16_t)(camera + 128), .world_y = -160,
      .age_ticks = ticks, .phase_ticks = ticks, .pulse_ticks = ticks,
      .kind = kActionEffect_ForestCanopyLight,
      .phase = kActionEffectPhase_ForestCanopyLight,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = kActionEffectRenderLayer_Bg2Plane,
      .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
      .clip_rect = {-128,160,2176,672},
  };
  frame.decorations[1] = frame.decorations[2] = frame.decorations[0];
  frame.decorations[1].kind = kActionEffect_ForestLeaves;
  frame.decorations[1].render_layer = kActionEffectRenderLayer_Bg2Alpha;
  frame.decorations[2].kind = kActionEffect_ForestForwardLight;
  frame.decorations[2].render_layer = kActionEffectRenderLayer_ForegroundLight;
}

int main(int argc,char **argv) {
  static const unsigned cameras[] = {0,400,1040,1680,2320,2944,3264};
  static const unsigned clocks[] = {0,37,1023,2047,65535};
  LoadDefinition(1,1);
  assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,1,0,0));
  assert(ActionEffectRecipes_NeedsBgMask(&recipes,1,1,0,1));
  const uint32_t oracle[]={0x0c5e9a75u,0xbe3ba281u};
  for (unsigned view = 0; view < 2; ++view) {
    uint32_t hash = 2166136261u;
    for (unsigned c = 0; c < sizeof(cameras)/sizeof(cameras[0]); ++c)
      for (unsigned t = 0; t < sizeof(clocks)/sizeof(clocks[0]); ++t)
        for (unsigned gates = 1; gates < 4; ++gates) {
          Forest(cameras[c], clocks[t]);
          const uint32_t original=Digest(view,gates&1,gates&2);
          ActionEnvironmentScene scene=Scene(1,1,cameras[c],clocks[t]);
          /* Start empty: no native source capture or legacy visual fallback. */
          memset(&frame,0,sizeof(frame));
          ActionEffectRecipes_Apply(&recipes,1,1,0,scene.clock,&scene,&frame);
          assert(frame.ray_field_valid&&frame.decoration_count==3);
          const uint32_t reconstructed=Digest(view,gates&1,gates&2);
          assert(original==reconstructed);
          hash = HashWord(hash, reconstructed);
        }
    printf("forest pre-migration oracle view %u: %08x\n", view, hash);
    /* These values were recorded before parameterizing the original kernel. */
    assert(hash==oracle[view]);
  }
  /* A generic field also works outside Fillmore: there is no room-name shader
   * dispatch or hidden native source definition in this reconstruction. */
  Forest(1040,37);const uint32_t expected=Digest(0,true,true);
  LoadDefinition(5,1);ActionEnvironmentScene scene=Scene(5,1,1040,37);
  memset(&frame,0,sizeof(frame));
  ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
  assert(frame.decoration_count==3&&Digest(0,true,true)==expected);
  const ActionSceneEffectFrame retained=frame;
  char reload[16384];
  const int prefix=snprintf(reload,sizeof(reload),"[effects]\nversion=1\n[field:05:01:0:ray-field:46000000]\n");
  assert(ActionRayField_Write(&recipes.fields[0],reload+prefix,sizeof(reload)-(size_t)prefix));
  unsigned line;
  assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
  memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
  assert(Digest(0,true,true)==expected);
  previous=recipes;
  /* Complete field controls are not locked to the original window or room.
   * Change a valid recipe and prove the geometry responds, then disable each
   * component without introducing extra decorations or renderer work. */
  ActionRayField edited=recipes.fields[0];
  edited.values[kActionRay_OriginY]=48;
  edited.window[0]=-320;edited.window[2]=320;
  edited.rays[0].strength=.5f;
  assert(ActionRayField_Write(&edited,reload+prefix,sizeof(reload)-(size_t)prefix));
  assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
  memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
  assert(frame.decoration_count==3&&Digest(0,true,true)!=expected);
  edited.components=0;
  assert(ActionRayField_Write(&edited,reload+prefix,sizeof(reload)-(size_t)prefix));
  assert(ActionEffectRecipes_Parse(&recipes,reload,strlen(reload),&line));
  memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
  assert(frame.decoration_count==0&&frame.ray_field_valid);
  recipes=previous;
  assert(ActionRayField_Write(&recipes.fields[0],reload+prefix,sizeof(reload)-(size_t)prefix));
  edited=recipes.fields[0];edited.rays[0].half_width=0;
  assert(!ActionRayField_Valid(&edited)&&!ActionRayField_Write(&edited,NULL,0));
  edited=recipes.fields[0];edited.components=16;
  assert(!ActionRayField_Valid(&edited));
  edited=recipes.fields[0];edited.profiles[0].front_range=0;
  assert(!ActionRayField_Valid(&edited));
  edited=recipes.fields[0];edited.values[kActionRay_MotePeriod]=17;
  assert(!ActionRayField_Valid(&edited));
  const char *bad[]={"sway-period=nan\n","ray-1=0 0 1 .5 0 0\n","profile-count=3\n","unknown=1\n"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
    char text[16384];snprintf(text,sizeof(text),"%s%s",reload,bad[i]);
    assert(!ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));
    assert(!memcmp(&recipes,&previous,sizeof(recipes)));
  }
  const char *incomplete="[effects]\nversion=1\n[field:01:01:0:ray-field:1]\n";
  assert(!ActionEffectRecipes_Parse(&recipes,incomplete,strlen(incomplete),&line));
  assert(!memcmp(&recipes,&previous,sizeof(recipes)));
  /* Supporting artwork removal fails closed; retained frame owns its data. */
  map[7*16+9]=0;
  ActionEffectRecipes_Apply(&recipes,5,1,0,37,&scene,&frame);
  assert(!frame.decoration_count&&!frame.ray_field_valid);
  frame=retained;assert(Digest(0,true,true)==expected);
  if(argc>1&&!strcmp(argv[1],"--bundle")) {
    FILE *file=fopen("assets/effects/forest-ray-field.ini","rb");assert(file);
    const size_t n=fread(definition,1,sizeof(definition)-1,file);assert(!ferror(file));fclose(file);definition[n]=0;
    assert(ActionEffectRecipes_Parse(&recipes,definition,n,&line));
    assert(!memcmp(&recipes.fields[0],ActionRayField_Bundled(),sizeof(ActionRayField)));
  }
  Forest(1040,37);
  const clock_t start = clock();
  for (unsigned i = 0; i < 1000; ++i) {
    unsigned view = 0;
    for (unsigned layer = 0; layer < kActionEffectRenderLayer_Count; ++layer)
      assert(ActionSceneDecorationRender_Build(&frame, layer, true, true,
                                               Project, NULL, &view, &batch));
  }
  printf("forest geometry CPU sample: %.4f ms/frame (1000 iterations)\n",
         (double)(clock()-start)*1000/CLOCKS_PER_SEC/1000);
  return 0;
}
