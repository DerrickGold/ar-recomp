#include "diorama_astral_rift.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "render/scene3d_math.h"
#include "../../third_party/stb/stb_image.h"
#include "astral_rift_art.inc"

enum { kGlowSize = 64, kMeshVertices = 8192, kMeshIndices = 24576,
       kStars = 180, kClouds = 6, kRuins = 3, kEvents = 3, kFragments = 32 };
static const float kTau = 6.28318530718f;
static const float kArtAspect = 1672.0f / 941.0f;
static const float kSkyDistance = 3.25f, kSkyTanHalfFov = .41421356237f;
static const unsigned kEventClouds[kEvents] = {0, 2, 3};

typedef struct RiftPoint { float x, y, z; } RiftPoint;
typedef struct RiftRotation { float cx, sx, cy, sy, cz, sz; } RiftRotation;
typedef struct RiftPatch {
  RiftPoint center;
  RiftRotation rotation;
  float width, height, bend, opacity, charge;
  unsigned tile;
} RiftPatch;
typedef struct RiftActivity {
  uint16_t tick;
  RiftPatch clouds[kClouds], ruins[kRuins];
  RiftPatch monument;
  RiftPoint events[kEvents];
  float pulse[kEvents], progress[kEvents];
} RiftActivity;

/* One render-thread device owns the cache. Art is decoded at preparation,
 * uploaded once, and never sampled/read back by the CPU during a frame. */
static struct {
  const void *owner;
  ArRenderTexture nebula, clouds, ruins, floor, floor_emission, glow;
  bool attempted;
  ArRenderVertex2D vertices[kMeshVertices];
  int32_t indices[kMeshIndices];
  int vertex_count, index_count;
} s_rift;

static uint32_t Hash(uint32_t n) {
  n ^= n >> 16; n *= UINT32_C(0x7feb352d);
  n ^= n >> 15; n *= UINT32_C(0x846ca68b);
  return n ^ (n >> 16);
}
static float Random(uint32_t seed) { return (Hash(seed) >> 8) / 16777216.0f; }
static float Clamp(float x) { return fmaxf(0, fminf(1, x)); }
static float Mix(float a, float b, float t) { return a + (b-a)*t; }
static float Smooth(float x) { x=Clamp(x); return x*x*(3-2*x); }
static float Phase(uint16_t tick, unsigned frequency) {
  /* Reduce in integer clock units before trig. Large unreduced arguments
   * magnify native/WASM rounding differences, especially just before wrap. */
  return (((uint32_t)tick*frequency)&65535u)*(kTau/65536.0f);
}

static void DestroyTextures(ArRenderDevice *device) {
  ArRenderTexture *textures[] = {&s_rift.nebula, &s_rift.clouds, &s_rift.ruins,
      &s_rift.floor, &s_rift.floor_emission, &s_rift.glow};
  for (unsigned i = 0; i < sizeof(textures)/sizeof(*textures); ++i) {
    ArRenderDevice_DestroyTexture(device, *textures[i]);
    *textures[i] = ArRenderTexture_Invalid();
  }
}

void DioramaAstralRift_Reset(ArRenderDevice *device) {
  if (!device || s_rift.owner != device->context) return;
  DestroyTextures(device);
  memset(&s_rift, 0, sizeof(s_rift));
}

static bool Upload(ArRenderDevice *device, int width, int height,
    const uint32_t *pixels, ArRenderBlendMode blend, ArRenderTexture *texture) {
  const ArRenderTextureDesc desc = {.width=width, .height=height,
      .format=kArRenderPixelFormat_Argb8888, .usage=kArRenderTextureUsage_Static,
      .filter=kArRenderFilter_Linear, .blend=blend};
  return ArRenderDevice_CreateTexture(device, &desc, texture) &&
      ArRenderDevice_UpdateTexture(device, *texture, NULL, pixels, width*4);
}

static bool LoadArt(ArRenderDevice *device, const unsigned char *encoded,
    int length, ArRenderBlendMode blend, ArRenderTexture *texture,
    ArRenderTexture *emission) {
  int width, height, channels;
  stbi_uc *rgba = stbi_load_from_memory(encoded, length, &width, &height, &channels, 4);
  if (!rgba) return false;
  /* Numeric ARGB, independent of host byte order. The backend owns byte layout. */
  uint32_t *pixels = (uint32_t *)rgba;
  for (int i = 0; i < width*height; ++i) {
    const unsigned char *p = rgba+i*4;
    pixels[i] = ((uint32_t)p[3]<<24)|((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
  }
  if (emission) {
    /* The floor's dark cavities contain baked space. Reveal the live clouds
     * there, but retain the solid walking lip and luminous stone ridges.
     * This material matte is prepared once, never from a rendered frame. */
    for (int y=0;y<height;++y) {
      const float reveal=Smooth(((float)y/height-.705f)/.055f);
      for (int x=0;x<width;++x) {
        const int i=y*width+x;
        const uint32_t p=pixels[i];
        const float value=fmaxf((p>>16)&255,fmaxf((p>>8)&255,p&255));
        const float stone=Smooth((value-32)/64);
        const unsigned alpha=(unsigned)((p>>24)*Mix(1,stone,reveal)+.5f);
        pixels[i]=(p&UINT32_C(0x00ffffff))|(alpha<<24);
      }
    }
  }
  bool ok = Upload(device, width, height, pixels, blend, texture);
  if (ok && emission) {
    /* Extract the authored luminous mineral veins once, while the decoded
     * pixels are already on the CPU. This is never a framebuffer readback. */
    for (int i=0;i<width*height;++i) {
      const uint32_t p=pixels[i];
      const unsigned r=(p>>16)&255,g=(p>>8)&255,b=p&255;
      const float hot=Smooth((fmaxf(r,fmaxf(g,b))-112)/118);
      pixels[i]=(p&UINT32_C(0xff000000))|((unsigned)(r*hot)<<16)|
          ((unsigned)(g*hot)<<8)|(unsigned)(b*hot);
    }
    ok=Upload(device,width,height,pixels,kArRenderBlendMode_Add,emission);
  }
  stbi_image_free(rgba);
  return ok;
}

bool DioramaAstralRift_Prepare(ArRenderDevice *device) {
  if (!ArRenderDevice_IsReady(device)) return false;
  if (s_rift.owner && s_rift.owner != device->context) return false;
  if (s_rift.attempted) return ArRenderTexture_IsValid(s_rift.glow);
  s_rift.owner = device->context;
  s_rift.attempted = true;
  bool ok = LoadArt(device, kAstralNebulaPng, sizeof(kAstralNebulaPng),
          kArRenderBlendMode_Opaque, &s_rift.nebula, NULL) &&
      LoadArt(device, kAstralCloudsPng, sizeof(kAstralCloudsPng),
          kArRenderBlendMode_Alpha, &s_rift.clouds, NULL) &&
      LoadArt(device, kAstralRuinsPng, sizeof(kAstralRuinsPng),
          kArRenderBlendMode_Alpha, &s_rift.ruins, NULL) &&
      LoadArt(device, kAstralFloorPng, sizeof(kAstralFloorPng),
          kArRenderBlendMode_Alpha, &s_rift.floor, &s_rift.floor_emission);
  uint32_t pixels[kGlowSize*kGlowSize];
  for (int y = 0; y < kGlowSize; ++y) for (int x = 0; x < kGlowSize; ++x) {
    const float dx = (x+.5f)*2/kGlowSize-1, dy = (y+.5f)*2/kGlowSize-1;
    const float alpha = Clamp(expf(-(dx*dx+dy*dy)*5)-expf(-5));
    pixels[y*kGlowSize+x] = ((uint32_t)(alpha*255+.5f)<<24)|0xffffff;
  }
  ok = ok && Upload(device, kGlowSize, kGlowSize, pixels, kArRenderBlendMode_Add, &s_rift.glow);
  if (!ok) DestroyTextures(device);
  return ok;
}

static void BeginMesh(void) { s_rift.vertex_count = s_rift.index_count = 0; }
static bool GridFits(int columns, int rows) {
  return s_rift.vertex_count+(columns+1)*(rows+1)<=kMeshVertices &&
      s_rift.index_count+columns*rows*6<=kMeshIndices;
}
static void GridIndices(int base, int columns, int rows) {
  for (int y=0;y<rows;++y) for (int x=0;x<columns;++x) {
    const int a=base+y*(columns+1)+x,b=a+1,c=b+columns+1,d=c-1;
    const int32_t indices[]={a,b,c,a,c,d};
    memcpy(s_rift.indices+s_rift.index_count,indices,sizeof(indices));
    s_rift.index_count+=6;
  }
}
static bool Quad(const ArRenderVertex2D vertices[4]) {
  if (s_rift.vertex_count+4 > kMeshVertices || s_rift.index_count+6 > kMeshIndices) return false;
  const int base = s_rift.vertex_count;
  memcpy(s_rift.vertices+base, vertices, 4*sizeof(*vertices));
  static const int32_t corners[] = {0,1,2,0,2,3};
  for (int i = 0; i < 6; ++i) s_rift.indices[s_rift.index_count++] = base+corners[i];
  s_rift.vertex_count += 4;
  return true;
}
static bool Submit(ArRenderDevice *device, ArRenderTexture texture, ArRenderBlendMode blend) {
  if (!s_rift.index_count) return true;
  const ArRenderDrawState state = {.flags=kArRenderDrawState_Blend, .blend=blend};
  return ArRenderDevice_DrawGeometryWithState(device, texture, s_rift.vertices,
      s_rift.vertex_count, s_rift.indices, s_rift.index_count, &state);
}
static bool Billboard(float x, float y, float rx, float ry, ArRenderColorF color) {
  const ArRenderVertex2D v[] = {
    {{x-rx,y-ry},color,{0,0}}, {{x+rx,y-ry},color,{1,0}},
    {{x+rx,y+ry},color,{1,1}}, {{x-rx,y+ry},color,{0,1}},
  };
  return Quad(v);
}
static bool GlowSegment(Scene3DPoint a, Scene3DPoint b, float width,
    ArRenderColorF color) {
  const float dx=b.x-a.x,dy=b.y-a.y,length=sqrtf(dx*dx+dy*dy);
  if (length<.001f) return true;
  const float nx=-dy*width/length,ny=dx*width/length;
  const ArRenderVertex2D v[]={
    {{a.x+nx,a.y+ny},color,{0,0}},{{b.x+nx,b.y+ny},color,{1,0}},
    {{b.x-nx,b.y-ny},color,{1,1}},{{a.x-nx,a.y-ny},color,{0,1}},
  };
  return Quad(v);
}

/* A cover crop preserves the artwork's proportions at 4:3 through ultrawide.
 * The extra border supplies camera/flow overscan, never a wrapped sky seam. */
static ArRenderPointF ArtUV(float u, float v, int width, int height) {
  const float aspect = (float)width/height;
  return (ArRenderPointF){.5f+(u-.5f)*fminf(1,aspect/kArtAspect)*.94f,
      .5f+(v-.5f)*fminf(1,kArtAspect/aspect)*.94f};
}

static bool Nebula(ArRenderDevice *device, int w, int h, uint16_t tick,
    float yaw, float pitch, float brightness) {
  enum { columns=48, rows=27 };
  BeginMesh();
  if (!GridFits(columns,rows)) return false;
  const float along=.026f*sinf(Phase(tick,29))+.010f*sinf(Phase(tick,43)+1.4f);
  const float across=.014f*sinf(Phase(tick,23)+.7f);
  const float curl[]={.27f*sinf(Phase(tick,31)+.4f),
      -.31f*sinf(Phase(tick,37)+2.1f)};
  const float billow=Phase(tick,53);
  static const struct { float x,y,rx,ry; } eddies[]={
    {.18f,.27f,.36f,.32f}, {.87f,.31f,.31f,.38f},
  };
  /* Advect the main cloud ribbon and roll its blue/magenta banks separately.
   * The field lives in artwork coordinates, so aspect changes and camera
   * motion do not relocate the currents. Dense clouds move visibly while
   * the dark lower star field stays quiet. Everything fits the existing draw. */
  for (int y=0;y<=rows;++y) for (int x=0;x<=columns;++x) {
    const float u=(float)x/columns,t=(float)y/rows;
    ArRenderPointF uv=ArtUV(u,t,w,h);
    uv.x+=yaw*.012f;
    uv.y-=pitch*.012f;
    const float ridge=(uv.y-(.16f+.43f*uv.x))/.18f;
    const float ribbon=expf(-ridge*ridge);
    ArRenderPointF flow={ribbon*(along+.008f*sinf(uv.y*13-billow)),
        ribbon*(along*.43f+across+.006f*sinf(uv.x*17+billow))};
    for (unsigned i=0;i<sizeof(eddies)/sizeof(*eddies);++i) {
      const float dx=(uv.x-eddies[i].x)/eddies[i].rx;
      const float dy=(uv.y-eddies[i].y)/eddies[i].ry;
      const float turn=curl[i]*expf(-(dx*dx+dy*dy)*1.6f);
      flow.x-=dy*eddies[i].rx*turn;
      flow.y+=dx*eddies[i].ry*turn;
    }
    /* Ease displacement to zero at the art boundary rather than clamping
     * a moving strip of texels. No wrapping, exposed border, or extra target. */
    const float edge=Smooth(fminf(fminf(uv.x,1-uv.x),fminf(uv.y,1-uv.y))*10);
    const float motion=edge*(1-Smooth((uv.y-.62f)/.25f));
    uv.x+=flow.x*motion;
    uv.y+=flow.y*motion;
    const float exposure=brightness*.78f;
    s_rift.vertices[s_rift.vertex_count++]=(ArRenderVertex2D){
        {u*w,t*h},{exposure,exposure,exposure,1},{Clamp(uv.x),Clamp(uv.y)}};
  }
  GridIndices(0,columns,rows);
  return Submit(device,s_rift.nebula,kArRenderBlendMode_Opaque);
}

static RiftRotation Rotation(float x, float y, float z) {
  return (RiftRotation){cosf(x),sinf(x),cosf(y),sinf(y),cosf(z),sinf(z)};
}
static RiftPoint Rotate(RiftRotation r, RiftPoint p) {
  const float y=p.y*r.cx-p.z*r.sx,z=p.y*r.sx+p.z*r.cx;
  const float x=p.x*r.cy+z*r.sy;
  return (RiftPoint){x*r.cz-y*r.sz,x*r.sz+y*r.cz,-p.x*r.sy+z*r.cy};
}
static RiftPatch Patch(float u, float v, float z, float width, float height,
    unsigned tile, float opacity, RiftRotation rotation) {
  const float view_height=2*(kSkyDistance-z)*kSkyTanHalfFov;
  return (RiftPatch){.center={(u-.5f)*view_height*kArtAspect,(.5f-v)*view_height,z},
      .rotation=rotation,.width=width*view_height*kArtAspect,
      .height=height*view_height,.tile=tile,.opacity=opacity};
}
static RiftPoint PatchPoint(const RiftPatch *p, float u, float v,
    uint16_t tick, bool cloud) {
  RiftPoint local={(u-.5f)*p->width,(.5f-v)*p->height,
      p->bend*(1-(u*2-1)*(u*2-1))};
  if (cloud) {
    local.x+=p->height*.055f*sinf(v*9+Phase(tick,71)+p->tile);
    local.y+=p->height*.065f*sinf(u*8-Phase(tick,59)+p->tile);
  }
  local=Rotate(p->rotation,local);
  return (RiftPoint){p->center.x+local.x,p->center.y+local.y,p->center.z+local.z};
}

static RiftActivity Activity(uint16_t tick) {
  RiftActivity a={.tick=tick};
  /* Pairs of cloud pieces occupy fixed, separated depth ranges. Their
   * bounded drift never wraps/recycles at a visible screen edge. */
  static const struct { float u,v,z,w,h,alpha,roll; unsigned tile; } clouds[]={
    {.12f,.16f,-8,.86f,.58f,.38f,-.15f,0},
    {.93f,.22f,-8,.70f,.56f,.40f,-.12f,3},
    {.37f,.30f,-3.6f,.82f,.42f,.52f,-.20f,1},
    {.91f,.45f,-3.6f,.66f,.37f,.44f,-.16f,3},
    {.12f,.51f,-.8f,.66f,.29f,.34f,-.14f,2},
    {.62f,.60f,-.8f,.90f,.25f,.26f,-.17f,2},
  };
  for (unsigned i=0;i<kClouds;++i) {
    const float offset=i*1.9f;
    const unsigned depth=i/2;
    const float drift=.050f+depth*.020f;
    a.clouds[i]=Patch(clouds[i].u+drift*sinf(Phase(tick,21+depth*8+i)+offset),
        clouds[i].v+.028f*sinf(Phase(tick,17+i*2)+offset),clouds[i].z,
        clouds[i].w,clouds[i].h,clouds[i].tile,clouds[i].alpha,
        Rotation(.08f*sinf(Phase(tick,7)+offset),.10f*sinf(Phase(tick,9)+offset),
            clouds[i].roll+.065f*sinf(Phase(tick,23+i)+offset)));
    a.clouds[i].bend=.08f*a.clouds[i].width;
  }
  static const struct { float u,v,z,w,h,roll; unsigned tile; } ruins[]={
    {.58f,.06f,-6.6f,.36f,.59f,-.36f,0},
    {.93f,.04f,-5.8f,.52f,.53f,.32f,1},
    {.28f,.11f,-5.0f,.13f,.31f,-.34f,2},
  };
  for (unsigned i=0;i<kRuins;++i) {
    const float offset=i*2.3f;
    a.ruins[i]=Patch(ruins[i].u+.016f*sinf(Phase(tick,9+i)+offset),
        ruins[i].v+.015f*sinf(Phase(tick,7+i)+offset),ruins[i].z,
        ruins[i].w,ruins[i].h,ruins[i].tile,.32f,
        Rotation(.21f*sinf(Phase(tick,11+i)+offset),.32f*sinf(Phase(tick,13+i)+offset),
            ruins[i].roll+.15f*sinf(Phase(tick,15+i)+offset)));
    a.ruins[i].bend=.12f*a.ruins[i].width;
  }
  /* One immense, dim remnant moves behind the smaller ruins and cloud
   * banks. Its long orbit supplies scale without another bright gold arch. */
  const float passage=.5f+.5f*sinf(Phase(tick,11)+.8f);
  a.monument=Patch(.58f+.15f*sinf(Phase(tick,7)+.6f),
      .02f+.045f*sinf(Phase(tick,9)),-9.5f,1.28f,.94f,1,
      .16f+.18f*passage*passage,
      Rotation(.12f*sinf(Phase(tick,5)),.24f*sinf(Phase(tick,7)+1),
          -.14f+.12f*sinf(Phase(tick,9)+2)));
  a.monument.bend=.09f*a.monument.width;
  static const unsigned rates[kEvents]={47,61,79};
  for (unsigned i=0;i<kEvents;++i) {
    /* A several-second rise/decay, with no flash/strobe or runtime RNG.
     * Integer frequencies make tick 65535 -> 0 continuous. */
    const float cycle=(float)tick*rates[i]/65536.0f+i*.31f;
    const float age=cycle-floorf(cycle);
    a.pulse[i]=Smooth(age/.09f)*Smooth((.31f-age)/.22f);
    a.progress[i]=Clamp(age/.31f);
    a.clouds[kEventClouds[i]].charge=a.pulse[i];
    a.events[i]=PatchPoint(&a.clouds[kEventClouds[i]],.54f,.52f,tick,true);
  }
  return a;
}

static bool PatchMesh(const RiftPatch *patch, const float matrix[16],
    int w, int h, uint16_t tick, float brightness, bool cloud) {
  const int columns=cloud?16:12,rows=cloud?8:6;
  if (!GridFits(columns,rows)) return false;
  const int base=s_rift.vertex_count;
  for (int y=0;y<=rows;++y) for (int x=0;x<=columns;++x) {
    const float u=(float)x/columns,v=(float)y/rows;
    const RiftPoint world=PatchPoint(patch,u,v,tick,cloud);
    Scene3DPoint p;
    if (!Scene3D_ProjectWorldPoint(matrix,world.x,world.y,world.z,w,h,&p)) {
      s_rift.vertex_count=base;
      return true; /* A hidden primitive must not fail the scene. */
    }
    const float edge=Smooth(fminf(fminf(u,1-u),fminf(v,1-v))*16);
    const float dx=(u-.54f)*3,dy=(v-.52f)*3;
    const float light=cloud && patch->charge>0?
        patch->charge*expf(-dx*dx-dy*dy)*.65f:0;
    const float gain=brightness*((cloud?.62f:.70f)+light);
    s_rift.vertices[s_rift.vertex_count++]=(ArRenderVertex2D){
        {p.x,p.y},{gain*.81f,gain*.87f,gain,patch->opacity*edge},
        {((patch->tile&1)+u)*.5f,((patch->tile>>1)+v)*.5f}};
  }
  GridIndices(base,columns,rows);
  return true;
}

static bool Clouds(ArRenderDevice *device, const float matrix[16], int w, int h,
    const RiftActivity *a, float brightness, unsigned layer) {
  BeginMesh();
  for (unsigned i=layer*2;i<layer*2+2;++i)
    if (!PatchMesh(&a->clouds[i],matrix,w,h,a->tick,brightness,true)) return false;
  return Submit(device,s_rift.clouds,kArRenderBlendMode_Alpha);
}

static bool Ruins(ArRenderDevice *device, const float matrix[16], int w, int h,
    const RiftActivity *a, float brightness) {
  BeginMesh();
  if (!PatchMesh(&a->monument,matrix,w,h,a->tick,brightness*.80f,false)) return false;
  for (unsigned i=0;i<kRuins;++i)
    if (!PatchMesh(&a->ruins[i],matrix,w,h,a->tick,brightness,false)) return false;
  return Submit(device,s_rift.ruins,kArRenderBlendMode_Alpha);
}

static bool Stars(ArRenderDevice *device, const float matrix[16], int w, int h,
    const RiftActivity *a, float yaw, float pitch, float brightness) {
  BeginMesh();
  /* A distant ivory corona, mostly veiled by the foreground cloud ridge.
   * Fine radial filaments imply a celestial presence without a second boss
   * silhouette or an opaque, dominant gold ring. Uses the star glow batch. */
  const float cx=w*.61f-yaw*h*.055f, cy=h*.075f+pitch*h*.04f;
  if (!Billboard(cx,cy,h*.21f,h*.19f,
      (ArRenderColorF){.86f,.88f,1,.13f*brightness})) return false;
  for (unsigned i=0;i<32;++i) {
    const float angle=i*kTau/32+.018f*sinf(Phase(a->tick,3));
    const float dx=cosf(angle),dy=sinf(angle);
    const float inner=h*(.027f+Random(731+i)*.012f);
    const float outer=h*(.13f+Random(991+i)*.10f);
    const float half_width=h*(.001f+Random(201+i)*.0018f);
    const ArRenderColorF color={1,.91f,.76f,brightness*(.12f+Random(127+i)*.12f)};
    const ArRenderVertex2D v[]={
      {{cx+dx*inner-dy*half_width,cy+dy*inner+dx*half_width},color,{0,0}},
      {{cx+dx*outer-dy*half_width,cy+dy*outer+dx*half_width},color,{1,0}},
      {{cx+dx*outer+dy*half_width,cy+dy*outer-dx*half_width},color,{1,1}},
      {{cx+dx*inner+dy*half_width,cy+dy*inner-dx*half_width},color,{0,1}},
    };
    if (!Quad(v)) return false;
  }
  for (unsigned i=0;i<kEvents;++i) {
    Scene3DPoint p;
    const RiftPoint world=a->events[i];
    if (!Scene3D_ProjectWorldPoint(matrix,world.x,world.y,world.z,w,h,&p)) continue;
    const float radius=h*(.10f+.07f*a->progress[i]);
    const ArRenderColorF color={i==0?.35f:.72f,i==0?.65f:.32f,1,
        brightness*a->pulse[i]*.34f};
    if (!Billboard(p.x,p.y,radius*1.6f,radius,color)) return false;
  }
  for (unsigned i = 0; i < kStars; ++i) {
    const unsigned seed = 1193+i*13;
    const float depth = Random(seed+2);
    Scene3DPoint p;
    if (!Scene3D_ProjectWorldPoint(matrix, (Random(seed)-.5f)*22,
        (Random(seed+1)-.5f)*12,-2-depth*6,w,h,&p)) continue;
    if (p.x < -12 || p.x > w+12 || p.y < -12 || p.y > h+12) continue;
    const float pulse = .55f+.45f*sinf(Phase(a->tick,23+i%37)+Random(seed+3)*kTau);
    const float size = (.45f+Random(seed+4)*.65f)*h/448.0f;
    const float alpha = brightness*pulse*(.30f+depth*.50f);
    const ArRenderColorF color = {Mix(.55f,1,depth),Mix(.75f,.65f,depth),1,alpha};
    if (!Billboard(p.x,p.y,size*2,size*2,color)) return false;
    if (i%43==0 && !Billboard(p.x,p.y,size*8,size*8,
        (ArRenderColorF){.5f,.66f,1,alpha*.20f})) return false;
  }
  return Submit(device,s_rift.glow,kArRenderBlendMode_Add);
}

static bool Fragments(ArRenderDevice *device, const float matrix[16], int w, int h,
    const RiftActivity *a, float brightness, bool near) {
  BeginMesh();
  struct Fragment { RiftPoint center; RiftRotation rotation; float size, depth; unsigned seed; } fragments[kFragments];
  unsigned count=0;
  for (unsigned i=near?24:0;i<(near?kFragments:24);++i) {
    const unsigned seed=9001+i*11;
    const float orbit=Phase(a->tick,19+i%19)+Random(seed)*kTau;
    RiftPatch p=Patch(Random(seed+1)*1.3f-.15f,.12f+Random(seed+2)*.40f,
        near?-1.8f:-5.2f,0,0,0,0,Rotation(0,0,0));
    p.center.x+=cosf(orbit)*(.28f+Random(seed+3)*.55f);
    p.center.y+=sinf(orbit)*.18f;
    p.center.z+=sinf(orbit+1.3f)*.55f;
    const struct Fragment f={.center=p.center,.seed=seed,
        .rotation=Rotation(Phase(a->tick,37+i%17)+Random(seed+6)*kTau,
            Phase(a->tick,53+i%23)+Random(seed+7)*kTau,Phase(a->tick,29+i%13)),
        .size=(near?.055f:.018f)+Random(seed+4)*(near?.055f:.040f),
        .depth=Scene3D_ClipDepth(matrix,p.center.x,p.center.y,p.center.z)};
    /* Far-to-near painter order is sufficient for these separated convex
     * fragments; only their front faces are submitted. No depth target. */
    unsigned j=count++;
    while (j && fragments[j-1].depth<f.depth) { fragments[j]=fragments[j-1];--j; }
    fragments[j]=f;
  }
  static const RiftPoint shape[]={{-1,0,0},{1.1f,0,0},{0,2.1f,0},
      {.15f,-1.8f,0},{0,0,.7f},{0,0,-.7f}};
  static const uint8_t faces[][3]={{4,0,3},{4,3,1},{4,1,2},{4,2,0},
      {5,3,0},{5,1,3},{5,2,1},{5,0,2}};
  for (unsigned i=0;i<count;++i) {
    const struct Fragment *f=&fragments[i];
    RiftPoint rotated[6];Scene3DPoint p[6];bool visible=true;
    for (unsigned j=0;j<6;++j) {
      rotated[j]=Rotate(f->rotation,shape[j]);
      if (!Scene3D_ProjectWorldPoint(matrix,f->center.x+rotated[j].x*f->size,
          f->center.y+rotated[j].y*f->size,f->center.z+rotated[j].z*f->size,w,h,&p[j])) {
        visible=false;break;
      }
    }
    if (!visible) continue;
    for (unsigned face=0;face<8;++face) {
      const unsigned j=faces[face][0],k=faces[face][1],l=faces[face][2];
      if ((p[k].x-p[j].x)*(p[l].y-p[j].y)-(p[k].y-p[j].y)*(p[l].x-p[j].x)>=0) continue;
      const RiftPoint u={rotated[k].x-rotated[j].x,rotated[k].y-rotated[j].y,rotated[k].z-rotated[j].z};
      const RiftPoint v={rotated[l].x-rotated[j].x,rotated[l].y-rotated[j].y,rotated[l].z-rotated[j].z};
      const RiftPoint n={u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
      const float length=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z);
      const float lit=.04f+.24f*Clamp((n.x*-.4f+n.y*.7f+n.z*.6f)/length);
      const float tint=Random(f->seed+5);
      const ArRenderColorF color={brightness*lit*Mix(.55f,1,tint),
          brightness*lit*.65f,brightness*(lit+.04f),1};
      if (s_rift.vertex_count+3>kMeshVertices || s_rift.index_count+3>kMeshIndices) return false;
      const unsigned corners[]={j,k,l};
      for (unsigned c=0;c<3;++c) {
        s_rift.indices[s_rift.index_count++]=s_rift.vertex_count;
        s_rift.vertices[s_rift.vertex_count++]=(ArRenderVertex2D){
            {p[corners[c]].x,p[corners[c]].y},color,{0,0}};
      }
    }
  }
  return Submit(device,ArRenderTexture_Invalid(),kArRenderBlendMode_Alpha);
}

static bool CosmicCurrents(ArRenderDevice *device, const float matrix[16], int w,
    int h, const RiftActivity *a, float brightness) {
  BeginMesh();
  for (unsigned event=0;event<kEvents;++event) {
    if (a->pulse[event]<.002f) continue;
    const RiftPatch *cloud=&a->clouds[kEventClouds[event]];
    for (unsigned branch=0;branch<3;++branch) {
      Scene3DPoint previous={0};bool previous_valid=false;
      for (unsigned i=0;i<=20;++i) {
        const float t=i/20.0f;
        const float u=.18f+t*.65f;
        const float v=.50f+.055f*sinf(t*9+branch*1.3f)+
            (Random(371+event*131+branch*29+i)-.5f)*.05f+(branch-1.0f)*t*.10f;
        const RiftPoint world=PatchPoint(cloud,u,v,a->tick,true);
        Scene3DPoint p={0};
        const bool valid=Scene3D_ProjectWorldPoint(matrix,world.x,world.y,world.z,w,h,&p);
        if (valid && previous_valid) {
          const float travel=(t-a->progress[event])*7;
          const float energy=expf(-travel*travel)*a->pulse[event]*brightness;
          ArRenderColorF color={event==0?.38f:.78f,event==0?.70f:.42f,1,energy*.25f};
          if (!GlowSegment(previous,p,h*.008f,color)) return false;
          color.a=energy*.46f;
          if (!GlowSegment(previous,p,h*.0018f,color)) return false;
        }
        previous=p;previous_valid=valid;
      }
    }
  }
  return Submit(device,s_rift.glow,kArRenderBlendMode_Add);
}

static bool FloorPoint(const DioramaAstralRiftFrame *frame, float x, float y,
    ArRenderPointF *point) {
  const DioramaProjection *projection=frame->projection;
  if (!projection || !projection->valid || !Diorama_ProjectCapturedPoint(projection,
      frame->floor_capture_x+x,frame->floor_capture_y+y,2,point,NULL,NULL)) return false;
  point->x-=projection->output_x;
  point->y-=projection->output_y;
  return true;
}

static bool FloorCloudPatch(const DioramaAstralRiftFrame *frame, double cx,
    double cy, double width, double height, unsigned tile, float opacity,
    double phase, float brightness, bool surface) {
  enum { columns=12, rows=6 };
  if (!GridFits(columns,rows)) return false;
  const int base=s_rift.vertex_count;
  /* Deformation is separable: each column's Y bend is shared by all rows,
   * and each row's X bend by all columns. Keep double precision until the
   * final projection to preserve native/WASM agreement at strong tilt. */
  double y_bend[columns+1];
  for (int column=0;column<=columns;++column)
    y_bend[column]=height*.10*sin(((double)column/columns)*9-phase);
  for (int row=0;row<=rows;++row) {
    const double v=(double)row/rows;
    const double x_bend=height*.08*sin(v*7+phase);
    for (int column=0;column<=columns;++column) {
      const double u=(double)column/columns;
      const float x=(float)(cx+(u-.5)*width+x_bend);
      const float y=(float)(cy+(v-.5)*height+y_bend[column]);
      ArRenderPointF p;
      if (!FloorPoint(frame,x,y,&p)) { s_rift.vertex_count=base;return true; }
      const float edge=Smooth(fminf(fminf(u,1-u),fminf(v,1-v))*8);
      const float below=surface?1:Smooth((y-4)/14);
      const float tint=Clamp((x+384)/768);
      const ArRenderColorF color={brightness*Mix(.52f,.94f,tint),
          brightness*Mix(.68f,.38f,tint),brightness,opacity*edge*below};
      s_rift.vertices[s_rift.vertex_count++]=(ArRenderVertex2D){{p.x,p.y},color,
          {((tile&1)+u)*.5f,((tile>>1)+.15f+v*.70f)*.5f}};
    }
  }
  GridIndices(base,columns,rows);
  return true;
}

static bool FloorClouds(ArRenderDevice *device, const DioramaAstralRiftFrame *frame,
    const RiftActivity *activity, float brightness, bool surface) {
  if (!frame->projection || !frame->projection->valid) return true;
  BeginMesh();
  const unsigned count=surface?5:6;
  for (unsigned i=0;i<count;++i) {
    const double phase=(double)Phase(activity->tick,(surface?37:23)+i*4)+i*1.7;
    const double center=-350.0+i*(surface?175:140)+sin(phase)*(surface?23:42);
    const double y=surface?-3+2*sin((double)Phase(activity->tick,31)+i):
        53+12*sin(phase+1.2);
    const double width=(surface?205:240)+35.0*Random(1501+i);
    const double height=surface?26+7.0*Random(1801+i):94+24.0*Random(2001+i);
    const unsigned tile=surface?2:i%4;
    const float opacity=surface?.48+.14*sin(phase+1):.65;
    if (!FloorCloudPatch(frame,center,y,width,height,tile,opacity,phase,
        brightness,surface)) return false;
  }
  return Submit(device,s_rift.clouds,kArRenderBlendMode_Alpha);
}

static bool FloorParticles(ArRenderDevice *device, const DioramaAstralRiftFrame *frame,
    const RiftActivity *activity, float brightness, bool stone) {
  if (!frame->projection || !frame->projection->valid) return true;
  BeginMesh();
  const unsigned count=stone?18:64;
  for (unsigned i=0;i<count;++i) {
    const unsigned seed=8101+i*23+(stone?0:3001);
    float age=Phase(activity->tick,41+i%23)/kTau+Random(seed);
    age-=floorf(age);
    const float alpha=Smooth(age/.16f)*Smooth((1-age)/.26f);
    const bool falling=stone || i%3==0;
    const float x=-365+730*Random(seed+1)+(age-.5f)*(16+24*Random(seed+2));
    const float y=falling?12+age*age*98:-2-age*(8+12*Random(seed+3));
    const float size=stone?1.1f+Random(seed+4)*1.8f:.35f+Random(seed+4)*.65f;
    const float angle=Phase(activity->tick,79+i%37)+Random(seed+5)*kTau;
    const float cs=cosf(angle),sn=sinf(angle),tint=Clamp((x+384)/768);
    const ArRenderColorF color=stone?
        (ArRenderColorF){brightness*.14f,brightness*.13f,brightness*.25f,alpha*.85f}:
        (ArRenderColorF){Mix(.44f,1,tint),Mix(.75f,.40f,tint),1,brightness*alpha*.48f};
    ArRenderVertex2D vertices[4];bool visible=true;
    static const float corners[][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    static const float shard[][2]={{-1,-.6f},{.75f,-1.2f},{1,1.1f},{-.65f,.5f}};
    for (unsigned corner=0;corner<4;++corner) {
      const float dx=(stone?shard[corner][0]:corners[corner][0])*size;
      const float dy=(stone?shard[corner][1]*1.8f:corners[corner][1])*size;
      ArRenderPointF p;
      if (!FloorPoint(frame,x+dx*cs-dy*sn,y+dx*sn+dy*cs,&p)) { visible=false;break; }
      vertices[corner]=(ArRenderVertex2D){{p.x,p.y},color,
          {(corners[corner][0]+1)*.5f,(corners[corner][1]+1)*.5f}};
    }
    if (visible && !Quad(vertices)) return false;
  }
  return Submit(device,stone?ArRenderTexture_Invalid():s_rift.glow,
      stone?kArRenderBlendMode_Alpha:kArRenderBlendMode_Add);
}

static bool Floor(ArRenderDevice *device, const DioramaAstralRiftFrame *frame,
    const RiftActivity *activity, float brightness) {
  const DioramaProjection *projection = frame->projection;
  if (!projection || !projection->valid) return true;
  BeginMesh();
  /* Authored lip is at v=0.675. Native standing feet are at world Y=208:
   * object Y=184 + its 24-pixel contact extent. The platform follows the exact
   * priority-2 plane through zoom/aspect/orbit instead of a fixed screen row. */
  enum { columns=48, rows=10 };
  const float span = 768;
  if (!GridFits(columns,rows)) return false;
  for (int row=0;row<=rows;++row) for (int x=0;x<=columns;++x) {
    const float u=(float)x/columns,t=.58f+.42f*row/rows;
    ArRenderPointF p;
    if (!FloorPoint(frame,(u-.5f)*span,(t-.675f)*span/kArtAspect,&p)) {
      BeginMesh();return true;
    }
    s_rift.vertices[s_rift.vertex_count++]=(ArRenderVertex2D){
        {p.x,p.y},
        {brightness*.78f,brightness*.78f,brightness*.84f,1},{u,t}};
  }
  GridIndices(0,columns,rows);
  if (!Submit(device,s_rift.floor,kArRenderBlendMode_Alpha)) return false;
  /* Same fixed projection/UVs, including zoom/orbit. Only emissive color
   * changes; energy follows the painted cracks instead of sliding new cracks
   * or the standing surface beneath the actors. */
  /* Charge depends only on the column. Evaluate the three Gaussian light
   * envelopes once per column instead of repeating them on all eleven rows. */
  float column_charge[columns+1];
  for (int column=0;column<=columns;++column) {
    const float u=s_rift.vertices[column].tex_coord.x;
    float charge=.50f;
    for (unsigned event=0;event<kEvents;++event) {
      const float center=.30f+event*.21f;
      const float dx=(u-center)*7;
      charge+=activity->pulse[event]*.48f*expf(-dx*dx);
    }
    column_charge[column]=charge;
  }
  for (int i=0;i<s_rift.vertex_count;++i) {
    ArRenderVertex2D *v=&s_rift.vertices[i];
    const float u=v->tex_coord.x,t=v->tex_coord.y;
    const float wave=.5f+.5f*sinf(u*29+t*13-Phase(activity->tick,113));
    const float crossing=.5f+.5f*sinf(u*47-t*19+Phase(activity->tick,71));
    const float crest=wave*wave*wave;
    const float branches=crossing*crossing*crossing*crossing;
    const float strength=brightness*Clamp(column_charge[i%(columns+1)]*
        (.26f+.95f*crest+.48f*branches));
    v->color=(ArRenderColorF){Mix(.53f,1,u),Mix(.74f,.44f,u),1,strength};
  }
  return Submit(device,s_rift.floor_emission,kArRenderBlendMode_Add);
}

static PresentationOutcome Draw(void *userdata, ArRenderRectI viewport) {
  const DioramaAstralRiftFrame *frame = userdata;
  ArRenderDevice *device = frame->device;
  const int w = viewport.w, h = viewport.h;
  const float brightness = (frame->inidisp&0x80)?0:(frame->inidisp&15)/15.0f;
  if (!brightness) {
    const ArRenderRectF screen = {0,0,w,h};
    return ArRenderDevice_DrawSolidRect(device,&screen,
        (ArRenderColorF){0,0,0,1},kArRenderBlendMode_Opaque)
        ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
  }
  /* Nebula is an opaque, viewport-covering mesh. Clearing beneath it would
   * add a redundant fullscreen draw on every visible frame. */
  /* Every temporal frequency is an integer multiple of the pausable 16-bit
   * clock period, so a long fight wraps without a discontinuity. */
  const RiftActivity activity=Activity(frame->tick);
  const Scene3DCamera camera = {.tilt_x=frame->pitch*.35f,.tilt_y=frame->yaw*.35f,
      .distance=kSkyDistance,.fov_y=.7853981634f};
  float matrix[16];
  Scene3D_BuildViewProjection(&camera,w,h,matrix);
  if (!Nebula(device,w,h,activity.tick,frame->yaw,frame->pitch,brightness) ||
      !Stars(device,matrix,w,h,&activity,frame->yaw,frame->pitch,brightness) ||
      !Clouds(device,matrix,w,h,&activity,brightness,0) ||
      !Ruins(device,matrix,w,h,&activity,brightness) ||
      !Fragments(device,matrix,w,h,&activity,brightness,false) ||
      !Clouds(device,matrix,w,h,&activity,brightness,1) ||
      !CosmicCurrents(device,matrix,w,h,&activity,brightness) ||
      !Fragments(device,matrix,w,h,&activity,brightness,true) ||
      !Clouds(device,matrix,w,h,&activity,brightness,2)) return kPresentationOutcome_CoreFailure;
  /* Retain textured color behind combat rather than fading most of the
   * composition to black as the first prototype did. */
  BeginMesh();
  const ArRenderVertex2D shade[] = {
    {{0,h*.4f},{0,0,0,0},{0,0}}, {{w,h*.4f},{0,0,0,0},{0,0}},
    {{w,h},{0,0,0,.46f},{0,0}}, {{0,h},{0,0,0,.46f},{0,0}},
  };
  return Quad(shade) && Submit(device,ArRenderTexture_Invalid(),kArRenderBlendMode_Alpha) &&
      FloorClouds(device,frame,&activity,brightness,false) &&
      FloorParticles(device,frame,&activity,brightness,true) &&
      Floor(device,frame,&activity,brightness) &&
      FloorClouds(device,frame,&activity,brightness,true) &&
      FloorParticles(device,frame,&activity,brightness,false)
      ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
}

bool DioramaAstralRift_Attach(bool enabled, DioramaScene *scene,
    DioramaRenderOptions *options, DioramaAstralRiftFrame *frame) {
  if (!enabled || !scene || !options || !frame || scene->backdrop ||
      scene->map_group!=7 || scene->map_number!=8 || scene->layer_section ||
      !isfinite(frame->yaw) || !isfinite(frame->pitch) ||
      !DioramaAstralRift_Prepare(frame->device)) return false;
  options->visible_planes &= ~((1u<<kDioramaPlane_Backdrop)|(1u<<SR_PPU_OVERLAY_BG1)|
      (1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Bg1Far)|(1u<<SR_PPU_OVERLAY_BG2)|
      (1u<<kDioramaPlane_Bg2Hi)|(1u<<kDioramaPlane_Bg2Far));
  options->skybox = kDioramaSky_Off;
  options->shoebox = false;
  scene->effect_obj_priority_mask |= 1u<<2; /* Also project without a preview actor. */
  scene->render = options;
  scene->backdrop = Draw;
  scene->backdrop_userdata = frame;
  return true;
}
