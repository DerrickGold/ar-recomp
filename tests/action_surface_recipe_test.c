#include "action/action_effect_render.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_source.h"
#include "action/action_effect_render_internal.h"
#include "action/action_lava_bubbles.h"
#include "action/action_effect_masks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static ActionSurfaceField legacy_pit,legacy_lake;
static unsigned hash=2166136261u;
static void Word(unsigned v){hash=(hash^v)*16777619u;}
static void Float(float v){unsigned b;memcpy(&b,&v,4);Word(b);}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p){(void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;}
static void LegacyOracle(void){unsigned long vertices=0,indices=0;
 const unsigned kinds[]={kActionEffect_AitosLavaPit,kActionEffect_AitosLavaReservoir,kActionEffect_AitosWaterSplash,kActionEffect_AitosWaterfall,kActionEffect_AitosWaterfallMist};
 const unsigned phases[]={kActionEffectPhase_AitosLavaPit,kActionEffectPhase_AitosLavaReservoir,kActionEffectPhase_AitosWaterSplash,kActionEffectPhase_AitosWaterfallFlow,kActionEffectPhase_AitosWaterfallMist};
 for(unsigned mode=0;mode<5;++mode)for(unsigned shape=0;shape<3;++shape)for(unsigned i=0;i<80;++i){
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1};
 frame.surface_fields_valid=3;frame.surface_fields[0]=legacy_pit;frame.surface_fields[1]=legacy_lake;
 float rx=mode>2?256:mode==1?48+shape*128:24+shape*16;
 frame.decorations[0]=(ActionEffectInstance){.kind=kinds[mode],.phase=phases[mode],.generation=0x1234,.pulse_generation=0x7364,
 .world_x=128,.world_y=100,.phase_ticks=i*101,.pulse_ticks=i*79,.flags=kActionEffectFlag_Visible,
 .render_layer=kActionEffectRenderLayer_WorldOverlay,.projection_plane=kActionEffectProjectionPlane_Bg1,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-rx,-16,rx,mode>2?312:16}}};
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldOverlay,i%4!=1,i%4!=2,Project,NULL,NULL,&batch));
 vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
 for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
 for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);

 }
 assert(vertices==559800&&indices==2220480);
#if defined(__OPTIMIZE__)
 /* Rechecked against the pre-up-light renderer with both legacy fixtures:
  * all 559800 vertices are bit-identical, including their colors. */
 assert(hash==0x847eeed9u);
#endif
 printf("%08x %lu %lu\n",hash,vertices,indices);
}

static ActionEffectRecipes recipes;
static char text[16384],exported[16384];
static bool LavaOracleClip(void *context,const ActionEffectInstance *e,ActionEffectLocalRect *r) {
 (void)context;(void)e;
 r->x0=fmaxf(r->x0,-117.25f);r->x1=fminf(r->x1,149.75f);
 r->y0=fmaxf(r->y0,-137.5f);r->y1=fminf(r->y1,19.25f);
 return r->x0<r->x1&&r->y0<r->y1;
}
/* Pin the pre-cleanup lava output, including clipped moving illumination,
 * native flashes/sparks, molten detail and mist illumination across a seek. */
static void LavaRenderingOracle(void) {
 const unsigned saved_hash=hash;hash=2166136261u;
 unsigned long vertices=0,indices=0;
 const unsigned layers[]={kActionEffectRenderLayer_WorldDust,kActionEffectRenderLayer_Bg1HighPlane};
 for(unsigned lake=0;lake<2;++lake)for(unsigned shape=0;shape<3;++shape) {
   frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=3};
   frame.surface_fields[0]=*ActionSurfaceField_Bundled(0);
   frame.surface_fields[1]=*ActionSurfaceField_Bundled(1);
   ActionSurfaceField *f=&frame.surface_fields[lake];
   if(lake) {
     f->NativeReaction[1]=(float[]){8,40,64}[shape];f->NativeReaction[2]=shape+2;
     f->UpMotion[0]=shape*.5f;f->UpMotion[2]=(float[]){120,840,4096}[shape];
     f->BurstLight[1]=(float[]){96,56,8}[shape];
   }
   ActionSurfaceField_Prepare(f);
   const float half=lake?48+shape*128:24+shape*16;
   ActionEffectInstance *e=&frame.decorations[0];
   *e=(ActionEffectInstance){.kind=lake?kActionEffect_AitosLavaReservoir:kActionEffect_AitosLavaPit,
     .phase=lake?kActionEffectPhase_AitosLavaReservoir:kActionEffectPhase_AitosLavaPit,
     .generation=47,.pulse_generation=113+shape*9176,.world_x=500,.world_y=400,
     .flags=kActionEffectFlag_Visible,.render_layer=layers[lake],.projection_plane=kActionEffectProjectionPlane_Bg1High,
     .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-half,-16,half,16}},
     .tuning={.light_receivers_set=1,.light_receivers=7}};
   frame.native_bubble_count=lake?8:0;
   for(unsigned tick=0;tick<96;++tick) {
     e->pulse_ticks=(uint16_t)(tick*691);e->phase_ticks=(uint16_t)(tick*271);
     for(unsigned i=0;i<frame.native_bubble_count;++i)frame.native_bubbles[i]=(ActionNativeBubbleEvent){
       .owner=i==7?48:47,.seed=i*173,.x=(int16_t)(500-half+16+i*16),.y=396,
       .age=(uint8_t)((tick+i*3)%(4*(shape*7+1))),.cadence=(uint8_t)(shape*7+1)};
     for(unsigned layer=0;layer<2;++layer) {
       assert(ActionSceneDecorationRender_Build(&frame,layers[layer],tick%4!=1,tick%4!=2,
           Project,tick&1?LavaOracleClip:NULL,NULL,&batch));
       vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
       for(int i=0;i<batch.vertex_count;++i) {
         const ArRenderVertex2D *v=&batch.vertices[i];
         Float(v->position.x);Float(v->position.y);Float(v->color.r);Float(v->color.g);
         Float(v->color.b);Float(v->color.a);Float(v->tex_coord.x);Float(v->tex_coord.y);
       }
       for(int i=0;i<batch.index_count;++i)Word(batch.indices[i]);
     }
     ArRenderPointF points[7*41];float light[7*41];unsigned samples=0;
     for(int y=320;y<=416;y+=16)for(int x=160;x<=840;x+=17)
       points[samples++]=(ArRenderPointF){x,y};
     ActionLavaActivity_Sample(&frame,points,samples,light);
     for(unsigned i=0;i<samples;++i)Float(light[i]);
   }
 }
 assert(vertices==412342&&indices==1701162);
#if defined(__OPTIMIZE__) && defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
 /* This exact pre-cleanup oracle was captured on Apple Clang/arm64. Other
  * libm implementations may round the sine-driven effects differently. */
 assert(hash==0x71ef2004u);
#endif
 printf("lava %08x %lu %lu\n",hash,vertices,indices);
 hash=saved_hash;
 ActionLavaActivity_Sample(NULL,NULL,0,NULL);
}
static bool CaveParse(const char *s){unsigned line;return ActionEffectRecipes_Parse(&recipes,s,strlen(s),&line);}
static bool CaveProject(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p) {
  (void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;
}
static void TestCaveLifeRendering(void) {
  static ActionSceneEffectFrame frame;
  static ActionSceneEffectRenderBatch batch,repeat;
  assert(CaveParse("[effects]\nversion=1\n[emitter:04:04:0:cloud-bank:1]\n"
    "x=100\ny=100\nwidth=40\nheight=64\ntextured=1\nplume=1\nstrands=1\n"
    "activity=.58\nlifetime=480\nparticles=0\ncolor=dfb58d\ncolor-end=786a72\n"));
  unsigned quiet=0,active=0;float top=132,bottom=68;
  for(unsigned tick=0;tick<1200;tick+=7) {
    frame=(ActionSceneEffectFrame){0};ActionEffectRecipes_Apply(&recipes,4,4,0,tick,NULL,&frame);
    assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,CaveProject,NULL,NULL,&batch));
    if(!batch.vertex_count)++quiet;else ++active;
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderVertex2D *v=&batch.vertices[i];
      assert(isfinite(v->position.x)&&v->position.y>=68&&v->position.y<=132);
      assert(v->color.a>=0&&v->color.a<=.8f);
      top=fminf(top,v->position.y);bottom=fmaxf(bottom,v->position.y);
    }
  }
  assert(quiet>20&&active>20&&top<85&&bottom>124);
  /* At gameplay scale the puff needs a feathered crown and sides, rather
   * than a noisy sheet whose narrow fade disappears between pixels. */
  frame=(ActionSceneEffectFrame){0};ActionEffectRecipes_Apply(&recipes,4,4,0,180,NULL,&frame);
  for(unsigned tick=0;tick<600;tick+=19) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,CaveProject,NULL,NULL,&batch));
    if(!batch.vertex_count)continue;
    float u0=1,v0=1,u1=0,v1=0;
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderPointF uv=batch.vertices[i].tex_coord;
      u0=fminf(u0,uv.x);u1=fmaxf(u1,uv.x);v0=fminf(v0,uv.y);v1=fmaxf(v1,uv.y);
    }
    const float u=(u0+u1)*.5f,v=(v0+v1)*.5f;
    assert(ActionEffectMasks_Sample(u0+(u1-u0)*.04f,v)<.15f);
    assert(ActionEffectMasks_Sample(u1-(u1-u0)*.04f,v)<.15f);
    assert(ActionEffectMasks_Sample(u,v0+(v1-v0)*.04f)<.15f);
  }

  for(unsigned landing=1;landing<3;++landing) {
    char text[512];snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:04:04:0:drips:1]\n"
      "x=100\ny=100\nwidth=4\nheight=128\ndebris=1\nlanding=%u\nparticles=3\n"
      "activity=.13\nlifetime=930\nsize-min=.6\nsize-max=1.05\n",landing);
    assert(CaveParse(text));quiet=active=0;bool impact=false;
    for(unsigned tick=0;tick<1200;tick+=3) {
      frame=(ActionSceneEffectFrame){0};ActionEffectRecipes_Apply(&recipes,4,4,0,tick,NULL,&frame);
      assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,CaveProject,NULL,NULL,&batch));
      if(!batch.vertex_count)++quiet;else ++active;
      assert(batch.vertex_count<=48);
      for(int i=0;i<batch.vertex_count;++i) {
        assert(batch.vertices[i].position.y>=34&&batch.vertices[i].position.y<=165.1f);
        impact|=landing==1?batch.vertices[i].tex_coord.x!=0:batch.vertices[i].color.r==1;
      }
      if(tick==180)repeat=batch;
    }
    assert(quiet>280&&active>15&&impact);
    frame=(ActionSceneEffectFrame){0};ActionEffectRecipes_Apply(&recipes,4,4,0,180,NULL,&frame);
    assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,CaveProject,NULL,NULL,&batch));
    assert(batch.vertex_count==repeat.vertex_count&&!memcmp(batch.vertices,repeat.vertices,batch.vertex_count*sizeof(batch.vertices[0])));
  }
}
static void LakeLighting(void) {
 static ActionReceiverLighting lighting;
 static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=2};
 frame.surface_fields[1]=*ActionSurfaceField_Bundled(1);
 /* Isolate the extended source from the small, already-tested lip glow. */
 ActionSurfaceField *f=&frame.surface_fields[1];
 f->BurstLight[0]=0;f->UpMotion[0]=0;
 for(unsigned i=12;i<=24;i+=4)f->Body[i]=0;
 ActionSurfaceField_Prepare(f);
 ActionEffectInstance *e=&frame.decorations[0];
 *e=(ActionEffectInstance){.kind=kActionEffect_AitosLavaReservoir,.phase=kActionEffectPhase_AitosLavaReservoir,
   .world_x=500,.world_y=400,.generation=47,.pulse_generation=113,.pulse_ticks=137,
   .flags=kActionEffectFlag_Visible,.render_layer=kActionEffectRenderLayer_Bg1HighPlane,
   .projection_plane=kActionEffectProjectionPlane_Bg1High,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-320,-4,320,4}},
   .tuning={.light_receivers_set=1,.light_receivers=7}};
 assert(ActionEffectReceivers_Prepare(&frame,4,4,400,200,400,200,true,&lighting));
 float center[3],left[3],right[3],far[3],outside[3],add[3];
 ActionEffectReceivers_Sample(&lighting,2,500,336,center,add);
 ActionEffectReceivers_Sample(&lighting,2,300,336,left,add);
 ActionEffectReceivers_Sample(&lighting,2,700,336,right,add);
 ActionEffectReceivers_Sample(&lighting,2,500,208,far,add);
 ActionEffectReceivers_Sample(&lighting,2,500,432,outside,add);
 assert(center[0]>1.3f&&center[0]>center[1]&&center[1]>center[2]);
 assert(fabsf(left[0]-center[0])<.0001f&&fabsf(right[0]-center[0])<.0001f);
 assert(far[0]<center[0]&&outside[0]==1);
 /* Opaque scenery softens direct illumination while retaining bounced fill. */
 frame.scenery=(ActionMoonlightOcclusion){.valid=1,.count=1,
   .rectangles={{100,300,900,355}}};
 assert(ActionEffectReceivers_Prepare(&frame,4,4,400,200,400,200,true,&lighting));
 ActionEffectReceivers_Sample(&lighting,2,500,336,left,add);
 assert(left[0]<center[0]&&left[0]>1);
 frame.scenery=(ActionMoonlightOcclusion){0};
 /* Receiver switches, deterministic seeking and translation share the field. */
 for(unsigned mask=0;mask<8;++mask) {
   e->tuning.light_receivers=mask;
   for(unsigned role=1;role<=4;role*=2) {
     frame.light_receiver=role;
     assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&batch));
     assert((batch.vertex_count>0)==!!(mask&role));
   }
 }
 frame.light_receiver=0;e->tuning.light_receivers=7;
 assert(ActionEffectReceivers_Prepare(&frame,4,4,400,200,400,200,true,&lighting));
 ActionEffectReceivers_Sample(&lighting,2,500,336,left,add);
 assert(fabsf(left[0]-center[0])<.0001f);
 e->world_x+=27;
 assert(ActionEffectReceivers_Prepare(&frame,4,4,427,200,427,200,true,&lighting));
 ActionEffectReceivers_Sample(&lighting,2,527,336,left,add);
 assert(fabsf(left[0]-center[0])<.0001f);
 assert(ActionEffectReceivers_Prepare(&frame,4,4,427,200,427,200,false,&lighting));
 ActionEffectReceivers_Sample(&lighting,2,527,336,left,add);assert(left[0]==1);
 /* Four lakes totaling the twelve-segment budget fit CPU and GPU packets. */
 frame.decoration_count=frame.decoration_visible_count=4;
 for(unsigned i=0;i<4;++i) {
   frame.decorations[i]=*e;frame.decorations[i].world_x=200+i*300;
   frame.decorations[i].geometry.data.rect=(ActionEffectLocalRect){-144,-4,144,4};
 }
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,Project,NULL,NULL,&batch));
 ActionEffectSourceBatch source={.primitives=primitives,.capacity=kActionSourceMaximumPrimitives};
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,ActionEffectSource_ProjectPoint,NULL,&source,&batch));
 bool shadows=false;for(unsigned i=0;i<source.count;++i)shadows|=primitives[i].meta[3]!=0;
 assert(!source.failed&&source.count>0&&shadows);
 ActionSurfaceField bad=*f;bad.UpLight[1]=225;assert(!ActionSurfaceField_Valid(&bad));
 bad=*f;bad.UpLight[3]=1.1f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.UpLight[0]=1;assert(!ActionSurfaceField_Valid(&bad));
}
static void MoltenLake(void) {
 static ActionSceneEffectRenderBatch first,repeat;
 static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=2};
 ActionSurfaceField *f=&frame.surface_fields[1];*f=*ActionSurfaceField_Bundled(1);f->Particles[0]=0;
 ActionEffectInstance *e=&frame.decorations[0];
 *e=(ActionEffectInstance){.kind=kActionEffect_AitosLavaReservoir,.phase=kActionEffectPhase_AitosLavaReservoir,
   .generation=31,.pulse_generation=173,.world_x=500,.world_y=400,.flags=kActionEffectFlag_Visible,
   .render_layer=kActionEffectRenderLayer_Bg1HighPlane,.projection_plane=kActionEffectProjectionPlane_Bg1High,
   .tuning={.light_receivers_set=1,.light_receivers=7}};
 for(unsigned size=0;size<2;++size) {
   const float half=size?320:24;
   e->geometry=(ActionEffectGeometry){.kind=kActionEffectGeometry_Rect,.data.rect={-half,-4,half,4}};
   bool airborne=false,dark=false,changing=false,flash=false;
   for(unsigned tick=0;tick<960;tick+=13) {
     e->pulse_ticks=tick;
     assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&batch));
     assert(batch.vertex_count>0&&batch.vertex_count<=kActionSceneEffectMoltenVerticesPerPit);
     for(int v=0;v<batch.vertex_count;++v) {
       const ArRenderVertex2D *a=&batch.vertices[v];
       assert(isfinite(a->position.x)&&isfinite(a->position.y)&&isfinite(a->color.a));
       assert(a->position.x>=500-half&&a->position.x<=500+half);
       assert(a->position.y>375&&a->position.y<406);
       airborne|=a->position.y<390&&a->color.a>.1f;
       dark|=a->color.r<.25f&&a->color.a>.25f;
     }
     if(!tick)first=batch;
     else changing|=batch.vertex_count!=first.vertex_count||memcmp(first.vertices,batch.vertices,(size_t)batch.vertex_count*sizeof(batch.vertices[0]));
     for(float x=500-half;x<=500+half;x+=8)flash|=ActionLavaActivity_Light(&frame,x,390)>.2f;
     assert(ActionLavaActivity_Light(&frame,500,200)==0);
     assert(ActionLavaActivity_Light(&frame,500,420)==0); /* No flash in the deep lava fill. */
     assert(ActionLavaActivity_Light(&frame,1000,390)==0);
   }
   assert(airborne&&dark&&changing&&flash);
   e->pulse_ticks=0;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
   assert(first.vertex_count==repeat.vertex_count&&!memcmp(first.vertices,repeat.vertices,(size_t)first.vertex_count*sizeof(first.vertices[0])));
   e->world_x+=37;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
   for(int v=0;v<first.vertex_count;++v)assert(fabsf(repeat.vertices[v].position.x-first.vertices[v].position.x-37)<.001f);
   e->world_x-=37;
 }
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,true,false,Project,NULL,NULL,&batch));
 assert(batch.vertex_count==0); /* Particle toggle owns all visible molten detail. */
 f->Components[0]=1;
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,true,true,Project,NULL,NULL,&batch));
 assert(batch.vertex_count==0);f->Components[0]=3;
 /* The same local event illuminates nearby smoke; zero strength, a disabled
  * source, a non-scenery receiver or a distant point cannot tint it. */
 float peak=0,peak_x=0;unsigned peak_tick=0;
 for(unsigned tick=0;tick<360;++tick) {
   e->pulse_ticks=tick;
   for(float x=180;x<820;x+=16) {
     const float light=ActionLavaActivity_Light(&frame,x,380);
     if(light>peak){peak=light;peak_x=x;peak_tick=tick;}
   }
 }
 assert(peak>.5f);e->pulse_ticks=peak_tick;
 e->world_x+=37;assert(fabsf(ActionLavaActivity_Light(&frame,peak_x+37,380)-peak)<.0001f);e->world_x-=37;
 e->flags|=kActionEffectFlag_LightingOff;assert(ActionLavaActivity_Light(&frame,peak_x,380)==0);e->flags&=~kActionEffectFlag_LightingOff;
 e->tuning.light_receivers=6;assert(ActionLavaActivity_Light(&frame,peak_x,380)==0);e->tuning.light_receivers=7;
 const char smoke[]="[effects]\nversion=1\n[emitter:06:02:0:cloud-bank:1]\nx=500\ny=380\nwidth=32\nheight=24\nanchor=bg1\nplacement=background\ntextured=1\nrolling=.8\nvolume=.7\nstrands=1\ncolor=603018\ncolor-end=201820\n";
 unsigned line;assert(ActionEffectRecipes_Parse(&recipes,smoke,sizeof(smoke)-1,&line));
 ActionEffectRecipes_Apply(&recipes,6,2,0,peak_tick,NULL,&frame);
 assert(frame.authored_count==1);frame.authored[0].world_x=(int)peak_x;
 assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&first));
 f->BurstLight[0]=0;
 assert(ActionLavaActivity_Light(&frame,peak_x,380)==0);
 assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&repeat));
 assert(first.vertex_count==repeat.vertex_count&&first.vertex_count>0);
 bool smoke_lit=false;
 for(int v=0;v<first.vertex_count;++v) {
   assert(!memcmp(&first.vertices[v].position,&repeat.vertices[v].position,sizeof(ArRenderPointF)));
   smoke_lit|=first.vertices[v].color.r>repeat.vertices[v].color.r+.05f;
 }
 assert(smoke_lit);frame.authored_count=0;f->BurstLight[0]=.9f;
 /* Maximum source widths/counts, with shadows and local flashes, fit both
  * ordinary geometry and resident GPU packets on each independent layer. */
 for(unsigned layout=0;layout<2;++layout) {
   frame.decoration_count=frame.decoration_visible_count=layout?2:4;
   e->geometry.data.rect=(ActionEffectLocalRect){layout?-288:-144,-4,layout?288:144,4};
   for(unsigned i=1;i<frame.decoration_count;++i){frame.decorations[i]=*e;frame.decorations[i].pulse_generation+=i*97;}
   for(unsigned tick=0;tick<480;tick+=17) {
     for(unsigned i=0;i<frame.decoration_count;++i)frame.decorations[i].pulse_ticks=tick;
     const unsigned layers[]={kActionEffectRenderLayer_WorldDust,kActionEffectRenderLayer_Bg1HighPlane};
     for(unsigned j=0;j<2;++j) {
       assert(ActionSceneDecorationRender_Build(&frame,layers[j],true,true,Project,NULL,NULL,&batch));
       ActionEffectSourceBatch source={.primitives=primitives,.capacity=kActionSourceMaximumPrimitives};
       assert(ActionSceneDecorationRender_Build(&frame,layers[j],true,true,ActionEffectSource_ProjectPoint,ActionEffectSource_ClipBounds,&source,&batch));
       assert(!source.failed&&source.count<=kActionSourceMaximumPrimitives);
     }
   }
 }
 ActionSurfaceField bad=*f;bad.BurstLight[0]=1.1f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*f;bad.BurstLight[1]=7;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(2);bad.MoltenBursts[0]=1;assert(!ActionSurfaceField_Valid(&bad));
}

static void MoltenDetail(void){
 static ActionSceneEffectRenderBatch first,repeat;
 static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=1};
 frame.surface_fields[0]=*ActionSurfaceField_Bundled(0);
 frame.surface_fields[0].Particles[0]=0; /* Isolate the new detail. */
 frame.decorations[0]=(ActionEffectInstance){.kind=kActionEffect_AitosLavaPit,.phase=kActionEffectPhase_AitosLavaPit,
   .generation=31,.pulse_generation=173,.world_x=128,.world_y=100,.flags=kActionEffectFlag_Visible,
   .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-64,-16,64,16}},
   .tuning={.light_receivers_set=1,.light_receivers=7}};
 bool airborne=false,dark=false,changed=false;
 for(unsigned tick=0;tick<960;tick+=11){
   frame.decorations[0].pulse_ticks=tick;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&batch));
   assert(batch.vertex_count>0&&batch.vertex_count<=kActionSceneEffectMoltenVerticesPerPit);
   for(int i=0;i<batch.vertex_count;++i){const ArRenderVertex2D *v=&batch.vertices[i];
     assert(isfinite(v->position.x)&&isfinite(v->position.y)&&isfinite(v->color.a));
     assert(v->position.x>=64&&v->position.x<=192); /* No crust or droplets over the banks. */
     assert(v->position.y>30&&v->position.y<116);
     airborne|=v->position.y<65&&v->color.a>.1f;
     dark|=v->color.r<.25f&&v->color.a>.25f;
   }
   if(!tick)first=batch;
   else changed|=batch.vertex_count!=first.vertex_count||memcmp(batch.vertices,first.vertices,(size_t)batch.vertex_count*sizeof(batch.vertices[0]))!=0;
 }
 assert(airborne&&dark&&changed);
 frame.decorations[0].pulse_ticks=0;
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
 assert(first.vertex_count==repeat.vertex_count&&!memcmp(first.vertices,repeat.vertices,(size_t)first.vertex_count*sizeof(first.vertices[0])));
 frame.decorations[0].world_x+=37;
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
 assert(first.vertex_count==repeat.vertex_count);
 for(int i=0;i<first.vertex_count;++i){assert(fabsf(repeat.vertices[i].position.x-first.vertices[i].position.x-37)<.001f);assert(fabsf(repeat.vertices[i].position.y-first.vertices[i].position.y)<.001f);}
 for(unsigned mask=0;mask<8;++mask){frame.decorations[0].tuning.light_receivers=mask;
   assert(ActionEffectReceivers_Layer(&frame.decorations[0])==kActionEffectRenderLayer_Bg1Plane);
   for(unsigned role=1;role<=4;role*=2){frame.light_receiver=role;
     assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Plane,true,false,Project,NULL,NULL,&batch));
     assert((batch.vertex_count>0)==!!(mask&role));}}
 frame.light_receiver=0;
 /* The maximum editable source/cell/burst counts fit both CPU and GPU
  * packet budgets, with lighting in its separate existing receiver batch. */
 frame.surface_fields[0].MoltenSurface[1]=16;frame.surface_fields[0].MoltenBursts[0]=4;
 frame.surface_fields[0].Particles[0]=12;
 frame.decoration_count=frame.decoration_visible_count=16;
 for(unsigned i=1;i<16;++i){frame.decorations[i]=frame.decorations[0];frame.decorations[i].pulse_generation+=i*97;}
 for(unsigned tick=0;tick<480;tick+=17){
   for(unsigned i=0;i<16;++i)frame.decorations[i].pulse_ticks=tick;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&batch));
   ActionEffectSourceBatch source={.primitives=primitives,.capacity=kActionSourceMaximumPrimitives};
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,ActionEffectSource_ProjectPoint,ActionEffectSource_ClipBounds,&source,&batch));
   assert(!source.failed&&source.count<=kActionSourceMaximumPrimitives);
 }
 ActionSurfaceField bad=*ActionSurfaceField_Bundled(0);bad.MoltenSurface[1]=17;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.MoltenBursts[2]=.9f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.Projection[1]=0;assert(!ActionSurfaceField_Valid(&bad));
}
static void MovingLavaAndNativeReactions(void) {
 static ActionSceneEffectRenderBatch first,second;
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=2};
 ActionSurfaceField *f=&frame.surface_fields[1];*f=*ActionSurfaceField_Bundled(1);
 f->BurstLight[0]=0;f->Particles[0]=0;f->MoltenSurface[0]=0;f->MoltenBursts[0]=0;
 for(unsigned i=12;i<=24;i+=4)f->Body[i]=f->Spill[i]=0;
 ActionSurfaceField_Prepare(f);
 ActionEffectInstance *e=&frame.decorations[0];
 *e=(ActionEffectInstance){.kind=kActionEffect_AitosLavaReservoir,.phase=kActionEffectPhase_AitosLavaReservoir,
   .generation=47,.pulse_generation=113,.world_x=400,.world_y=400,.flags=kActionEffectFlag_Visible,
   .render_layer=kActionEffectRenderLayer_Bg1HighPlane,.projection_plane=kActionEffectProjectionPlane_Bg1High,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-144,-4,144,4}},
   .tuning={.light_receivers_set=1,.light_receivers=7}};
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&first));
 e->pulse_ticks=180;
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&second));
 assert(first.vertex_count==second.vertex_count&&first.vertex_count>0);
 unsigned changed=0;
 for(int i=0;i<first.vertex_count;++i) {
   if(first.vertices[i].color.a>0||second.vertices[i].color.a>0)
     assert(!memcmp(&first.vertices[i].position,&second.vertices[i].position,sizeof(ArRenderPointF)));
   changed+=fabsf(first.vertices[i].color.a-second.vertices[i].color.a)>.01f;
 }
 assert(changed>20); /* Moving illumination reuses the same receiver mesh. */
 e->pulse_ticks=0;assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&second));
 assert(!memcmp(first.vertices,second.vertices,first.vertex_count*sizeof(first.vertices[0])));
 f->UpLight[0]=0;ActionSurfaceField_Prepare(f);
 frame.native_bubble_count=1;
 frame.native_bubbles[0]=(ActionNativeBubbleEvent){.owner=47,.seed=187,.x=400,.y=396,.cadence=8};
 bool flashed=false,sparked=false;
 for(unsigned age=0;age<32;++age) {
   frame.native_bubbles[0].age=age;
   assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&first));
   bool visible=false;for(int v=0;v<first.vertex_count;++v)visible|=first.vertices[v].color.a>0;
   assert(visible==(age>12&&age<30));flashed|=visible;
   assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,Project,NULL,NULL,&second));
   assert(second.vertex_count==(age>=16?12:0));sparked|=second.vertex_count>0;
   if(age>=16)for(int v=0;v<second.vertex_count;++v)assert(second.vertices[v].position.y<399&&second.vertices[v].position.y>378);
 }
 assert(flashed&&sparked);
 frame.native_bubbles[0].age=21;
 assert(ActionLavaActivity_Light(&frame,400,384)>.3f);
 frame.native_bubbles[0].owner=48;
 assert(ActionLavaActivity_Light(&frame,400,384)==0);
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,Project,NULL,NULL,&first));
 for(int v=0;v<first.vertex_count;++v)assert(first.vertices[v].color.a==0);
 /* The batched sampler must retain ownership and receiver filtering across
  * multiple lakes, including a full captured mound list and empty samples. */
 frame.decoration_count=frame.decoration_visible_count=2;
 frame.decorations[1]=*e;frame.decorations[1].generation=48;frame.decorations[1].world_x=900;
 frame.native_bubble_count=kActionNativeBubbleMax;
 for(unsigned i=0;i<kActionNativeBubbleMax;++i)frame.native_bubbles[i]=(ActionNativeBubbleEvent){
   .owner=i&1?48:47,.x=i&1?900:400,.y=396,.age=21,.cadence=8};
 const ArRenderPointF points[]={{400,382},{900,382},{-400,382},{650,382}};
 float light[4];
 ActionLavaActivity_Sample(&frame,points,4,light);
 assert(light[0]==1&&light[1]==1&&light[2]==0&&light[3]==0);
 frame.decorations[1].tuning.light_receivers=6;
 ActionLavaActivity_Sample(&frame,points,4,light);
 assert(light[0]==1&&light[1]==0&&light[2]==0&&light[3]==0);
 assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,Project,NULL,NULL,&first));
 f->Components[0]=0;
 ActionLavaActivity_Sample(&frame,points,4,light);
 for(unsigned i=0;i<4;++i)assert(light[i]==0);
 ActionSurfaceField bad=*f;bad.UpMotion[0]=1.01f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*f;bad.UpMotion[2]=119;assert(!ActionSurfaceField_Valid(&bad));
 bad=*f;bad.NativeReaction[2]=4.5f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.NativeReaction[0]=.5f;assert(!ActionSurfaceField_Valid(&bad));
}
static void NativeBubbles(void) {
 uint8_t art[1024],saved[1024],tile[32];
 for(unsigned i=0;i<sizeof(art);++i)art[i]=(uint8_t)(i/256*33+(i&31));
 memcpy(saved,art,sizeof(art));
 ActionLavaBubbles s={.characters=art,.chance=1,.pause_min=48,.pause_max=144,.cadence=8,.seed=9176,.room=4};
 int previous=-1,last_end=-1;unsigned different=0,bursts=0,phases=0;
 for(unsigned tick=0;tick<6000;++tick) {
  s.clock=(uint16_t)tick;
  const int phase=ActionLavaBubbles_Phase(&s,20,37);
  if(phase!=ActionLavaBubbles_Phase(&s,24,37))++different;
  const int age=ActionLavaBubbles_Age(&s,20,37);
  assert(phase==(age<0?-1:age/s.cadence));
  if(phase>=0)phases|=1u<<phase;
  if(phase<0&&previous==3)last_end=(int)tick;
  if(phase==0&&previous<0&&last_end>=0){
   assert((int)tick-last_end>=48&&(int)tick-last_end<=144);++bursts;
  }
  for(unsigned q=0;q<4;++q) {
   const unsigned word=(q/2)*4+(q&1);
   assert(ActionLavaBubbles_Character(&s,2,(uint16_t)(0x3000|word),40+(q&1),74+q/2,tile));
   for(unsigned i=0;i<32;++i)assert(tile[i]==(phase<0?0:art[phase*256+word*32+i]));
  }
  previous=phase;
 }
 assert(bursts>30&&different>1000&&phases==15&&!memcmp(art,saved,sizeof(art)));
 s.clock=137;const int phase=ActionLavaBubbles_Phase(&s,20,37);
 s.clock=UINT16_MAX;(void)ActionLavaBubbles_Phase(&s,20,37);
 s.clock=137;assert(ActionLavaBubbles_Phase(&s,20,37)==phase);
 assert(!ActionLavaBubbles_Character(&s,1,0x3002,40,74,tile));
 assert(!ActionLavaBubbles_Character(&s,3,0x10a0,40,74,tile));
 assert(!ActionLavaBubbles_Character(&s,2,0x1234,40,74,tile));
 s.chance=0;assert(ActionLavaBubbles_Character(&s,2,0x3004,40,75,tile));
 for(unsigned i=0;i<32;++i)assert(!tile[i]);
 s.characters=NULL;assert(!ActionLavaBubbles_Character(&s,2,0x3004,40,75,tile));
 ActionSurfaceField f=*ActionSurfaceField_Bundled(1);
 assert(f.NativeBubbles[0]==1);
 f.NativeBubbles[2]=145;assert(!ActionSurfaceField_Valid(&f));
 f=*ActionSurfaceField_Bundled(1);f.NativeBubbles[4]=0;assert(!ActionSurfaceField_Valid(&f));
 f=*ActionSurfaceField_Bundled(1);f.NativeBubbles[5]=65536;assert(!ActionSurfaceField_Valid(&f));
 f=*ActionSurfaceField_Bundled(0);f.NativeBubbles[0]=1;assert(!ActionSurfaceField_Valid(&f));
}
int main(void){
 TestCaveLifeRendering();
 NativeBubbles();
 MovingLavaAndNativeReactions();
 FILE *legacy=fopen("tests/fixtures/legacy-lava-pit-field.ini","rb");assert(legacy);
 size_t bytes=fread(text,1,sizeof(text)-1,legacy);fclose(legacy);text[bytes]=0;
 unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,bytes,&line));
 legacy_pit=recipes.surface_fields[0];
 assert(legacy_pit.MoltenSurface[0]==0&&legacy_pit.MoltenBursts[0]==0);
 assert(legacy_pit.Projection[1]==0&&legacy_pit.Receivers[0]==8);
 assert(ActionSurfaceField_Write(&legacy_pit,exported,sizeof(exported))&&strstr(exported,"molten-surface=0 "));
 legacy=fopen("tests/fixtures/legacy-lava-lake-field.ini","rb");assert(legacy);
 bytes=fread(text,1,sizeof(text)-1,legacy);fclose(legacy);text[bytes]=0;
 assert(ActionEffectRecipes_Parse(&recipes,text,bytes,&line));legacy_lake=recipes.surface_fields[0];
 assert(legacy_lake.UpLight[0]==0&&legacy_lake.Receivers[0]==8);
 assert(legacy_lake.MoltenSurface[0]==0&&legacy_lake.MoltenBursts[0]==0&&legacy_lake.BurstLight[0]==0);
 assert(legacy_lake.NativeBubbles[0]==0&&legacy_lake.UpMotion[0]==0&&legacy_lake.NativeReaction[0]==0&&legacy_lake.NativeReaction[2]==0);
 assert(ActionSurfaceField_Write(&legacy_lake,exported,sizeof(exported))&&strstr(exported,"up-light=0 "));
 LegacyOracle();
 LavaRenderingOracle();
 MoltenDetail();
 LakeLighting();
 MoltenLake();
 const char *names[]={"lava-pit","lava-lake","splash","waterfall","waterfall-mist"};
 for(unsigned index=0;index<5;++index){
  char path[96];snprintf(path,sizeof(path),"assets/effects/%s-field.ini",names[index]);
  FILE *file=fopen(path,"rb");assert(file);size_t n=fread(text,1,sizeof(text)-1,file);fclose(file);text[n]=0;
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,n,&line));assert(recipes.surface_field_count==1);
  const ActionSurfaceField *field=ActionSurfaceField_Bundled(index);assert(field&&!memcmp(field,&recipes.surface_fields[0],sizeof(*field)));
  assert(ActionSurfaceField_Write(field,exported,sizeof(exported)));ActionSurfaceField copy={0};
  for(char *at=exported;*at;){char *end=strchr(at,'\n');assert(end);*end=0;char *eq=strchr(at,'=');assert(eq);*eq=0;assert(ActionSurfaceField_Set(&copy,at,eq+1));at=end+1;}
  assert(ActionSurfaceField_Valid(&copy));ActionSurfaceField_Prepare(&copy);assert(!memcmp(&copy,field,sizeof(copy)));
  ActionSurfaceField *f=&recipes.surface_fields[0];f->Mode[0]=1;f->SourceCount[0]=1;
  memcpy(f->Source1,(float[]){128,100,-32,-16,32,16,7},sizeof(f->Source1));f->Receivers[0]=7;
  ActionSurfaceField_Prepare(f);assert(ActionSurfaceField_Valid(f));
  recipes.records[0].group=6;recipes.records[0].room=2;
  ActionEnvironmentScene scene={.group=6,.room=2,.clock=456};
  frame=(ActionSceneEffectFrame){0};
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);assert(frame.surface_fields_valid==(1u<<index));
  assert(frame.decoration_count==1&&frame.decorations[0].world_x==128&&frame.decorations[0].world_y==100);
  assert(frame.decorations[0].tuning.light_receivers==7&&frame.decorations[0].tuning.light_receivers_set);
  frame.decorations[0].tuning.light_receivers_set=0;unsigned vertices=0;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));vertices+=batch.vertex_count;}
  assert(vertices>0);unsigned previous_hash=frame.surface_fields[index].hash;
  f->Motion[1]+=1;ActionSurfaceField_Prepare(f);assert(frame.surface_fields[index].hash==previous_hash);
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);assert(frame.decoration_count==1&&frame.surface_fields[index].hash!=previous_hash);
  recipes.records[0].enabled=0;ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);
  assert(frame.surface_fields[index].Components[0]==0);vertices=0;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));vertices+=batch.vertex_count;}
  assert(!vertices);
  ActionSurfaceField bad=*field;bad.Spill[8]=2;assert(!ActionSurfaceField_Valid(&bad));
  bad=*field;bad.Clock[2]=0;assert(!ActionSurfaceField_Valid(&bad));bad=*field;bad.Particles[0]=129;assert(!ActionSurfaceField_Valid(&bad));
  bad=*field;bad.Mode[0]=1;bad.SourceCount[0]=1;assert(!ActionSurfaceField_Valid(&bad));
 }
 ActionHeatRenderMesh before,after;
 ActionSurfaceField heat=*ActionSurfaceField_Bundled(1);
 assert(ActionHeatRender_Build(219,(ArRenderRectI){0,0,640,360},640,360,256,&before));
 assert(ActionHeatRender_BuildWithField(&heat,219,(ArRenderRectI){0,0,640,360},640,360,256,&after));
 assert(!memcmp(&before,&after,sizeof(before)));
 heat.HeatAmplitude[1]*=.5f;ActionSurfaceField_Prepare(&heat);
 assert(ActionHeatRender_BuildWithField(&heat,219,(ArRenderRectI){0,0,640,360},640,360,256,&after));
 assert(before.vertex_count==after.vertex_count&&before.index_count==after.index_count);
 assert(memcmp(before.vertices,after.vertices,sizeof(before.vertices)));
 puts("surface definitions: exact native geometry, source placement, ownership, toggles and heat passed");
}
