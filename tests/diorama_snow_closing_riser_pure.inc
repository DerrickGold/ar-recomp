static bool SnowRiserReceiver(const void*unused,int x,int y,int*bottom) {
  (void)unused;(void)x;*bottom=(y/16+1)*16;if(*bottom>y+8)*bottom=y+8;return true;
}
static void SnowRiserPure(void) {
  static DioramaGroundCoverVertex expected[8192];
  for(unsigned end=0;end<2;++end)for(unsigned actor=0;actor<2;++actor) {
    ActionEffectInstance e=Example();e.field_style.points[0].y=end?0:160;e.field_style.points[1].y=end?160:0;
    ActionGroundCoverBank bank;assert(ActionGroundCover_Prepare(&e,&bank));
    DioramaGroundCoverOwner owner={.asset=&bank,.revision=100+end*2+actor};
    DioramaGroundCoverCap cap=Cap(0,.2f,0);cap.from=(ActionGroundCoverPoint){end?78:80,280};cap.to=(ActionGroundCoverPoint){end?80:82,280};cap.receiver=SnowRiserReceiver;cap.closing_riser[end]=2;
    for(unsigned j=0;j<4;++j){cap.corners[j].position[0]=(j==1||j==2)?cap.to.x:cap.from.x;cap.corners[j].position[1]=280;}
    ActionSceneEffectFrame frame={0};if(actor){frame.actor_count=1;frame.actors[0]=(ActionEffectActor){.visible=true,.player=true,.x=80,.feet_y=280,.body_radius=8};}
    ActionGroundCoverPressure pressure;assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
    unsigned n;assert(DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,vertices,8192,&n)&&n);
    DioramaGroundCoverCap lower=cap;lower.closing_riser[0]=lower.closing_riser[1]=0;
    lower.from=(ActionGroundCoverPoint){end?80:79,282};lower.to=(ActionGroundCoverPoint){end?81:80,282};
    for(unsigned j=0;j<4;++j){lower.corners[j].position[0]=(j==1||j==2)?lower.to.x:lower.from.x;lower.corners[j].position[1]=282;}
    ActionGroundCoverPressure lp;assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&lower,&lp));
    unsigned upper_count=0,lower_count=0;
    for(unsigned i=0;i<n;++i) {
      const DioramaGroundCoverVertex*v=vertices+i;Near(v->position[0],80);assert(v->position[2]>=0&&v->position[2]<=.200001f);
      const bool down=v->sample.native_floor_y==282;assert(down||v->sample.native_floor_y==280);
      DioramaGroundCoverVertex known;assert(GroundCoverSample(&bank,down?&lp:&pressure,down?&lower:&cap,down?(float)!end:(float)end,v->position[2]/.2f,&known));
      Near(v->position[1],known.position[1]);Near(v->sample.alpha,known.sample.alpha);Near(v->sample.height,known.sample.height);Near(v->sample.compression,known.sample.compression);Near(v->sample.crest_mix,known.sample.crest_mix);
      if(down)++lower_count;else++upper_count;
    }assert(upper_count&&lower_count);
    unsigned m;assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,&pressure,&cap,1.01f,expected,8192,&m)&&m);
    unsigned matches=0;for(unsigned i=0;i<m;++i)if(fabsf(expected[i].native_x-80)<.00001f)for(unsigned j=0;j<n;++j)
      if(fabsf(expected[i].position[2]-vertices[j].position[2])<.00001f&&vertices[j].sample.native_floor_y==280){Near(expected[i].position[1],vertices[j].position[1]);Near(expected[i].sample.alpha,vertices[j].sample.alpha);++matches;break;}
    assert(matches);
    cap.closing_riser[end]=0;assert(DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,NULL,0,&n)&&!n);
    cap.closing_riser[end]=3;assert(DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,NULL,0,&n)&&!n);
    cap.closing_riser[end]=2;assert(!DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,vertices,2,&n)&&!n);
    /* The upper contact alone cannot close a bank that terminates exactly at
     * the step: the lower endpoint must belong to this same bank too. */
    ActionEffectInstance stopped=e;
    if(end){stopped.field_style.points[1].x=80;stopped.field_style.points[1].y=80;}
    else {stopped.field_style.points[0].x=80;stopped.field_style.points[0].y=80;}
    assert(ActionGroundCover_Prepare(&stopped,&bank));
    assert(DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,NULL,0,&n)&&!n);
    e.world_y+=30;assert(ActionGroundCover_Prepare(&e,&bank));assert(DioramaGroundCover_BuildClosingRisers(&cache,owner,&bank,&frame,&pressure,&cap,1.01f,NULL,0,&n)&&!n);
  }
  puts("PASS Snow closing-riser endpoint pressure/alpha/phase and negative ownership controls");
}
