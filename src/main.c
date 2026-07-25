#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"
#include "types.h"
#include "actraiser_rtl.h"
#include "common_cpu_infra.h"
#include "config.h"
#include "settings.h"
#include "settings_overlay.h"
#include "input_map.h"
#include "scene_inspector.h"
#include "scene_asset_dump.h"
#include "diorama.h"
#include "diorama_scroll_math.h"  /* kInterpPhaseNone */
#include "forced_input.h"
#include "save_system.h"
#include "hd_replacements.h"
#include "music_replacements.h"
#include "sfx_census.h"
#include "run_dir.h"
#include "launcher.h"
#include "util.h"
#include "actraiser_spc_player.h"
#include "actraiser_game.h"
#include "snes/snes.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "widescreen.h"
#include "present.h"
#include "frame_slot.h"
#include "host_audio.h"
#include "host_display.h"
#include "input_replay.h"
#include "oracle_trace.h"
#include "portable_paths.h"
#include "runtime_diagnostics.h"
#include "scheduled_settings.h"
#include "user_data_dir.h"
#include "sim_phase0_trace.h"
#include "sim_render_metadata.h"
#include "sim_render_atlas.h"
#include "sim_town_canvas.h"
#include "sim_world_map.h"
#include "sim3d.h"
#include "scene3d_math.h"

/* HD art substitution (hd_replacements.c manifest entries). PNG only;
 * decoded once at startup. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
enum {
  kDefaultPowerOnWramFill = 0x55,
  kPowerOnGameFrameSentinel =
      kDefaultPowerOnWramFill | (kDefaultPowerOnWramFill << 8),
};
/* Reverse-domain app identifier: compositors key window grouping and icon
 * lookup off this, and a shipped .desktop file must share its basename. */
#define AR_APP_IDENTIFIER "dev.quintet-enix.actraiser-recomp"
#define AR_APP_VERSION "0.1.0-dev"
/* Not static: present.c and host_display.c read these presentation resources
 * directly. They are boot-created once and, after that, either read-only
 * pointers or used synchronously on the main thread — not part of the
 * g_ppu/g_settings state boundary D6 fences off. */
SDL_Window *g_window;
SDL_Renderer *g_renderer;
/* M8: true once the "gpu" backend was successfully requested (AR_GPU_SHADERS=1)
 * AND created. Individual shader effects (present.c) must still check their
 * OWN AR_GPU_FX_* toggle on top of this — this only gates whether the
 * SDL_GPURenderState machinery is usable at all. */
bool g_gpu_shaders_requested;
bool g_gpu_shaders_active;
SDL_Texture *g_texture;
SDL_Texture *g_hud_bg_texture;
SDL_Texture *g_hud_obj_texture;
static uint8 g_paused;
uint8 g_turbo;  /* external: read by FrameSlot_Capture (frame_slot.c) */
static bool g_scene_inspector_owns_pause;
/* InspectorPresentationKind/InspectorPresentationSelection now live in
 * present.h (D4) — shared between this file's InspectWindowPoint (live
 * hit-test) and present.c's renderer (fed from the FrameSlot snapshot). */
/* external: read by FrameSlot_Capture (frame_slot.c) */
InspectorPresentationSelection g_scene_inspector_presentation;
static bool g_paused_redraw_pending;
static bool g_window_hidden;  /* true while MINIMIZED or HIDDEN: skip present */
static uint32 g_input_state;
typedef enum {
  kHostLifecycle_None,
  kHostLifecycle_Restart,
  kHostLifecycle_Exit,
} HostLifecycleRequest;
static HostLifecycleRequest g_host_lifecycle_request;
/* external: read by FrameSlot_Capture (frame_slot.c) */
int g_snes_width = 256, g_snes_height = 224;
/* Framebuffer sized for the PPU's full widescreen budget (448 wide) so the
 * active width can change live without reallocating storage; each frame uses
 * only the leading g_snes_width*4 bytes per row. */
uint8_t g_pixels[
    kPpuBufWidth * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_bg_pixels[
    kPpuBufWidth * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_obj_pixels[
    kPpuBufWidth * 4 * kHostDisplayFramebufferHeight];
/* Overlay surfaces for manifest-driven HD replacements, allocated lazily per
 * source at bind time. The captured authentic pixels are never presented
 * (the HD textures replace them); the bindings exist because RemoveFromGame
 * only engages on a bound source. BG3/OBJ reuse the HUD surfaces above. */
static uint8_t *g_hd_overlay_pixels[kPpuOverlaySource_Count];
/* Mode-7 override surface: the engine renders substituted canvas art into
 * this at kHdMode7Scale subsamples per axis (supersampled AA after the
 * matrix warp); the host composites it between the game frame and the
 * OBJ/HUD overlays. Allocated only when a mode7 manifest entry has art. */
enum { kHdMode7Scale = 4 };
uint8_t *g_m7_overlay_pixels;
SDL_Texture *g_m7_texture;

/* Diorama per-plane capture buffers, indexed by kDioramaPlane_* (engine
 * sources = the priority-0 remainder of each layer, appended entries = the
 * priority-band splits; see diorama_planes.h). Dedicated set separate from
 * the HUD/HD overlay buffers (BG3/OBJ reuse those for the widescreen HUD
 * split, and HD replacements claim per-source capture slots — see §4.3).
 * Allocated lazily on first diorama capture (actraiser_rtl.c); never freed
 * (matches the existing buffer convention — §D18). BG4 is never drawn in
 * Mode 1, so excluded; the backdrop slot stays NULL (RenderDiorama points
 * it at g_pixels). */
uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
bool g_diorama_dump_pending;
/* The single diorama gate (§D14): mode armed, the new PPU path can run, and
 * we are in an action stage. Capture and render both early-out on this, so
 * there is exactly one spelling of "diorama is happening". */
bool Diorama_IsActiveThisFrame(void) {
  extern uint8 g_ram[0x20000];
  return g_settings.diorama_mode && Diorama_NewPpuCapable() &&
         ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]);
}
bool g_diorama_frame_active;
SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
SDL_Texture *g_sim_obj_atlas_texture;
SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
SDL_Texture *g_sim3d_flat_texture;
bool g_sim3d_textures_ready;
static bool g_sim3d_camera_dragging;
static bool g_sim3d_camera_settings_dirty;
static uint64_t g_sim3d_camera_settings_dirty_at;

static bool Sim3D_ProfileUsesGround(SimRenderFeatureMask features) {
  const SimRenderFeatureMask required =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection;
  return (features & required) == required;
}

static bool Sim3D_FreeCameraActiveThisFrame(void) {
  /* The drag edits the free pose. In Dynamic Cam that pose is not what the
   * projection is built from, so a drag would silently change nothing the
   * player can see -- worse than it simply not responding. */
  if (g_settings.sim3d_camera_mode != kSimCam_Free) return false;
  if (!g_settings.sim3d_mode || !Diorama_NewPpuCapable() ||
      !g_sim3d_textures_ready ||
      !(Sim3D_ImplementedFeatures() & kSimFeature_GroundProjection) ||
      !ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                  g_ram[kActRaiserWram_CurrentMap]) ||
      ActRaiser_SimMapPickerActive())
    return false;
  return Sim3D_ProfileUsesGround(Settings_Sim3DRequestedFeatures());
}

static int ClampInt(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

static void Sim3D_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
  int yaw = g_settings.sim3d_tilt_y_mrad + (int)(d_yaw * 1000.0f);
  int pitch = g_settings.sim3d_tilt_x_mrad + (int)(d_pitch * 1000.0f);
  g_settings.sim3d_tilt_y_mrad = ClampInt(yaw, -700, 700);
  g_settings.sim3d_tilt_x_mrad = ClampInt(pitch, -700, 700);
  if (d_zoom != 0.0f) {
    float distance = g_settings.sim3d_distance_x100 > 0
        ? (float)g_settings.sim3d_distance_x100 / 100.0f
        : Scene3D_AutoFitDistance(0.4f);
    distance += d_zoom;
    if (distance < 2.0f) distance = 2.0f;
    if (distance > 20.0f) distance = 20.0f;
    g_settings.sim3d_distance_x100 = (int)(distance * 100.0f);
  }
  g_sim3d_camera_settings_dirty = true;
  g_sim3d_camera_settings_dirty_at = SDL_GetTicks();
  g_paused_redraw_pending = true;
}

static void Sim3D_ResetCamera(void) {
  /* Resets the pose of the mode currently in use, not always the free one.
   * "Reset camera" should put back whatever the player is looking through;
   * restoring a pose that is not on screen would read as the button doing
   * nothing. */
  static const char *const free_keys[] = {
    "sim3d_tilt_x_mrad", "sim3d_tilt_y_mrad", "sim3d_distance_x100",
  };
  static const char *const dynamic_keys[] = {
    "sim3d_dyncam_baseline_tilt_x_mrad", "sim3d_dyncam_baseline_tilt_y_mrad",
    "sim3d_dyncam_baseline_distance_x100",
  };
  const char *const *keys = g_settings.sim3d_camera_mode == kSimCam_Dynamic
      ? dynamic_keys : free_keys;
  for (size_t i = 0; i < 3; i++) {
    const SettingDesc *row = Settings_Find(keys[i]);
    if (row) Settings_Reset(row);
  }
  g_sim3d_camera_settings_dirty = true;
  g_sim3d_camera_settings_dirty_at = SDL_GetTicks();
  g_paused_redraw_pending = true;
}

/* Polled analog camera control (input_map.h): the right stick orbits and the
 * triggers zoom the diorama / 3D-town Free Cam, the same poses the mouse
 * right-drag and wheel already edit. Integrated over REAL elapsed time, not
 * per host iteration, so the orbit speed does not change with frame rate or
 * with the emulator being paused.
 *
 * Deliberately routed through the same Diorama_AdjustCamera /
 * Sim3D_AdjustCamera entry points as the mouse, so clamping, the settings
 * write-back, and the paused-redraw flag all stay on one path. */
static void ApplyAnalogCameraInput(void) {
  static uint64_t last_ns;
  uint64_t now_ns = SDL_GetTicksNS();
  uint64_t elapsed_ns = last_ns ? now_ns - last_ns : 0;
  last_ns = now_ns;
  /* A long stall (load, alt-tab) must not teleport the camera. */
  if (elapsed_ns > 100000000ull) elapsed_ns = 100000000ull;
  if (!elapsed_ns) return;

  bool diorama = !SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame();
  bool sim3d = !SettingsOverlay_IsOpen() && !diorama &&
               Sim3D_FreeCameraActiveThisFrame();
  if (!diorama && !sim3d) return;

  float dt = (float)elapsed_ns / 1e9f;
  float gain = (float)g_settings.input_cam_sensitivity / 100.0f;

  /* Base rates: a full stick sweeps the +-0.7 rad tilt clamp in a bit over a
   * second, and crosses the 2..20 distance range in about three. */
  static const float kYawRadPerSec = 1.2f;
  static const float kPitchRadPerSec = 1.2f;
  static const float kZoomPerSec = 6.0f;

  float yaw = InputMap_AnalogAction(kInputAction_CamYawRight) -
              InputMap_AnalogAction(kInputAction_CamYawLeft);
  float pitch = InputMap_AnalogAction(kInputAction_CamPitchDown) -
                InputMap_AnalogAction(kInputAction_CamPitchUp);
  float zoom = InputMap_AnalogAction(kInputAction_CamZoomOut) -
               InputMap_AnalogAction(kInputAction_CamZoomIn);
  if (g_settings.input_cam_invert_y) pitch = -pitch;
  if (yaw == 0.0f && pitch == 0.0f && zoom == 0.0f) return;

  float d_yaw = yaw * kYawRadPerSec * gain * dt;
  float d_pitch = pitch * kPitchRadPerSec * gain * dt;
  float d_zoom = zoom * kZoomPerSec * gain * dt;
  if (diorama) Diorama_AdjustCamera(d_yaw, d_pitch, d_zoom);
  else Sim3D_AdjustCamera(d_yaw, d_pitch, d_zoom);
}

/* Widescreen master switch + per-side extra-column budget — the definitions
 * for the runner's widescreen.h externs (each game defines them; 0/false =
 * authentic 256-wide, all PPU margin machinery inert). Set once at startup
 * from ExtendedAspectRatio/AspectPAR in config.ini; per-frame policy lives in
 * ActRaiser_ApplyWidescreenPolicy (actraiser_rtl.c). */
bool g_ws_active;
int g_ws_extra;
/* Margin the *display* crops to (aspect-derived). Normally equal to
 * g_ws_extra; diorama mode widens the render margin to kWsExtraMax so the
 * tilt reveals real content, while the flat presentation keeps showing the
 * user's chosen aspect. */
int g_ws_display_extra;

extern Snes *g_snes;
extern Ppu *g_ppu;
struct SpcPlayer *g_spc_player;

extern const RtlGameInfo kActRaiserGameInfo;

bool g_new_ppu = true;

void ActRaiser_RebindPpuOutputSurfaces(void);

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

/* SDL3 render primitives take float rects. All internal geometry is integer
 * SNES-pixel math, so convert only at the SDL draw call. */
static SDL_FRect ToFRect(SDL_Rect r) {
  return (SDL_FRect){ (float)r.x, (float)r.y, (float)r.w, (float)r.h };
}

void OpenGLRenderer_Create(struct RendererFuncs *funcs) {
  (void)funcs;
}

/* The 12 joypad bits now live in input_map.c, keyed by SCANCODE (physical key
 * position) rather than keycode, so a bind made on one keyboard layout stays
 * on the same physical key on another. g_input_state remains the one word the
 * runner, the force-input hooks, and the oracle record/replay path read. */
static void HandleInput(int scancode, bool pressed) {
  InputMap_HandleKey(scancode, pressed);
  g_input_state = InputMap_State();
}

/* Every place that freezes the game (menu open, inspector selection) has to
 * drop held bits, or a direction held across the freeze leaks back out. */
static void ClearHeldInput(void) {
  InputMap_Clear();
  g_input_state = 0;
}

/* Which device owns the settings overlay right now. Mirrors InputMap_State's
 * gameplay arbitration (input_map.c:487-496) so ONE physical Steam-Deck press
 * — which desktop-mode Steam Input can surface as a real pad event AND a
 * synthesized keyboard twin — moves the menu once, not twice. Exactly one of
 * these is true at any time: gamepad owns only when a pad is present and the
 * user hasn't pinned Keyboard; otherwise the keyboard owns (this folds in
 * InputMap_State's !s_pad_count safety valve, so Gamepad-mode with no pad
 * connected still lets the keyboard drive/close the menu — no lockout). */
static bool MenuGamepadIsActiveDevice(void) {
  return g_settings.input_device != kInputDevice_Keyboard &&
         InputMap_GamepadCount() > 0;
}
/* NOT the negation of the gamepad predicate — these mirror InputMap_State's two
 * INDEPENDENT gameplay gates (input_map.c:491-494), so in Auto (the default)
 * BOTH devices drive the menu, exactly as the input_device="Auto" description
 * promises ("keeps the keyboard and the selected gamepad both live"). Only an
 * explicit Gamepad pin (with a pad connected) takes the keyboard off the menu;
 * that matches InputMap_State's !s_pad_count safety valve so a Gamepad pin with
 * no pad still lets the keyboard reach/close the menu — no lockout. */
static bool MenuKeyboardIsActiveDevice(void) {
  return g_settings.input_device != kInputDevice_Gamepad ||
         InputMap_GamepadCount() == 0;
}

static void RtlDrawPpuFrame(void) {
  g_rtl_game_info->draw_ppu_frame();
}

/* Runtime presentation settings can change while game execution is paused.
 * Re-render the same emulated PPU state once after such a change; ordinary
 * paused iterations retain that texture, so pause never advances the game or
 * repeatedly replays the scanline/HDMA renderer. */
/* Returns true if a redraw actually happened, so the paused loop knows a
 * fresh frame is worth submitting (§2.5). */
static bool RedrawPausedFrameIfNeeded(void) {
  if ((g_paused || SettingsOverlay_IsOpen()) && g_paused_redraw_pending) {
    RtlDrawPpuFrame();
    g_paused_redraw_pending = false;
    return true;
  }
  return false;
}

/* Write g_pixels to an open PPM, cropped to the current display mode's sub-rect
 * (pitch is g_snes_width*4 — see PpuBeginDrawing). A 4:3 capture is therefore a
 * true 256-wide image rather than the wide framebuffer with black bars baked
 * in, while wide modes capture the full framebuffer exactly as before. */
static SDL_Point WriteFramebufferPpm(FILE *pf) {
  /* A promoted HUD is composited after the SNES framebuffer. When a renderer
   * exists, capture that actual host-space result so F2 and visual-regression
   * screenshots include the independently scaled overlay. Headless runs
   * without video retain the historical internal-framebuffer capture. */
  /* P13: this does a full render pass + SDL_RenderReadPixels (F2, AR_SHOT_*).
   * It used to be bracketed by a present-thread quiesce so the two couldn't
   * touch g_renderer/g_texture concurrently; with rendering main-thread-only
   * the readback simply runs inline — nothing else can be mid-present. */
  FrameSlot ppm_slot;
  bool have_ppm_slot = false;
  SDL_Surface *argb = NULL;
  if (g_renderer && g_hud_bg_texture) {
    FrameSlot_Capture(&ppm_slot);
    PresentUpload(&ppm_slot);
    PresentComposite(&ppm_slot, NULL, kInterpPhaseNone);
    have_ppm_slot = true;
  }
  if (have_ppm_slot) {
    /* SDL3 SDL_RenderReadPixels returns a newly allocated surface in the
     * renderer's native format; convert it to ARGB8888 so the byte-order
     * extraction below is exact regardless of the backend's format. */
    SDL_Surface *raw = SDL_RenderReadPixels(g_renderer, NULL);
    argb = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (raw) SDL_DestroySurface(raw);
  }
  if (argb) {
    int out_w = argb->w, out_h = argb->h;
    fprintf(pf, "P6\n%d %d\n255\n", out_w, out_h);
    for (int y = 0; y < out_h; y++) {
      const uint8_t *row = (const uint8_t *)argb->pixels +
                           (size_t)y * argb->pitch;
      for (int x = 0; x < out_w; x++) {
        fputc(row[x * 4 + 2], pf);
        fputc(row[x * 4 + 1], pf);
        fputc(row[x * 4 + 0], pf);
      }
    }
    SDL_DestroySurface(argb);
    return (SDL_Point){ out_w, out_h };
  }

  int x0 = Settings_VisibleX0();
  int w = Settings_VisibleWidth();
  fprintf(pf, "P6\n%d %d\n255\n", w, g_snes_height);
  for (int y = 0; y < g_snes_height; y++) {
    const uint8_t *row = g_pixels + ((size_t)y * g_snes_width + x0) * 4;
    for (int x = 0; x < w; x++) {
      fputc(row[x * 4 + 2], pf);  /* R */
      fputc(row[x * 4 + 1], pf);  /* G */
      fputc(row[x * 4 + 0], pf);  /* B */
    }
  }
  return (SDL_Point){ w, g_snes_height };
}

static void TogglePause(void) {
  g_paused = !g_paused;
  fprintf(stderr, "[pause] %s\n", g_paused ? "on" : "off");
}

static void CloseSceneInspectorSelection(void) {
  SceneInspector_Clear();
  SettingsOverlay_HideDebugPanel();
  memset(&g_scene_inspector_presentation, 0,
         sizeof(g_scene_inspector_presentation));
  if (g_scene_inspector_owns_pause) g_paused = false;
  g_scene_inspector_owns_pause = false;
  ClearHeldInput();
}

static void ToggleTurbo(void) {
  g_turbo = !g_turbo;
  if (g_turbo)
    fprintf(stderr, "[turbo] ON (%dx)\n", g_settings.turbo_multiplier);
  else
    fprintf(stderr, "[turbo] off\n");
}

static uint16_t ReadWram16(unsigned address) {
  return (uint16_t)(g_ram[address & 0x1ffff] |
                    (g_ram[(address + 1) & 0x1ffff] << 8));
}

static const char *InspectorSceneName(uint8 map_group, uint8 map) {
  static const char *const regions[] = {
    "Non-action", "Fillmore act", "Bloodpool act", "Kasandora act",
    "Aitos act", "Marahna act", "Northwall act", "Death Heim", "Ending",
  };
  static const char *const non_action[] = {
    "Title", "Fillmore sim", "Bloodpool sim", "Kasandora sim", "Aitos sim",
    "Marahna sim", "Northwall sim", "Sky Palace", "Temple", "World map",
  };
  if (map_group == kActRaiserMapGroup_NonAction &&
      map < sizeof(non_action) / sizeof(non_action[0]))
    return non_action[map];
  if (map_group < sizeof(regions) / sizeof(regions[0])) return regions[map_group];
  return "Unknown";
}

static void FormatInspectorInfo(char *buffer, size_t buffer_size) {
  uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 map = g_ram[kActRaiserWram_CurrentMap];
  char music[128];
  MusicReplacements_FormatPlaybackStatus(music, sizeof(music));
  extern int snes_frame_counter;
  snprintf(buffer, buffer_size,
           "SCENE %-11.11s $18/$19 $%02X/$%02X\n"
           "GF $%04X HOST %d P:%c T:%s\n"
           "CAM $%04X,$%04X MAP %uX%u\n"
           "PPU MODE %u MAIN $%02X SUB $%02X\n"
           "%s",
           InspectorSceneName(map_group, map), map_group, map,
           ReadWram16(kActRaiserWram_GameFrame), snes_frame_counter,
           g_paused ? 'Y' : 'N', g_turbo ? "ON" : "OFF",
           ReadWram16(kActRaiserWram_Bg1CameraX),
           ReadWram16(kActRaiserWram_Bg1CameraY),
           ReadWram16(kActRaiserWram_Bg1Width),
           ReadWram16(kActRaiserWram_Bg1Height),
           g_ppu ? PPU_mode(g_ppu) : 0,
           g_ppu ? g_ppu->screenEnabled[0] : 0,
           g_ppu ? g_ppu->screenEnabled[1] : 0,
           music);
}

static bool DumpSceneAssets(void) {
  if (!g_ppu) return false;
  RedrawPausedFrameIfNeeded();
  static unsigned dump_number;
  extern int snes_frame_counter;
  unsigned game_frame = ReadWram16(kActRaiserWram_GameFrame);
  char directory[320];
  RunDirFile(directory, sizeof(directory), "scene_assets_%02u_h%d_gf%u",
             dump_number++, snes_frame_counter, game_frame);
  return SceneAssetDump_Write(directory, g_ppu, g_ram, snes_frame_counter);
}

static void PerformWarp(void) {
  extern void ActRaiser_Warp(unsigned region, unsigned map);
  unsigned target = g_settings.warp_target;
  ActRaiser_Warp((target >> 8) & 0xff, target & 0xff);
}

static void TakeFullSnapshot(void) {
  /* Capture all emulated presentation state under a frame-unique prefix.
   * This is shared by F2 and the overlay ACTION row. */
  RedrawPausedFrameIfNeeded();
  static int snap_n;
  char snapdir[320];
  RunDirFile(snapdir, sizeof snapdir, "snapshots");
#ifndef _WIN32
  mkdir(snapdir, 0755);
#endif
  const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
  char prefix[336];
  RunDirFile(prefix, sizeof prefix, "snapshots/snap_%02d_gf%u",
             snap_n++, gf);
  ActRaiser_FullSnapshot(prefix);
  char ppm[344];
  snprintf(ppm, sizeof ppm, "%s.ppm", prefix);
  FILE *pf = fopen(ppm, "wb");
  if (pf) {
    (void)WriteFramebufferPpm(pf);
    fclose(pf);
  }
  fprintf(stderr,
          "[snap] -> %s.{wram,vram,cgram,oam,ppm} (gf=%u)\n",
          prefix, gf);
}

static bool WritePngFromArgb(const char *path, const uint8_t *argb_pixels,
                             int width, int height) {
  size_t row_bytes = (size_t)width * 4;
  uint8_t *rgba = malloc(row_bytes * (size_t)height);
  if (!rgba) return false;
  for (int y = 0; y < height; y++) {
    const uint8_t *src = argb_pixels + (size_t)y * row_bytes;
    uint8_t *dst = rgba + (size_t)y * row_bytes;
    for (int x = 0; x < width; x++) {
      dst[0] = src[2];  // R (BGRA byte 2 → RGBA byte 0)
      dst[1] = src[1];  // G
      dst[2] = src[0];  // B (BGRA byte 0 → RGBA byte 2)
      dst[3] = src[3];  // A
      src += 4;
      dst += 4;
    }
  }
  bool ok = WritePng(path, rgba, width, height);
  free(rgba);
  return ok;
}

static void DumpDioramaLayers(void) {
  const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
  char dir[320];
  RunDirFile(dir, sizeof dir, "diorama_dump");
#ifndef _WIN32
  mkdir(dir, 0755);
#else
  _mkdir(dir);
#endif
  /* Primaries hold each layer's priority-0 remainder once the band splits
   * are bound; the _hi/_p* files are the priority bands. */
  static const struct { int source; const char *name; } kLayers[] = {
    { kPpuOverlaySource_Bg1, "bg1" },
    { kDioramaPlane_Bg1Hi,   "bg1_hi" },
    { kPpuOverlaySource_Bg2, "bg2" },
    { kDioramaPlane_Bg2Hi,   "bg2_hi" },
    { kPpuOverlaySource_Bg3, "bg3" },
    { kPpuOverlaySource_Obj, "obj_p0" },
    { kDioramaPlane_Obj1,    "obj_p1" },
    { kDioramaPlane_Obj2,    "obj_p2" },
    { kDioramaPlane_Obj3,    "obj_p3" },
  };
  int dumped = 0;
  for (int i = 0; i < (int)(sizeof(kLayers) / sizeof(kLayers[0])); i++) {
    uint8_t *px = g_diorama_layer_pixels[kLayers[i].source];
    if (!px) continue;
    char path[344];
    snprintf(path, sizeof path, "%s/%s_gf%u.png", dir, kLayers[i].name, gf);
    if (WritePngFromArgb(path, px, g_snes_width, 224)) dumped++;
  }
  char path[344];
  snprintf(path, sizeof path, "%s/backdrop_gf%u.png", dir, gf);
  if (WritePngFromArgb(path, g_pixels, g_snes_width, 224)) dumped++;
  fprintf(stderr, "[diorama] dumped %d layer PNGs to %s/ (gf=%u, w=%d)\n",
          dumped, dir, gf, g_snes_width);
}

static bool BuildSaveEditRequest(SaveEditRequest *edits) {
  static const int region_states[kSaveProgressEdit_Count] = {
    -1,
    kSaveRegionState_Act1,
    kSaveRegionState_Act1Cleared,
    kSaveRegionState_Act2,
    kSaveRegionState_Act2Cleared,
  };
  static const int item_values[] = {
    -1, 0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0d, 0x0e, 0x0f, 0x12, 0x13, 0x14,
  };
  if (!edits) return false;
  SaveEditRequest_Clear(edits);
  bool staged = false;
  for (int i = 0; i < kActRaiserSaveRegionCount; i++) {
    int selector = g_settings.save_region_progress[i];
    if (selector < 0 || selector >= kSaveProgressEdit_Count) continue;
    edits->region_state[i] = region_states[selector];
    staged = staged || edits->region_state[i] >= 0;
  }
#define SAVE_STAGE_DIRECT(request_field, setting_field) do { \
  edits->request_field = g_settings.setting_field > 0 \
      ? g_settings.setting_field : -1; \
  staged = staged || edits->request_field >= 0; \
} while (0)
#define SAVE_STAGE_ZERO(request_field, setting_field) do { \
  edits->request_field = g_settings.setting_field > 0 \
      ? g_settings.setting_field - 1 : -1; \
  staged = staged || edits->request_field >= 0; \
} while (0)
  SAVE_STAGE_DIRECT(master_level, save_master_level);
  SAVE_STAGE_DIRECT(master_hp, save_master_hp);
  SAVE_STAGE_ZERO(master_mp, save_master_mp);
  SAVE_STAGE_DIRECT(lives, save_lives);
  SAVE_STAGE_ZERO(angel_sp_current, save_angel_sp_current);
  SAVE_STAGE_ZERO(angel_sp_max, save_angel_sp_max);
  SAVE_STAGE_ZERO(angel_hp_current, save_angel_hp_current);
  SAVE_STAGE_DIRECT(angel_hp_max, save_angel_hp_max);
  SAVE_STAGE_ZERO(message_speed, save_message_speed);
#undef SAVE_STAGE_DIRECT
#undef SAVE_STAGE_ZERO

  if (g_settings.save_player_name[0]) {
    edits->player_name_set = true;
    snprintf(edits->player_name, sizeof(edits->player_name), "%s",
             g_settings.save_player_name);
    staged = true;
  }
  if (g_settings.save_professional_mode > 0) {
    edits->professional_mode = g_settings.save_professional_mode - 1;
    staged = true;
  }
  if (g_settings.save_death_heim_state > 0) {
    static const int death_heim_states[] = { -1, 0, 1, 4 };
    int selector = g_settings.save_death_heim_state;
    if (selector < (int)(sizeof(death_heim_states) /
                         sizeof(death_heim_states[0]))) {
      edits->death_heim_state = death_heim_states[selector];
      staged = true;
    }
  }
  if (g_settings.save_equipped_magic > 0) {
    edits->equipped_magic = g_settings.save_equipped_magic - 1;
    staged = true;
  }
  for (int i = 0; i < 4; i++) {
    if (g_settings.save_magic_slots[i] <= 0) continue;
    edits->magic_slots[i] = g_settings.save_magic_slots[i] - 1;
    staged = true;
  }
  for (int i = 0; i < 8; i++) {
    int selector = g_settings.save_item_slots[i];
    if (selector <= 0 ||
        selector >= (int)(sizeof(item_values) / sizeof(item_values[0])))
      continue;
    edits->item_slots[i] = item_values[selector];
    staged = true;
  }
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    for (int act = 0; act < 2; act++) {
      int selector = g_settings.save_scores[region][act];
      if (selector <= 0) continue;
      edits->scores[region][act] = (selector - 1) * 10;
      staged = true;
    }
  }
  return staged;
}

static bool OnSettingsAction(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;
  if (!strcmp(desc->key, "toggle_pause")) {
    TogglePause();
  } else if (!strcmp(desc->key, "toggle_turbo")) {
    ToggleTurbo();
  } else if (!strcmp(desc->key, "save_state")) {
    RtlSaveLoad(kSaveLoad_Save, 0);
    fprintf(stderr, "State saved.\n");
  } else if (!strcmp(desc->key, "load_state")) {
    /* Rewrites all of g_ppu (VRAM/CGRAM/OAM/regs). No quiesce needed: the
     * next present happens later on this same thread (#18/P13). The loaded
     * state is an unrelated scene, so the interpolation history must not
     * survive it. */
    RtlSaveLoad(kSaveLoad_Load, 0);
    HostDisplay_InvalidatePresentHistory();
    fprintf(stderr, "State loaded.\n");
  } else if (!strcmp(desc->key, "warp_now")) {
    PerformWarp();
  } else if (!strcmp(desc->key, "take_snapshot")) {
    TakeFullSnapshot();
  } else if (!strcmp(desc->key, "diorama_reset")) {
    Diorama_ResetCamera();
  } else if (!strcmp(desc->key, "sim3d_reset_camera")) {
    Sim3D_ResetCamera();
  } else if (!strcmp(desc->key, "dump_scene_assets")) {
    if (!DumpSceneAssets()) return false;
  } else if (!strcmp(desc->key, "save_apply_session") ||
             !strcmp(desc->key, "save_apply_persist")) {
    SaveEditRequest edits;
    BuildSaveEditRequest(&edits);
    SaveError error = {{0}};
    bool persist = !strcmp(desc->key, "save_apply_persist");
    if (!SaveSystem_ApplyEdits(
            &edits, g_settings.save_edit_armed, persist,
            g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] %s failed: %s\n",
              persist ? "apply and save" : "session apply", error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] staged save edits applied%s\n",
            persist ? " and saved" : " for this session");
  } else if (!strcmp(desc->key, "save_import")) {
    const char *path = getenv("AR_SAVE_IMPORT");
    if (!path || !path[0]) {
      FILE *probe = fopen("saves/import.srm", "rb");
      if (probe) {
        fclose(probe);
        path = "saves/import.srm";
      } else {
        path = "saves/import.ini";
      }
    }
    SaveError error = {{0}};
    if (!SaveSystem_Import(path, g_settings.save_autobackup, &error)) {
      fprintf(stderr, "[save-editor] import %s failed: %s\n", path,
              error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] imported %s -> %s\n", path,
            SaveSystem_ActivePath());
  } else if (!strcmp(desc->key, "save_export_srm") ||
             !strcmp(desc->key, "save_export_ini")) {
    bool ini = !strcmp(desc->key, "save_export_ini");
    const char *path = ini ? "saves/export.ini" : "saves/export.srm";
    SaveError error = {{0}};
    if (!SaveSystem_Export(ini ? kSaveFileFormat_Ini
                               : kSaveFileFormat_NativeSrm,
                           path, &error)) {
      fprintf(stderr, "[save-editor] export %s failed: %s\n", path,
              error.message);
      return false;
    }
    fprintf(stderr, "[save-editor] export -> %s\n", path);
  } else if (!strcmp(desc->key, "restart_game") ||
             !strcmp(desc->key, "exit_desktop")) {
    /* Settings normally persist at mutation time, but repeat the write here.
     * Battery SRAM is flushed through the active Phase-6 backend by the shared
     * shutdown path. */
    char settings_path[1024];
    UserDataFile(settings_path, sizeof settings_path, "settings.ini");
    if (!Settings_Save(settings_path)) {
      fprintf(stderr, "[lifecycle] could not save settings.ini\n");
      return false;
    }
    g_host_lifecycle_request = !strcmp(desc->key, "restart_game")
        ? kHostLifecycle_Restart : kHostLifecycle_Exit;
    SettingsOverlay_Close();
    fprintf(stderr, "[lifecycle] %s requested\n",
            g_host_lifecycle_request == kHostLifecycle_Restart
                ? "restart" : "exit");
  } else {
    return false;
  }
  return true;
}

/* Gamepad-only host actions (input_map.h). Deliberately the same entry points
 * the keyboard hotkeys use, so a pad press and a keypress cannot diverge. */
static void OnGamepadHostAction(InputAction action) {
  switch (action) {
    case kInputAction_Menu:
      if (SettingsOverlay_IsOpen()) {
        SettingsOverlay_Close();
      } else {
        ClearHeldInput();
        SettingsOverlay_Open();
      }
      break;
    case kInputAction_Pause:
      TogglePause();
      break;
    case kInputAction_CamReset:
      /* Same split the middle-click reset uses: whichever 3D view is on
       * screen owns the button, and neither responds outside them. */
      if (Diorama_IsActiveThisFrame()) Diorama_ResetCamera();
      else if (Sim3D_FreeCameraActiveThisFrame()) Sim3D_ResetCamera();
      break;
    case kInputAction_Turbo:
      ToggleTurbo();
      break;
    case kInputAction_SaveState:
      (void)OnSettingsAction(Settings_Find("save_state"));
      break;
    case kInputAction_LoadState:
      (void)OnSettingsAction(Settings_Find("load_state"));
      break;
    default:
      break;
  }
}

static void OnRuntimeSettingChanged(const SettingDesc *desc,
                                    SettingChangeResult result) {
  (void)result;
  /* Several branches below mutate the renderer/window wholesale
   * (SDL_SetWindowFullscreen, HostDisplay_ResolveVideoGeometry ->
   * RebindPpuOutputSurfaces + HostDisplay_ApplyWindowScale's logical sizing/
   * SDL_SetWindowSize — §2.9(a)). This used to be bracketed by a
   * present-thread quiesce; with rendering main-thread-only (#18/P13) the
   * dispatch is ordinary straight-line code — the next present cannot
   * overlap it. */
  if (desc->field == &g_settings.audio_master_volume)
    HostAudio_SetMasterVolumePercent(g_settings.audio_master_volume);
  if (desc->field == &g_settings.audio_enabled)
    HostAudio_SetEnabled(g_settings.audio_enabled);
  if (desc->field == &g_settings.music_replacements)
    MusicReplacements_ApplySetting();
  if (desc->field == &g_settings.scene_inspector &&
      !g_settings.scene_inspector)
    CloseSceneInspectorSelection();
  if (desc->field == &g_settings.window_mode && g_window) {
    HostDisplay_ApplyWindowMode();
    HostDisplay_UpdateProperties();  /* exclusive fullscreen can change the mode */
  }
  /* B1a (followup doc): live-apply without a restart — mirrors the boot-time
   * SDL_SetRenderVSync read near SDL_CreateRenderer. Refresh rate owns vsync
   * now; the frame-limit interval is polled per-present by
   * HostDisplay_SubmitFrame, so a Limit-FPS change needs no explicit apply. */
  if ((desc->field == &g_settings.refresh_mode ||
       desc->field == &g_settings.uncapped_framerate) && g_renderer)
    HostDisplay_ApplyRefreshVsync();
  if (desc->field == &g_settings.extended_aspect ||
      desc->field == &g_settings.pixel_aspect) {
    HostDisplay_ResolveVideoGeometry(true);
    g_paused_redraw_pending = true;
    return;
  }
  /* Menu edits of the camera rows re-seed the live camera, the mirror of the
   * write-back Diorama_AdjustCamera does for mouse input (§D13). */
  if (desc->field == &g_settings.diorama_tilt_x_mrad ||
      desc->field == &g_settings.diorama_tilt_y_mrad ||
      desc->field == &g_settings.diorama_distance_x100)
    Diorama_SeedCameraFromSettings();
  if (desc->field == &g_settings.new_renderer) {
    g_new_ppu = g_settings.new_renderer || g_ws_active;
    g_paused_redraw_pending = true;
  }
  /* HUD scale is a display-category value but must not resize a manually
   * resized window. Only the display profile and geometry policies own the
   * presentation/window dimensions.
   *
   * window_scale is the one row whose whole purpose is to set the window size,
   * so it gets HostDisplay_ApplyWindowScale. The others change what is DRAWN,
   * not how big the window is: re-derive the logical presentation and leave
   * the user's window alone (a hand-resized window must survive an aspect
   * change). */
  if (desc->field == &g_settings.window_scale)
    HostDisplay_ApplyWindowScale();
  else if (desc->field == &g_settings.display_mode ||
           desc->field == &g_settings.ignore_aspect_ratio ||
           desc->category == kSettingCat_Widescreen)
    HostDisplay_RecomputeLogicalPresentation();
  if (desc->category == kSettingCat_Display ||
      desc->category == kSettingCat_Widescreen ||
      Settings_CategoryIsSim3D(desc->category))
    g_paused_redraw_pending = true;
  /* A settings change can span an arbitrary human-scale gap and may have
   * rebuilt the geometry; don't interpolate the next frame against a camera
   * snapshot from before it. */
  HostDisplay_InvalidatePresentHistory();
}

/* GetPresentationViewport moved to present.h/present.c as
 * ComputePresentationViewport (M5, D4/D6): pure, no globals, so both this
 * file's live callers (below) and present.c's slot-fed composite get the
 * same math. */

static void AdjustHudOutputScale(int delta_percent) {
  const SettingDesc *desc = Settings_Find("hud_scale_percent");
  if (!desc) return;
  int current = g_settings.hud_scale_percent;
  if (!current && g_renderer) {
    SDL_Rect viewport = ComputePresentationViewport(
        g_renderer, g_ws_active, g_settings.ignore_aspect_ratio,
        g_active_pixel_aspect, Settings_VisibleWidth(), g_snes_height);
    current = (viewport.h * 100 + g_snes_height / 2) / g_snes_height;
    current = ((current + 12) / 25) * 25;
  }
  if (!current) current = 100;
  int next = current + delta_percent;
  if (next < 25) next = 25;
  if (next > 400) next = 400;
  SettingChangeResult result = Settings_SetLong(desc, next);
  char formatted[32];
  Settings_FormatValue(desc, formatted, sizeof(formatted));
  fprintf(stderr, "[hud-overlay] scale -> %s (%s; 1.00x = native output)\n",
          formatted, Settings_ChangeResultName(result));
}



static bool PointInRect(int x, int y, SDL_Rect rect) {
  return x >= rect.x && x < rect.x + rect.w &&
         y >= rect.y && y < rect.y + rect.h;
}

static bool HudChunkPixelVisible(const HudPresentationChunk *chunk,
                                 int source_x, int source_y) {
  const uint8_t *pixels = chunk->inspector_kind == kInspectorPresentation_HudObj
      ? g_hud_obj_pixels : g_hud_bg_pixels;
  int texture_x = source_x + (g_snes_width - 256) / 2;
  if (!pixels || texture_x < 0 || texture_x >= g_snes_width ||
      source_y < 0 || source_y >= g_snes_height)
    return false;
  return pixels[((size_t)source_y * g_snes_width + texture_x) * 4 + 3] != 0;
}

static bool WindowPointToOutput(int event_x, int event_y,
                                int *output_x, int *output_y) {
  if (!g_window || !g_renderer) return false;
  int window_width = 0, window_height = 0;
  int output_width = 0, output_height = 0;
  SDL_GetWindowSize(g_window, &window_width, &window_height);
  if (!SDL_GetRenderOutputSize(g_renderer, &output_width, &output_height) ||
      window_width <= 0 || window_height <= 0 ||
      output_width <= 0 || output_height <= 0)
    return false;
  /* SDL3 does NOT pre-transform mouse events by the renderer's logical
   * presentation — event x/y stay in window-client coordinates. All the
   * downstream hit-testing (GetPresentationViewport, the HUD chunk rects)
   * works in renderer-output-pixel space, so the only mapping this needs is
   * the window -> output-pixel scale, which also covers high-DPI backing
   * scale. This was the SDL2 "no logical size" fallback path; under SDL3 it
   * is correct for every case. */
  if (output_x)
    *output_x = (int)(((int64_t)event_x * output_width +
                       window_width / 2) / window_width);
  if (output_y)
    *output_y = (int)(((int64_t)event_y * output_height +
                       window_height / 2) / window_height);
  return true;
}

/* Resolve the OBJ HUD-icon slot from LIVE g_ppu, the same computation
 * present.c's BuildProjectionInputsFromSlot does from the FrameSlot (D4 —
 * one algorithm, two callers). */
static void FillLiveHudProjectionInputs(HudProjectionInputs *in) {
  memset(in, 0, sizeof(*in));
  in->hud_bg_texture = g_hud_bg_texture;
  in->hud_obj_texture = g_hud_obj_texture;
  /* Same density correction the FrameSlot producer applies (D4 — one
   * algorithm, two callers). */
  in->hud_scale_percent =
      Settings_ScalePercentToOutput(g_settings.hud_scale_percent);
  in->pixel_aspect = g_active_pixel_aspect;
  in->snes_width = g_snes_width;
  in->snes_height = g_snes_height;
  in->visible_width = Settings_VisibleWidth();
  if (!g_ppu) return;
  in->hud_split_height = g_ppu->wsHudSplitHeight;
  in->hud_left_end = g_ppu->wsHudLeftEnd;
  in->hud_right_start = g_ppu->wsHudRightStart;
  in->hud_player_row_y = g_ppu->wsHudPlayerRowY;
  in->hud_left_only_y = g_ppu->wsHudLeftOnlyY;
  in->extra_left_right = g_ppu->extraLeftRight;
  const PpuOverlayCapture *bg3_capture =
      &g_ppu->overlayCaptures[kPpuOverlaySource_Bg3];
  if (bg3_capture->y1 > (int16_t)in->hud_split_height && bg3_capture->y1 <= 240)
    in->hud_body_y1 = (uint8_t)bg3_capture->y1;
  const PpuOverlayCapture *obj_capture =
      &g_ppu->overlayCaptures[kPpuOverlaySource_Obj];
  if (obj_capture->oamCount == 4) {
    int first = obj_capture->oamFirst;
    in->obj_icon_x = (g_ppu->oam[first * 2] & 0xff) |
        ((g_ppu->highOam[first >> 2] >> ((first & 3) * 2)) & 1) << 8;
    in->obj_icon_y = g_ppu->oam[first * 2] >> 8;
    in->obj_icon_valid = true;
  }
}

static bool InspectWindowPoint(int window_x, int window_y) {
  int output_x = 0, output_y = 0;
  if (!WindowPointToOutput(window_x, window_y, &output_x, &output_y))
    return false;
  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, g_ws_active, g_settings.ignore_aspect_ratio,
      g_active_pixel_aspect, Settings_VisibleWidth(), g_snes_height);
  bool had_selection = SceneInspector_HasSelection();
  bool was_paused = g_paused != 0;
  int output_width = 0, output_height = 0;
  SDL_GetRenderOutputSize(g_renderer, &output_width, &output_height);

  HudProjectionInputs hud_inputs;
  FillLiveHudProjectionInputs(&hud_inputs);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int chunk_count = BuildHudPresentationChunks(viewport, &hud_inputs, chunks);
  bool selected = false;
  for (int i = chunk_count - 1; i >= 0 && !selected; i--) {
    const HudPresentationChunk *chunk = &chunks[i];
    if (!PointInRect(output_x, output_y, chunk->output_destination))
      continue;
    double source_x = chunk->screen_source.x +
        (double)(output_x - chunk->output_destination.x) *
        chunk->screen_source.w / chunk->output_destination.w;
    double source_y = chunk->screen_source.y +
        (double)(output_y - chunk->output_destination.y) *
        chunk->screen_source.h / chunk->output_destination.h;
    int sample_x = (int)source_x;
    int sample_y = (int)source_y;
    if (!HudChunkPixelVisible(chunk, sample_x, sample_y)) continue;
    int inspector_x = sample_x + chunk->inspector_x_bias;
    unsigned bg_mask = chunk->inspector_kind == kInspectorPresentation_HudBg
        ? kSceneInspectorBg3 : 0;
    bool inspect_objects =
        chunk->inspector_kind == kInspectorPresentation_HudObj;
    if (!SceneInspector_SelectFiltered(
            inspector_x, sample_y, bg_mask, inspect_objects))
      continue;
    g_scene_inspector_presentation = (InspectorPresentationSelection){
      chunk->inspector_kind, source_x, source_y,
      output_x, output_y, output_width, output_height,
    };
    fprintf(stderr,
            "[scene-inspector-hit] event=%d,%d output=%d,%d target=%s "
            "source=%.3f,%.3f dst=%d,%d,%d,%d\n",
            window_x, window_y, output_x, output_y,
            chunk->inspector_kind == kInspectorPresentation_HudBg
                ? "hud-bg3" : "hud-obj",
            source_x, source_y,
            chunk->output_destination.x,
            chunk->output_destination.y,
            chunk->output_destination.w,
            chunk->output_destination.h);
    selected = true;
  }

  if (!selected) {
    if (!PointInRect(output_x, output_y, viewport)) return false;
    int visible_left = Settings_VisibleX0() - g_ws_extra;
    double screen_position_x = visible_left +
        (double)(output_x - viewport.x) * Settings_VisibleWidth() /
        viewport.w;
    double screen_position_y =
        (double)(output_y - viewport.y) * g_snes_height / viewport.h;
    int screen_x = visible_left +
        (int)((double)(output_x - viewport.x) * Settings_VisibleWidth() /
              viewport.w);
    int screen_y =
        (int)((double)(output_y - viewport.y) * g_snes_height /
              viewport.h);
    if (!SceneInspector_Select(screen_x, screen_y)) return false;
    g_scene_inspector_presentation = (InspectorPresentationSelection){
      kInspectorPresentation_Base, screen_position_x, screen_position_y,
      output_x, output_y, output_width, output_height,
    };
    fprintf(stderr,
            "[scene-inspector-hit] event=%d,%d output=%d,%d target=base "
            "screen=%.3f,%.3f viewport=%d,%d,%d,%d\n",
            window_x, window_y, output_x, output_y,
            screen_position_x, screen_position_y,
            viewport.x, viewport.y, viewport.w, viewport.h);
  }
  if (!had_selection)
    g_scene_inspector_owns_pause = !was_paused;
  ClearHeldInput();
  g_paused = true;
  return true;
}

/* Load the HD replacement manifest and decode each screen-plane entry's art
 * once at startup. A missing manifest or image leaves entries textureless and
 * therefore fully inert (no capture requests, authentic rendering).
 * AR_HD_MANIFEST overrides the default manifest location for experiments. */
static void LoadHdReplacements(void) {
  const char *path = getenv("AR_HD_MANIFEST");
  if (!path || !path[0]) path = "game-assets/manifest.ini";
  if (!HdReplacements_Load(path)) return;
  int with_art = 0;
  for (int i = 0; i < g_hd_replacement_count; i++) {
    HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Tiles) continue;
    /* Entries ship in the manifest without their art; a missing image file
     * is the normal "hook available, art not provided" state and stays
     * silent. A file that exists but fails to decode is a real error. */
    FILE *probe = fopen(entry->image, "rb");
    if (!probe) continue;
    fclose(probe);
    int w = 0, h = 0, comp = 0;
    stbi_uc *rgba = stbi_load(entry->image, &w, &h, &comp, 4);
    if (!rgba) {
      fprintf(stderr, "[hd-manifest] [replace:%s] cannot decode %s (%s)\n",
              entry->name, entry->image, stbi_failure_reason());
      continue;
    }
    if (entry->plane == kHdPlane_Mode7) {
      /* The engine sampler consumes raw ARGB words, not an SDL texture. */
      uint32_t *argb = malloc((size_t)w * h * 4);
      if (argb) {
        for (size_t p = 0; p < (size_t)w * h; p++) {
          const stbi_uc *s = rgba + p * 4;
          argb[p] = (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16 |
                    (uint32_t)s[1] << 8 | s[2];
        }
        entry->pixels = argb;
        entry->pixels_width = w;
        entry->pixels_height = h;
        with_art++;
        fprintf(stderr, "[hd-manifest] [replace:%s] %s (%dx%d, mode7)\n",
                entry->name, entry->image, w, h);
      }
      stbi_image_free(rgba);
      continue;
    }
    /* ABGR8888 matches stb's little-endian R,G,B,A byte order directly. */
    SDL_Texture *texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (texture && SDL_UpdateTexture(texture, NULL, rgba, w * 4)) {
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
      /* Match the SDL2 global nearest scale-quality the build relied on. */
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
      entry->texture = texture;
      with_art++;
      fprintf(stderr, "[hd-manifest] [replace:%s] %s (%dx%d)\n",
              entry->name, entry->image, w, h);
    } else {
      if (texture) SDL_DestroyTexture(texture);
      fprintf(stderr, "[hd-manifest] [replace:%s] texture upload failed: %s\n",
              entry->name, SDL_GetError());
    }
    stbi_image_free(rgba);
  }
  fprintf(stderr, "[hd-manifest] %d entries, %d with art\n",
          g_hd_replacement_count, with_art);
}

/* Bind overlay surfaces for every source a loaded screen-plane entry can
 * capture. BG3/OBJ are already bound to the HUD surfaces; the other sources
 * get lazily allocated buffers. Must run after the HUD bindings. */
static void BindHdReplacementSurfaces(void) {
  for (int i = 0; i < g_hd_replacement_count; i++) {
    const HdReplacement *entry = &g_hd_replacements[i];
    if (entry->plane == kHdPlane_Mode7 && entry->pixels &&
        !g_m7_overlay_pixels && g_renderer) {
      size_t capacity_pitch =
          (size_t)kPpuBufWidth * kHdMode7Scale * 4;
      size_t active_pitch =
          (size_t)g_snes_width * kHdMode7Scale * 4;
      g_m7_overlay_pixels =
          calloc(1, capacity_pitch * 224 * kHdMode7Scale);
      g_m7_texture = SDL_CreateTexture(
          g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          kPpuBufWidth * kHdMode7Scale,
          g_snes_height * kHdMode7Scale);
      if (g_m7_overlay_pixels && g_m7_texture) {
        SDL_SetTextureBlendMode(g_m7_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_m7_texture, SDL_SCALEMODE_NEAREST);
        PpuBindMode7OverlaySurface(g_ppu, g_m7_overlay_pixels, active_pitch,
                                   kHdMode7Scale);
      }
      continue;
    }
    if (entry->plane != kHdPlane_Screen || !entry->texture) continue;
    int source = entry->source;
    if (source == kPpuOverlaySource_Bg3 || source == kPpuOverlaySource_Obj ||
        g_hd_overlay_pixels[source])
      continue;
    g_hd_overlay_pixels[source] = calloc(1, kPpuBufWidth * 4 * 240);
    if (g_hd_overlay_pixels[source])
      PpuBindOverlaySurface(g_ppu, (PpuOverlaySource)source,
                            g_hd_overlay_pixels[source], g_snes_width * 4);
  }
}

/* RENDER_TARGETS_RESET/DEVICE_RESET: STATIC textures are emptied by the
 * driver. Destroy and re-upload the HD replacement textures from disk. */
static void ReloadHdReplacementTextures(void) {
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (g_hd_replacements[i].texture) {
      SDL_DestroyTexture((SDL_Texture *)g_hd_replacements[i].texture);
      g_hd_replacements[i].texture = NULL;
    }
  }
  LoadHdReplacements();
  BindHdReplacementSurfaces();
}

void ActRaiser_RebindPpuOutputSurfaces(void) {
  if (!g_ppu) return;
  size_t pitch = (size_t)g_snes_width * 4;
  PpuBeginDrawing(g_ppu, g_pixels, pitch, 0);
  PpuClearOverlayBindings(g_ppu);
  PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Bg3,
                        g_hud_bg_texture ? g_hud_bg_pixels : NULL, pitch);
  PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Obj,
                        g_hud_obj_texture ? g_hud_obj_pixels : NULL, pitch);
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    if (source == kPpuOverlaySource_Bg3 ||
        source == kPpuOverlaySource_Obj ||
        !g_hd_overlay_pixels[source])
      continue;
    PpuBindOverlaySurface(g_ppu, (PpuOverlaySource)source,
                          g_hd_overlay_pixels[source], pitch);
  }
  if (g_m7_overlay_pixels)
    PpuBindMode7OverlaySurface(
        g_ppu, g_m7_overlay_pixels,
        (size_t)g_snes_width * kHdMode7Scale * 4, kHdMode7Scale);
  if (g_ws_active)
    PpuSetExtraSpaceCentered(g_ppu, (uint8_t)g_ws_extra);
  else
    PpuSetExtraSpace(g_ppu, 0);
}

/* Called by the diorama_mode descriptor's change hook, so the menu row and
 * the D hotkey share one path. The render margin widens to kWsExtraMax while
 * the mode is armed (HostDisplay_ResolveVideoGeometry), so the geometry must be
 * re-derived and the PPU surfaces rebound at the new pitch; the display crop
 * and window size are deliberately unaffected. */
void Diorama_OnModeChanged(void) {
  if (!g_settings.diorama_mode) g_diorama_frame_active = false;
  if (!g_ppu) return;   /* pre-boot settings load */
  /* Memsets the live pixel buffers and rebinds PPU output surfaces. Formerly
   * quiesce-bracketed; main-thread-only rendering makes that unnecessary
   * (#18/P13), same as OnRuntimeSettingChanged. */
  HostDisplay_ResolveVideoGeometry(false);
  memset(g_pixels, 0, sizeof(g_pixels));
  memset(g_hud_bg_pixels, 0, sizeof(g_hud_bg_pixels));
  memset(g_hud_obj_pixels, 0, sizeof(g_hud_obj_pixels));
  ActRaiser_RebindPpuOutputSurfaces();
  g_paused_redraw_pending = true;
  HostDisplay_InvalidatePresentHistory();
}

/* M6 (ar-recomp-threading-impl.md §3, Phase 2 fixed-timestep). Per-tick
 * input resolution: the debug force-input hooks and the differential-oracle
 * record/replay, both keyed on the game's own $0088 frame counter. Must run
 * once per EMULATED tick (§3.7) — with the M6 accumulator loop, a single
 * outer host iteration can run zero, one, or several ticks (catch-up), so
 * this can no longer live inline before a single RtlRunFrame call. Headless
 * still calls this exactly once per outer iteration (§3.6 — headless never
 * runs more than one tick per iteration), so its behavior is unchanged. */
static uint32 ComputeGameInputs(bool *stop_running) {
  /* Re-read rather than trusting the last event: a gamepad's held bits are
   * owned by input_map.c and change without a keyboard event ever firing. */
  g_input_state = InputMap_State();
  const uint32 inputs =
      ForcedInput_Apply(g_input_state, snes_frame_counter);
  const InputReplayFrameResult replay_result = InputReplay_Resolve(inputs);
  if (replay_result.stop_requested) *stop_running = false;
  return replay_result.inputs;
}

/* One emulated tick: sample input, run the recompiled game logic, apply
 * turbo's extra same-input sub-frames (§3.2 — unchanged mechanism, just
 * relocated so it fires once per emulated tick instead of once per outer
 * host iteration), and the AR_PERF/AR_APUPROF instrumentation that measures
 * it (§3.5 — "wrap the per-tick RtlRunFrame"). Called once per outer
 * iteration by the headless loop (§3.6) and 0-N times per outer iteration by
 * the non-headless fixed-timestep accumulator loop (§3.1). */
static void RunOneEmulatedTick(bool *stop_running) {
  extern uint8 g_ram[];
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;
  uint32 perf_t0 = perf_on ? SDL_GetTicks() : 0;
  /* AR_APUPROF=<ms>: per-frame APU-stall attribution. Any game frame whose
   * wall time reaches the threshold (default 8 ms; the flag value overrides
   * when >= 2) prints one [apuprof] line splitting the frame into lock-wait
   * vs SPC catch-up vs handshake-spin vs upload vs music-hook time. */
  static int apuprof_ms = -2;
  if (apuprof_ms == -2) {
    extern int ApuProfEnabled(void);
    apuprof_ms = ApuProfEnabled() ? atoi(getenv("AR_APUPROF")) : -1;
    if (apuprof_ms >= 0 && apuprof_ms < 2) apuprof_ms = 8;
  }
  uint64_t apuprof_t0 = 0;
  unsigned long apuprof_push0 = 0;
  uint64_t apuprof_loop0 = 0;
  if (apuprof_ms > 0) {
    extern void ApuProfFrameReset(void);
    extern uint64_t audio_trace_wall_ns(void);
    extern unsigned long g_recomp_push_count;
    extern uint64_t g_watchdog_loop_headers;
    ApuProfFrameReset();
    apuprof_push0 = g_recomp_push_count;
    apuprof_loop0 = g_watchdog_loop_headers;
    apuprof_t0 = audio_trace_wall_ns();
  }

  uint32 inputs = ComputeGameInputs(stop_running);

  /* No frame-wide APU lock here (removed 2026-07-16). Every APU-touching
   * path inside the frame takes RtlApuLock itself (RtlApuWrite,
   * snes_readBBus, ReadRegWord, the SPC upload HLE), and the engine's
   * audio thread renders in short locked batches precisely so the two
   * threads interleave. Holding the lock across the whole frame starved
   * the audio callback during transition frames — a map-load frame runs
   * 20-55 ms of collapsed multi-hardware-frame work ([apuprof] loops
   * 25k-75k vs ~3k normal), the callback missed 2-3 fill deadlines, and
   * every level/song transition audibly dropped out even with 250 ms of
   * DSP ring buffered. It also pinned scheduled port-write latency at the
   * produced+3-quanta ceiling (~50 ms) because `produced` could not
   * advance while the game thread held the lock. */
  bool r = RtlRunFrame(inputs);
  (void)r;
  /* TURBO ('t' toggle): real fast-forward = run extra game frames per
   * emulated TICK (not per rendered/present frame — that decoupling is
   * M5's job). Same input word each sub-frame (level-held buttons repeat;
   * fine for skipping sim waits). Cheats/pins apply inside RtlRunFrame, so
   * they hold during the skipped frames too. */
  if (g_turbo) {
    int mult = g_settings.turbo_multiplier;
    for (int tf = 1; tf < mult; tf++) RtlRunFrame(inputs);
  }
  if (apuprof_t0) {
    extern uint64_t audio_trace_wall_ns(void);
    extern uint64_t g_apuprof_lockwait_ns, g_apuprof_catchup_ns,
        g_apuprof_catchup_cyc, g_apuprof_hook_ns, g_apuprof_upload_ns,
        g_apuprof_sched_lat_max;
    extern uint32_t g_apuprof_catchup_calls, g_apuprof_port_reads,
        g_apuprof_port_writes;
    extern const char *g_apuprof_last_port_func;
    extern unsigned long g_recomp_push_count;
    extern uint64_t g_watchdog_loop_headers;
    extern uint64_t g_apuprof_audiowait_max_ns;
    uint64_t dt_ns = audio_trace_wall_ns() - apuprof_t0;
    if (dt_ns >= (uint64_t)apuprof_ms * 1000000u) {
      const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
      double audiowait_ms = g_apuprof_audiowait_max_ns / 1e6;
      g_apuprof_audiowait_max_ns = 0;
      fprintf(stderr,
              "[apuprof] gf=%u dt=%.1fms lockwait=%.2fms "
              "catchup=%.2fms/%llucyc/%uc reads=%u writes=%u "
              "hook=%.2fms upload=%.2fms schedlat=%llusmp pushes=%lu "
              "loops=%llu audiowait-max=%.2fms last=%s\n",
              gf, dt_ns / 1e6, g_apuprof_lockwait_ns / 1e6,
              g_apuprof_catchup_ns / 1e6,
              (unsigned long long)g_apuprof_catchup_cyc,
              g_apuprof_catchup_calls, g_apuprof_port_reads,
              g_apuprof_port_writes, g_apuprof_hook_ns / 1e6,
              g_apuprof_upload_ns / 1e6,
              (unsigned long long)g_apuprof_sched_lat_max,
              g_recomp_push_count - apuprof_push0,
              (unsigned long long)(g_watchdog_loop_headers - apuprof_loop0),
              audiowait_ms,
              g_apuprof_last_port_func ? g_apuprof_last_port_func : "-");
    }
  }
  if (perf_on) {
    extern void snes_catchup_stats(uint64_t *calls, uint64_t *cycles);
    static uint32 win_start, run_ms_sum, run_ms_max; static int win_frames;
    static uint64_t last_cu_calls, last_cu_cycles; static unsigned last_gf;
    uint32 t1 = SDL_GetTicks();
    uint32 dt = t1 - perf_t0;
    run_ms_sum += dt; if (dt > run_ms_max) run_ms_max = dt;
    win_frames++;
    if (!win_start) win_start = t1;
    if (t1 - win_start >= 1000) {
      uint64_t cc, cy; snes_catchup_stats(&cc, &cy);
      const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr, "[perf] fps=%d run-ms avg=%.1f max=%u gf+=%u "
              "apu-catchup calls=%llu cyc=%llu $18=%02x\n",
              win_frames, (double)run_ms_sum / win_frames, run_ms_max,
              (unsigned)(uint16)(gf - last_gf),
              (unsigned long long)(cc - last_cu_calls),
              (unsigned long long)(cy - last_cu_cycles),
              g_ram[kActRaiserWram_MapGroup]);
      last_cu_calls = cc; last_cu_cycles = cy; last_gf = gf;
      win_start = t1; run_ms_sum = 0; run_ms_max = 0; win_frames = 0;
    }
  }
}

/* Per-outer-iteration draw + present (§3.5 — "PPM screenshot capture" and
 * the draw step both stay per-outer-iteration, not per-tick: even if the
 * accumulator ran several catch-up ticks this iteration, we draw/present
 * only the LAST one's resulting PPU state once). Caller gates this on
 * "did at least one tick actually run" (headless: always; non-headless:
 * produced_frame).
 *
 * alpha (R17/C4): the sub-tick phase, forwarded to the present. Headless passes
 * kInterpPhaseNone — it never presents to a display and its cadence is
 * deliberately one-tick-per-iteration (§3.6). */
static void DrawAndPresentFrame(bool headless, float alpha) {
  extern uint8 g_ram[];
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;

  uint32 perf_draw_t0 = perf_on ? SDL_GetTicks() : 0;
  RtlDrawPpuFrame();
  /* #16: function-scope so the annotated sim outlives the block below and can
   * be published to FrameSlot_Capture around the HostDisplay_SubmitFrame tail. */
  SimFrameData sim;
  {
    extern int snes_frame_counter;
    SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram, g_ppu);
    SimRenderMetadata_CaptureFrame(
        &sim, g_ram, g_settings.sim3d_mode,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&sim, &tuning);
    /* This site runs on every frame including headless, unlike
     * FrameSlot_Capture, which only runs when a present thread consumes it. */
    Sim3D_RenderTownCanvas(&sim, g_ram, g_ppu);
    sim.town_canvas_serial = SimTownCanvas_Serial();
    Sim3D_LogViewTransition(&sim);
    SceneInspector_SetSimFrameData(&sim);
    SimRenderMetadata_TraceFrame(
        (uint32)snes_frame_counter, &sim, g_pixels,
        g_snes_width, g_snes_height, g_snes_width * 4);
  }
  if (g_diorama_dump_pending) {
    DumpDioramaLayers();
    g_diorama_dump_pending = false;
    if (!g_settings.diorama_mode)
      ActRaiser_RebindPpuOutputSurfaces();
  }
  g_paused_redraw_pending = false;
  if (perf_on) {
    static uint32 draw_win_start, draw_ms_sum, draw_ms_max;
    static int draw_win_frames;
    uint32 now = SDL_GetTicks();
    uint32 dt = now - perf_draw_t0;
    draw_ms_sum += dt;
    if (dt > draw_ms_max) draw_ms_max = dt;
    draw_win_frames++;
    if (!draw_win_start) draw_win_start = now;
    if (now - draw_win_start >= 1000) {
      fprintf(stderr,
              "[draw-perf] frames=%d draw-ms avg=%.1f max=%u $18=%02x $19=%02x\n",
              draw_win_frames, (double)draw_ms_sum / draw_win_frames,
              draw_ms_max, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap]);
      draw_win_start = now;
      draw_ms_sum = 0;
      draw_ms_max = 0;
      draw_win_frames = 0;
    }
  }

  /* Framebuffer capture to PPM (works headless — g_pixels is always populated).
   * AR_SHOT_AT_GF=N      : one shot to saves/shot.ppm at game-frame >= N.
   * AR_SHOT_EVERY=N      : a SERIES — saves/shot_<gf>.ppm every N game-frames,
   *   optionally bounded by AR_SHOT_FROM / AR_SHOT_TO. Lets us compare steady
   *   state vs bug state frame by frame. */
  { const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
    const char *sg = getenv("AR_SHOT_AT_GF");
    const char *se = getenv("AR_SHOT_EVERY");
    int want = 0; char fname[320]; fname[0] = 0;
    static int shot_done = 0;
    if (sg && sg[0] && !shot_done && gf >= (unsigned)strtoul(sg, NULL, 0)) {
      shot_done = 1; want = 1; RunDirFile(fname, sizeof(fname), "shot.ppm");
    } else if (se && se[0]) {
      unsigned every = (unsigned)strtoul(se, NULL, 0); if (!every) every = 1;
      const char *sf = getenv("AR_SHOT_FROM"); const char *st = getenv("AR_SHOT_TO");
      unsigned lo = sf ? (unsigned)strtoul(sf, NULL, 0) : 0;
      unsigned hi = st ? (unsigned)strtoul(st, NULL, 0) : 0xffffffffu;
      if (gf >= lo && gf <= hi && (gf % every) == 0) {
        want = 1; RunDirFile(fname, sizeof(fname), "shot_%u.ppm", gf);
      }
    }
    if (want) {
      FILE *pf = fopen(fname, "wb");
      if (pf) {
        SDL_Point shot_size = WriteFramebufferPpm(pf);
        fclose(pf);
        fprintf(stderr, "[shot] wrote %s at gf=%u (%dx%d) margins=%d/%d mode=%s\n",
                fname, gf, shot_size.x, shot_size.y,
                g_ppu->extraLeftCur, g_ppu->extraRightCur,
                Settings_DisplayModeName(g_settings.display_mode));
      }
    }
  }

  if (!headless) {
    /* #16: FrameSlot_Capture inside this call copies the sim annotated above
     * instead of recomputing it (identical inputs, same thread, nothing
     * mutates them in between). Cleared immediately after: the screenshot
     * and paused/menu-redraw captures run outside this window and must
     * self-annotate. */
    FrameSlot_SetPendingAnnotatedSim(&sim);
    (void)HostDisplay_SubmitFrame(kHostDisplayPresent_GameTick, alpha);
    FrameSlot_SetPendingAnnotatedSim(NULL);
  }
}

/* Per-outer-iteration host-side housekeeping (§3.5): polls / one-shot
 * triggers that are not coupled to the emulated tick rate. Runs once per
 * outer iteration regardless of how many ticks the accumulator fired. */
static void RunOuterIterationHousekeeping(void) {
  extern uint8 g_ram[];
  /* Re-read the display mode ~1/s. The mode-changed events are an
   * optimization, not the source of truth: a compositor can change the
   * effective refresh without emitting one (gamescope does so deliberately). */
  HostDisplay_PollProperties();

  /* Surface audio-chunk drops the callback counted (R12). Reported here, off
   * the audio thread, and coalesced so a sustained problem cannot spam. */
  {
    int dropped = HostAudio_TakeRejectedChunkCount();
    if (dropped) {
      static int total;
      total += dropped;
      fprintf(stderr, "[audio] %d chunk(s) rejected by SDL_PutAudioStreamData "
                      "(%d total this session) — audio glitched\n",
              dropped, total);
    }
  }

  /* Complete the SPC engine's resident uploader once it enters the $CC-wait,
   * for the case where the CPU's HLEd $9A56 ran before the engine got there
   * (takes its own APU lock — must be outside the lock above). */
  { extern void ar_uploader_complete_tick(void); ar_uploader_complete_tick(); }

  /* Music replacement live policy (setting toggled off mid-song). Takes
   * its own APU lock — also outside the lock above. */
  MusicReplacements_FrameTick();

  /* AR_WARP_AT=<gameframe>: fire the AR_WARP target automatically once the
   * 16-bit game-frame counter reaches the value. Headless runs can't press
   * F6; used e.g. to sweep the warp table capturing each level's music src
   * (AR_MUSICLOG). Same transition-capable-state caveats as F6. */
  {
    static long warp_at = -2;
    static bool warp_fired;
    if (warp_at == -2) {
      const char *at = getenv("AR_WARP_AT");
      warp_at = (at && at[0]) ? strtol(at, NULL, 0) : -1;
    }
    if (warp_at >= 0 && !warp_fired) {
      const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
      if (gf >= (unsigned)warp_at) {
        warp_fired = true;
        PerformWarp();
      }
    }
  }

  /* AR_DIORAMA_AT=<gameframe>: flip Diorama 3D on once the game-frame counter
   * reaches the value, through the same descriptor path the D hotkey uses.
   * Booting straight into diorama changes the widescreen margin budget and
   * desyncs game-frame-keyed input replays, so a visual-regression run has to
   * replay flat into the stage and only then switch. */
  {
    static long diorama_at = -2;
    static bool diorama_fired;
    if (diorama_at == -2) {
      const char *at = getenv("AR_DIORAMA_AT");
      diorama_at = (at && at[0]) ? strtol(at, NULL, 0) : -1;
    }
    if (diorama_at >= 0 && !diorama_fired) {
      const unsigned gf = ReadWram16(kActRaiserWram_GameFrame);
      /* $0088 is $5555-filled before the game initialises it; ignore that
       * boot sentinel or every target fires on frame 0. */
      if (gf != kPowerOnGameFrameSentinel &&
          gf >= (unsigned)diorama_at) {
        diorama_fired = true;
        const SettingDesc *mode = Settings_Find("diorama_mode");
        if (mode && Settings_IsAvailable(mode) && !g_settings.diorama_mode) {
          Settings_SetLong(mode, 1);
          fprintf(stderr, "[diorama] ON via AR_DIORAMA_AT at gf=%u\n", gf);
        }
      }
    }
  }

  Diorama_FlushSettingsIfDirty();
  if (g_sim3d_camera_settings_dirty && !g_sim3d_camera_dragging &&
      SDL_GetTicks() - g_sim3d_camera_settings_dirty_at > 500) {
    g_sim3d_camera_settings_dirty = false;
    char settings_path[1024];
    UserDataFile(settings_path, sizeof settings_path, "settings.ini");
    if (!Settings_Save(settings_path))
      fprintf(stderr, "[sim3d] failed to persist camera settings\n");
  }

  /* Auto-persist battery SRAM the moment the game writes a save, so progress
   * survives a freeze/force-quit (the clean-exit save-system write never runs
   * if the game hangs). Cheap: only writes when the 8KB SRAM actually changes.
   * SKIPPED during input replay: a replay is keyed on the game-frame counter
   * from a fixed boot state, so letting the replayed run overwrite save.srm
   * mid-playthrough would change the boot state for the NEXT replay and break
   * the frame alignment (the recording then no longer reaches the same spot). */
  if (!InputReplay_ShouldProtectSaveData()) {
    static bool write_error_reported;
    SaveError error = {{0}};
    if (!SaveSystem_AutoPersistIfChanged(&error)) {
      if (!write_error_reported)
        fprintf(stderr, "[saves] auto-persist failed: %s\n", error.message);
      write_error_reported = true;
    } else {
      write_error_reported = false;
    }
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  const char *rom_path = NULL;
  const char *config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (argv[i][0] != '-') {
      rom_path = argv[i];
    }
  }

  /* Portability: in a shipped bundle, chdir next to the executable so
   * config.ini, settings.ini, saves/, runs/, and game-assets/ resolve — and
   * are regenerated by the existing mkdir paths — beside the binary no matter
   * where it was launched from (double-click, moved folder, script). ROM and
   * config arguments are absolutized first so a relative path on the command
   * line still resolves after the chdir. An in-tree dev build has no marker
   * beside build/ActRaiserRecomp, so the CWD stays authoritative and the dev
   * workflow is unchanged. Must precede RunDirInit (console tee into runs/)
   * and any relative file access. */
  static char rom_abs[1024], config_abs[1024];
  if (PortablePaths_IsBundle()) {
    if (rom_path && snesrecomp_abspath(rom_path, rom_abs, sizeof rom_abs))
      rom_path = rom_abs;
    if (config_path &&
        snesrecomp_abspath(config_path, config_abs, sizeof config_abs))
      config_path = config_abs;
    snesrecomp_anchor_to_exe_dir();
  }

  /* Per-run artifact ringfence (runs/<ts>/): must run before anything prints
   * (console tee) or reads an AR_* output path. See run_dir.h. */
  RunDirInit(argc, argv);

  cpu_trace_init();

  /* AR_DRIFT_FRAME=N: arm the stack-drift tripwire to fire on the first
   * NORMAL function exit at/after frame N whose exit S != entry S (the
   * unbalanced push/pop leaker). Diagnostic only. */
#if SNESRECOMP_TRACE
  { const char *v = getenv("AR_DRIFT_FRAME");
    if (v && v[0]) {
      extern void cpu_trace_arm_stack_drift_tripwire(int32_t);
      cpu_trace_arm_stack_drift_tripwire((int32_t)strtol(v, NULL, 0));
      fprintf(stderr, "[AR_DRIFT_FRAME] stack-drift tripwire armed at frame %s\n", v);
    } }
#endif

  Settings_ClearConfigLayer();
  if (config_path)
    ParseConfigFile(config_path);
  else
    ParseConfigFile("config.ini");

  /* Now that config-file AR_* values are env-bridged, point bare output
   * filenames into the per-run dir (see run_dir.h). */
  RunDirRebaseEnvOutputs();

  if (!rom_path) {
    fprintf(stderr, "Usage: %s <rom.sfc> [--config config.ini]\n", argv[0]);
    return 1;
  }

  size_t rom_size = 0;
  uint8 *rom_data = ReadWholeFile(rom_path, &rom_size);
  if (!rom_data) {
    fprintf(stderr, "Error: cannot open ROM file '%s'\n", rom_path);
    return 1;
  }
  fprintf(stderr, "Loaded ROM: %s (%zu bytes)\n", rom_path, rom_size);

  /* Headless mode for the differential-oracle harness: no window/renderer,
   * run uncapped. PPU emulation still runs (HDMA/IRQ timing affects game
   * state); only the on-screen present is skipped. Parallels snesref's
   * SNESREF_HEADLESS. */
  bool headless = getenv("AR_HEADLESS") && getenv("AR_HEADLESS")[0]
                  && getenv("AR_HEADLESS")[0] != '0';
  bool headless_video = headless && getenv("AR_HEADLESS_VIDEO") &&
                        getenv("AR_HEADLESS_VIDEO")[0] &&
                        getenv("AR_HEADLESS_VIDEO")[0] != '0';
  bool video = !headless || headless_video;

  /* Widescreen budget from config. internal_width = 224 * (ax/ay) display
   * units, divided by the 7:6 pixel stretch when the 4:3-corrected look is
   * on (AspectPAR=4:3, default): 16:9 -> 342 px (extra=43/side), 16:10 -> 308
   * (26); square pixels: 399 (72) / 359 (52). Headless (oracle/differential)
   * runs force authentic geometry so comparisons never see wide framebuffers,
   * unless AR_WS_HEADLESS=1 explicitly opts a visual-regression run into the
   * configured wide geometry. The oracle harness leaves it unset. */
  bool ws_headless = getenv("AR_WS_HEADLESS") && getenv("AR_WS_HEADLESS")[0]
                     && getenv("AR_WS_HEADLESS")[0] != '0';
  HostDisplay_SetWidescreenRuntimeAllowed(!headless || ws_headless);
  /* Resolve application and game settings before allocating presentation
   * resources. Known config.ini values were staged by ParseConfigFile;
   * settings.ini overrides them, and real environment variables win last.
   * The default load path is the SAME portable-relative location every
   * Settings_Save site writes. AR_SETTINGS_PATH still wins so replay fixtures
   * (tools/sim3d_demo.py) can keep their pinned settings. */
  char settings_file[1024];
  const char *settings_path = getenv("AR_SETTINGS_PATH");
  if (!settings_path || !settings_path[0])
    settings_path = UserDataFile(settings_file, sizeof settings_file,
                                 "settings.ini");
  Settings_InitWithFile(settings_path);
  HostDisplay_ResolveVideoGeometry(false);

  /* Display presets depend on whether the resolved aspect selected a wide
   * budget. Finalize only after g_ws_active/g_ws_extra are authoritative. */
  Settings_FinalizeDisplayMode();
  /* AR_MXCHECK=1: enable the per-function-entry m/x invariant check
   * (validates the emitter's static m/x analysis on every direct call). */
  { extern int g_ar_mx_check; const char *e = getenv("AR_MXCHECK");
    g_ar_mx_check = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* AR_MXHIST=1: per-PC runtime m/x histogram + live misdecode anomaly trap. */
  { extern int g_ar_mxhist; extern void ar_mxhist_dump(void);
    const char *e = getenv("AR_MXHIST");
    g_ar_mxhist = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (g_ar_mxhist) atexit(ar_mxhist_dump); }
  /* AR_EXITMX=1: per-function EXIT m/x check — fires when a function's runtime
   * exit (m,x) differs from what the emitter told its callers (exit-mx
   * misdecode, e.g. $03:9156). AR_EXITS=1: per-function EXIT stack-balance
   * check — fires when a paired frame's RTS/RTL drifts S (e.g. $01:B8CF).
   * Symmetric twins of AR_MXCHECK; name the culprit at its own return. */
  { extern int g_ar_exit_mx_check; const char *e = getenv("AR_EXITMX");
    g_ar_exit_mx_check = (e && e[0] && e[0] != '0') ? 1 : 0; }
  { extern int g_ar_exit_s_check; const char *e = getenv("AR_EXITS");
    g_ar_exit_s_check = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* AR_CALLMX=1: per-CALL-SITE m/x invariant check — fires at every JSR/JSL
   * when runtime (m,x) disagrees with what the decoder statically knew at
   * that exact instruction. Catches (m,x) corruption from ANYWHERE upstream
   * of a call (not just decode-time mistakes AR_MXCHECK/AR_EXITMX cover),
   * narrowed to the first call site downstream of the corruption. */
  { extern int g_ar_call_mx_check; const char *e = getenv("AR_CALLMX");
    g_ar_call_mx_check = (e && e[0] && e[0] != '0') ? 1 : 0; }

  /* AR_TRAPFN=<substring>: dump the recomp call stack the first time a matching
   * function is entered (finds the dispatch chain into a misdecode variant). */
  { extern const char *g_ar_trapfn;
    const char *e = getenv("AR_TRAPFN");
    g_ar_trapfn = (e && e[0]) ? e : 0; }

  /* App metadata, BEFORE SDL_Init — SDL documents that it "should be called as
   * early as possible, before SDL_Init", and it cannot be retrofitted later.
   * The identifier "must be in reverse-domain format" and is what "desktop
   * compositors [use] to identify and group windows together": without it a
   * Linux/Deck window gets a generic icon and no taskbar grouping, and the
   * matching .desktop file (same basename as this identifier) has nothing to
   * associate with. */
  SDL_SetAppMetadata(kWindowTitle, AR_APP_VERSION, AR_APP_IDENTIFIER);
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");

  SDL_InitFlags sdl_flags = SDL_INIT_AUDIO;
  if (video) sdl_flags |= SDL_INIT_VIDEO;
  if (!headless) sdl_flags |= SDL_INIT_GAMEPAD;
  /* SDL3 returns true on success (the SDL2 0-on-success convention flipped). */
  if (!SDL_Init(sdl_flags)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (video) {
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
      fprintf(stderr, "[video] WARNING: '%s' renders nowhere — the window will "
                      "never appear. Set SDL_VIDEODRIVER to a real backend from "
                      "the list above, or use an SDL build that has one.\n", driver);

    int scale = g_settings.window_scale ? g_settings.window_scale : 3;
    /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
     * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
     * the height), so derive the window from the target ratio, not the
     * framebuffer. Faithful mode keeps the historical width*scale.
     *
     * Must use the DISPLAY crop (Settings_VisibleWidth), not g_snes_width:
     * diorama mode inflates the render width to the full kWsExtraMax margin
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
     * WindowPointToOutput already maps window points -> output pixels. */
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (headless_video ? SDL_WINDOW_HIDDEN : 0) |
        (g_settings.window_mode != kWindowMode_Windowed
             ? SDL_WINDOW_FULLSCREEN : 0);
    /* SDL3 SDL_CreateWindow no longer takes an x,y position; it is created at
     * a default (centered) position. */
    g_window = SDL_CreateWindow(
      kWindowTitle,
      win_w, win_h,
      window_flags
    );
    if (!g_window) Die("SDL_CreateWindow failed");

    /* SDL3 renderer creation takes a driver NAME (NULL = first available
     * accelerated backend) instead of an index + flag bitmask. Vsync is set
     * separately, and the software backend is selected by name.
     *
     * M8 (ar-recomp-threading-impl.md §7, optional GPU shader polish): the
     * gpu_shaders_enabled setting (kSettingCat_Graphics, kApply_Restart —
     * this backend choice is fixed for the process lifetime) requests the
     * "gpu" backend instead, a prerequisite for SDL_CreateGPURenderState/
     * SDL_SetGPURenderState (used by the diorama shader effects, each still
     * independently toggleable in the same menu). Off by default: this
     * swaps the render backend for the WHOLE app (HUD, flat mode,
     * screenshots, settings overlay), not just diorama, so it needs to earn
     * trust on its own before any shader effect is layered on top. Falls
     * back to the normal auto-selected backend if "gpu" isn't available,
     * rather than dying — this is opt-in polish, not a requirement to run
     * at all. Settings_InitWithFile() has already run by this point, so
     * g_settings reflects settings.ini/config.ini/the legacy AR_GPU_SHADERS
     * env var per the usual priority chain. */
    g_gpu_shaders_requested = g_settings.gpu_shaders_enabled;
    if (headless_video) {
      g_renderer = SDL_CreateRenderer(g_window, SDL_SOFTWARE_RENDERER);
    } else if (g_gpu_shaders_requested) {
      g_renderer = SDL_CreateRenderer(g_window, SDL_GPU_RENDERER);
      if (g_renderer) {
        g_gpu_shaders_active = true;
      } else {
        fprintf(stderr, "[gpu-shaders] \"gpu\" renderer unavailable (%s) — "
                "falling back to the default backend, shaders disabled\n",
                SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, NULL);
      }
    } else {
      g_renderer = SDL_CreateRenderer(g_window, NULL);
    }
    if (!g_renderer) Die("SDL_CreateRenderer failed");
    /* B1a (followup doc): "Uncapped framerate" row (kSettingCat_Graphics).
     * This is the mechanism the toggle actually needs to change something —
     * a bare setting with nothing reading it would be inert. Disabling
     * vsync stops SDL_RenderPresent from blocking the present thread until
     * the display's next refresh; see the present-cadence read below for
     * the other half (redrawing often enough for that to matter). */
    if (!headless_video)
      HostDisplay_ApplyRefreshVsync();

    /* Exclusive fullscreen needs its video mode set after creation; borderless
     * and windowed are already handled by the creation flag. */
    if (!headless_video && g_settings.window_mode == kWindowMode_Exclusive)
      HostDisplay_ApplyWindowMode();
    HostDisplay_UpdateProperties();

    /* Aspect-correct letterboxing via SDL's logical presentation — one
     * implementation shared with the resize/settings paths so boot and runtime
     * can never disagree (4:3-PAR encodes the 7:6 stretch in the logical size;
     * ignore_aspect_ratio stretches instead). */
    HostDisplay_RecomputeLogicalPresentation();

    g_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPpuBufWidth, g_snes_height);
    if (!g_texture) Die("SDL_CreateTexture failed");
    /* The base framebuffer is opaque: the PPU writes RGB with the alpha byte
     * left 0 (see ppu_old.c). SDL2 defaulted new textures to BLENDMODE_NONE so
     * that alpha was ignored, but SDL3 defaults them to BLENDMODE_BLEND — which
     * would blend those alpha-0 pixels to fully transparent and present a BLACK
     * screen. Force NONE to restore the SDL2 opaque blit. (The HUD/overlay
     * textures below deliberately keep BLEND; they carry real alpha.) */
    SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_NONE);
    /* SDL3 textures default to linear filtering; the SDL2 build set the global
     * SDL_HINT_RENDER_SCALE_QUALITY=0 (nearest). Set nearest per-texture so
     * the pixel-art framebuffer and HUD planes upscale crisply. */
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_NEAREST);

    g_hud_bg_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPpuBufWidth, g_snes_height);
    g_hud_obj_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPpuBufWidth, g_snes_height);
    if (!g_hud_bg_texture || !g_hud_obj_texture)
      Die("SDL_CreateTexture for HUD overlay failed");
    SDL_SetTextureBlendMode(g_hud_bg_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(g_hud_obj_texture, SDL_BLENDMODE_BLEND);
    /* Nearest filtering (see g_texture above; the global scale-quality hint
     * SDL2 relied on is gone in SDL3). */
    SDL_SetTextureScaleMode(g_hud_bg_texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(g_hud_obj_texture, SDL_SCALEMODE_NEAREST);

    /* D1b semantic OBJ atlas. It is uploaded every supported SIM frame but is
     * not selected by the compositor until the later separated-composite
     * capability lands, keeping this checkpoint visually authentic. */
    g_sim_obj_atlas_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimObjAtlasWidth, kSimObjAtlasHeight);
    if (g_sim_obj_atlas_texture) {
      SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(g_sim_obj_atlas_texture, SDL_SCALEMODE_NEAREST);
      /* Static storage is zero-initialized before the game thread starts. */
      SDL_UpdateTexture(g_sim_obj_atlas_texture, NULL,
                        g_sim_obj_atlas_pixels, kSimObjAtlasPitch);
    } else {
      fprintf(stderr, "[sim3d-d1] semantic atlas texture unavailable: %s\n",
              SDL_GetError());
    }

    /* D2's observational Mode-1 capture family. Layer textures are retained
     * for inspector/future geometry use; the pitch-zero reference and its
     * absolute-difference image have dedicated opaque streaming textures. */
    g_sim3d_textures_ready = true;
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      g_sim3d_layer_textures[plane] = SDL_CreateTexture(
          g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          kSim3DMaxWidth, kSim3DMaxHeight);
      if (!g_sim3d_layer_textures[plane]) {
        g_sim3d_textures_ready = false;
        break;
      }
      SDL_SetTextureBlendMode(g_sim3d_layer_textures[plane],
                              SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(g_sim3d_layer_textures[plane],
                              SDL_SCALEMODE_NEAREST);
    }
    g_sim3d_flat_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSim3DMaxWidth, kSim3DMaxHeight);
    if (!g_sim3d_flat_texture)
      g_sim3d_textures_ready = false;
    if (g_sim3d_textures_ready) {
      SDL_SetTextureBlendMode(g_sim3d_flat_texture, SDL_BLENDMODE_NONE);
      SDL_SetTextureScaleMode(g_sim3d_flat_texture, SDL_SCALEMODE_NEAREST);
    } else {
      fprintf(stderr, "[sim3d-d2] capture textures unavailable: %s\n",
              SDL_GetError());
      for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
        SDL_DestroyTexture(g_sim3d_layer_textures[plane]);
        g_sim3d_layer_textures[plane] = NULL;
      }
      SDL_DestroyTexture(g_sim3d_flat_texture);
      g_sim3d_flat_texture = NULL;
    }

    LoadHdReplacements();

    /* One streaming texture per diorama plane (priority bands included).
     * Only the backdrop is opaque — every other plane alpha-blends. */
    /* Live report (2026-07-21): a persistent pink/garbage-colored line at
     * the diorama's right edge, root-caused across two failed attempts (the
     * B1b-crisp supersample copy, then suspected in the DOF/edge-AA shader)
     * before landing on the actual source: every consumer that ever samples
     * near the true edge of what Diorama_Upload writes (u=uv_u1 =
     * snes_width/kPpuBufWidth, always < 1.0 — the buffer is allocated at
     * the PPU's max width but a layer's real captured content is narrower,
     * capped by kWsExtraMax's SNES OAM-wrap hardware limit) can reach into
     * columns snes_width..kPpuBufWidth-1, which Diorama_Upload's
     * SDL_UpdateTexture never touches. SDL_TEXTUREACCESS_STREAMING content
     * is undefined until written (no zero guarantee, confirmed non-zero in
     * practice on this backend), so that tail is genuine garbage, not just
     * theoretically risky — and every fix so far (B1b's UV-window clamp,
     * B1b-crisp's valid-subrect blit, the skybox blur's UV inset) was
     * patching ONE consumer at a time as each was discovered, while the DOF/
     * edge-AA shader's own unclamped blur sampling proved there would always
     * be another. Fix it once at the SOURCE instead: zero-fill each
     * texture's FULL extent immediately after creation, before any real
     * frame ever writes into it. Diorama_Upload only ever touches the valid
     * {0,0,snes_width,snes_height} sub-rect afterward, so the margin stays
     * deterministically transparent black (not garbage) for the texture's
     * entire lifetime — every current and future consumer is safe without
     * needing its own clamp/inset workaround. */
    uint8_t *zero_fill = calloc(1, (size_t)kPpuBufWidth * g_snes_height * 4);
    for (int i = 0; i < kDioramaPlane_Count; i++) {
      if (i == kPpuOverlaySource_Bg4) continue;
      g_diorama_textures[i] = SDL_CreateTexture(g_renderer,
          SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
          kPpuBufWidth, g_snes_height);
      if (g_diorama_textures[i]) {
        SDL_SetTextureBlendMode(g_diorama_textures[i],
            i == kDioramaPlane_Backdrop ? SDL_BLENDMODE_NONE
                                        : SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_diorama_textures[i], SDL_SCALEMODE_NEAREST);
        if (zero_fill)
          SDL_UpdateTexture(g_diorama_textures[i], NULL, zero_fill,
                            kPpuBufWidth * 4);
      }
    }
    free(zero_fill);

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);

    /* Take keyboard focus on launch. A window created by SDL is ordered in
     * but the process is not necessarily activated — launched from a terminal
     * (or as an un-bundled binary on macOS) the shell keeps focus and the
     * game starts behind it, silently swallowing input until the user clicks
     * on it. SDL_RaiseWindow both raises and, with the default
     * SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, activates the application.
     * Deliberately last in the video setup so focus lands on a window that is
     * fully configured, and skipped for headless_video (that window is
     * SDL_WINDOW_HIDDEN and must never steal focus from a batch run). */
    if (!headless_video && !SDL_RaiseWindow(g_window))
      fprintf(stderr, "[window] could not raise to foreground: %s\n",
              SDL_GetError());
  }

  if (!SettingsOverlay_Init(g_renderer, rom_data, rom_size))
    Die("SDL font atlas creation for settings overlay failed");
  /* The world map underlay reads three uncompressed ROM blobs once. A failure
   * is not fatal: the stage reports nothing usable and simply never draws. */
  SimWorldMap_Init(rom_data, rom_size);
  SettingsOverlay_SetInspectorInfoProvider(FormatInspectorInfo);

  Settings_SetChangeObserver(OnRuntimeSettingChanged);
  Settings_SetActionObserver(OnSettingsAction);
  /* After the action observer is installed: the pad's save/load-state
   * bindings route through it. */
  InputMap_Init();
  InputMap_SetActionHandler(OnGamepadHostAction);
  Diorama_SeedCameraFromSettings();

  /* Music replacement is audio-side and works headless too (unlike the HD
   * manifest load above, which needs the renderer for textures). Same
   * manifest file; [music:] sections. AR_MUSIC_MANIFEST overrides. */
  {
    const char *music_manifest = getenv("AR_MUSIC_MANIFEST");
    if (!music_manifest || !music_manifest[0])
      music_manifest = "game-assets/manifest.ini";
    MusicReplacements_Load(music_manifest);
    MusicReplacements_InstallHooks();
  }

  /* After music: the census chains the APU port seam music installs. */
  SfxCensus_Init();

  g_spc_player = ActRaiserSpcPlayer_Create();

  RtlRegisterGame(&kActRaiserGameInfo);
  Snes *snes = SnesInit(rom_data, (int)rom_size);
  if (!snes) Die("SnesInit failed");

  BindHdReplacementSurfaces();
  ActRaiser_RebindPpuOutputSurfaces();
  /* Frame-0 margin state: pillarboxed-authentic (render the 256 columns
   * centered in the wide framebuffer). Re-applied every frame by
   * ActRaiser_ApplyWidescreenPolicy since ppu_reset zeroes these fields. */
  if (g_ws_active)
    PpuSetExtraSpaceCentered(g_ppu, (uint8_t)g_ws_extra);

  /* Power-on WRAM fill. The SNES does not clear WRAM at power-on; snes9x (our
   * reference emulator) fills it with the 0x55 pattern, and ActRaiser's title
   * sequence depends on that — with zero-filled WRAM the title's per-frame loop
   * takes a path that underflows the SNES stack and crashes into the $2100
   * open-bus reads (bank_02_AF86). Match snes9x: fill g_ram with 0x55 before
   * boot so uninitialized-RAM reads agree with the reference. AR_WRAM_INIT
   * overrides with an exact dump (used by the differential harness). */
  {
    const char *fenv = getenv("AR_WRAM_FILL");
    int fill = fenv
        ? (int)strtoul(fenv, NULL, 0)
        : kDefaultPowerOnWramFill;
    memset(g_ram, fill, kActRaiserWramSize);
    const char *wp0 = getenv("AR_WRAM_INIT");
    if (wp0 && wp0[0]) {
      FILE *f = fopen(wp0, "rb");
      if (f) { size_t n = fread(g_ram, 1, 0x20000, f); fclose(f);
        fprintf(stderr, "[wram-init] seeded %zu bytes from %s\n", n, wp0); }
      else fprintf(stderr, "AR_WRAM_INIT: cannot open %s\n", wp0);
    }
  }

  /* Power-on battery SRAM fill. A never-written cartridge battery is NOT zero;
   * snes9x (our reference) powers SRAM up to the 0x60 pattern, and ActRaiser
   * validates its save data — an all-zero SRAM is misread as a corrupt/level-0
   * save (the "must be level 1" symptom) instead of "blank -> new game". Match
   * the reference so the save-validity check behaves identically. Only applies
   * to a fresh cart (cart_load zero-fills it); a real .sav load overrides. */
  {
    extern uint8 *g_sram; extern int g_sram_size;
    const char *senv = getenv("AR_SRAM_FILL");
    int sfill = senv ? (int)strtoul(senv, NULL, 0) : 0x60;
    if (g_sram && g_sram_size > 0) memset(g_sram, sfill, g_sram_size);
  }

  /* Load persisted battery save (overrides the fresh-cart fill if present).
   * Portable builds use saves/ beside the executable after the bundle anchor;
   * developer runs use saves/ under their launch directory. */
  char saves_dir[1024], save_srm[1024], save_ini[1024];
  UserDataFile(saves_dir, sizeof saves_dir, "saves");
  mkdir(saves_dir, 0755);
  RtlMigrateLegacySram(kActRaiserGameInfo.title);
  {
    extern uint8 *g_sram; extern int g_sram_size;
    SaveError error = {{0}};
    const char *native_path = getenv("AR_SAVE_NATIVE_PATH");
    const char *ini_path = getenv("AR_SAVE_INI_PATH");
    if (!native_path || !native_path[0]) {
      UserDataFile(save_srm, sizeof save_srm, "saves/save.srm");
      native_path = save_srm;
    }
    if (!ini_path || !ini_path[0]) {
      UserDataFile(save_ini, sizeof save_ini, "saves/save.ini");
      ini_path = save_ini;
    }
    if (!SaveSystem_Attach(g_sram, (size_t)g_sram_size,
                           (SaveBackend)g_settings.save_backend,
                           native_path, ini_path, &error))
      Die(error.message);
    if (!SaveSystem_LoadActive(&error)) {
      fprintf(stderr, "[saves] active load rejected: %s; using fresh SRAM\n",
              error.message);
      SaveSystem_ResyncShadow();
    }

    SaveEditRequest edits;
    bool staged = BuildSaveEditRequest(&edits);
    if (staged && g_settings.save_edit_armed) {
      if (!SaveSystem_ApplyEdits(
              &edits, true, false, g_settings.save_autobackup, &error))
        fprintf(stderr, "[save-editor] boot edits rejected: %s\n",
                error.message);
      else
        fprintf(stderr, "[save-editor] boot edits applied for this session\n");
    } else if (staged) {
      fprintf(stderr,
              "[save-editor] staged boot edits ignored; save editing is not armed\n");
    }
  }

  OracleTrace_Init();
  ForcedInput_Init();
  InputReplay_Init();
  ScheduledSettings_Init();

  if (!HostAudio_Init(Settings_AudioFrequencyHz(), g_settings.audio_samples,
                      g_settings.audio_master_volume,
                      g_settings.audio_enabled)) {
    fprintf(stderr, "[audio] host audio disabled for this session\n");
  }

  /* AR_LOADSTATE=<slot>: load a savestate at boot (before the main loop), so a
   * headless/instrumented run can start from a captured moment instead of
   * replaying from power-on. Runs a few frames first so the game reaches a
   * stable frame boundary, then loads — matches the F7 hotkey path. */
  { const char *ls = getenv("AR_LOADSTATE");
    if (ls && ls[0]) {
      int slot = atoi(ls);
      for (int i = 0; i < 4; i++) RtlRunFrame(0);
      RtlSaveLoad(kSaveLoad_Load, slot);
      fprintf(stderr, "[loadstate] loaded slot %d at boot\n", slot);
    } }

  /* Phase 0 (MY-AUDIT-render-off-thread) + #18/P13: there is no present
   * thread. SDL3's 2D render API is main-thread-only (SDL_render.h:46-47) and
   * PresentThreadFn was the sole contract violator, which is why the default
   * GL/EGL backend would not boot on Wayland. All rendering runs synchronously
   * on this (the main) thread via HostDisplay_SubmitFrame. Fixed-timestep
   * decoupling is preserved by the M6 accumulator (owns the emulated tick
   * rate) plus vsync (SDL_RenderPresent blocks briefly when vsync is on). */

  bool running = true;
  uint32 last_tick = SDL_GetTicks();  /* headless-only pacing (§3.6) */

  /* M6/§3.1,§3.3: fixed-timestep accumulator, non-headless only. */
  static const int kMaxCatchupFrames = 3;     /* spiral-of-death cap, §3.1 */
  uint64_t accumulator = 0;
  uint64_t last_time_ns = SDL_GetTicksNS();

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;
        /* Dragging the window to another monitor, or that monitor changing
         * mode, can change the refresh rate the Vsync row reports — and the
         * SAME monitor can re-mode under us (user flips 60->144Hz in OS
         * settings, Windows dynamic refresh re-modes on power state), which
         * arrives as a display-level event, not a window one. The R2 soft-
         * cap and the menu's UI pacing both derive from this value. The same
         * events change the window's PIXEL DENSITY (dragging between a Retina
         * and a 1x monitor), which the pinned HUD/menu scale percentages are
         * corrected by — so refresh both together. */
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_ADDED:
        case SDL_EVENT_DISPLAY_REMOVED:
          HostDisplay_UpdateProperties();
          g_paused_redraw_pending = true;
          break;
        /* The window is the USER's: a drag-resize re-derives the picture inside
         * it and nothing else. HostDisplay_RecomputeLogicalPresentation never
         * calls SDL_SetWindowSize — calling the window-sizing path here
         * snapped every manual resize straight back to window_scale and could
         * oscillate on a fractional-scale compositor, where SDL notes the
         * granted size "may not match the exact size requested". The render
         * resolution and aspect settings are still fully respected: they
         * letterbox inside whatever size the user chose.
         *
         * Phase 0 made rendering main-thread-only, so re-deriving the logical
         * presentation here no longer races the (removed) present thread. */
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          HostDisplay_RecomputeLogicalPresentation();
          g_paused_redraw_pending = true;
          break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
          g_window_hidden = true;
          break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
          g_window_hidden = false;
          g_paused_redraw_pending = true;
          break;
        /* GPU device/target reset: STATIC textures lose their contents and
         * must be recreated — both the HD replacements and the settings
         * overlay's atlases (fonts/icons/dialog frame, uploaded once at
         * Init). DEVICE_LOST is unrecoverable. */
        case SDL_EVENT_RENDER_TARGETS_RESET:
        case SDL_EVENT_RENDER_DEVICE_RESET:
          ReloadHdReplacementTextures();
          if (!SettingsOverlay_ReloadTextures(rom_data, rom_size))
            fprintf(stderr,
                    "[settings-menu] atlas reload after device reset failed\n");
          g_paused_redraw_pending = true;
          /* R17/C2: the retained re-present slot copies hd_entries[].texture
           * as raw SDL_Texture* (present.h). ReloadHdReplacementTextures just
           * destroyed and recreated every one of them, so those copies are
           * now dangling — re-compositing the retained slot would be a
           * use-after-free. Drop it; the next tick present retains a fresh
           * one. This also makes retained-frame upload skipping
           * safe: the textures it relies on are never stale-by-reset. */
          HostDisplay_InvalidatePresentHistory();
          break;
        case SDL_EVENT_RENDER_DEVICE_LOST:
          fprintf(stderr, "[render] device lost, cannot recover — exiting\n");
          running = false;
          break;
        case SDL_EVENT_KEY_DOWN:
          /* An armed binding row consumes the raw key: it needs the scancode,
           * and it must win over F5/F9/etc. so those stay bindable. */
          if (SettingsOverlay_HandleCaptureEvent(&event)) break;
          if (SettingsOverlay_IsOpen()) {
            /* Only the menu's active device drives navigation, so one
             * physical press (+ its synthesized twin) moves the menu once. */
            if (MenuKeyboardIsActiveDevice()) {
              bool was_open = true;
              bool consumed = SettingsOverlay_HandleKey(
                  event.key.key, true, event.key.repeat != 0);
              if (was_open && !SettingsOverlay_IsOpen()) ClearHeldInput();
              if (consumed) break;
            } else {
              /* Menu owns the screen but keyboard isn't its device: swallow
               * the key so it never reaches gameplay HandleInput. */
              break;
            }
          }
          /* The settings UI is host-owned and safe in every emulated state.
           * Escape/F1 are not SNES inputs, so consume them before HandleInput
           * and clear held joypad state before freezing game advancement. */
          if (!event.key.repeat &&
              (event.key.key == SDLK_ESCAPE ||
               event.key.key == SDLK_F1)) {
            ClearHeldInput();
            SettingsOverlay_Open();
          } else if (event.key.key == SDLK_P) {
            if (SceneInspector_HasSelection()) {
              bool inspector_owned_pause = g_scene_inspector_owns_pause;
              CloseSceneInspectorSelection();
              if (!inspector_owned_pause) TogglePause();
            } else {
              TogglePause();
            }
          } else if (event.key.key == SDLK_T) {
            ToggleTurbo();
          } else if (event.key.key == SDLK_F3) {
            if (!event.key.repeat) {
              const SettingDesc *inspector = Settings_Find("scene_inspector");
              SettingChangeResult result = Settings_SetLong(
                  inspector, !g_settings.scene_inspector);
              char settings_path[1024];
              UserDataFile(settings_path, sizeof settings_path, "settings.ini");
              if (result > kSettingChange_Unchanged &&
                  !Settings_Save(settings_path))
                fprintf(stderr,
                        "[scene-inspector] could not save settings.ini\n");
              fprintf(stderr, "[scene-inspector] %s (%s)\n",
                      g_settings.scene_inspector
                          ? "enabled — click the game to inspect"
                          : "disabled",
                      Settings_ChangeResultName(result));
            }
          } else if (event.key.key == SDLK_MINUS ||
                     event.key.key == SDLK_KP_MINUS) {
            if (!event.key.repeat) AdjustHudOutputScale(-25);
          } else if (event.key.key == SDLK_EQUALS ||
                     event.key.key == SDLK_PLUS ||
                     event.key.key == SDLK_KP_PLUS) {
            if (!event.key.repeat) AdjustHudOutputScale(25);
          } else if (event.key.key == SDLK_F5) {
            (void)OnSettingsAction(Settings_Find("save_state"));
          } else if (event.key.key == SDLK_F7) {
            (void)OnSettingsAction(Settings_Find("load_state"));
          } else if (event.key.key == SDLK_F9) {
            /* Cycle 4:3 -> widescreen RAW -> widescreen FULL, for capturing
             * before/after comparison shots without a settings UI. Requires
             * booting with ExtendedAspectRatio set: the wide framebuffer and
             * window are sized once at boot, so an authentic-booted run has no
             * margins to reveal and stays pinned to 4:3. Shift+F9 retains the
             * long-standing diagnostic dump command. Ignore key-repeat so one
             * physical press advances exactly one preset. */
            if (event.key.repeat) {
              /* no-op */
            } else if (event.key.mod & SDL_KMOD_SHIFT) {
              DumpDiagState("hotkey");
            } else if (!g_ws_active) {
              fprintf(stderr, "[display] F9 needs ExtendedAspectRatio "
                      "(e.g. 16:9) in config.ini; staying 4:3\n");
            } else {
              /* A1 (followup doc): Settings_CycleDisplayMode now routes
               * through Settings_SetLong, whose FinishChange fires
               * OnRuntimeSettingChanged — that observer already re-derives
               * the logical presentation and sets g_paused_redraw_pending for
               * kSettingCat_Display. Doing it again here would re-mutate the
               * renderer redundantly. */
              int m = Settings_CycleDisplayMode();
              fprintf(stderr, "[display] mode %d/%d -> %s\n", m + 1,
                      kDisplayMode_PresetCount, Settings_DisplayModeName(m));
            }
          } else if (event.key.key == SDLK_F6) {
            /* Level warp: stage the game's own sim->act transition to the raw
             * registry target seeded by AR_WARP=<region_hex><map_hex>. The low byte is $19,
             * not a uniform act number (e.g. Kasandora act 2 is 0303). Press
             * from a transition-capable state; see README + docs/SEAMS.md. */
            PerformWarp();
          } else if (event.key.key == SDLK_F2) {
            /* On-demand FULL snapshot — each press writes a unique set of files
             * tagged with the game-frame: WRAM + VRAM + CGRAM + OAM (via
             * ActRaiser_FullSnapshot) plus a .ppm screenshot. Lets several
             * moments be grabbed while driving the game manually so the
             * internals (esp. VRAM, where the bridge tiles live) can be watched
             * change over time alongside the picture. */
            /* If F9 and F2 were queued in the same paused host iteration,
             * render the new preset before capturing it. */
            TakeFullSnapshot();
          } else if (event.key.key == SDLK_D && !event.key.repeat) {
            if (event.key.mod & SDL_KMOD_SHIFT) {
              extern uint8 g_ram[0x20000];
              if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
                fprintf(stderr, "[diorama] layer dump requires an action stage "
                        "($18=%02x)\n", g_ram[kActRaiserWram_MapGroup]);
              } else {
                g_diorama_dump_pending = true;
                fprintf(stderr, "[diorama] layer capture armed for next frame\n");
              }
            } else {
              /* Route through the descriptor so the hotkey, the menu, and
               * settings.ini stay one path — the change callback does the
               * geometry rebind. */
              const SettingDesc *mode = Settings_Find("diorama_mode");
              if (mode && !Settings_IsAvailable(mode)) {
                fprintf(stderr, "[diorama] requires the new renderer\n");
              } else if (mode) {
                Settings_SetLong(mode, !g_settings.diorama_mode);
                fprintf(stderr, "[diorama] %s\n",
                        g_settings.diorama_mode ? "ON" : "OFF");
              }
            }
          } else if (g_settings.diorama_mode && !event.key.repeat &&
                     event.key.key >= SDLK_1 && event.key.key <= SDLK_5) {
            /* Layer visibility hotkeys, gated behind diorama so the digits
             * stay free otherwise. Order matches the on-screen back-to-front
             * stack: 1 backdrop, 2 BG2, 3 BG1, 4 sprites, 5 HUD. */
            static const char *const kLayerKeys[] = {
              "diorama_layer_backdrop", "diorama_layer_bg2",
              "diorama_layer_bg1", "diorama_layer_obj", "diorama_layer_bg3",
            };
            int index = event.key.key - SDLK_1;
            const SettingDesc *row = Settings_Find(kLayerKeys[index]);
            long value = 0;
            if (row && Settings_GetLong(row, &value)) {
              Settings_SetLong(row, !value);
              fprintf(stderr, "[diorama] %s %s\n", row->label,
                      value ? "hidden" : "shown");
              g_paused_redraw_pending = true;
            }
          } else {
            HandleInput(event.key.scancode, true);
          }
          break;
        case SDL_EVENT_TEXT_INPUT:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleText(event.text.text);
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          /* Diorama owns right-drag (orbit) and middle-click (reset) while it
           * is on screen; §8.7 disables click-inspect in diorama for v1
           * because the flat hit-testing does not follow the tilted planes. */
          if (!SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame()) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              Diorama_SetDragging(true);
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              Diorama_ResetCamera();
          } else if (!SettingsOverlay_IsOpen() &&
                     Sim3D_FreeCameraActiveThisFrame()) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              g_sim3d_camera_dragging = true;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              Sim3D_ResetCamera();
          } else if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
              CloseSceneInspectorSelection();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
              /* SDL3 mouse event coordinates are floats; the hit-testing works
               * at SNES-pixel granularity, so truncating to int is exact. */
              int event_x = (int)event.button.x;
              int event_y = (int)event.button.y;
              int output_x = 0, output_y = 0;
              if (!WindowPointToOutput(event_x, event_y,
                                       &output_x, &output_y) ||
                  !SettingsOverlay_BeginDebugPanelDrag(
                      output_x, output_y))
                (void)InspectWindowPoint(event_x, event_y);
            }
          }
          break;
        case SDL_EVENT_MOUSE_MOTION:
          if (Diorama_IsDragging() && Diorama_IsActiveThisFrame()) {
            Diorama_AdjustCamera(event.motion.xrel * Diorama_DragRadPerPx(),
                                 event.motion.yrel * Diorama_DragRadPerPx(),
                                 0.0f);
          } else if (g_sim3d_camera_dragging &&
                     Sim3D_FreeCameraActiveThisFrame()) {
            Sim3D_AdjustCamera(event.motion.xrel * Diorama_DragRadPerPx(),
                               event.motion.yrel * Diorama_DragRadPerPx(),
                               0.0f);
          } else if (SettingsOverlay_IsDebugPanelDragging()) {
            int output_x = 0, output_y = 0;
            if (WindowPointToOutput((int)event.motion.x, (int)event.motion.y,
                                    &output_x, &output_y))
              SettingsOverlay_DragDebugPanel(output_x, output_y);
          }
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          /* Wheel up zooms in, i.e. decreases the camera distance. */
          if (!SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame())
            Diorama_AdjustCamera(0.0f, 0.0f,
                                 -event.wheel.y * Diorama_ZoomStep());
          else if (!SettingsOverlay_IsOpen() &&
                   Sim3D_FreeCameraActiveThisFrame())
            Sim3D_AdjustCamera(0.0f, 0.0f,
                               -event.wheel.y * Diorama_ZoomStep());
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (event.button.button == SDL_BUTTON_RIGHT) {
            Diorama_SetDragging(false);
            g_sim3d_camera_dragging = false;
          }
          if (event.button.button == SDL_BUTTON_LEFT)
            SettingsOverlay_EndDebugPanelDrag();
          break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          /* An armed binding row consumes the raw pad event regardless of the
           * active menu device — mirrors the keyboard capture at the KEY_DOWN
           * case above, so a gamepad-bind row can be captured even when the
           * menu's active device is the keyboard (input_device=Keyboard). */
          if (SettingsOverlay_IsOpen() &&
              SettingsOverlay_HandleCaptureEvent(&event))
            break;
          /* While the menu owns the screen the pad drives menu NAVIGATION, not
           * the game — but only when the pad is a menu-active device. */
          if (SettingsOverlay_IsOpen()) {
            if (MenuGamepadIsActiveDevice())
              (void)SettingsOverlay_HandleGamepadEvent(&event);
            break;
          }
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_KEY_UP:
          if (SettingsOverlay_IsOpen()) {
            if (MenuKeyboardIsActiveDevice())
              (void)SettingsOverlay_HandleKey(event.key.key, false, false);
          } else {
            HandleInput(event.key.scancode, false);
          }
          break;
      }
    }

    if (g_host_lifecycle_request != kHostLifecycle_None) {
      running = false;
      continue;
    }

    ApplyAnalogCameraInput();

    /* Host-owned pauses do not issue the game's native SPC $F2 command. Keep
     * the HD decoder aligned explicitly; its independent driver-pause latch
     * still prevents resume until both pause reasons have cleared. */
    MusicReplacements_SetHostPaused(
        g_paused || SettingsOverlay_IsOpen());

    if (g_paused || SettingsOverlay_IsOpen()) {
      /* §3.4: don't accumulate wall-clock time spent paused — otherwise
       * unpausing would fire a burst of catch-up ticks. last_time_ns is
       * re-stamped every paused iteration below, so it's always "just now"
       * by the time the game actually unpauses. */
      accumulator = 0;
      /* R17/C2: a pause can last minutes, and a settings change applied during
       * it (ScheduledSettings_ApplyIfDue runs below, on the first unpaused
       * iteration, BEFORE any tick can fire) re-derives geometry. Drop the
       * retained slot so the first iteration after unpausing cannot
       * re-present a pre-pause frame at pre-change geometry. */
      HostDisplay_InvalidatePresentHistory();
      /* Advance hold-to-accelerate value stepping. Wall-clock scheduled, so
       * it is correct at whatever rate this loop runs; a value it changes
       * sets g_paused_redraw_pending, which the redraw below honors. */
      SettingsOverlay_Tick();
      /* Re-render the emulated frame only when something changed (a settings
       * edit, a resize); it is not re-rendered per iteration. */
      RedrawPausedFrameIfNeeded();
      /* §2.5: re-present unconditionally to keep the window alive while
       * paused — the removed present thread used to do this from its own idle
       * timeout. The present is paced by HostDisplay_SubmitFrame's host-UI
       * interval (menu: panel rate; plain pause: idle keep-alive rate).
       * game_tick=false (R16): no tick ran, so this must not feed M7's scroll
       * history or its tick-span average. */
      bool presented = false;
      if (!headless && !g_window_hidden) {
        const HostDisplayPresentMode present_mode =
            SettingsOverlay_IsOpen()
                ? kHostDisplayPresent_Menu
                : kHostDisplayPresent_Paused;
        presented = HostDisplay_SubmitFrame(
            present_mode, kInterpPhaseNone);
      }
      /* Pacing comes from the present itself (vsync block, or the
       * display-refresh UI throttle in HostDisplay_SubmitFrame), so this loop
       * — and with it menu input polling and repaints — runs at the
       * panel's native rate, whatever it is. The fixed sleep remains only
       * as the anti-spin fallback when nothing presents (hidden window,
       * headless). */
      if (!presented) SDL_Delay(16);
      last_time_ns = SDL_GetTicksNS();
      continue;
    }

    ScheduledSettings_ApplyIfDue();

    if (headless) {
      /* §3.6: headless keeps the OLD model verbatim — uncapped by default,
       * exactly one tick per outer iteration, no present thread. The
       * oracle/replay tooling depends on this running as fast as the CPU
       * allows. */
      RunOneEmulatedTick(&running);
      RunOuterIterationHousekeeping();
      DrawAndPresentFrame(true, kInterpPhaseNone);

      extern int snes_frame_counter;
      static int quit_frames = -2;
      if (quit_frames == -2) { const char *q = getenv("AR_QUIT_FRAMES");
        quit_frames = q ? atoi(q) : -1; }
      if (quit_frames > 0 && snes_frame_counter >= quit_frames) running = false;
      /* AR_PACE=1: throttle headless to ~60fps so the emulated SPC (advanced
       * in real time by the audio thread) stays in sync with the game thread —
       * a faithful reproduction of normal play, vs. the default headless turbo
       * which runs the game thread uncapped and confounds APU-handshake timing. */
      static int pace = -2;
      if (pace == -2) pace = getenv("AR_PACE") ? 1 : 0;
      if (pace) {
        uint32 now = SDL_GetTicks();
        uint32 elapsed = now - last_tick;
        if (elapsed < 16) SDL_Delay(16 - elapsed);
        last_tick = SDL_GetTicks();
      }
      continue;
    }

    /* M6/§3.1: fixed-timestep accumulator (non-headless only). The game
     * thread is no longer paced by SDL_Delay(16) here —
     * HostDisplay_SubmitFrame owns the present wait, and this wall-clock
     * accumulator owns the emulated tick rate independently. */
    {
      uint64_t now_ns = SDL_GetTicksNS();
      uint64_t dt = now_ns - last_time_ns;
      last_time_ns = now_ns;
      accumulator += dt;
      /* Spiral-of-death cap (§3.1) — with Limit-aware headroom. Phase 0 put
       * the Refresh=Limit throttle sleep on THIS thread, so at Limit <~30fps
       * one deliberate present interval exceeds three emulation ticks and the
       * fixed cap would discard wall time EVERY iteration, permanently
       * slowing the game (~58.3Hz at Limit=25; exactly 60.000Hz at 20 — the
       * audio-drift rate the emulation interval protects). Allow one limit
       * interval + one tick so the intentional sleep always banks; genuine
       * hitches beyond that still clamp. Vsync/Unlimited intervals are far
       * below the base cap, so behavior there is unchanged. */
      const uint64_t catchup_cap_ns =
          HostDisplay_CatchupCapNs(kMaxCatchupFrames);
      if (accumulator > catchup_cap_ns) accumulator = catchup_cap_ns;

      bool produced_frame = false;
      while (accumulator >= kHostDisplayEmulationFrameIntervalNs) {
        RunOneEmulatedTick(&running);
        accumulator -= kHostDisplayEmulationFrameIntervalNs;
        produced_frame = true;
      }

      /* R17/C4: the sub-tick phase, taken AFTER the drain — whatever wall-clock
       * time has accrued toward the next tick but has not yet produced one.
       * This is the quantity present-time interpolation used to reconstruct
       * from its own clock divided by an EMA of the tick period; here it is
       * exact, and it cannot be corrupted by presents because presents do not
       * write the accumulator. The drain loop's own exit condition guarantees
       * the range. */
      SDL_assert(accumulator < kHostDisplayEmulationFrameIntervalNs);
      const float alpha =
          (float)accumulator /
          (float)kHostDisplayEmulationFrameIntervalNs;

      RunOuterIterationHousekeeping();

      /* R17/C5: the render rate is now independent of the tick rate.
       *
       * Presents used to fire ONLY when the drain produced a tick, which pinned
       * the present rate at (or below) 60.0988Hz no matter what the display or
       * the user's Refresh-rate setting said, and left every wall-clock-driven
       * present-side animation sampled at exactly the tick rate. For scroll
       * interpolation that was fatal rather than merely coarse: with one present
       * per capture, the phase was always ~0, so the feature could not do
       * anything at all.
       *
       * Between ticks the game state has not changed, so there is nothing to
       * re-capture — but the PHASE has advanced, so re-compositing the retained
       * slot at the new phase is real new output. Pacing still comes solely from
       * the shared deadline, so the Refresh-rate setting keeps meaning exactly
       * what it says.
       *
       * Gated on the pair being interpolable, using the SAME predicate the math
       * uses (C4), and on LIVE diorama/setting state rather than the retained
       * slot's copy — the slot is one tick stale by construction. Without the
       * gate a re-present would spend a full composite to draw a
       * byte-identical image. */
      bool presented = false;
      if (!g_window_hidden) {
        if (produced_frame) {
          DrawAndPresentFrame(false, alpha);
          presented = true;
        } else if (HostDisplay_TryRepresentFrame(
                       alpha,
                       g_diorama_frame_active,
                       g_settings.gpu_interp_enabled,
                       g_paused_redraw_pending)) {
          presented = true;
        }
      }
      /* INVARIANT: every iteration either presents (which blocks on vsync or on
       * a guaranteed-nonzero throttle interval) or yields. One unconditional
       * line, not a property of the branch structure above — four independent
       * reviewers found this exact hole in an earlier draft where the sleep was
       * attached to the hidden-window arm, and "the code happens to fall through
       * to a sleep" is precisely the kind of structural invariant this codebase
       * has now lost five times. The no-present/no-sleep counter must stay 0. */
      HostDisplay_YieldIfNoPresent(
          presented, g_window_hidden, produced_frame);
    }
  }

  /* D10's present-thread join is gone with the thread itself (#18/P13):
   * nothing can be mid-render here, so teardown below (SettingsOverlay_Destroy
   * + the DestroyTexture block + SDL_DestroyRenderer) is safe in source order.
   *
   * Flush only game-originated battery changes on exit. Deliberate
   * session-only editor changes re-sync the save-system shadow, so using
   * Restart/Exit after one must not turn it into a persistent edit. Skip the
   * flush during replay so a replayed run never mutates the active save (see
   * the auto-persist note above — it would break the next replay's alignment). */
  if (!InputReplay_ShouldProtectSaveData()) {
    SaveError error = {{0}};
    if (!SaveSystem_AutoPersistIfChanged(&error))
      fprintf(stderr, "[saves] shutdown flush failed: %s\n", error.message);
  }
  DumpDiagState(g_host_lifecycle_request == kHostLifecycle_Restart
                    ? "restart" : "exit");
  SimPhase0Trace_Close();
  SimRenderMetadata_TraceClose();

  /* Before tearing down audio: the census reads only its own accumulators,
   * but the report should land while the run dir is still current. */
  SfxCensus_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  HostAudio_Shutdown();
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (g_hd_replacements[i].texture)
      SDL_DestroyTexture((SDL_Texture *)g_hd_replacements[i].texture);
    free(g_hd_replacements[i].pixels);
  }
  SDL_DestroyTexture(g_m7_texture);
  SDL_DestroyTexture(g_sim_obj_atlas_texture);
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    SDL_DestroyTexture(g_sim3d_layer_textures[plane]);
  SDL_DestroyTexture(g_sim3d_flat_texture);
  SettingsOverlay_Destroy();
  /* Release the game coroutine's stack mapping / fiber. Safe here: the game
   * thread is this thread and the main loop has exited, so nothing can be
   * running on that stack. */
  ActRaiser_DestroyGameCoroutine();
  InputMap_Shutdown();
  SDL_DestroyTexture(g_hud_obj_texture);
  SDL_DestroyTexture(g_hud_bg_texture);
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  free(rom_data);

  if (g_host_lifecycle_request == kHostLifecycle_Restart) {
    fprintf(stderr, "[lifecycle] restarting process\n");
#ifdef _WIN32
    _execvp(argv[0], (const char *const *)argv);
#else
    execvp(argv[0], argv);
#endif
    fprintf(stderr, "[lifecycle] restart failed: %s\n", strerror(errno));
    return 1;
  }
  return 0;
}
