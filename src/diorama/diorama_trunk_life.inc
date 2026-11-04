/* Bounded decoration on existing native surfaces: no texture, shader, target
 * or readback. This file is included after the hollow contour helpers. */
typedef struct HollowLifeLight {float x,y,strength,r,g,b,gust;} HollowLifeLight;
typedef struct HollowFrost {float x,y,seed;unsigned light;} HollowFrost;
static struct {
  float wrap,gust,frost,bark;
  bool light,particles,valid;
  int room;
  unsigned clock,count,frost_count;
  HollowLifeLight lights[12];
  HollowFrost frost_points[768];
  ActionTrunkBarkEvent events[8];
  uint32_t rim_key;
} s_life={.wrap=1,.gust=1,.frost=1,.bark=1};
static float LifeUnit(unsigned n) {
  n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;
  return (n&65535)/65535.f;
}
static bool LifeSolid(int room,int x,int y,int kind) {
  (void)room;return ActionTrunk_Solid(s_trunk_assets,x,y,(unsigned)kind);
}
static void TrunkLifePrepare(const ActionSceneEffectFrame *frame,unsigned clock,bool light,bool particles) {
  s_life.valid=frame&&frame->trunk.enabled&&s_trunk_assets&&s_trunk_settings.enabled;
  s_life.wrap=s_trunk_settings.wrap;s_life.gust=s_trunk_settings.gust;
  s_life.frost=s_trunk_settings.frost;s_life.bark=s_trunk_settings.bark;
  s_life.light=s_life.particles=false;s_life.count=0;
  if(!s_life.valid)return;
  const int room=(int)s_trunk_assets->room;
  s_life.room=room;s_life.clock=clock;
  memcpy(s_life.events,frame->trunk_bark,sizeof(s_life.events));
  s_life.light=light;s_life.particles=particles;s_life.count=0;
  uint32_t key=s_trunk_assets->revision;
  for(unsigned i=0;i<frame->authored_count&&s_life.count<12;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    if(e->kind!=kActionEffect_AuthoredFan||!e->field_style.opening||e->tuning.intensity<=0)continue;
    s_life.lights[s_life.count++]=(HollowLifeLight){e->world_x,e->world_y,
      fminf(1,e->tuning.intensity/2.5f),((e->tuning.color>>16)&255)/255.f,
      ((e->tuning.color>>8)&255)/255.f,(e->tuning.color&255)/255.f,ActionTrunk_Gust(e,0)};
    key=(key^(unsigned)e->world_x)*16777619u;key=(key^(unsigned)e->world_y)*16777619u;
  }
  if(key!=s_life.rim_key) {
    s_life.rim_key=key;s_life.frost_count=0;
    for(unsigned i=0;i<s_life.count;++i) {
      const int cx=(int)s_life.lights[i].x,cy=(int)s_life.lights[i].y,dir=cx<512?1:-1;
      unsigned made=0;
      for(int y=cy-22;y<=cy+22&&made<64;++y)for(int x=cx-19;x<=cx+19&&made<64;++x) {
        const float seed=LifeUnit((unsigned)(x*139+y*479+room*997));
        if(seed>.54f||!LifeSolid(room,x,y,0)||
            (LifeSolid(room,x+dir,y,0)&&LifeSolid(room,x,y-1,0)))continue;
        s_life.frost_points[s_life.frost_count++]=(HollowFrost){x+.5f,y+.35f,seed,i};++made;
      }
    }
  }
}

static bool LifeLightingActive(void){return s_life.valid&&s_life.light&&s_life.wrap>0;}
static ArRenderColorF LifeWallColor(float x,float y,ArRenderColorF base) {
  if(!s_life.light||s_life.wrap<=0)return base;
  float shadow=0;
  /* A short soft penumbra below branch silhouettes; taps are in world space
   * and remain fixed through scroll/orbit. The center column is excluded. */
  if(x<484||x>540)for(int j=1;j<=4;++j) {
    const int yy=(int)y-j*6;float taps=0;
    for(int k=-1;k<=1;++k)taps+=LifeSolid(s_life.room,(int)x+k*j,yy,1)?1.f:0;
    shadow=fmaxf(shadow,taps*(5-j)*.0105f);
  }
  float r=1-shadow,g=r,b=r;
  const float q=(x-512)/160,nx=-q*.78f,nz=1/sqrtf(1+nx*nx);
  for(unsigned i=0;i<s_life.count;++i) {
    const HollowLifeLight *l=&s_life.lights[i];
    const float dx=(x-l->x)/190,dy=(y-l->y-12)/95;
    float fall=fmaxf(0,1-dx*dx-dy*dy);fall*=fall;
    const float direction=l->x<512?-1.f:1.f;
    const float normal=fmaxf(.12f,(.72f+direction*nx*.65f)*nz);
    const float v=fall*normal*l->strength*.72f;
    r+=v*l->r;g+=v*l->g;b+=v*l->b;
  }
  base.r*=1+(r-1)*s_life.wrap;base.g*=1+(g-1)*s_life.wrap;base.b*=1+(b-1)*s_life.wrap;
  return base;
}
static ArRenderColorF LifeBranchColor(float x,float y) {
  ArRenderColorF color={1,1,1,1};if(!LifeLightingActive())return color;
  for(unsigned i=0;i<s_life.count;++i) {
    const HollowLifeLight *l=&s_life.lights[i];
    const float dx=(x-l->x)/160,dy=(y-l->y)/100;
    const float f=fmaxf(0,1-dx*dx-dy*dy)*.18f*l->strength*s_life.wrap;
    color.r+=f*l->r;color.g+=f*l->g;color.b+=f*l->b;
  }
  return color;
}
static ArRenderVertex2D s_life_vertices[4096];
static int32_t s_life_indices[6144];
static unsigned s_life_quads;
static bool LifeQuad(const DioramaCapture *capture,const DioramaViewGeometry *g,float z,
    float x,float y,float width,float height,float angle,ArRenderColorF color) {
  if(s_life_quads>=1024)return false;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  if(x<ox-4||x>ox+capture->width+4||y<oy-4||y>oy+capture->height+4)return true;
  const float c=cosf(angle),s=sinf(angle);const int corners[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
  for(int i=0;i<4;++i) {
    const float px=x+c*corners[i][0]*width-s*corners[i][1]*height;
    const float py=y+s*corners[i][0]*width+c*corners[i][1]*height;
    ArRenderVertex2D *v=&s_life_vertices[s_life_quads*4+i];v->tex_coord=(ArRenderPointF){0,0};v->color=color;
    if(!ProjectWorldPoint(g->matrix,((px-ox)/capture->width-.5f)*g->aspect_x,
        (.5f-(py-oy)/capture->height)*g->height_scale,z,g->width,g->height,&v->position))return true;
  }
  const int indices[6]={0,1,2,0,2,3};
  for(int i=0;i<6;++i)s_life_indices[s_life_quads*6+i]=(int32_t)s_life_quads*4+indices[i];
  ++s_life_quads;return true;
}
static bool DrawHollowLife(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,float fade) {
  if(!s_life.valid||fade<=0)return true;
  s_life_quads=0;
  const float z=layer->authored->z-.5f;
  if(layer->description->plane==kDioramaPlane_Bg1Hi&&s_life.light&&s_life.frost>0) {
    for(unsigned i=0;i<s_life.frost_count;++i) {
      const HollowFrost *p=&s_life.frost_points[i];const HollowLifeLight *l=&s_life.lights[p->light];
      /* Broad angular reflection, not a time-based flashing star. */
      float glint=fmaxf(0,cosf(g->camera.tilt_y*2.4f+p->seed*7));glint*=glint;glint*=glint;
      ArRenderColorF color={.r=.64f+.32f*glint,.g=.82f+.16f*glint,.b=.97f,
        .a=s_life.frost*(.24f+.48f*glint)*l->strength*fade};
      if(!LifeQuad(capture,g,z,p->x,p->y,.35f+p->seed*.35f,.22f+p->seed*.16f,0,color))return false;
    }
  }
  if(layer->description->plane==SR_PPU_OVERLAY_BG1&&s_life.particles&&s_life.bark>0) {
    for(unsigned i=0;i<8;++i) {
      const ActionTrunkBarkEvent *e=&s_life.events[i];const unsigned age=(uint16_t)(s_life.clock-e->born);
      if(!e->valid||age>=96)continue;
      for(unsigned j=0;j<3;++j) {
        const float delay=2+j*5+LifeUnit(e->seed+j)*5,t=(age-delay)/80;
        if(t<=0||t>=1)continue;
        const float seed=LifeUnit(e->seed+j*139),x=e->x+(seed-.5f)*12+sinf(t*7+seed*9)*6*t;
        const float y=e->y+8*t+44*t*t,envelope=fminf(1,t*12)*fminf(1,(1-t)*5);
        const ArRenderColorF tint=LifeWallColor(x,y,(ArRenderColorF){.42f,.38f,.47f,envelope*fade*.8f*s_life.bark});
        if(!LifeQuad(capture,g,z,x,y,.35f+.45f*seed,.8f+seed*.5f,t*8+seed*6,tint))return false;
      }
    }
  }
  if(!s_life_quads)return true;
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Alpha};
  return ArRenderDevice_DrawGeometryWithState(device,ArRenderTexture_Invalid(),s_life_vertices,
      (int)s_life_quads*4,s_life_indices,(int)s_life_quads*6,&state);
}
