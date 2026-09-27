#include "host/host_video.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "app/settings.h"
#include "host/host_display.h"
#include "platform/sdl/render_sdl.h"
#include "present/render_preparation.h"
#include "snesrecomp/game/types.h"

SDL_Window *g_window;
ArRenderDevice g_render_device;
bool g_gpu_shaders_active;

/* SDL GPU driver name for each GpuBackend. */
static const char *const kGpuBackendDrivers[kGpuBackend_Count] = {
  [kGpuBackend_Automatic] = NULL,
  [kGpuBackend_Direct3D12] = "direct3d12",
  [kGpuBackend_Vulkan] = "vulkan",
  [kGpuBackend_Metal] = "metal",
};

/* Publishes the backends this build can offer and returns the SDL driver to
 * request, or NULL for SDL's own order. A saved choice this platform does not
 * offer (a settings.ini carried over from another OS) quietly means Automatic
 * and is kept for that other machine. */
static const char *SelectGpuDriver(void) {
  uint32_t offered = 1u << kGpuBackend_Automatic;
  for (int backend = kGpuBackend_Automatic + 1; backend < kGpuBackend_Count;
       ++backend) {
#if defined(__APPLE__)
    /* SDL's Apple builds compile Vulkan in (3.4.12 lists "metal vulkan"), but
     * it needs MoltenVK, which is not shipped; Metal is the native API. */
    if (backend == kGpuBackend_Vulkan) continue;
#endif
    if (ArSdlRenderBackend_HasGpuDriver(kGpuBackendDrivers[backend]))
      offered |= 1u << backend;
  }
  Settings_SetGpuBackendsOffered(offered);
  const int requested = g_settings.gpu_backend;
  return Settings_ValueAvailable(Settings_Find("gpu_backend"), requested)
      ? kGpuBackendDrivers[requested] : NULL;
}

static void PublishActiveGpuBackend(void) {
  const char *driver = ArSdlRenderBackend_GpuDriver(&g_render_device);
  for (int backend = kGpuBackend_Automatic + 1; backend < kGpuBackend_Count;
       ++backend) {
    if (driver && !SDL_strcasecmp(driver, kGpuBackendDrivers[backend])) {
      Settings_SetGpuBackendActive(backend);
      return;
    }
  }
}

void HostVideo_Create(const char *title, bool hidden_capture) {
  /* Which backend SDL actually chose. A "dummy"/"offscreen" driver makes
   * every video call succeed while nothing reaches the screen (audio is
   * unaffected), so a silent window is otherwise indistinguishable from a
   * working one. Listing the compiled-in drivers also tells you instantly
   * whether a hand-supplied libSDL3 was built without a real backend. */
  const char *driver = SDL_GetCurrentVideoDriver();
  fprintf(stderr, "[video] driver: %s (available:", driver ? driver : "(none)");
  for (int i = 0, n = SDL_GetNumVideoDrivers(); i < n; i++)
    fprintf(stderr, " %s", SDL_GetVideoDriver(i));
  fprintf(stderr, ")\n");
  if (driver && (SDL_strcmp(driver, "dummy") == 0 ||
                 SDL_strcmp(driver, "offscreen") == 0))
    Die("SIM3D requires a real GPU video driver; dummy/offscreen is unsupported");

  int scale = g_settings.window_scale ? g_settings.window_scale : 3;
  /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
   * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
   * the height), so derive the window from the target ratio, not the
   * framebuffer. Faithful mode keeps the historical width*scale.
   *
   * Must use the DISPLAY crop (Settings_VisibleWidth), not g_snes_width:
   * diorama mode inflates the render width to the full
   * kActRaiserWidescreenExtraMax margin
   * (HostDisplay_ResolveVideoGeometry) while the displayed width stays
   * aspect-derived. HostDisplay_CalculateWindowSize shares the same
   * calculation with later explicit scale/aspect changes. */
  /* Clamp the scale to what the desktop can actually hold — the setting
   * allows up to 8x (~2400px wide), which overflows small laptop panels
   * (1366x768) with no recourse: the oversized window's title bar can land
   * off-screen. Usable bounds (excludes docks/taskbars) of the primary
   * display, checked against the WIDEST possible window for this scale
   * (the 16:9-of-height display width); shrink until it fits, floor 1x. */
  /* Points, not pixels — the same conversion
   * HostDisplay_ApplyWindowScale uses, so boot and later re-apply agree on
   * what Nx means. The density is not known until the window exists, so use
   * the primary display's content scale as the boot-time stand-in; the first
   * HostDisplay_UpdateProperties call corrects it. */
  {
    float boot_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (boot_scale > 1.0f) {
      int points = (int)((float)scale / boot_scale + 0.5f);
      scale = points > 0 ? points : 1;
    }
  }
  {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
      while (scale > 1 &&
             ((g_snes_height * scale * 16 + 4) / 9 > usable.w ||
              g_snes_height * scale > usable.h))
        scale--;
    }
  }
  int win_w;
  int win_h;
  HostDisplay_CalculateWindowSize(scale, &win_w, &win_h);
  /* SDL3 merged FULLSCREEN_DESKTOP into FULLSCREEN (borderless desktop is
   * the default fullscreen mode when no exclusive video mode is set).
   * Exclusive fullscreen's video mode is set after window creation by
   * HostDisplay_ApplyWindowMode; at boot the flag just requests fullscreen. */
  /* HIGH_PIXEL_DENSITY: request a native-resolution backing store on
   * scaled displays (Retina macOS, scaled Wayland). Without it SDL creates
   * a 1x store and the compositor upscales — the game, PAR resample, and
   * overlay all render soft at logical resolution. Downstream needs no
   * change: every consumer sizes itself from SDL_GetRenderOutputSize, and
   * HostDisplay_WindowPointToOutput already maps window points to output
   * pixels. */
  SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE |
      SDL_WINDOW_HIGH_PIXEL_DENSITY |
      (hidden_capture ? SDL_WINDOW_HIDDEN : 0) |
      (g_settings.window_mode != kWindowMode_Windowed
           ? SDL_WINDOW_FULLSCREEN : 0);
  /* SDL3 SDL_CreateWindow no longer takes an x,y position; it is created at
   * a default (centered) position. */
  g_window = SDL_CreateWindow(
    title,
    win_w, win_h,
    window_flags
  );
  if (!g_window) Die("SDL_CreateWindow failed");

  /* SIM3D now relies on per-pixel depth testing, so SDL's cross-platform
   * GPU renderer is a baseline requirement rather than an optional shader
   * effects switch. Hidden capture windows use the same backend: a software
   * renderer would produce screenshots from a different visibility model.
   * SPIR-V feeds Vulkan, DXIL feeds D3D12, and MSL feeds Metal. Individual
   * feature requirements are prepared and gated before gameplay below. */
  g_settings.gpu_shaders_enabled = true;  /* legacy config/UI mirror */
  if (!ArSdlRenderBackend_CreateForWindow(
          &g_render_device, g_window, SelectGpuDriver()))
    Die("SDL GPU render backend creation failed");
  PublishActiveGpuBackend();
  RenderFeatureMask prepared_features = 0;
  const uint64_t preparation_started = SDL_GetTicks();
  if (!RenderPreparation_Prepare(&g_render_device, &prepared_features))
    Die("Graphics startup preparation failed; unable to safely prepare the renderer");
  Settings_ApplyRenderCapabilities(prepared_features);
  fprintf(stderr, "[graphics-prepare] completed in %llu ms; cpu-cores=%d system-ram=%d MB\n",
      (unsigned long long)(SDL_GetTicks() - preparation_started),
      SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
  g_gpu_shaders_active = true;
  /* Apply the selected refresh policy after renderer creation. Hidden-video
   * automation requests vsync off and uses no host throttle; a platform
   * swapchain may still serialize SDL_RenderPresent at its own cadence.
   * Interactive Limit/Uncapped modes use host deadlines; VSync delegates to
   * SDL and Unlimited deliberately has no host throttle. */
  if (hidden_capture)
    HostDisplay_DisableVsync();
  else
    HostDisplay_ApplyRefreshVsync();

  /* Exclusive fullscreen needs its video mode set after creation; borderless
   * and windowed are already handled by the creation flag. */
  if (!hidden_capture && g_settings.window_mode == kWindowMode_Exclusive)
    HostDisplay_ApplyWindowMode();
  HostDisplay_UpdateProperties();

  /* Aspect-correct letterboxing via SDL's logical presentation — one
   * implementation shared with the resize/settings paths so boot and runtime
   * can never disagree (4:3-PAR encodes the 7:6 stretch in the logical size;
   * Screen ratio > Stretch opts out of aspect fitting). */
  HostDisplay_RecomputeLogicalPresentation();
}

void HostVideo_TakeFocus(void) {
  /* Direct launches from the builder or terminal may need to activate the
   * application. Request this once at startup. A successful request does not
   * prove focus was granted; the event loop logs subsequent focus changes. */
  if (!SDL_RaiseWindow(g_window))
    fprintf(stderr, "[window] could not raise to foreground: %s\n",
            SDL_GetError());
  SDL_Window *keyboard = SDL_GetKeyboardFocus();
  fprintf(stderr,
      "[window] startup focus: SDL=%d window=%u keyboard-focus=%u "
      "flags=$%llx key-events=%d/%d\n",
      SDL_GetVersion(), (unsigned)SDL_GetWindowID(g_window),
      keyboard ? (unsigned)SDL_GetWindowID(keyboard) : 0,
      (unsigned long long)SDL_GetWindowFlags(g_window),
      SDL_EventEnabled(SDL_EVENT_KEY_DOWN), SDL_EventEnabled(SDL_EVENT_KEY_UP));
}

void HostVideo_Destroy(void) {
  ArSdlRenderBackend_Destroy(&g_render_device);
  SDL_DestroyWindow(g_window);
  g_window = NULL;
}
