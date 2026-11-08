#include "host/host_comparison_window.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "app/settings.h"
#include "app/user_data_dir.h"
#include "host/host_input.h"
#include "host/host_video.h"
#include "platform/sdl/render_sdl_interop.h"
#include "present/present_internal.h"
#include "render/presentation_layout.h"

static SDL_Window *s_window;
static SDL_GPUDevice *s_gpu;
static ArRenderTexture s_target;
static int s_width, s_height;
static bool s_visible, s_failed, s_fullscreen, s_drawable;
static bool s_swallow_escape, s_swallow_return;
static bool s_focus_hint_owned;
static char *s_focus_hint;

static void KeepFullscreenVisible(bool enabled) {
  if (enabled == s_focus_hint_owned) return;
  if (enabled) {
    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
    s_focus_hint = hint ? SDL_strdup(hint) : NULL;
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0", SDL_HINT_OVERRIDE);
  } else {
    if (s_focus_hint)
      SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
                             s_focus_hint, SDL_HINT_OVERRIDE);
    else SDL_ResetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
    SDL_free(s_focus_hint);
    s_focus_hint = NULL;
  }
  s_focus_hint_owned = enabled;
}

static void SavePreference(const char *key, bool value) {
  if (Settings_SetLong(Settings_Find(key), value) <= kSettingChange_Unchanged)
    return;
  char path[kHostPathCapacity];
  UserDataFile(path, sizeof(path), "settings.ini");
  if (!Settings_SaveDeferred(path))
    fprintf(stderr, "[picture-in-picture] could not save settings.ini\n");
  HostInput_RequestPausedRedraw();
}

void HostComparisonWindow_Destroy(void) {
  ArRenderDevice_DestroyTexture(&g_render_device, s_target);
  s_target = ArRenderTexture_Invalid();
  if (s_gpu && s_window) SDL_ReleaseWindowFromGPUDevice(s_gpu, s_window);
  if (s_window) SDL_DestroyWindow(s_window);
  KeepFullscreenVisible(false);
  s_window = NULL;
  s_gpu = NULL;
  s_width = s_height = 0;
  s_visible = s_failed = s_fullscreen = s_drawable = false;
  s_swallow_escape = s_swallow_return = false;
}

static bool FallBackToInset(void) {
  fprintf(stderr, "[picture-in-picture] separate window unavailable; using inset: %s\n",
          SDL_GetError());
  HostComparisonWindow_Destroy();
  s_failed = true;
  return true;
}

static bool CreateWindow(void) {
  s_gpu = ArSdlRenderBackend_GpuDevice(&g_render_device);
  if (!s_gpu) return false;
  s_window = SDL_CreateWindow("ActRaiser - Authentic comparison", 768, 576,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!s_window) return false;
  if (!SDL_ClaimWindowForGPUDevice(s_gpu, s_window)) {
    /* The window was never claimed, so there is nothing to release. */
    s_gpu = NULL;
    return false;
  }
  /* The main window owns pacing. Prefer mailbox, then immediate, so a slower
   * second monitor does not impose its refresh interval on gameplay. */
  SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
  if (SDL_WindowSupportsGPUPresentMode(s_gpu, s_window, SDL_GPU_PRESENTMODE_MAILBOX))
    mode = SDL_GPU_PRESENTMODE_MAILBOX;
  else if (SDL_WindowSupportsGPUPresentMode(s_gpu, s_window, SDL_GPU_PRESENTMODE_IMMEDIATE))
    mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
  if (!SDL_SetGPUSwapchainParameters(s_gpu, s_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
    return false;

  SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
  int count = 0;
  SDL_DisplayID *displays = SDL_GetDisplays(&count);
  for (int i = 0; i < count; ++i) {
    if (displays[i] == display) continue;
    display = displays[i];
    break;
  }
  SDL_free(displays);
  SDL_Rect bounds;
  if (SDL_GetDisplayUsableBounds(display, &bounds)) {
    const int width = SDL_min(768, bounds.w);
    const int height = SDL_min(576, bounds.h);
    SDL_SetWindowSize(s_window, width, height);
    SDL_SetWindowPosition(s_window,
        bounds.x + (bounds.w - width) / 2, bounds.y + (bounds.h - height) / 2);
  }
  return true;
}

static bool PrepareTarget(void) {
  int width = 0, height = 0;
  if (!SDL_GetWindowSizeInPixels(s_window, &width, &height)) return false;
  s_drawable = width > 0 && height > 0 &&
      !(SDL_GetWindowFlags(s_window) & SDL_WINDOW_MINIMIZED);
  if (!s_drawable) return true;
  if (width == s_width && height == s_height && ArRenderTexture_IsValid(s_target))
    return true;
  const ArRenderTextureDesc desc = {
    .width = width, .height = height, .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target, .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Opaque,
  };
  ArRenderTexture target = ArRenderTexture_Invalid();
  if (!ArRenderDevice_CreateTexture(&g_render_device, &desc, &target)) return false;
  ArRenderDevice_DestroyTexture(&g_render_device, s_target);
  s_target = target;
  s_width = width;
  s_height = height;
  return true;
}

bool HostComparisonWindow_Prepare(bool active) {
#ifdef __ANDROID__
  active = false;
#endif
  active = active && g_settings.picture_in_picture_window && g_window &&
      !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_HIDDEN);
  if (!active) {
    if (s_visible) SDL_HideWindow(s_window);
    KeepFullscreenVisible(false);
    s_visible = s_failed = false;
    return false;
  }
  if (s_failed) return false;
  KeepFullscreenVisible(true);
  bool ok = s_window || CreateWindow();
  if (ok && s_fullscreen != g_settings.picture_in_picture_fullscreen) {
    ok = SDL_SetWindowFullscreen(s_window, g_settings.picture_in_picture_fullscreen);
    if (ok) s_fullscreen = g_settings.picture_in_picture_fullscreen;
  }
  if (ok && !s_visible) {
    ok = SDL_ShowWindow(s_window);
    if (ok) s_visible = true;
  }
  if (ok) ok = PrepareTarget();
  if (ok) return true;
  (void)FallBackToInset();
  return false;
}

bool HostComparisonWindow_Draw(const FrameSlot *slot, bool blank) {
  if (s_visible && !s_drawable) return true;
  if (!s_visible || !ArRenderTexture_IsValid(s_target)) return false;
  ArRenderTargetState saved;
  const ArRenderTargetBeginResult begin =
      ArRenderDevice_BeginTarget(&g_render_device, s_target, &saved);
  if (begin == kArRenderTargetBegin_StateLost) return false;
  if (begin != kArRenderTargetBegin_Ready) return FallBackToInset();
  bool ok = ArRenderDevice_Clear(&g_render_device, (ArRenderColorF){0, 0, 0, 1});
  if (ok && !blank) {
    const ArRenderRectI viewport = ArPresentationLayout_ResolveViewport(
        s_width, s_height, false, true, kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight);
    ok = PresentAuthenticComparisonImage(slot, viewport);
  }
  if (!ArRenderDevice_EndTarget(&g_render_device, &saved)) return false;
  if (!ok) return FallBackToInset();
  if (!ArSdlRenderBackend_SubmitPending(&g_render_device)) return false;
  SDL_Texture *texture = ArSdlRenderBackend_UnwrapTexture(s_target);
  SDL_GPUTexture *source = SDL_GetPointerProperty(SDL_GetTextureProperties(texture),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
  if (!source) return FallBackToInset();
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(s_gpu);
  if (!commands) return FallBackToInset();
  SDL_GPUTexture *swapchain = NULL;
  Uint32 width = 0, height = 0;
  if (!SDL_AcquireGPUSwapchainTexture(commands, s_window, &swapchain, &width, &height)) {
    SDL_CancelGPUCommandBuffer(commands);
    return FallBackToInset();
  }
  if (swapchain) {
    const SDL_GPUBlitInfo blit = {
      .source = {.texture = source, .w = (Uint32)s_width, .h = (Uint32)s_height},
      .destination = {.texture = swapchain, .w = width, .h = height},
      .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_LINEAR,
    };
    SDL_BlitGPUTexture(commands, &blit);
  }
  /* A busy/minimized swapchain may return no image. Submit the empty buffer
   * to retire work, without adding a wait or allocating an unbounded queue. */
  return SDL_SubmitGPUCommandBuffer(commands) || FallBackToInset();
}

static SDL_WindowID EventWindow(const SDL_Event *event) {
  if (!event) return 0;
  if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST)
    return event->window.windowID;
  switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: return event->key.windowID;
    case SDL_EVENT_TEXT_INPUT: return event->text.windowID;
    case SDL_EVENT_TEXT_EDITING: return event->edit.windowID;
    case SDL_EVENT_MOUSE_MOTION: return event->motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: return event->button.windowID;
    case SDL_EVENT_MOUSE_WHEEL: return event->wheel.windowID;
    default: return 0;
  }
}

bool HostComparisonWindow_OwnsEvent(const SDL_Event *event) {
  /* IDs in queued notifications can outlive a destroyed window. No other
   * application window uses this game loop; consume those stale events too. */
  const SDL_WindowID id = EventWindow(event);
  return id && g_window && id != SDL_GetWindowID(g_window);
}

bool HostComparisonWindow_HandleEvent(const SDL_Event *event) {
  if (!HostComparisonWindow_OwnsEvent(event)) return false;
  if (!s_window || EventWindow(event) != SDL_GetWindowID(s_window)) return true;
  switch (event->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      SavePreference("picture_in_picture_window", false);
      SDL_HideWindow(s_window);
      KeepFullscreenVisible(false);
      s_visible = false;
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event->button.button == SDL_BUTTON_LEFT && event->button.clicks == 2)
        SavePreference("picture_in_picture_fullscreen", !g_settings.picture_in_picture_fullscreen);
      return true;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      s_fullscreen = event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
      SavePreference("picture_in_picture_fullscreen", s_fullscreen);
      return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      bool *consumed = event->key.key == SDLK_ESCAPE ? &s_swallow_escape :
          event->key.key == SDLK_RETURN ? &s_swallow_return : NULL;
      if (consumed && *consumed) {
        if (event->type == SDL_EVENT_KEY_UP) *consumed = false;
        return true;
      }
      if ((event->key.key == SDLK_RETURN && (event->key.mod & SDL_KMOD_ALT)) ||
          (event->key.key == SDLK_ESCAPE && g_settings.picture_in_picture_fullscreen)) {
        if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
          if (consumed) *consumed = true;
          SavePreference("picture_in_picture_fullscreen",
                         !g_settings.picture_in_picture_fullscreen);
        }
        return true;
      }
      /* Keyboard gameplay and the comparison binding work in either window. */
      return false;
    }
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
      return false;
    default:
      if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST)
        HostInput_RequestPausedRedraw();
      return true;
  }
}
