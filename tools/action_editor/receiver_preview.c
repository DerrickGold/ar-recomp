#include "receiver_preview.h"

#include "snes/ppu.h"
#include <math.h>
#include <string.h>

enum { kArtSize = 96, kArtAnchor = 48, kCharacters = 8192, kPalette = 96 };
static uint32_t s_art[2][kArtSize * kArtSize];
static ArRenderRectI s_art_bounds[2];
static bool s_ready, s_cached;
static uint32_t s_pixels[SR_PPU_SURFACE_MAX_WIDTH * SR_PPU_SURFACE_MAX_HEIGHT];
static ArRenderRectI s_previous_bounds;
typedef struct ReceiverPixels {
  int x, y;
  uint16_t multiply[3], add[3];
} ReceiverPixels;
static struct PreviewKey {
  ReceiverPixels roles[2];
  unsigned mask, facing;
  int width, height;
} s_key;

static uint16_t Word(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

bool EditorReceiverPreview_Load(const uint8_t *bytes, size_t size) {
  if (!bytes || size < kCharacters + kPalette + 5) return false;
  const uint8_t *picture = bytes + kCharacters + kPalette;
  const unsigned count = picture[4];
  if (!count || count > 64 || size != kCharacters + kPalette + 5 + count * 7)
    return false;
  PpuObjPart parts[2][64];
  for (unsigned facing = 0; facing < 2; ++facing) for (unsigned i = 0; i < count; ++i) {
    const uint8_t *p = picture + 5 + i * 7;
    const int x = kArtAnchor - (int8_t)picture[facing] + p[1 + facing];
    const int y = kArtAnchor - (int8_t)picture[2] - 1 + p[3];
    const uint16_t attr = Word(p + 5);
    const int side = p[0] ? 16 : 8;
    if (p[0] > 1 || (attr & 0x100) || ((attr >> 9) & 7) >= 3 ||
        x < 0 || y < 0 || x + side > kArtSize || y + side > kArtSize) return false;
    /* Native player attribute bias: palette 4, common OBJ bank zero. */
    parts[facing][i] = (PpuObjPart){.x=x, .y=y, .size=side,
        .tile_attr=(uint16_t)((attr ^ (facing ? 0x4000 : 0)) | 0x0800)};
  }
  Ppu *ppu = ppu_init();
  if (!ppu) return false;
  ppu->obsel = 1;
  ppu->inidisp = 15;
  for (unsigned i = 0; i < kCharacters / 2; ++i) ppu->vram[0x2000 + i] = Word(bytes + i * 2);
  for (unsigned i = 0; i < kPalette / 2; ++i) ppu->cgram[0xc0 + i] = Word(bytes + kCharacters + i * 2);
  uint32_t art[2][kArtSize * kArtSize];
  const PpuObjRangeBounds bounds = {.x0=0, .y0=0, .x1=kArtSize, .y1=kArtSize};
  const bool ok = PpuRasterizeParts(ppu, parts[0], count, &bounds, art[0], kArtSize, kArtSize, kArtSize * 4) &&
      PpuRasterizeParts(ppu, parts[1], count, &bounds, art[1], kArtSize, kArtSize, kArtSize * 4);
  ppu_free(ppu);
  if (!ok) return false;
  ArRenderRectI extents[2];
  for (unsigned facing = 0; facing < 2; ++facing) {
    int x0=kArtSize, y0=kArtSize, x1=0, y1=0;
    for (int y=0; y<kArtSize; ++y) for (int x=0; x<kArtSize; ++x) {
      if (!art[facing][y*kArtSize+x]) continue;
      if (x<x0) x0=x; if (y<y0) y0=y;
      if (x+1>x1) x1=x+1; if (y+1>y1) y1=y+1;
    }
    if (x1<=x0 || y1<=y0) return false;
    extents[facing]=(ArRenderRectI){x0,y0,x1-x0,y1-y0};
  }
  memcpy(s_art,art,sizeof(art)); memcpy(s_art_bounds,extents,sizeof(extents));
  s_ready=true; s_cached=false;
  return true;
}
bool EditorReceiverPreview_Ready(void) { return s_ready; }
void EditorReceiverPreview_Reset(void) {
  memset(s_pixels,0,sizeof(s_pixels));
  s_previous_bounds=(ArRenderRectI){0}; s_cached=false;
}

static ArRenderRectI Union(ArRenderRectI a, ArRenderRectI b) {
  if (!a.w || !a.h) return b;
  if (!b.w || !b.h) return a;
  const int x=a.x<b.x?a.x:b.x, y=a.y<b.y?a.y:b.y;
  const int x1=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w;
  const int y1=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
  return (ArRenderRectI){x,y,x1-x,y1-y};
}
static ArRenderRectI Clip(ArRenderRectI r,int width,int height) {
  int x=r.x<0?0:r.x, y=r.y<0?0:r.y;
  int x1=r.x+r.w>width?width:r.x+r.w, y1=r.y+r.h>height?height:r.y+r.h;
  return x1>x && y1>y ? (ArRenderRectI){x,y,x1-x,y1-y} : (ArRenderRectI){0};
}
bool EditorReceiverPreview_Capture(ArRenderDevice *device,
    const EditorReceiverPreviewSettings *settings, const ActionReceiverLighting *lighting,
    int camera_x,int camera_y,int extra,const DioramaCapture *capture,
    ArRenderTexture *texture,const uint8_t **pixels,unsigned *uploads) {
  *pixels=NULL;
  if (!settings->mask) return true;
  if (!s_ready) return false;
  struct PreviewKey key={.mask=settings->mask, .facing=settings->flipped,
      .width=capture->width+capture->obj_apron*2, .height=capture->height};
  const ArRenderRectI art=s_art_bounds[key.facing];
  const int feet=art.y+art.h;
  ArRenderRectI bounds[2], combined={0};
  for (unsigned role=0; role<2; ++role) {
    if (!(key.mask & (1u<<role))) { bounds[role]=(ArRenderRectI){0}; continue; }
    ReceiverPixels *r=&key.roles[role];
    r->x=settings->x[role]-camera_x+extra+capture->obj_apron-kArtAnchor;
    r->y=settings->feet_y[role]-camera_y+capture->authentic_y0-feet;
    float multiply[3]={1,1,1},add[3]={0};
    if (lighting) ActionEffectReceivers_Sample(lighting,
        role?kActionReceiver_Enemies:kActionReceiver_Player,
        settings->x[role],settings->feet_y[role]-(feet-kArtAnchor),multiply,add);
    for (unsigned c=0; c<3; ++c) {
      r->multiply[c]=(uint16_t)(multiply[c]*256+.5f);
      r->add[c]=(uint16_t)(add[c]*255+.5f);
    }
    bounds[role]=Clip((ArRenderRectI){r->x+art.x,r->y+art.y,art.w,art.h},key.width,key.height);
    combined=Union(combined,bounds[role]);
  }
  const bool created=!ArRenderTexture_IsValid(*texture);
  if (created) {
    const ArRenderTextureDesc desc={.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming,
        .filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device,&desc,texture)) return false;
  }
  if (created || !s_cached || memcmp(&key,&s_key,sizeof(key))) {
    const int pitch=SR_PPU_SURFACE_MAX_WIDTH;
    const ArRenderRectI dirty=created
        ? (ArRenderRectI){0,0,pitch,SR_PPU_SURFACE_MAX_HEIGHT} : Union(combined,s_previous_bounds);
    for (int y=dirty.y; y<dirty.y+dirty.h; ++y)
      memset(s_pixels+y*pitch+dirty.x,0,(size_t)dirty.w*4);
    /* Player wins overlaps, as the earlier native OAM owner does. */
    for (int role=1; role>=0; --role) {
      const ReceiverPixels *r=&key.roles[role];
      const ArRenderRectI box=bounds[role];
      for (int y=box.y; y<box.y+box.h; ++y) for (int x=box.x; x<box.x+box.w; ++x) {
        const uint32_t source=s_art[key.facing][(y-r->y)*kArtSize+x-r->x];
        if (!source) continue;
        uint32_t result=source&0xff000000u;
        /* Exact full-brightness OBJ capture transform, including integer
         * quantization/saturation. Rim samples these already-lit pixels. */
        for (unsigned c=0; c<3; ++c) {
          const unsigned shift=16-8*c;
          unsigned v=((source>>shift)&255u)*r->multiply[c]/256u+r->add[c];
          result|=(v>255?255:v)<<shift;
        }
        s_pixels[y*pitch+x]=result;
      }
    }
    if (dirty.w && dirty.h) {
      if (!ArRenderDevice_UpdateTexture(device,*texture,&dirty,s_pixels+dirty.y*pitch+dirty.x,pitch*4)) {
        s_cached=false; s_previous_bounds=dirty; return false;
      }
      ++*uploads;
    }
    s_key=key; s_previous_bounds=combined; s_cached=true;
  }
  if (combined.w && combined.h) *pixels=(const uint8_t *)s_pixels;
  return true;
}

bool EditorReceiverPreview_Pick(const DioramaProjection *p,
    float sx,float sy,ArRenderPointF *out) {
  if (!p || !out || !p->valid || !isfinite(sx) || !isfinite(sy)) return false;
  float x=128,y=112;
  /* Bounded Newton inversion of the actual published plane, including room
   * shape, framing, aspect and tilt. No per-frame work; used only on input. */
  for (int i=0;i<12;++i) {
    ArRenderPointF a,b,c;
    if (!Diorama_ProjectCapturedPoint(p,x,y,2,&a,NULL,NULL) ||
        !Diorama_ProjectCapturedPoint(p,x+1,y,2,&b,NULL,NULL) ||
        !Diorama_ProjectCapturedPoint(p,x,y+1,2,&c,NULL,NULL)) return false;
    const float dx=sx-a.x,dy=sy-a.y;
    if (fabsf(dx)+fabsf(dy)<.05f) { *out=(ArRenderPointF){x,y}; return true; }
    const float bx=b.x-a.x,by=b.y-a.y,cx=c.x-a.x,cy=c.y-a.y;
    const float det=bx*cy-cx*by;
    if (!isfinite(det) || fabsf(det)<.00001f) return false;
    x+=(dx*cy-cx*dy)/det; y+=(bx*dy-dx*by)/det;
    if (!isfinite(x) || !isfinite(y) || fabsf(x)>32768 || fabsf(y)>32768) return false;
  }
  return false;
}
