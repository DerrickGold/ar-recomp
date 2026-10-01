#include "room_scene.h"
#include "diorama/diorama_snapshot.h"
#include "diorama/diorama_rom_backdrop.h"
#include <stdlib.h>
#include <string.h>

ArRenderDevice *DioramaPreview_Device(void);
uint8_t *DioramaPreview_Input(void);
void DioramaPreview_Reset(void);
bool DioramaPreview_Begin(int width, int height);

static EditorRoomScene *s_room;
static ArRenderTexture s_textures[kDioramaPlane_Count];
static const uint8_t *s_pixels[kDioramaPlane_Count];
static uint32_t s_storage[7][640 * 352];
static uint32_t s_next[640 * 352];
static const int kPlanes[] = {kDioramaPlane_Backdrop, SR_PPU_OVERLAY_BG1,
  kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG2,
  kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far};
static bool s_cached;
static int s_x, s_y, s_extra, s_vertical;
static uint32_t s_frame;
static unsigned s_uploads;
static uint64_t s_bg2_revision;
static ArRenderTexture s_page, s_view;
static uint32_t s_view_pixels[640 * 352];
static uint64_t s_view_revision;
static uint32_t s_page_pixels[256 * 256];
static int s_named_source;
static uint32_t s_named_art[256 * 256], s_named_default, s_named_fill;
static ArRenderTexture s_named_texture;
static bool s_named_uploaded;

int RoomPreview_SkyboxSource(void) {
  if (!s_room) return 0;
  DioramaResolvedLayer layers[kDioramaPlane_Count];
  int count = Diorama_ResolveSceneLayers(EditorRoomScene_Scene(s_room), layers);
  return DioramaLayerOrder_SkyboxSource(layers, count);
}
unsigned RoomPreview_SkyboxRoom(void) {
  uint8_t g, m, bg;
  return DioramaLayerOrder_DecodeActionBgSource(RoomPreview_SkyboxSource(), &g, &m, &bg)
      ? ((unsigned)g << 16) | ((unsigned)m << 8) | bg : 0;
}
int RoomPreview_LoadSkybox(unsigned size) {
  ActionSceneSnapshot assets;
  uint8_t g, m, bg;
  uint32_t default_fill;
  const int source = RoomPreview_SkyboxSource();
  if (!DioramaLayerOrder_DecodeActionBgSource(source, &g, &m, &bg) ||
      size > kActionSceneSnapshotMaxBytes ||
      !ActionSceneSnapshot_Decode(DioramaPreview_Input(), size, &assets) ||
      assets.scene.group != g || assets.scene.map != m ||
      !DioramaRomBackdrop_RenderScenePage(&assets.scene, bg, 0, true,
          &default_fill, s_next, 256 * 256)) return 0;
  memcpy(s_named_art, s_next, sizeof(s_named_art));
  s_named_default = default_fill; s_named_source = source; s_named_uploaded = false;
  return 1;
}
static ArRenderTexture ResolveSkybox(void *context, ArRenderDevice *device, int source,
    bool configured, uint32_t fill, bool *failed) {
  (void)context; *failed = false;
  if (source != s_named_source || !source) return ArRenderTexture_Invalid();
  if (!ArRenderTexture_IsValid(s_named_texture)) {
    const ArRenderTextureDesc desc = {.width = 256, .height = 256,
      .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_named_texture)) {
      *failed = true; return ArRenderTexture_Invalid();
    }
  }
  if (!configured) fill = s_named_default;
  if (!s_named_uploaded || s_named_fill != fill) {
    for (unsigned i = 0; i < 256 * 256; ++i) s_next[i] = s_named_art[i] ? s_named_art[i] : fill;
    if (!ArRenderDevice_UpdateTexture(device, s_named_texture, NULL, s_next, 256 * 4)) {
      *failed = true; return ArRenderTexture_Invalid();
    }
    s_named_fill = fill; s_named_uploaded = true; ++s_uploads;
  }
  return s_named_texture;
}

uint32_t RoomPreview_Hash(void) { return EditorRoomScene_Hash(s_room); }
unsigned RoomPreview_Width(void) { return s_room ? EditorRoomScene_Width(s_room) : 0; }
unsigned RoomPreview_Height(void) { return s_room ? EditorRoomScene_Height(s_room) : 0; }
unsigned RoomPreview_Uploads(void) { return s_uploads; }
void RoomPreview_Reset(void) {
  ArRenderDevice *device = DioramaPreview_Device();
  Diorama_ResetCompositorResources(device);
  for (unsigned i = 0; i < kDioramaPlane_Count; ++i) {
    ArRenderDevice_DestroyTexture(device, s_textures[i]);
    s_textures[i] = ArRenderTexture_Invalid(); s_pixels[i] = NULL;
  }
  ArRenderDevice_DestroyTexture(device, s_page);
  s_page = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(device, s_view); s_view = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(device, s_named_texture);
  s_named_texture = ArRenderTexture_Invalid(); s_named_source = 0; s_named_uploaded = false;
  EditorRoomScene_Destroy(s_room); s_room = NULL; s_cached = false;
  memset(s_storage, 0, sizeof(s_storage));
}
int RoomPreview_Load(unsigned size) {
  ActionSceneSnapshot assets;
  if (size > kActionSceneSnapshotMaxBytes ||
      !ActionSceneSnapshot_Decode(DioramaPreview_Input(), size, &assets)) return 0;
  EditorRoomScene *next = EditorRoomScene_Create(&assets);
  if (!next) return 0;
  RoomPreview_Reset(); DioramaPreview_Reset(); s_room = next;
  return 1;
}
int RoomPreview_Configure(unsigned size) {
  if (!s_room || size > 1024 * 1024 || memchr(DioramaPreview_Input(), 0, size)) return 0;
  DioramaPreview_Input()[size] = 0;
  if (!EditorRoomScene_Configure(s_room, (char *)DioramaPreview_Input())) return 0;
  s_cached = false;
  return 1;
}
static bool Upload(void) {
  ArRenderDevice *device = DioramaPreview_Device();
  const DioramaCapture *capture = EditorRoomScene_Capture(s_room);
  const SrSceneSurfaces *surfaces = EditorRoomScene_Surfaces(s_room);
  for (unsigned i = 0; i < sizeof(kPlanes) / sizeof(kPlanes[0]); ++i) {
    int plane = kPlanes[i];
    const uint8_t *source = capture->pixels[plane];
    s_pixels[plane] = NULL;
    if (!source) continue;
    memset(s_next, 0, sizeof(s_next));
    for (int y = 0; y < surfaces->height; ++y) {
      uint32_t *row = s_next + y * 640;
      memcpy(row, source + (size_t)y * surfaces->pitch_pixels * 4, surfaces->pitch_pixels * 4);
      if (plane == kDioramaPlane_Backdrop)
        for (int x = 64; x < surfaces->width + 64; ++x) row[x] |= UINT32_C(0xff000000);
    }
    bool created = false;
    if (!ArRenderTexture_IsValid(s_textures[plane])) {
      const ArRenderTextureDesc desc = {.width = 640, .height = 352,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_textures[plane])) return false;
      created = true;
    }
    if (created || memcmp(s_storage[i], s_next, sizeof(s_next))) {
      if (!ArRenderDevice_UpdateTexture(device, s_textures[plane], NULL, s_next, 640 * 4)) return false;
      memcpy(s_storage[i], s_next, sizeof(s_next)); ++s_uploads;
      if (plane == SR_PPU_OVERLAY_BG2) ++s_bg2_revision;
    }
    s_pixels[plane] = (const uint8_t *)s_storage[i];
  }
  if (surfaces->background_view) {
    bool created = false;
    if (!ArRenderTexture_IsValid(s_view)) {
      const ArRenderTextureDesc desc = {.width = 640, .height = 352,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_view)) return false;
      created = true;
    }
    memset(s_next, 0, sizeof(s_next));
    for (int y = 0; y < surfaces->height; ++y)
      memcpy(s_next + y * 640, surfaces->background_view + y * surfaces->view_width,
          surfaces->view_width * sizeof(uint32_t));
    if (created || memcmp(s_view_pixels, s_next, sizeof(s_view_pixels))) {
      if (!ArRenderDevice_UpdateTexture(device, s_view, NULL, s_next, 640 * 4)) return false;
      memcpy(s_view_pixels, s_next, sizeof(s_view_pixels)); ++s_uploads; ++s_view_revision;
    }
  }
  if (surfaces->native_pages[1]) {
    bool created = false;
    if (!ArRenderTexture_IsValid(s_page)) {
      const ArRenderTextureDesc desc = {.width = 256, .height = 256,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &s_page)) return false;
      created = true;
    }
    if (created || memcmp(s_page_pixels, surfaces->native_pages[1], sizeof(s_page_pixels))) {
      if (!ArRenderDevice_UpdateTexture(device, s_page, NULL, surfaces->native_pages[1], 256 * 4)) return false;
      memcpy(s_page_pixels, surfaces->native_pages[1], sizeof(s_page_pixels)); ++s_uploads;
    }
  }
  return true;
}
int RoomPreview_Render(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float distance, float yaw, float pitch, int skybox, int pixel_aspect) {
  if (!s_room || !(distance >= 0.5f && distance <= 2.5f) || !(yaw >= -0.7f && yaw <= 0.7f) ||
      !(pitch >= -0.7f && pitch <= 0.7f) || skybox < 0 || skybox > 2 ||
      pixel_aspect < 0 || pixel_aspect > 1 || width < 1 || height < 1 ||
      width > 2048 || height > 2048) return 0;
  s_uploads = 0;
  if (!s_cached || x != s_x || y != s_y || frame != s_frame || extra != s_extra || vertical != s_vertical) {
    s_cached = false;
    if (!EditorRoomScene_Render(s_room, x, y, frame, extra, vertical) || !Upload()) return 0;
    s_x = x; s_y = y; s_frame = frame; s_extra = extra; s_vertical = vertical; s_cached = true;
  }
  if (!DioramaPreview_Begin(width, height)) return 0;
  DioramaCapture capture = *EditorRoomScene_Capture(s_room);
  capture.textures = s_textures; capture.pixels = s_pixels;
  /* Orbit/aspect changes reuse the filtered skybox until its pixels change. */
  capture.bg2_revision = s_bg2_revision;
  capture.bg2_dynamic = false;
  DioramaSkyboxView extended = *capture.skybox;
  if (EditorRoomScene_Surfaces(s_room)->background_view) {
    extended.texture = s_view; extended.revision = s_view_revision; capture.skybox = &extended;
  }
  const DioramaSkyboxView page = {.texture = s_page, .width = 256, .periodic = true};
  if (skybox == kDioramaSky_Only && EditorRoomScene_Surfaces(s_room)->native_pages[1])
    capture.skybox = &page;
  DioramaScene scene = *EditorRoomScene_Scene(s_room);
  DioramaRenderOptions options = *scene.render;
  options.resolve_skybox = ResolveSkybox;
  options.skybox = (DioramaSkyMode)skybox; scene.render = &options;
  DioramaView view = {.camera = {.tilt_x = pitch, .tilt_y = yaw},
    .distance_scale = distance, .camera_framing_weight = 1, .pixel_aspect = pixel_aspect,
    .visible_width = capture.width, .viewport = {0,0,width,height}};
  DioramaProjection projection;
  return PresentationOutcome_IsUsable(Diorama_Composite(
      DioramaPreview_Device(), &capture, &view, &scene, &projection));
}
