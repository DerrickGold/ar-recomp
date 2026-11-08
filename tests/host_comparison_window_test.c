/* Window and GPU protocol tests run without a desktop or graphics driver.
 * Production control flow is retained; only SDL/resource boundaries are fake. */
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "support/test_assert.h"
#include "app/settings.h"
#include "host/host_video.h"
#include "present/present_internal.h"

static max_align_t s_main_token, s_aux_token, s_gpu_token, s_source_token;
static max_align_t s_commands_token, s_swapchain_token;
static SDL_DisplayID s_displays[] = {10, 20};
SDL_Window *g_window = (SDL_Window *)&s_main_token;
ArRenderDevice g_render_device;
Settings g_settings;

static struct {
  bool hidden, minimized, no_gpu, fail_claim, fail_target, fail_acquire, no_swapchain;
  bool fail_draw, fail_restore, fail_flush, fail_submit, bound, claimed;
  int width, height, x, y, live_targets;
  unsigned created, destroyed, shown, hidden_calls, fullscreen, released;
  unsigned targets, target_destroys, clears, draws, restored, flushed;
  unsigned commands, acquired, blitted, submitted, cancelled, saves, redraws;
  SDL_GPUPresentMode present_mode;
  ArRenderRectI viewport;
  ArRenderTexture target;
  ArRenderTargetBeginResult begin_result;
  const FrameSlot *slot;
} s_probe;

static SDL_Window *TestCreateWindow(const char *title, int width, int height,
                                   SDL_WindowFlags flags) {
  assert(title && width > 0 && height > 0);
  const SDL_WindowFlags required = SDL_WINDOW_RESIZABLE |
      SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
  assert((flags & required) == required);
  ++s_probe.created;
  return (SDL_Window *)&s_aux_token;
}

static SDL_WindowID TestWindowId(SDL_Window *window) {
  assert(window == g_window || window == (SDL_Window *)&s_aux_token);
  return window == g_window ? 1 : 2;
}

static SDL_WindowFlags TestWindowFlags(SDL_Window *window) {
  if (window == g_window) return s_probe.hidden ? SDL_WINDOW_HIDDEN : 0;
  assert(TestWindowId(window) == 2);
  return s_probe.minimized ? SDL_WINDOW_MINIMIZED : 0;
}

static void TestDestroyWindow(SDL_Window *window) {
  assert(window == (SDL_Window *)&s_aux_token && !s_probe.claimed);
  ++s_probe.destroyed;
}

static bool TestClaim(SDL_GPUDevice *gpu, SDL_Window *window) {
  assert(gpu == (SDL_GPUDevice *)&s_gpu_token && TestWindowId(window) == 2);
  if (s_probe.fail_claim) return false;
  s_probe.claimed = true;
  return true;
}

static void TestRelease(SDL_GPUDevice *gpu, SDL_Window *window) {
  assert(gpu == (SDL_GPUDevice *)&s_gpu_token && TestWindowId(window) == 2);
  assert(s_probe.claimed && !s_probe.live_targets);
  s_probe.claimed = false;
  ++s_probe.released;
}

static bool TestSupportsMode(SDL_GPUDevice *gpu, SDL_Window *window,
                             SDL_GPUPresentMode mode) {
  assert(gpu == (SDL_GPUDevice *)&s_gpu_token && TestWindowId(window) == 2);
  assert(s_probe.claimed);
  return mode == SDL_GPU_PRESENTMODE_MAILBOX;
}

static bool TestSetMode(SDL_GPUDevice *gpu, SDL_Window *window,
    SDL_GPUSwapchainComposition composition, SDL_GPUPresentMode mode) {
  assert(gpu == (SDL_GPUDevice *)&s_gpu_token && TestWindowId(window) == 2);
  assert(composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR && s_probe.claimed);
  s_probe.present_mode = mode;
  return true;
}

static SDL_DisplayID TestDisplayForWindow(SDL_Window *window) {
  assert(window == g_window);
  return 10;
}

static SDL_DisplayID *TestDisplays(int *count) {
  *count = 2;
  return s_displays;
}

static void TestFree(void *pointer) {
  if (pointer != s_displays) SDL_free(pointer);
}

static bool TestBounds(SDL_DisplayID display, SDL_Rect *bounds) {
  assert(display == 20);
  *bounds = (SDL_Rect){1920, 0, 1920, 1080};
  return true;
}

static bool TestSetSize(SDL_Window *window, int width, int height) {
  assert(TestWindowId(window) == 2 && width > 0 && height > 0);
  return true;
}

static bool TestSetPosition(SDL_Window *window, int x, int y) {
  assert(TestWindowId(window) == 2);
  s_probe.x = x;
  s_probe.y = y;
  return true;
}

static bool TestGetSize(SDL_Window *window, int *width, int *height) {
  assert(TestWindowId(window) == 2);
  *width = s_probe.width;
  *height = s_probe.height;
  return true;
}

static bool TestShow(SDL_Window *window) {
  assert(TestWindowId(window) == 2);
  ++s_probe.shown;
  return true;
}

static bool TestHide(SDL_Window *window) {
  assert(TestWindowId(window) == 2);
  ++s_probe.hidden_calls;
  return true;
}

static bool TestFullscreen(SDL_Window *window, bool fullscreen) {
  assert(TestWindowId(window) == 2);
  assert(fullscreen == g_settings.picture_in_picture_fullscreen);
  ++s_probe.fullscreen;
  return true;
}

static const char *TestError(void) { return "injected window error"; }

static SDL_PropertiesID TestTextureProperties(SDL_Texture *texture) {
  assert((uintptr_t)texture == s_probe.target.value);
  return 1;
}

static void *TestPointerProperty(SDL_PropertiesID properties, const char *name,
                                 void *fallback) {
  assert(properties == 1 && !strcmp(name, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER));
  assert(!fallback);
  return &s_source_token;
}

static SDL_GPUCommandBuffer *TestCommands(SDL_GPUDevice *gpu) {
  assert(gpu == (SDL_GPUDevice *)&s_gpu_token && !s_probe.bound && s_probe.flushed);
  ++s_probe.commands;
  return (SDL_GPUCommandBuffer *)&s_commands_token;
}

static bool TestAcquire(SDL_GPUCommandBuffer *commands, SDL_Window *window,
    SDL_GPUTexture **texture, Uint32 *width, Uint32 *height) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands_token && TestWindowId(window) == 2);
  ++s_probe.acquired;
  *width = (Uint32)s_probe.width;
  *height = (Uint32)s_probe.height;
  *texture = s_probe.no_swapchain ? NULL : (SDL_GPUTexture *)&s_swapchain_token;
  return !s_probe.fail_acquire;
}

static bool TestCancel(SDL_GPUCommandBuffer *commands) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands_token);
  ++s_probe.cancelled;
  return true;
}

static bool TestSubmit(SDL_GPUCommandBuffer *commands) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands_token && !s_probe.bound);
  ++s_probe.submitted;
  return !s_probe.fail_submit;
}

static void TestBlit(SDL_GPUCommandBuffer *commands, const SDL_GPUBlitInfo *blit) {
  assert(commands == (SDL_GPUCommandBuffer *)&s_commands_token && !s_probe.no_swapchain);
  assert(blit->source.texture == (SDL_GPUTexture *)&s_source_token);
  assert(blit->destination.texture == (SDL_GPUTexture *)&s_swapchain_token);
  assert(blit->source.w == (Uint32)s_probe.width && blit->source.h == (Uint32)s_probe.height);
  assert(blit->destination.w == blit->source.w && blit->destination.h == blit->source.h);
  ++s_probe.blitted;
}

#define SDL_CreateWindow TestCreateWindow
#define SDL_GetWindowID TestWindowId
#define SDL_GetWindowFlags TestWindowFlags
#define SDL_DestroyWindow TestDestroyWindow
#define SDL_ClaimWindowForGPUDevice TestClaim
#define SDL_ReleaseWindowFromGPUDevice TestRelease
#define SDL_WindowSupportsGPUPresentMode TestSupportsMode
#define SDL_SetGPUSwapchainParameters TestSetMode
#define SDL_GetDisplayForWindow TestDisplayForWindow
#define SDL_GetDisplays TestDisplays
#define SDL_free TestFree
#define SDL_GetDisplayUsableBounds TestBounds
#define SDL_SetWindowSize TestSetSize
#define SDL_SetWindowPosition TestSetPosition
#define SDL_GetWindowSizeInPixels TestGetSize
#define SDL_ShowWindow TestShow
#define SDL_HideWindow TestHide
#define SDL_SetWindowFullscreen TestFullscreen
#define SDL_GetError TestError
#define SDL_GetTextureProperties TestTextureProperties
#define SDL_GetPointerProperty TestPointerProperty
#define SDL_AcquireGPUCommandBuffer TestCommands
#define SDL_AcquireGPUSwapchainTexture TestAcquire
#define SDL_CancelGPUCommandBuffer TestCancel
#define SDL_SubmitGPUCommandBuffer TestSubmit
#define SDL_BlitGPUTexture TestBlit
#include "../src/host/host_comparison_window.c"

SDL_GPUDevice *ArSdlRenderBackend_GpuDevice(const ArRenderDevice *device) {
  assert(device == &g_render_device);
  return s_probe.no_gpu ? NULL : (SDL_GPUDevice *)&s_gpu_token;
}

SDL_Texture *ArSdlRenderBackend_UnwrapTexture(ArRenderTexture texture) {
  return (SDL_Texture *)texture.value;
}

bool ArSdlRenderBackend_SubmitPending(const ArRenderDevice *device) {
  assert(device == &g_render_device && !s_probe.bound && s_probe.restored);
  ++s_probe.flushed;
  return !s_probe.fail_flush;
}

bool ArRenderDevice_CreateTexture(ArRenderDevice *device,
    const ArRenderTextureDesc *desc, ArRenderTexture *texture) {
  assert(device == &g_render_device && desc->usage == kArRenderTextureUsage_Target);
  assert(desc->width == s_probe.width && desc->height == s_probe.height);
  ++s_probe.targets;
  if (s_probe.fail_target) return false;
  *texture = (ArRenderTexture){100 + s_probe.targets};
  s_probe.target = *texture;
  ++s_probe.live_targets;
  return true;
}

void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  assert(device == &g_render_device && !s_probe.bound);
  if (!ArRenderTexture_IsValid(texture)) return;
  assert(s_probe.live_targets > 0);
  --s_probe.live_targets;
  ++s_probe.target_destroys;
}

ArRenderTargetBeginResult ArRenderDevice_BeginTarget(ArRenderDevice *device,
    ArRenderTexture texture, ArRenderTargetState *saved) {
  assert(device == &g_render_device && texture.value == s_probe.target.value);
  assert(saved && !s_probe.bound);
  if (s_probe.begin_result != kArRenderTargetBegin_Ready) return s_probe.begin_result;
  s_probe.bound = true;
  return kArRenderTargetBegin_Ready;
}

bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color) {
  assert(device == &g_render_device && s_probe.bound);
  assert(color.r == 0 && color.g == 0 && color.b == 0 && color.a == 1);
  ++s_probe.clears;
  return true;
}

bool PresentAuthenticComparisonImage(const FrameSlot *slot, ArRenderRectI viewport) {
  assert(slot == s_probe.slot && s_probe.bound);
  s_probe.viewport = viewport;
  ++s_probe.draws;
  return !s_probe.fail_draw;
}

bool ArRenderDevice_EndTarget(ArRenderDevice *device, const ArRenderTargetState *saved) {
  assert(device == &g_render_device && saved && s_probe.bound);
  s_probe.bound = false;
  ++s_probe.restored;
  return !s_probe.fail_restore;
}

const SettingDesc *Settings_Find(const char *key) {
  static const SettingDesc window = {.field = &g_settings.picture_in_picture_window};
  static const SettingDesc fullscreen = {.field = &g_settings.picture_in_picture_fullscreen};
  if (!strcmp(key, "picture_in_picture_window")) return &window;
  assert(!strcmp(key, "picture_in_picture_fullscreen"));
  return &fullscreen;
}

SettingChangeResult Settings_SetLong(const SettingDesc *desc, long value) {
  bool *field = desc->field;
  if (*field == (value != 0)) return kSettingChange_Unchanged;
  *field = value != 0;
  return kSettingChange_Applied;
}

char *UserDataFile(char *buffer, size_t size, const char *leaf) {
  snprintf(buffer, size, "/test/%s", leaf);
  return buffer;
}

bool Settings_SaveDeferred(const char *path) {
  assert(!strcmp(path, "/test/settings.ini"));
  ++s_probe.saves;
  return true;
}

void HostInput_RequestPausedRedraw(void) { ++s_probe.redraws; }

static void Reset(void) {
  HostComparisonWindow_Destroy();
  memset(&s_probe, 0, sizeof(s_probe));
  memset(&g_settings, 0, sizeof(g_settings));
  s_probe.width = 1280;
  s_probe.height = 720;
  g_settings.picture_in_picture_window = true;
}

static void CheckLifecycleAndRendering(void) {
  Reset();
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "1", SDL_HINT_OVERRIDE);
  assert(!HostComparisonWindow_Prepare(false) && !s_probe.created);
  g_settings.picture_in_picture_window = false;
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.created);
  g_settings.picture_in_picture_window = true;
  g_window = NULL;
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.created);
  g_window = (SDL_Window *)&s_main_token;
  s_probe.hidden = true;
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.created);
  s_probe.hidden = false;
  assert(HostComparisonWindow_Prepare(true));
  assert(!strcmp(SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS), "0"));
  assert(s_probe.created == 1 && s_probe.shown == 1 && s_probe.live_targets == 1);
  assert(s_probe.x >= 1920 && s_probe.present_mode == SDL_GPU_PRESENTMODE_MAILBOX);
  assert(HostComparisonWindow_Prepare(true) && s_probe.targets == 1);

  static FrameSlot slot;
  s_probe.slot = &slot;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.viewport.x == 160 && s_probe.viewport.y == 0);
  assert(s_probe.viewport.w == 960 && s_probe.viewport.h == 720);
  assert(s_probe.draws == 1 && s_probe.clears == 1 && s_probe.restored == 1);
  assert(s_probe.acquired == 1 && s_probe.blitted == 1 && s_probe.submitted == 1);
  assert(HostComparisonWindow_Draw(&slot, true));
  assert(s_probe.clears == 2 && s_probe.draws == 1 && s_probe.submitted == 2);

  s_probe.no_swapchain = true;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.blitted == 2 && s_probe.submitted == 3 && !s_probe.cancelled);
  s_probe.no_swapchain = false;
  s_probe.minimized = true;
  assert(HostComparisonWindow_Prepare(true) && HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.submitted == 3 && s_probe.targets == 1 && !s_probe.destroyed);
  s_probe.minimized = false;
  s_probe.width = 0;
  assert(HostComparisonWindow_Prepare(true) && HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.submitted == 3 && s_probe.targets == 1 && !s_probe.destroyed);
  s_probe.width = 800;
  s_probe.height = 600;
  assert(HostComparisonWindow_Prepare(true));
  assert(s_probe.targets == 2 && s_probe.target_destroys == 1 && s_probe.live_targets == 1);
  g_settings.picture_in_picture_fullscreen = true;
  assert(HostComparisonWindow_Prepare(true) && s_probe.fullscreen == 1);
  assert(HostComparisonWindow_Prepare(true) && s_probe.fullscreen == 1);
  assert(!HostComparisonWindow_Prepare(false) && s_probe.hidden_calls == 1);
  assert(!strcmp(SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS), "1"));
  assert(HostComparisonWindow_Prepare(true) && s_probe.created == 1 && s_probe.shown == 2);
  HostComparisonWindow_Destroy();
  assert(s_probe.released == 1 && s_probe.destroyed == 1 && !s_probe.live_targets);
  assert(!strcmp(SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS), "1"));
  SDL_ResetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
}

static void CheckEvents(void) {
  Reset();
  assert(HostComparisonWindow_Prepare(true));
  SDL_Event event = {.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED};
  event.window.windowID = 1;
  assert(!HostComparisonWindow_OwnsEvent(&event));
  assert(!HostComparisonWindow_HandleEvent(&event));
  event.window.windowID = 2;
  assert(HostComparisonWindow_OwnsEvent(&event));
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(!g_settings.picture_in_picture_window && s_probe.saves == 1 && s_probe.hidden_calls == 1);
  g_settings.picture_in_picture_window = true;
  assert(HostComparisonWindow_Prepare(true));

  event = (SDL_Event){.type = SDL_EVENT_KEY_DOWN};
  event.key.windowID = 2;
  event.key.key = SDLK_X;
  assert(HostComparisonWindow_OwnsEvent(&event));
  assert(!HostComparisonWindow_HandleEvent(&event));
  event = (SDL_Event){.type = SDL_EVENT_MOUSE_MOTION};
  event.motion.windowID = 2;
  assert(HostComparisonWindow_HandleEvent(&event));
  event = (SDL_Event){.type = SDL_EVENT_TEXT_INPUT};
  event.text.windowID = 2;
  assert(!HostComparisonWindow_HandleEvent(&event));
  event = (SDL_Event){.type = SDL_EVENT_MOUSE_BUTTON_DOWN};
  event.button.windowID = 2;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.clicks = 2;
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(g_settings.picture_in_picture_fullscreen && s_probe.saves == 2);
  event = (SDL_Event){.type = SDL_EVENT_KEY_DOWN};
  event.key.windowID = 2;
  event.key.key = SDLK_ESCAPE;
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(!g_settings.picture_in_picture_fullscreen && s_probe.saves == 3);
  event.key.repeat = true;
  assert(HostComparisonWindow_HandleEvent(&event) && s_probe.saves == 3);
  event.type = SDL_EVENT_KEY_UP;
  event.key.repeat = false;
  assert(HostComparisonWindow_HandleEvent(&event));
  event.type = SDL_EVENT_KEY_DOWN;
  assert(!HostComparisonWindow_HandleEvent(&event));
  event.key.key = SDLK_RETURN;
  event.key.mod = SDL_KMOD_ALT;
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(g_settings.picture_in_picture_fullscreen && s_probe.saves == 4);
  event.key.mod = 0;
  event.type = SDL_EVENT_KEY_UP;
  assert(HostComparisonWindow_HandleEvent(&event) && s_probe.saves == 4);
  event = (SDL_Event){.type = SDL_EVENT_WINDOW_LEAVE_FULLSCREEN};
  event.window.windowID = 2;
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(!g_settings.picture_in_picture_fullscreen && s_probe.saves == 5);
  event.type = SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
  assert(HostComparisonWindow_HandleEvent(&event));
  assert(g_settings.picture_in_picture_fullscreen && s_probe.saves == 6);
  HostComparisonWindow_Destroy();
  event = (SDL_Event){.type = SDL_EVENT_WINDOW_HIDDEN};
  event.window.windowID = 2;
  assert(HostComparisonWindow_HandleEvent(&event));
  event = (SDL_Event){.type = SDL_EVENT_QUIT};
  assert(!HostComparisonWindow_HandleEvent(&event));
}

static void CheckFailureRecovery(void) {
  Reset();
  s_probe.fail_claim = true;
  assert(!HostComparisonWindow_Prepare(true));
  assert(s_probe.created == 1 && s_probe.destroyed == 1 && !s_probe.released);
  assert(!HostComparisonWindow_Prepare(true) && s_probe.created == 1);
  s_probe.fail_claim = false;
  assert(!HostComparisonWindow_Prepare(false));
  assert(HostComparisonWindow_Prepare(true));
  static FrameSlot slot;
  s_probe.slot = &slot;
  s_probe.fail_draw = true;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.restored == 1 && !s_probe.commands && !s_probe.bound);
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.live_targets);
  Reset();
  s_probe.slot = &slot;
  assert(HostComparisonWindow_Prepare(true));
  s_probe.fail_acquire = true;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.cancelled == 1 && !s_probe.submitted);
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.live_targets);
  Reset();
  s_probe.slot = &slot;
  assert(HostComparisonWindow_Prepare(true));
  s_probe.fail_submit = true;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(s_probe.submitted == 1 && !s_probe.cancelled);
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.live_targets);
  Reset();
  s_probe.slot = &slot;
  assert(HostComparisonWindow_Prepare(true));
  s_probe.begin_result = kArRenderTargetBegin_Omitted;
  assert(HostComparisonWindow_Draw(&slot, false));
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.live_targets);
  Reset();
  s_probe.slot = &slot;
  assert(HostComparisonWindow_Prepare(true));
  s_probe.begin_result = kArRenderTargetBegin_StateLost;
  assert(!HostComparisonWindow_Draw(&slot, false) && !s_probe.commands);
  s_probe.begin_result = kArRenderTargetBegin_Ready;
  s_probe.fail_restore = true;
  assert(!HostComparisonWindow_Draw(&slot, false));
  assert(!s_probe.commands);
  s_probe.fail_restore = false;
  s_probe.fail_flush = true;
  assert(!HostComparisonWindow_Draw(&slot, false) && !s_probe.commands);
  s_probe.fail_flush = false;
  s_probe.fail_target = true;
  s_probe.width = 1920;
  assert(!HostComparisonWindow_Prepare(true));
  assert(!s_probe.live_targets && s_probe.released == 1 && s_probe.destroyed == 1);
  Reset();
  s_probe.no_gpu = true;
  assert(!HostComparisonWindow_Prepare(true) && !s_probe.created);
  HostComparisonWindow_Destroy();
}

int main(void) {
  CheckLifecycleAndRendering();
  CheckEvents();
  CheckFailureRecovery();
  puts("host comparison window: lifecycle, rendering, input and recovery passed");
  return 0;
}
