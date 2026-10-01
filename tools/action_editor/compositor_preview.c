/* Browser backend for the unmodified production compositor. Imports are batch
 * resource/draw operations, never pixel/vertex callbacks. No SDL or host globals. */
#include "diorama/diorama_snapshot.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include <stdlib.h>
#include <string.h>

#define IMPORT(NAME) __attribute__((import_module("ar"), import_name(#NAME)))
IMPORT(create) extern int WebCreate(int w, int h, int usage, int filter, int blend);
IMPORT(destroy) extern void WebDestroy(int t);
IMPORT(update) extern int WebUpdate(int t, int x, int y, int w, int h, const void *p, int pitch);
IMPORT(target) extern int WebTarget(int t);
IMPORT(viewport) extern int WebViewport(int set, int x, int y, int w, int h);
IMPORT(clip) extern int WebClip(int set, int x, int y, int w, int h);
IMPORT(clear) extern int WebClear(float r, float g, float b, float a);
IMPORT(geometry) extern int WebGeometry(int t, const void *v, int nv, const void *i, int ni,
                                      int blend, int u, int vv);
IMPORT(effect) extern int WebEffect(int kind, const float *params, int count);
IMPORT(available) extern int WebAvailable(int kind);

static uint8_t s_input[kDioramaSnapshotCapacity];
static uint8_t *s_packet;
static DioramaSnapshot s_scene;
static ArRenderDevice s_device;
static ArRenderTargetState s_state;
static ArRenderTextureDesc s_textures[128];
static int s_width, s_height;

ArRenderDevice *DioramaPreview_Device(void) { return &s_device; }

static bool Create(void *ctx, const ArRenderTextureDesc *d, ArRenderTexture *out) {
  (void)ctx;
  if (d->format != kArRenderPixelFormat_Argb8888) return false;
  int id = WebCreate(d->width, d->height, d->usage, d->filter, d->blend);
  if (id <= 0 || id >= 128) return false;
  s_textures[id] = *d;
  *out = (ArRenderTexture){(uintptr_t)id};
  return true;
}
static void Destroy(void *ctx, ArRenderTexture t) {
  (void)ctx;
  WebDestroy((int)t.value);
  s_textures[t.value] = (ArRenderTextureDesc){0};
}
static bool Update(void *ctx, ArRenderTexture t, const ArRenderRectI *r,
                   const void *p, int pitch) {
  (void)ctx;
  ArRenderTextureDesc d = s_textures[t.value];
  ArRenderRectI box = r ? *r : (ArRenderRectI){0,0,d.width,d.height};
  return WebUpdate((int)t.value, box.x, box.y, box.w, box.h, p, pitch) != 0;
}
static bool Target(void *ctx, ArRenderTexture t) {
  (void)ctx;
  if (!WebTarget((int)t.value)) return false;
  s_state.target = t;
  return true;
}
static bool Coordinates(void *ctx) { (void)ctx; return true; }
static bool Size(void *ctx, int *w, int *h) {
  (void)ctx;
  ArRenderTextureDesc d = s_textures[s_state.target.value];
  *w = s_state.target.value ? d.width : s_width;
  *h = s_state.target.value ? d.height : s_height;
  return true;
}
static bool Viewport(void *ctx, const ArRenderRectI *r) {
  (void)ctx;
  ArRenderRectI v = r ? *r : (ArRenderRectI){0};
  if (!WebViewport(r != NULL, v.x,v.y,v.w,v.h)) return false;
  s_state.viewport_set = r != NULL;
  s_state.viewport = v;
  return true;
}
static bool Clip(void *ctx, const ArRenderRectI *r) {
  (void)ctx;
  ArRenderRectI v = r ? *r : (ArRenderRectI){0};
  if (!WebClip(r != NULL, v.x,v.y,v.w,v.h)) return false;
  s_state.clip_enabled = r != NULL;
  s_state.clip = v;
  return true;
}
static bool Capture(void *ctx, ArRenderTargetState *out) {
  (void)ctx;
  *out = s_state;
  out->valid = true;
  return true;
}
static bool Restore(void *ctx, const ArRenderTargetState *s) {
  return Target(ctx, s->target) && Viewport(ctx, s->viewport_set ? &s->viewport : NULL) &&
      Clip(ctx, s->clip_enabled ? &s->clip : NULL);
}
static bool Clear(void *ctx, ArRenderColorF c) {
  (void)ctx;
  return WebClear(c.r,c.g,c.b,c.a) != 0;
}
static bool Geometry(void *ctx, ArRenderTexture t, const ArRenderVertex2D *v,
    int nv, const int32_t *i, int ni, const ArRenderDrawState *s) {
  (void)ctx;
  int blend = t.value ? s_textures[t.value].blend : kArRenderBlendMode_Alpha;
  if (s && (s->flags & kArRenderDrawState_Blend)) blend = s->blend;
  int u = s && (s->flags & kArRenderDrawState_Address) ? s->address_u : 0;
  int vv = s && (s->flags & kArRenderDrawState_Address) ? s->address_v : 0;
  return WebGeometry((int)t.value, v,nv,i,ni,blend,u,vv) != 0;
}
static bool Texture(void *ctx, ArRenderTexture t, const ArRenderRectF *src,
    const ArRenderRectF *dst, const ArRenderDrawState *s) {
  const ArRenderTextureDesc d = s_textures[t.value];
  ArRenderRectF a = src ? *src : (ArRenderRectF){0,0,d.width,d.height};
  int w,h;
  Size(ctx,&w,&h);
  ArRenderRectF b = dst ? *dst : (ArRenderRectF){0,0,w,h};
  ArRenderColorF c = s && (s->flags & kArRenderDrawState_Tint)
      ? s->tint : (ArRenderColorF){1,1,1,1};
  const float u0=a.x/d.width, v0=a.y/d.height;
  const float u1=(a.x+a.w)/d.width, v1=(a.y+a.h)/d.height;
  const ArRenderVertex2D v[] = {
    {{b.x,b.y},c,{u0,v0}}, {{b.x+b.w,b.y},c,{u1,v0}},
    {{b.x+b.w,b.y+b.h},c,{u1,v1}}, {{b.x,b.y+b.h},c,{u0,v1}},
  };
  const int32_t indices[] = {0,1,2,0,2,3};
  return Geometry(ctx,t,v,4,indices,6,s);
}
static bool Present(void *ctx) { (void)ctx; return true; }
static const char *Error(void *ctx) { (void)ctx; return "WebGL2 backend failed"; }
static const ArRenderBackendOps kOps = {sizeof(ArRenderBackendOps),
  Create,Destroy,Update,Target,Coordinates,Size,Viewport,Clip,Capture,Restore,
  Clear,Texture,Geometry,Present,Error};

bool DioramaEffectBackend_IsAvailable(ArRenderDevice *d, DioramaEffectKind k) {
  (void)d;
  return WebAvailable(k) != 0;
}
bool DioramaEffectBackend_BindBlur(ArRenderDevice *d, const DioramaBlurEffectParams *p) {
  (void)d;
  float a[] = {p->texel_width,p->texel_height,p->radius};
  return WebEffect(0,a,3) != 0;
}
bool DioramaEffectBackend_BindRimLight(ArRenderDevice *d, const DioramaRimLightEffectParams *p) {
  (void)d;
  float a[] = {p->texel_width,p->texel_height,p->strength};
  return WebEffect(1,a,3) != 0;
}
bool DioramaEffectBackend_BindDofEdge(ArRenderDevice *d, const DioramaDofEdgeEffectParams *p) {
  (void)d; float a[] = {p->texel_width,p->texel_height,p->blur_radius,p->u_min,
      p->u_max,p->v_min,p->v_max,p->edge_feather,p->lower_content_v_max};
  return WebEffect(2,a,9) != 0;
}
bool DioramaEffectBackend_BindPrioritySurface(ArRenderDevice *d,
    const DioramaPrioritySurfaceEffectParams *p) {
  (void)d;
  float a[] = {p->width,p->height,p->high_band,p->additive};
  return WebEffect(3,a,4) != 0;
}
bool DioramaEffectBackend_Unbind(ArRenderDevice *d) { (void)d; return WebEffect(-1,NULL,0) != 0; }
void DioramaEffectBackend_Reset(ArRenderDevice *d) { (void)DioramaEffectBackend_Unbind(d); }

uint8_t *DioramaPreview_Input(void) { return s_input; }
unsigned DioramaPreview_Capacity(void) { return sizeof(s_input); }
int DioramaPreview_Width(void) { return s_scene.view.viewport.w; }
int DioramaPreview_Height(void) { return s_scene.view.viewport.h; }
unsigned DioramaPreview_Version(void) { return 1; }
int DioramaPreview_Init(int maximum_texture_size) {
  const ArRenderCapabilities caps = {
    .flags = kArRenderCapability_StreamingTextures | kArRenderCapability_RenderTargets |
      kArRenderCapability_Geometry | kArRenderCapability_CustomShaders |
      kArRenderCapability_TextureWrap | kArRenderCapability_BlendAdd |
      kArRenderCapability_BlendModulate | kArRenderCapability_BlendMultiply |
      kArRenderCapability_ScopedRenderTargets,
    .maximum_texture_width=maximum_texture_size, .maximum_texture_height=maximum_texture_size,
    .maximum_render_target_width=maximum_texture_size,
    .maximum_render_target_height=maximum_texture_size,
  };
  return ArRenderDevice_Init(&s_device,&kOps,&s_state,caps);
}
void DioramaPreview_Reset(void) {
  Diorama_ResetCompositorResources(&s_device);
  DioramaSnapshot_ReleaseTextures(&s_scene,&s_device);
  free(s_packet);
  s_packet = NULL;
  memset(&s_scene,0,sizeof(s_scene));
}
int DioramaPreview_Load(unsigned size) {
  DioramaSnapshot next;
  if (size > sizeof(s_input) || !DioramaSnapshot_Decode(s_input,size,&next)) return 0;
  uint8_t *owned = malloc(size);
  if (!owned) return 0;
  memcpy(owned,s_input,size);
  if (!DioramaSnapshot_Decode(owned,size,&next) || !DioramaSnapshot_Upload(&next,&s_device)) {
    free(owned);
    return 0;
  }
  DioramaPreview_Reset();
  s_packet = owned;
  s_scene = next;
  DioramaSnapshot_Bind(&s_scene);
  return 1;
}
bool DioramaPreview_Begin(int width, int height) {
  if (width < 1 || height < 1 || width > 2048 || height > 2048) return false;
  s_width = width; s_height = height;
  return Target(NULL, ArRenderTexture_Invalid()) && Viewport(NULL, NULL) &&
      Clip(NULL, NULL) && Clear(NULL, (ArRenderColorF){0,0,0,1});
}
int DioramaPreview_Render(int width,int height,float zoom,float yaw,float pitch,int skybox) {
  if (!s_packet || width < 1 || height < 1 || width > 2048 || height > 2048 ||
      !(zoom >= 0.25f && zoom <= 4) || !(yaw >= -1 && yaw <= 1) ||
      !(pitch >= -1 && pitch <= 1) || skybox < -1 || skybox > 2) return 0;
  if (!DioramaPreview_Begin(width,height)) return 0;
  DioramaView view=s_scene.view;
  view.viewport=(ArRenderRectI){0,0,width,height};
  view.distance_scale *= zoom;
  view.camera.tilt_y += yaw;
  view.camera.tilt_x += pitch;
  DioramaRenderOptions options=s_scene.options;
  if (skybox >= 0) options.skybox=(DioramaSkyMode)skybox;
  DioramaScene scene=s_scene.scene;
  scene.render=&options;
  DioramaProjection projection;
  return PresentationOutcome_IsUsable(Diorama_Composite(
      &s_device,&s_scene.capture,&view,&scene,&projection));
}
