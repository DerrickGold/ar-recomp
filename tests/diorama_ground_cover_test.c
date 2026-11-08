#include "diorama/diorama_ground_cover.inc"
#include "support/test_assert.h"
#include <stdio.h>

static DioramaGroundCoverCache cache;
static DioramaGroundCoverVertex vertices[8192];
static void Near(float a,float b) { assert(fabsf(a-b)<.0002f); }
static ActionEffectInstance Example(void) {
  return (ActionEffectInstance){.kind=kActionEffect_AuthoredCloud,
      .flags=kActionEffectFlag_Visible,.world_y=200,
      .render_layer=kActionEffectRenderLayer_WorldDust,
      .projection_plane=kActionEffectProjectionPlane_Bg1,
      .tuning={.color=0xd8e6f6,.intensity=2},
      .particle_style={.color_end=0xfff4df,.seed=621},
      .field_style={.placement=2,.ground_depth=4,.ground_width=8,
          .point_count=2,.points={{0,0},{160,0}}}};
}
static DioramaGroundCoverCap Cap(float near,float far,float actor) {
  DioramaGroundCoverCap cap={.from={48,200},.to={112,200},
      .actor_z=actor,.contact_radius=.04f};
  for(unsigned i=0;i<4;++i) {
    const bool end=i==1||i==2,back=i>=2;
    cap.corners[i]=(DioramaGroundCoverCorner){.position={end?112:48,200,back?far:near},
        .lift={0,-1,0},.depth_fraction=back?1:0};
  }
  return cap;
}
static void ActualContactAndTriangleSupport(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  const DioramaGroundCoverOwner owner={.asset=&bank,.revision=1};
  const ActionGroundCoverPressure pressure={.feet={{80,8,1},{0}}};
  /* Core: actor at rear. Side room: actor at near. Generic: interior. */
  const float spans[3][3]={{0,-.24f,-.24f},{-.24f,-.42f,-.24f},{0,.24f,.12f}};
  for(unsigned mode=0;mode<3;++mode) {
    DioramaGroundCoverCap cap=Cap(spans[mode][0],spans[mode][1],spans[mode][2]);
    unsigned n;assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,&pressure,&cap,0,
        vertices,8192,&n)&&n>0&&n%3==0);
    bool center=false,quiet=false;
    for(unsigned i=0;i<n;++i) {
      const DioramaGroundCoverVertex *v=&vertices[i];
      Near(v->position[0],v->native_x);Near(v->position[1]+v->sample.height,200);
      assert(v->sample.alpha<1&&v->position[2]>=fminf(spans[mode][0],spans[mode][1])-.0001f&&
          v->position[2]<=fmaxf(spans[mode][0],spans[mode][1])+.0001f);
      if(fabsf(v->native_x-80)<.0001f&&fabsf(v->position[2]-cap.actor_z)<.0001f) {
        Near(v->sample.compression,1);Near(v->sample.height,v->sample.settled_height*.65f);center=true;
      }
      if(fabsf(v->native_x-80)<.0001f&&fabsf(v->position[2]-cap.actor_z)>.08f) {
        Near(v->sample.compression,0);Near(v->sample.height,v->sample.settled_height);quiet=true;
      }
    }
    assert(center&&quiet);
  }
  /* A folded/authored nonplanar quad must remain on its original 012/023
   * triangles. Bilinear interpolation would give 12*t*d and fail this. */
  DioramaGroundCoverCap cap=Cap(0,-.24f,-.12f);
  cap.corners[2].position[1]+=12;
  for(unsigned i=0;i<4;++i)cap.corners[i].lift[0]=.25f;
  unsigned n;assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n));
  for(unsigned i=0;i<n;++i) {
    const DioramaGroundCoverVertex *v=&vertices[i];
    const float t=(v->native_x-48)/64,d=v->position[2]/-.24f;
    Near(v->position[0]-v->sample.height*.25f,v->native_x);
    Near(v->position[1]+v->sample.height,200+12*fminf(t,d));
  }
}
static void CacheAndEmptyOwnership(void) {
  memset(&cache,0,sizeof(cache));
  ActionEffectInstance e=Example();ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  DioramaGroundCoverOwner owner={.asset=&bank,.revision=10,.kind=1};
  DioramaGroundCoverCap cap=Cap(0,.24f,.12f);unsigned n;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n);
  assert(cache.misses==1&&cache.hits==0);
  const ActionGroundCoverPressure pressure={.feet={{80,8,.75f},{0}}};
  /* Projection/actual lift changes are not immutable support association. */
  for(unsigned i=0;i<4;++i)cap.corners[i].position[0]+=30;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,&pressure,&cap,0,vertices,8192,&n));
  assert(cache.misses==1&&cache.hits==1);
  ++owner.revision;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n));
  assert(cache.misses==2);
  ++owner.kind;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n));
  assert(cache.misses==3);
  e.field_style.points[1].y=1;assert(ActionGroundCover_Prepare(&e,&bank));
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n==0);
  assert(cache.misses==4);
  assert(ActionGroundCover_SuppressesFlatTop(ActionGroundCover_ResolveClaim(true,true,true,n)));
  /* A different-height ledge and an absent top are successful emptiness. */
  cap.from.y=cap.to.y=230;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,NULL,0,&n)&&n==0);
  e=Example();assert(ActionGroundCover_Prepare(&e,&bank));cap=Cap(0,.24f,.12f);
  n=123;assert(!DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,3,&n)&&n==0);
  cap.contact_radius=0;
  assert(!DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n==0);
}
static void SplitAndTaperedReceivers(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  const DioramaGroundCoverOwner owner={.asset=&bank,.revision=33};
  DioramaGroundCoverCap full=Cap(0,.24f,.12f),near=full,far=full;
  /* Actor bands keep original surface fractions; their shared boundary cannot
   * restart the rear feather or compression curve. */
  for(unsigned i=2;i<4;++i){near.corners[i].position[2]=.12f;near.corners[i].depth_fraction=.5f;}
  for(unsigned i=0;i<2;++i){far.corners[i].position[2]=.12f;far.corners[i].depth_fraction=.5f;}
  unsigned count;assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&near,0,vertices,8192,&count));
  ActionGroundCoverSample joint={0};bool found=false;
  for(unsigned i=0;i<count;++i)if(fabsf(vertices[i].native_x-80)<.0001f&&fabsf(vertices[i].position[2]-.12f)<.0001f) {
    joint=vertices[i].sample;found=true;break;
  }
  assert(found);found=false;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&far,0,vertices,8192,&count));
  for(unsigned i=0;i<count;++i)if(fabsf(vertices[i].native_x-80)<.0001f&&fabsf(vertices[i].position[2]-.12f)<.0001f) {
    Near(vertices[i].sample.alpha,joint.alpha);Near(vertices[i].sample.height,joint.height);found=true;
  }
  assert(found);
  /* A side-hall join collapses at one end, widening toward the outside. */
  DioramaGroundCoverCap taper=Cap(-.24f,-.42f,-.24f);
  taper.corners[3].position[2]=-.24f;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&taper,0,vertices,8192,&count));
  for(unsigned i=0;i<count;++i) {
    const float t=(vertices[i].native_x-48)/64;
    assert(vertices[i].position[2]>=-.24f-.18f*t-.0001f&&vertices[i].position[2]<=-.24f+.0001f);
  }
  /* Two independent caps leave the absent native interval completely empty. */
  DioramaGroundCoverCap left=Cap(0,.24f,.12f),right=left;
  left.to.x=70;right.from.x=90;
  for(unsigned i=0;i<4;++i) {
    left.corners[i].position[0]=(i==1||i==2)?70:48;
    right.corners[i].position[0]=(i==1||i==2)?112:90;
  }
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&left,0,vertices,8192,&count));
  for(unsigned i=0;i<count;++i)assert(vertices[i].native_x<=70.0001f);
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&right,0,vertices,8192,&count));
  for(unsigned i=0;i<count;++i)assert(vertices[i].native_x>=89.9999f);
}
static float MaskArea(unsigned count,const uint16_t pixels[256],bool nonplanar) {
  float area=0;
  for(unsigned i=0;i<count;i+=3) {
    float t[3],d[3],u=0,v=0;
    for(unsigned j=0;j<3;++j) {
      const DioramaGroundCoverVertex *p=&vertices[i+j];
      t[j]=(p->native_x-48)/64;d[j]=p->position[2]/.24f;
      u+=(.0001f+15.9998f*t[j])/3;v+=(.0001f+15.9998f*d[j])/3;
      if(nonplanar)Near(p->position[1]+p->sample.height,200+12*fminf(t[j],d[j]));
    }
    assert(u>=0&&u<16&&v>=0&&v<16);
    assert(pixels[(unsigned)v*16+(unsigned)u]&0x8000);
    area+=fabsf((t[1]-t[0])*(d[2]-d[0])-(d[1]-d[0])*(t[2]-t[0]))*.5f;
  }
  return area;
}
static void AuthoredAlphaHoles(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  const DioramaGroundCoverOwner owner={.asset=&bank,.revision=45};
  DioramaGroundCoverCap cap=Cap(0,.24f,.12f);uint16_t pixels[256];
  cap.top_pixels=pixels;
  const float uv[8]={.0001f,.0001f,15.9999f,.0001f,15.9999f,15.9999f,.0001f,15.9999f};
  memcpy(cap.top_uv,uv,sizeof(uv));
  for(unsigned i=0;i<256;++i)pixels[i]=0x8000; /* Authored black is opaque. */
  unsigned n;assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n);
  Near(MaskArea(n,pixels,false),1);
  for(unsigned y=4;y<12;++y)for(unsigned x=4;x<12;++x)pixels[y*16+x]=0;
  cap.corners[2].position[1]+=12; /* Painted hole on a nonplanar actual cap. */
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n);
  Near(MaskArea(n,pixels,true),1-(8/15.9998f)*(8/15.9998f));
  memset(pixels,0,sizeof(pixels));
  for(unsigned x=0;x<16;++x)pixels[x]=0xffff;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n);
  Near(MaskArea(n,pixels,true),(1-.0001f)/15.9998f); /* Thin one-pixel rim. */
  memset(pixels,0,sizeof(pixels));
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,NULL,0,&n)&&n==0);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)pixels[y*16+x]=((x+y)&1)?0xffff:0;
  n=999;assert(!DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,3,&n)&&n==0);
  cap.top_uv[0]=NAN;
  assert(!DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&n)&&n==0);

  /* Two separately repeated16px artwork pieces use their real UVs, while
   * the snow feather retains a single full-depth physical material span. */
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)pixels[y*16+x]=x<8?0xffff:0;
  DioramaGroundCoverCap pieces[2]={Cap(0,.12f,.12f),Cap(.12f,.24f,.12f)};
  ActionGroundCoverSample joint={0};bool found[2]={false,false};float area=0;
  for(unsigned piece=0;piece<2;++piece) {
    pieces[piece].top_pixels=pixels;memcpy(pieces[piece].top_uv,uv,sizeof(uv));
    for(unsigned j=0;j<4;++j)pieces[piece].corners[j].depth_fraction=piece*.5f+(j>=2?.5f:0);
    assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&pieces[piece],0,vertices,8192,&n)&&n);
    for(unsigned i=0;i<n;i+=3) {
      float t[3],d[3];
      for(unsigned j=0;j<3;++j) {
        const DioramaGroundCoverVertex *p=&vertices[i+j];t[j]=(p->native_x-48)/64;d[j]=p->position[2]/.24f;
        assert(t[j]<=.50001f);
        if(fabsf(p->native_x-60)<.0001f&&fabsf(p->position[2]-.12f)<.0001f) {
          if(!piece)joint=p->sample;
          else {Near(p->sample.alpha,joint.alpha);Near(p->sample.height,joint.height);}
          found[piece]=true;
        }
      }
      area+=fabsf((t[1]-t[0])*(d[2]-d[0])-(d[1]-d[0])*(t[2]-t[0]))*.5f;
    }
  }
  assert(found[0]&&found[1]);Near(area,.5f);
}
typedef struct SnowRunFixture { unsigned queries; bool gap; } SnowRunFixture;
static bool SnowRun(const void *source,int x,int y,int *bottom) {
  SnowRunFixture *fixture=(SnowRunFixture *)source;++fixture->queries;
  if(x<48||x>=112||(fixture->gap&&x>=72&&x<88))return false;
  /* Real receiver columns have a continuous native body down to their tile
   * boundary. A separate higher shelf ends before the authored floor anchor. */
  if(y==184){*bottom=188;return true;}
  if(y>=192&&y<=200){*bottom=y+8;return true;}
  return false;
}
static void ReviewedNativeRunOwnership(void) {
  ActionEffectInstance e=Example();e.world_y=208;ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  SnowRunFixture fixture={0};const DioramaGroundCoverOwner owner={&fixture,80,0};
  DioramaGroundCoverCap cap=Cap(0,.24f,.12f);cap.receiver=SnowRun;
  cap.from.y=cap.to.y=200;
  for(unsigned i=0;i<4;++i)cap.corners[i].position[1]=cap.from.y;
  unsigned count;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&count);
  for(unsigned i=0;i<count;++i) {
    Near(vertices[i].sample.native_floor_y,200);
    Near(vertices[i].position[1]+vertices[i].sample.height,200);
  }
  const unsigned queries=fixture.queries;
  assert(queries==64);
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&count);
  assert(fixture.queries==queries); /* No native ink rescans while rotating. */
  cap.receiver=NULL;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&!count);
  cap.receiver=SnowRun;cap.from.y=cap.to.y=184;
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&!count);
  cap.from.y=cap.to.y=200;fixture.gap=true;
  DioramaGroundCoverOwner changed=owner;++changed.revision;
  assert(DioramaGroundCover_BuildCap(&cache,changed,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&count);
  for(unsigned i=0;i<count;i+=3) {
    const float middle=(vertices[i].native_x+vertices[i+1].native_x+vertices[i+2].native_x)/3;
    assert(middle<=72||middle>=88);
  }
  /* A native two-pixel ramp step is represented by its complete pixel-column
   * footprint: the trace reaches the source cell at an endpoint even when its
   * midpoint is1.5px above the cap. Geometry stays at the actual step height. */
  fixture.gap=false;e.world_y=194;e.field_style.points[0].y=0;e.field_style.points[1].y=-320;
  e.world_x=48;assert(ActionGroundCover_Prepare(&e,&bank));
  cap=Cap(0,.24f,.12f);cap.from=(ActionGroundCoverPoint){48,195};cap.to=(ActionGroundCoverPoint){49,195};
  cap.receiver=SnowRun;
  for(unsigned i=0;i<4;++i)cap.corners[i].position[0]=(i==1||i==2)?49:48;
  for(unsigned i=0;i<4;++i)cap.corners[i].position[1]=195;
  assert(DioramaGroundCover_BuildCap(&cache,changed,&bank,NULL,&cap,1.01f,vertices,8192,&count)&&count);
  float right=48;for(unsigned i=0;i<count;++i)right=fmaxf(right,vertices[i].native_x);Near(right,49);
}
static void PressureFollowsActualReceiver(void) {
  ActionEffectInstance e=Example();e.world_y=208;ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  ActionSceneEffectFrame frame={.actor_count=1};
  frame.actors[0]=(ActionEffectActor){.visible=1,.player=1,.body_radius=8,.x=80,.feet_y=200,
      .wake={.age=kActionActorWakeTicks}};
  ActionGroundCoverPressure old,actual;
  assert(ActionGroundCover_PreparePressure(&bank,&frame,&old));Near(old.feet[0].strength,0);
  assert(ActionGroundCover_PrepareSupportPressure(&bank,&frame,
      (ActionGroundCoverPoint){48,200},(ActionGroundCoverPoint){112,200},&actual));
  Near(actual.feet[0].strength,1);
  frame.actors[0].feet_y=184;
  assert(ActionGroundCover_PrepareSupportPressure(&bank,&frame,
      (ActionGroundCoverPoint){48,200},(ActionGroundCoverPoint){112,200},&actual));
  Near(actual.feet[0].strength,0);
}
static void TransformedFootContact(void) {
  ActionEffectInstance e=Example();e.field_style.wake=1;ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  ActionSceneEffectFrame frame={.actor_count=1};
  frame.actors[0]=(ActionEffectActor){.visible=1,.player=1,.body_radius=8,.x=80,.feet_y=200,
      .wake={.age=kActionActorWakeTicks}};
  ActionGroundCoverPressure pressure,legacy;
  DioramaGroundCoverCap native=Cap(0,.24f,.12f),cap=native;
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&native,&pressure));
  assert(ActionGroundCover_PrepareSupportPressure(&bank,&frame,native.from,native.to,&legacy));
  assert(!memcmp(&pressure,&legacy,sizeof(pressure))); /* Common native fast path. */
  for(unsigned mode=0;mode<3;++mode) {
    cap=native;
    for(unsigned i=0;i<4;++i) {
      const float x=native.corners[i].position[0]-80;
      cap.corners[i].position[0]=120+(mode==1?2:mode==2?.6f:1)*x;
      cap.corners[i].position[1]=216+(mode==2?.8f*x:0);
    }
    frame.actors[0].x=120;frame.actors[0].feet_y=216;
    assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
    Near(pressure.feet[0].x,80);Near(pressure.feet[0].strength,1);
    Near(pressure.feet[0].radius,mode==1?4:8);
    ActionGroundCoverSample pressed,settled;
    assert(ActionGroundCover_Sample(&bank,&pressure,80,.5f,1,&pressed));
    assert(ActionGroundCover_Sample(&bank,NULL,80,.5f,1,&settled));
    Near(pressed.settled_height,settled.settled_height);Near(pressed.height,settled.height*.65f);
    frame.actors[0].feet_y=196;
    assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
    Near(pressure.feet[0].strength,0); /* Jumping, even after XY rotation. */
    frame.actors[0].x=80;frame.actors[0].feet_y=200;
    assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
    Near(pressure.feet[0].strength,0); /* The old source-space foot cannot press it. */
  }
  /* Tilted depth: the actual actor plane intersects Y212, not either rim. */
  cap=native;
  for(unsigned i=0;i<4;++i)cap.corners[i].position[1]+=100*cap.corners[i].position[2];
  frame.actors[0].x=80;frame.actors[0].feet_y=212;
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
  Near(pressure.feet[0].x,80);Near(pressure.feet[0].strength,1);
  cap.actor_z=.4f;
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));Near(pressure.feet[0].strength,0);
  cap.actor_z=.12f;cap.corners[0].position[0]=NAN;
  assert(!DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
  cap=native;
  for(unsigned i=0;i<4;++i) {
    cap.corners[i].position[0]=120;cap.corners[i].position[1]=216;
  }
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));Near(pressure.feet[0].strength,0);
  cap=native;cap.actor_z=.00000001f;
  for(unsigned i=0;i<4;++i){cap.corners[i].position[1]+=16;cap.corners[i].position[2]*=.0000001f;}
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));Near(pressure.feet[0].strength,0);

  /* A nonplanar receiver intersects each original triangle independently.
   * A bilinear approximation would place the quarter point at Y201.5. */
  cap=native;cap.corners[2].position[1]+=12;
  for(unsigned i=0;i<4;++i)cap.corners[i].position[0]+=40;
  frame.actors[0].x=104;frame.actors[0].feet_y=203;
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
  Near(pressure.feet[0].x,64);Near(pressure.feet[0].strength,1);
  frame.actors[0].x=120;frame.actors[0].feet_y=206;
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
  Near(pressure.feet[0].x,80);Near(pressure.feet[0].strength,1);
  const ActionGroundCoverPressure whole=pressure;
  for(unsigned part=0;part<2;++part) {
    DioramaGroundCoverCap piece=cap;piece.pressure_quad=true;
    for(unsigned i=0;i<4;++i) {
      memcpy(piece.pressure_xyz+i*3,cap.corners[i].position,3*sizeof(float));
      piece.corners[i]=GroundCoverCorner(&cap,i==1||i==2?1:0,part*.5f+(i>=2?.5f:0));
    }
    assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&piece,&pressure));
    assert(!memcmp(&whole,&pressure,sizeof(pressure))); /* Actor/UV fragments share a full floor trace. */
  }
  /* Wake points use the same transformed contact, with the old age/strength policy. */
  frame.actors[0].feet_y=180;
  frame.actors[0].wake=(ActionActorWake){.x=120,.y=206,.age=0,.strength=.5f};
  assert(DioramaGroundCover_PrepareCapPressure(&bank,&frame,&cap,&pressure));
  Near(pressure.feet[0].x,80);Near(pressure.feet[0].strength,.2f);
}
static void ReceiverOwnedLip(void) {
  ActionEffectInstance e=Example();ActionGroundCoverBank bank;
  assert(ActionGroundCover_Prepare(&e,&bank));
  const DioramaGroundCoverOwner owner={&bank,95,0};
  const ActionGroundCoverPressure pressure={.feet={{80,8,1},{0}}};
  const float spans[3][3]={{0,-.24f,-.24f},{-.24f,-.42f,-.24f},{0,.24f,.12f}};
  static DioramaGroundCoverVertex tops[8192];
  unsigned count,top_count;
  for(unsigned mode=0;mode<3;++mode) {
    DioramaGroundCoverCap cap=Cap(spans[mode][0],spans[mode][1],spans[mode][2]);
    assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,&pressure,&cap,0,tops,8192,&top_count));
    assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,&pressure,&cap,0,vertices,8192,&count)&&count);
    unsigned crests=0,base=0,feather=0;
    for(unsigned i=0;i<count;++i) {
      const DioramaGroundCoverVertex *v=&vertices[i];
      Near(v->position[0],v->native_x);Near(v->position[2],spans[mode][0]);
      assert(v->sample.alpha<1);
      if(v->sample.height>0) {
        bool matched=false;
        for(unsigned j=0;j<top_count;++j)if(fabsf(tops[j].native_x-v->native_x)<.0001f&&
            fabsf(tops[j].position[2]-v->position[2])<.0001f) {
          Near(tops[j].position[1],v->position[1]);matched=true;break;
        }
        assert(matched);Near(v->sample.crest_mix,1);++crests;
      } else if(v->sample.alpha==0) {Near(v->position[1],201.5f);++feather;}
      else {Near(v->position[1],200);Near(v->sample.crest_mix,0);++base;}
    }
    assert(crests&&base&&feather);
  }
  /* The same original near edge survives actor/atlas cuts exactly once. */
  DioramaGroundCoverCap cap=Cap(0,.24f,.12f),near=cap,far=cap;
  for(unsigned i=2;i<4;++i){near.corners[i].position[2]=.12f;near.corners[i].depth_fraction=.5f;}
  for(unsigned i=0;i<2;++i){far.corners[i].position[2]=.12f;far.corners[i].depth_fraction=.5f;}
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&near,0,vertices,8192,&count)&&count);
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&far,0,NULL,0,&count)&&!count);
  /* Move and rotate the actual front without moving its native support. */
  cap=Cap(-.24f,-.42f,-.24f);
  for(unsigned i=0;i<4;++i) {
    const float x=cap.corners[i].position[0];
    cap.corners[i].position[0]=30+1.2f*x;cap.corners[i].position[1]=250+.4f*x;
    cap.corners[i].lift[0]=.6f;cap.corners[i].lift[1]=-.8f;
  }
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&count)&&count);
  for(unsigned i=0;i<count;++i) {
    const DioramaGroundCoverVertex *v=&vertices[i];
    const float lift=v->sample.alpha==0?-1.5f:v->sample.height;
    Near(v->position[0]-.6f*lift,30+1.2f*v->native_x);
    Near(v->position[1]+.8f*lift,250+.4f*v->native_x);Near(v->position[2],-.24f);
  }
  /* A painted front-rim gap removes its lip too. Interior alpha edges never
   * become a new front lip, and a fully erased rim emits no replacement. */
  cap=Cap(0,.24f,.12f);uint16_t pixels[256];
  for(unsigned i=0;i<256;++i)pixels[i]=0xffff;
  cap.top_pixels=pixels;
  const float uv[8]={0,.5f,15.9999f,.5f,15.9999f,15.9999f,0,15.9999f};
  memcpy(cap.top_uv,uv,sizeof(uv));
  for(unsigned x=6;x<10;++x)pixels[x]=0;
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&count)&&count);
  for(unsigned i=0;i<count;i+=3) {
    const float source=(vertices[i].native_x+vertices[i+1].native_x+vertices[i+2].native_x)/3;
    assert(source<=72.001f||source>=87.999f);
  }
  memset(pixels,0,16*sizeof(*pixels));
  assert(DioramaGroundCover_BuildCap(&cache,owner,&bank,NULL,&cap,0,vertices,8192,&count)&&count);
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&cap,0,NULL,0,&count)&&!count);
  cap.top_pixels=NULL;cap.from.y=cap.to.y=230;
  assert(DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&cap,0,NULL,0,&count)&&!count);
  cap=Cap(0,.24f,.12f);count=123;
  assert(!DioramaGroundCover_BuildLip(&cache,owner,&bank,NULL,&cap,0,vertices,11,&count)&&!count);
}
#include "diorama_snow_closing_riser_pure.inc"
int main(void) {
  ActualContactAndTriangleSupport();CacheAndEmptyOwnership();SplitAndTaperedReceivers();
  AuthoredAlphaHoles();
  ReviewedNativeRunOwnership();PressureFollowsActualReceiver();TransformedFootContact();
  ReceiverOwnedLip();SnowRiserPure();
  puts("diorama ground cover cap/cache tests passed");return 0;
}
