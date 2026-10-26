#include "actraiser2/ar2_presentation.h"
#include "app/settings.h"
#include "present/present.h"
#include "render/presentation_layout.h"
#include <math.h>
#include <stdlib.h>

static ArRenderDevice *s_device;
static ArRenderTexture s_main, s_hud, s_screen_objects, s_planes[kDioramaPlane_Count];
static DioramaCameraObserver s_observer;
static DioramaCameraPresenter s_presenter;
static DioramaCameraFrame s_camera;
static unsigned s_frame;
static uint16_t s_scene, s_x, s_y;
static uint64_t s_capture_ns;

/* Native AR2 priority ownership is game data; presentation policy and camera
 * controls are captured from the same owners used by AR1 each frame. */
static const DioramaResolvedLayer kLayers[] = {
    {.plane = kDioramaPlane_Backdrop, .z = 0, .alpha = 255},
    {.plane = SR_PPU_OVERLAY_OBJ, .z = .51f, .alpha = 255},
    {.plane = kDioramaPlane_Obj1, .z = .51f, .alpha = 255},
    {.plane = SR_PPU_OVERLAY_BG2, .z = .20f, .alpha = 255},
    {.plane = SR_PPU_OVERLAY_BG1, .z = .50f, .alpha = 255},
    {.plane = kDioramaPlane_Obj2, .z = .51f, .alpha = 255},
    {.plane = kDioramaPlane_Bg2Hi, .z = .21f, .alpha = 255},
    {.plane = kDioramaPlane_Bg1Hi, .z = .51f, .alpha = 255},
    {.plane = kDioramaPlane_Obj3, .z = .52f, .alpha = 255},
    {.plane = kDioramaPlane_Bg2Far, .z = .60f, .alpha = 255},
    {.plane = SR_PPU_OVERLAY_BG3, .z = .60f, .alpha = 255},
};

static bool CreateTexture(ArRenderTexture *out, ArRenderBlendMode blend) {
  const ArRenderTextureDesc desc = {AR2_CAPTURE_PITCH, AR2_CAPTURE_HEIGHT,
      kArRenderPixelFormat_Argb8888, kArRenderTextureUsage_Streaming,
      kArRenderFilter_Nearest, blend};
  return ArRenderDevice_CreateTexture(s_device, &desc, out);
}

bool Ar2Presentation_Open(ArRenderDevice *device) {
  s_device = device;
  s_observer = (DioramaCameraObserver)DIORAMA_CAMERA_OBSERVER_INIT;
  s_presenter = (DioramaCameraPresenter)DIORAMA_CAMERA_PRESENTER_INIT;
  s_frame = UINT32_MAX;
  s_capture_ns = 0;
  if (!CreateTexture(&s_main, kArRenderBlendMode_Opaque) ||
      !CreateTexture(&s_hud, kArRenderBlendMode_Alpha) ||
      !CreateTexture(&s_screen_objects, kArRenderBlendMode_Alpha)) goto failed;
  for (unsigned i = 0; i < sizeof(kLayers) / sizeof(*kLayers); i++) {
    const int plane = kLayers[i].plane;
    if (!CreateTexture(&s_planes[plane], plane == kDioramaPlane_Backdrop
            ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha)) goto failed;
  }
  return true;
failed:
  Ar2Presentation_Close();
  return false;
}

static DioramaCameraView Camera(const ArImportedGameFrame *frame, uint64_t now) {
  if (frame->frame != s_frame) {
    const unsigned ticks = frame->frame - s_frame;
    const bool continuous = frame->scene == s_scene && ticks > 0 && ticks <= 5;
    int dx = continuous ? ((int)frame->player_x - s_x) / (int)ticks : 0;
    int dy = continuous ? ((int)frame->player_y - s_y) / (int)ticks : 0;
    /* Room warps/retries are discontinuities, not camera impulses. */
    if (abs(dx) > 32 || abs(dy) > 32) dx = dy = 0;
    const DioramaCameraObservation observation = {
        .velocity_x = dx * 256, .velocity_y = dy * 256, .hp = frame->health,
        .horizontal_locked = frame->video.world_width <= 256 + 2 * frame->video.display_extra};
    if (!continuous) s_observer.previous_hp = frame->health;
    s_camera.motion = DioramaCamera_Observe(&s_observer, &observation, continuous ? ticks : 1);
    s_frame = frame->frame;
    s_scene = frame->scene;
    s_x = frame->player_x;
    s_y = frame->player_y;
    s_capture_ns = now;
  }
  Diorama_CaptureCameraPresentationState(&s_camera.controls);
  s_camera.reactive_strength = g_settings.diorama_reactive_strength;
  return DioramaCamera_Present(&s_presenter, &s_camera, s_capture_ns, now);
}

static bool DrawScene(const ArImportedGameFrame *game, ArRenderRectI viewport, uint64_t now) {
  const Ar2VideoFrame *frame = &game->video;
  const int visible_width = 256 + frame->display_extra * 2;
  const bool crt = g_settings.pixel_aspect == kPixelAspect_Crt43;
  const DioramaCameraView camera = Camera(game, now);
  DioramaRenderOptions options;
  Diorama_CaptureRenderOptions(&options);
  /* AR1 room IDs and authored layer replacements have no meaning in AR2. */
  options.layers = NULL;
  options.resolved_layers = kLayers;
  options.resolved_layer_count = sizeof(kLayers) / sizeof(*kLayers);
  const bool diorama = g_settings.diorama_mode && frame->level;
  if (diorama) {
    const uint8_t *pixels[kDioramaPlane_Count] = {0};
    for (unsigned i = 0; i < sizeof(kLayers) / sizeof(*kLayers); i++) {
      const int plane = kLayers[i].plane;
      pixels[plane] = (const uint8_t *)(plane == SR_PPU_OVERLAY_BG3
          ? frame->hud : frame->planes[plane]);
      if (!ArRenderDevice_UpdateTexture(s_device, s_planes[plane], NULL, pixels[plane],
                                        AR2_CAPTURE_PITCH * 4)) return false;
    }
    DioramaCapture capture = {.width = frame->width, .height = frame->height,
        .camera_y = frame->camera_y, .bg1_camera_x = frame->camera_x,
        .bg2_camera_x = frame->bg2_camera_x, .bg2_camera_y = frame->bg2_camera_y,
        .bg2_world_height = frame->bg2_world_height, .bg2_revision = game->frame,
        .textures = s_planes, .pixels = pixels};
    capture.horizontal_bounds = DioramaHorizontalBounds_Resolve(
        SR_PPU_OVERLAY_BG1, frame->camera_x, 0, frame->world_width, frame->width, 0);
    capture.vertical_bounds = DioramaVerticalBounds_Resolve(
        SR_PPU_OVERLAY_BG1, frame->camera_y, frame->world_height, 0, frame->height);
    const DioramaView view = {.visible_width = visible_width, .viewport = viewport,
        .pixel_aspect = g_settings.pixel_aspect,
        .ignore_aspect_ratio = Settings_IgnoreAspectRatio(),
        .distance_scale = camera.distance_scale, .distance_offset = camera.distance_offset,
        .camera_framing_weight = camera.framing_weight, .camera = camera.pose};
    const DioramaScene scene = {.render = &options,
        .additive_plane_mask = 1u << kDioramaPlane_Bg2Far};
    DioramaProjection projection;
    if (!PresentationOutcome_IsUsable(Diorama_Composite(
            s_device, &capture, &view, &scene, &projection))) return false;
  } else {
    const ArRenderRectF source = {
        frame->render_extra - frame->display_extra, 0, visible_width, 224};
    const ArRenderRectF destination = {viewport.x, viewport.y, viewport.w, viewport.h};
    if (!ArRenderDevice_UpdateTexture(s_device, s_main, NULL, frame->main, AR2_CAPTURE_PITCH * 4) ||
        !ArRenderDevice_DrawTexture(s_device, s_main, &source, &destination)) return false;
  }
  if (!diorama || options.hud_flat) {
    const int pinned = Settings_ScalePercentToOutput(g_settings.hud_scale_percent);
    float scale = pinned ? pinned / 100.f : viewport.h / 224.f;
    const float par = crt ? 7.f / 6.f : 1;
    scale = fminf(scale, viewport.w / (256.f * par));
    const ArRenderRectF source = {frame->render_extra, 0, 256, 224};
    const ArRenderRectF destination = {viewport.x + (viewport.w - 256 * par * scale) / 2,
        viewport.y, 256 * par * scale, 224 * scale};
    if (!ArRenderDevice_UpdateTexture(s_device, s_hud, NULL, frame->hud, AR2_CAPTURE_PITCH * 4) ||
        !ArRenderDevice_DrawTexture(s_device, s_hud, &source, &destination)) return false;
  }
  /* Native START/PAUSE belongs to the screen, independently of world camera,
   * diorama depth and the player's HUD-size preference. */
  const float text_width = viewport.w * 256.f / visible_width;
  const ArRenderRectF text_source = {frame->render_extra, 0, 256, 224};
  const ArRenderRectF text_destination = {
      viewport.x + (viewport.w - text_width) / 2, viewport.y, text_width, viewport.h};
  if (!ArRenderDevice_UpdateTexture(s_device, s_screen_objects, NULL,
                                    frame->screen_objects, AR2_CAPTURE_PITCH * 4) ||
      !ArRenderDevice_DrawTexture(s_device, s_screen_objects,
                                  &text_source, &text_destination)) return false;
  return true;
}

bool Ar2Presentation_Draw(const ArImportedGameFrame *frame, uint64_t now_ns,
                          ArRenderRectI *viewport) {
  int width, height;
  if (!frame || !frame->video.main || !ArRenderDevice_GetOutputSize(s_device, &width, &height))
    return false;
  const int visible = 256 + frame->video.display_extra * 2;
  *viewport = ArPresentationLayout_ResolveViewport(width, height, Settings_IgnoreAspectRatio(),
      g_settings.pixel_aspect == kPixelAspect_Crt43, visible, 224);
  const CrtPostConfig crt = Present_CaptureCrtConfig();
  (void)CrtPost_Begin(s_device, &crt);
  bool ok = ArRenderDevice_Clear(s_device, (ArRenderColorF){0, 0, 0, 1}) &&
      DrawScene(frame, *viewport, now_ns);
  *viewport = CrtPost_End(s_device, visible, 224, *viewport);
  return ok;
}

void Ar2Presentation_Close(void) {
  if (!s_device) return;
  Diorama_ResetCompositorResources(s_device);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(s_device, s_planes[i]);
    s_planes[i] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(s_device, s_main);
  ArRenderDevice_DestroyTexture(s_device, s_hud);
  ArRenderDevice_DestroyTexture(s_device, s_screen_objects);
  s_main = s_hud = s_screen_objects = ArRenderTexture_Invalid();
  s_device = NULL;
}
