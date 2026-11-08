/* Collector owns the receiver sidecar just like clip W, UVs and lighting. */
static void TestOwnedHeatQueue(ArSdlDioramaDepthSink *state,ArRenderDevice *device,
    ArRenderTexture target,ArRenderTexture white,ArRenderTexture black) {
  for(unsigned mode=1;mode<=2;++mode) {
    const DioramaDepthSink *sink=Begin(state,device,target);
    ArRenderVertex2D v[]={{{4,4},{1,1,1,1},{.1f,.1f}},{{12,4},{1,1,1,1},{.2f,.1f}},
        {{12,12},{1,1,1,1},{.2f,.2f}},{{4,12},{1,1,1,1},{.1f,.2f}}};
    float depth[]={.4f,.4f,.4f,.4f},w[]={1,1.25f,1.25f,1};int32_t ix[]={0,1,2,0,2,3};
    ArRenderPointF source[]={{100,160},{132,160},{132,192},{100,192}},saved[4];memcpy(saved,source,sizeof(saved));
    DioramaSceneryLightEffectParams light={.illumination=black,.shade={1,1,1,1},
        .heat={.source={400,800,1024,2048},.bounds={{80,120,180,240}}},
        .heat_owned={1/256.f,1/256.f,(float)mode,1}};
    DioramaDepthSubmission draw={.texture=white,.vertices=v,.depths=depth,.indices=ix,.vertex_count=4,.index_count=6,
        .blend=kArRenderBlendMode_Alpha,.lighting=&light,.clip_w=w,.heat_world_xy=source};
    assert(sink->submit(sink->userdata,device,&draw));assert(state->count==1&&state->vertex_count==4);
    memset(source,0xa5,sizeof(source));
    for(unsigned i=0;i<4;++i)assert(!memcmp(&state->vertices[i].heat_world_xy,&saved[i],sizeof(saved[i])));
    for(unsigned bad=0;bad<5;++bad) {
      memcpy(source,saved,sizeof(source));draw.heat_world_xy=source;draw.clip_w=w;draw.native_actor=false;
      if(!bad)draw.heat_world_xy=NULL;
      if(bad==1)source[2].x=NAN;
      if(bad==2)source[1].y=INFINITY;
      if(bad==3)draw.clip_w=NULL;
      if(bad==4)draw.native_actor=true;
      assert(!sink->submit(sink->userdata,device,&draw));assert(state->count==1&&state->vertex_count==4);
      for(unsigned i=0;i<4;++i)assert(!memcmp(&state->vertices[i].heat_world_xy,&saved[i],sizeof(saved[i])));
    }
    /* A following ordinary draw, and a reused array in the next Begin, must
     * initialize absent sidecars rather than leak the previous material. */
    light.heat_owned=(DioramaHeatOwned){0};draw.heat_world_xy=NULL;draw.clip_w=NULL;draw.native_actor=false;
    assert(sink->submit(sink->userdata,device,&draw));assert(state->count==2&&state->vertex_count==8);
    for(unsigned i=4;i<8;++i)assert(state->vertices[i].heat_world_xy.x==0&&state->vertices[i].heat_world_xy.y==0);
    memset(&light,0,sizeof(light));assert(sink->finish(sink->userdata,device,true));Check(device,target,8,8,0xffffffff);
    sink=Begin(state,device,target);draw.lighting=NULL;
    assert(sink->submit(sink->userdata,device,&draw));assert(state->count==1&&state->vertex_count==4);
    for(unsigned i=0;i<4;++i)assert(state->vertices[i].heat_world_xy.x==0&&state->vertices[i].heat_world_xy.y==0);
    assert(sink->finish(sink->userdata,device,true));Check(device,target,8,8,0xffffffff);
  }
  puts("Owned heat queue: borrowed sidecar/material ownership, rejected invalid submissions and NULL/reuse zero defaults passed");
}
