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
#include <sys/stat.h>
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
#include "framedump.h"
#include "widescreen.h"
#include "present.h"
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
/* Not static: present.c (M5, D6) reads these presentation resources
 * directly. They are boot-created once and, after that, either read-only
 * pointers or exclusively touched under the M5.3 present-thread handshake —
 * not part of the g_ppu/g_settings race class D6 fences off. */
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
static uint8 g_paused, g_turbo;
static bool g_scene_inspector_owns_pause;
/* InspectorPresentationKind/InspectorPresentationSelection now live in
 * present.h (D4) — shared between this file's InspectWindowPoint (live
 * hit-test) and present.c's renderer (fed from the FrameSlot snapshot). */
static InspectorPresentationSelection g_scene_inspector_presentation;
static bool g_paused_redraw_pending;
static uint32 g_input_state;
typedef enum {
  kHostLifecycle_None,
  kHostLifecycle_Restart,
  kHostLifecycle_Exit,
} HostLifecycleRequest;
static HostLifecycleRequest g_host_lifecycle_request;
static int g_snes_width = 256, g_snes_height = 224;
/* Audio-format settings retain boot snapshots because reopening the live SDL
 * device is still restart-class. Video geometry is rebound live below. */
static int g_active_aspect_x, g_active_aspect_y;
static int g_active_pixel_aspect = kPixelAspect_Crt43;
static int g_active_audio_frequency = 44100;
static int g_active_audio_samples = 2048;
static bool g_widescreen_runtime_allowed;
/* Framebuffer sized for the PPU's full widescreen budget (448 wide) so the
 * active width can change live without reallocating storage; each frame uses
 * only the leading g_snes_width*4 bytes per row. */
uint8_t g_pixels[kPpuBufWidth * 4 * 240];
uint8_t g_hud_bg_pixels[kPpuBufWidth * 4 * 240];
uint8_t g_hud_obj_pixels[kPpuBufWidth * 4 * 240];
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

static SDL_Mutex *g_audio_mutex;
/* The audio callback runs on SDL's audio thread, while settings mutate on the
 * main/game thread. Keep the callback's one live input in an SDL atomic mirror
 * instead of racing on g_settings. */
static SDL_AtomicInt g_audio_master_percent;
/* SDL3 binds output to an SDL_AudioStream; keep the stream (and derived device
 * id) so pause/resume/close can address the opened device. */
static SDL_AudioStream *g_audio_stream;
static bool g_audio_open;
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

/* Diagnostic state dump (hotkey Shift+F9, and automatically on exit). Writes the
 * live execution state to saves/ so a frozen/buggy moment can be handed off
 * for analysis without re-navigating menus: full WRAM + battery SRAM as raw
 * binaries, plus a human-readable summary (CPU regs, frame, current function,
 * recomp call stack, and a few documented game-state bytes). */
void DumpDiagState(const char *tag) {
  extern uint8 g_ram[0x20000];
  extern uint8 *g_sram; extern int g_sram_size;
  extern CpuState g_cpu;
  extern int snes_frame_counter;
  extern const char *g_last_recomp_func;
  extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
#ifndef _WIN32
  mkdir("saves", 0755);
#endif
  /* Hotkey dumps get frame-unique filenames so a mid-bug Shift+F9 snapshot
   * isn't clobbered by the automatic exit dump (or by another). Exit keeps the
   * fixed names existing tooling expects. */
  int hotkey = tag && strcmp(tag, "hotkey") == 0;
  char p_wram[320], p_sram[320], p_state[320], p_disp[320];
  if (hotkey) {
    RunDirFile(p_wram, sizeof p_wram, "dump_f%d_wram.bin", snes_frame_counter);
    RunDirFile(p_sram, sizeof p_sram, "dump_f%d_sram.bin", snes_frame_counter);
    RunDirFile(p_state, sizeof p_state, "dump_f%d_state.txt", snes_frame_counter);
    RunDirFile(p_disp, sizeof p_disp, "dump_f%d_dispatch_log.json", snes_frame_counter);
  } else {
    RunDirFile(p_wram, sizeof p_wram, "dump_wram.bin");
    RunDirFile(p_sram, sizeof p_sram, "dump_sram.bin");
    RunDirFile(p_state, sizeof p_state, "dump_state.txt");
    RunDirFile(p_disp, sizeof p_disp, "dump_dispatch_log.json");
  }
  FILE *f = fopen(p_wram, "wb");
  if (f) { fwrite(g_ram, 1, 0x20000, f); fclose(f); }
  if (g_sram && g_sram_size > 0) {
    f = fopen(p_sram, "wb");
    if (f) { fwrite(g_sram, 1, (size_t)g_sram_size, f); fclose(f); }
  }
  f = fopen(p_state, "w");
  if (f) {
    fprintf(f, "=== ActRaiser recomp state dump (%s) ===\n", tag ? tag : "");
    fprintf(f, "frame=%d  last_func=%s\n", snes_frame_counter,
            g_last_recomp_func ? g_last_recomp_func : "(none)");
    fprintf(f, "A=%04x X=%04x Y=%04x S=%04x D=%04x DB=%02x PB=%02x P=%02x"
            " m=%d x=%d\n", g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.D,
            g_cpu.DB, g_cpu.PB, g_cpu.P, g_cpu.m_flag, g_cpu.x_flag);
    fprintf(f, "recomp call stack (innermost first), depth=%d:\n",
            g_recomp_stack_top);
    for (int i = g_recomp_stack_top - 1; i >= 0; i--)
      fprintf(f, "  [%d] %s\n", g_recomp_stack_top - 1 - i,
              g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    /* A few documented WRAM bytes (see docs/ram-map.md). */
    fprintf(f, "game-state: $18=%02x $19=%02x  town-level $0291=%04x\n",
            g_ram[0x18], g_ram[0x19], g_ram[0x0291] | (g_ram[0x0292] << 8));
    /* Recent executed-block PCs (oldest-first) — reveals an infinite
     * loop's block cycle when the watchdog trips. */
    {
      extern int ar_block_history3(uint32_t *, uint32_t *, uint16_t *, int);
      uint32_t hist[256], aux[256]; uint16_t srec[256];
      int n = ar_block_history3(hist, aux, srec, 256);
      fprintf(f, "block history (last %d, oldest-first) pc m x S X  "
                 "(watch S drift across a call to find the unbalanced subroutine):\n", n);
      for (int i = 0; i < n; i++)
        fprintf(f, "  %06X m=%u x=%u S=%04X X=%04X\n", hist[i],
                (aux[i] >> 16) & 1, (aux[i] >> 17) & 1, srec[i], aux[i] & 0xFFFF);
    }
    fclose(f);
  }
  {
    /* Post-mortem dispatch ring: last DISPATCH_LOG_CAP runtime dispatches
     * (pc24, source, func, m/x, found/miss, frame) feeding into the exit/crash.
     * The offline equivalent of the TCP `dispatch_log_get` command. */
    extern void CpuDispatchLogWriteFile(const char *path);
    CpuDispatchLogWriteFile(p_disp);
  }
  fprintf(stderr, "[dump] wrote %s + wram/sram/dispatch_log (%s)\n",
          p_state, tag ? tag : "");
}

/* Game-thread id for AR_APUPROF lock-wait attribution (set in main before
 * the game loop; 0 = not yet known, wait timing disabled). */
static SDL_ThreadID g_game_thread_id;

void RtlApuLock(void) {
  if (!g_audio_mutex) return;
  extern int ApuProfEnabled(void);
  if (ApuProfEnabled()) {
    extern uint64_t g_apuprof_lockwait_ns, g_apuprof_audiowait_max_ns;
    extern uint64_t audio_trace_wall_ns(void);
    /* SDL3 SDL_TryLockMutex returns true when the lock was acquired. */
    if (SDL_TryLockMutex(g_audio_mutex)) return;
    uint64_t t0 = audio_trace_wall_ns();
    SDL_LockMutex(g_audio_mutex);
    uint64_t waited = audio_trace_wall_ns() - t0;
    if (g_game_thread_id != 0 && SDL_GetCurrentThreadID() == g_game_thread_id)
      g_apuprof_lockwait_ns += waited;
    else if (waited > g_apuprof_audiowait_max_ns)
      g_apuprof_audiowait_max_ns = waited;
    return;
  }
  SDL_LockMutex(g_audio_mutex);
}

void RtlApuUnlock(void) {
  if (g_audio_mutex) SDL_UnlockMutex(g_audio_mutex);
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

/* SDL3 audio-stream callback: fires when the bound device needs more data.
 * `additional_amount` is the number of BYTES the stream wants right now, in
 * the stream's input format (16-bit stereo interleaved = 4 bytes per sample
 * frame — the same shape RtlRenderAudio produced for the SDL2 callback).
 *
 * We render into a fixed on-stack scratch buffer in bounded chunks and push
 * each to the stream. A fixed buffer (rather than alloca(additional_amount))
 * keeps the audio-thread stack safe no matter how large a request SDL makes —
 * SDL may ask for the whole device buffer at once, and alloca cannot fail
 * gracefully. The APU lock is taken once around the whole request so the
 * batch renders atomically with respect to the game thread. */
static void SDLCALL AudioCallback(void *userdata, SDL_AudioStream *stream,
                                  int additional_amount, int total_amount) {
  (void)userdata;
  (void)total_amount;
  if (additional_amount <= 0) return;
  /* 2048 stereo 16-bit frames per chunk; loop for larger requests. */
  enum { kChunkBytes = 2048 * 4 };
  Uint8 chunk[kChunkBytes];

  /* AR_APUPROF: time this acquire — it is the audio thread's outer lock
   * (RtlRenderAudio's internal RtlApuLock calls are recursive re-entries and
   * can never block), so any starvation of the callback shows up here. SDL3
   * mutex locks never fail (void return), so no lock-failure early-out. */
  extern int ApuProfEnabled(void);
  if (ApuProfEnabled()) {
    extern uint64_t g_apuprof_audiowait_max_ns;
    extern uint64_t audio_trace_wall_ns(void);
    uint64_t t0 = audio_trace_wall_ns();
    SDL_LockMutex(g_audio_mutex);
    uint64_t waited = audio_trace_wall_ns() - t0;
    if (waited > g_apuprof_audiowait_max_ns)
      g_apuprof_audiowait_max_ns = waited;
  } else {
    SDL_LockMutex(g_audio_mutex);
  }
  int volume = SDL_GetAtomicInt(&g_audio_master_percent);
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  int remaining = additional_amount;
  while (remaining > 0) {
    int bytes = remaining < kChunkBytes ? remaining : kChunkBytes;
    RtlRenderAudio((int16 *)chunk, bytes / 4, 2);
    if (volume != 100) {
      int16 *samples = (int16 *)chunk;
      const int sample_count = bytes / (int)sizeof(*samples);
      for (int i = 0; i < sample_count; i++)
        samples[i] = (int16)(((int32)samples[i] * volume) / 100);
    }
    SDL_PutAudioStreamData(stream, chunk, bytes);
    remaining -= bytes;
  }
  SDL_UnlockMutex(g_audio_mutex);
}

static bool OpenHostAudio(void) {
  if (g_audio_open) return true;
  SDL_AudioSpec want = {0};
  want.freq = g_active_audio_frequency > 0
      ? g_active_audio_frequency : 44100;
  want.format = SDL_AUDIO_S16;
  want.channels = 2;
  /* SDL3's AudioSpec no longer carries a buffer size; the device sample-frame
   * count is controlled by a hint. Preserve the configured buffer depth. */
  if (g_active_audio_samples > 0) {
    char frames[16];
    snprintf(frames, sizeof(frames), "%d", g_active_audio_samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
  }
  g_audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, AudioCallback, NULL);
  if (!g_audio_stream) {
    fprintf(stderr, "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
    return false;
  }
  /* `want` is the STREAM INPUT format — the rate our callback produces at.
   * SDL resamples that to the device's native rate internally, so the
   * emulator's output rate must be want.freq, NOT the device rate. (This
   * differs from SDL2, where SDL_OpenAudio's callback ran at the device rate
   * `have.freq`; feeding the device rate here would mis-time RtlRenderAudio
   * and detune all audio whenever the device rate differs from want.freq.) */
  RtlSetAudioOutputRate(want.freq);
  /* The device's actual rate/buffer is informational only (SDL resamples). */
  SDL_AudioSpec device = want;
  int device_frames = 0;
  SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio_stream),
                           &device, &device_frames);
  fprintf(stderr, "[audio] stream input %d Hz, device %d Hz %d-frame buffer "
                  "(requested %d frames)\n",
          want.freq, device.freq, device_frames, g_active_audio_samples);
  g_audio_open = true;
  return true;
}

/* SDL3 opens the device paused; playback needs an explicit resume. */
static void ApplyAudioEnabled(void) {
  if (g_settings.audio_enabled) {
    if (OpenHostAudio()) SDL_ResumeAudioStreamDevice(g_audio_stream);
  } else if (g_audio_open) {
    SDL_PauseAudioStreamDevice(g_audio_stream);
  }
}

static void RtlDrawPpuFrame(void) {
  g_rtl_game_info->draw_ppu_frame();
}

/* Runtime presentation settings can change while game execution is paused.
 * Re-render the same emulated PPU state once after such a change; ordinary
 * paused iterations retain that texture, so pause never advances the game or
 * repeatedly replays the scanline/HDMA renderer. */
/* M5.3 (ar-recomp-threading-impl.md §2, Appendix D5-D11): the present
 * thread. Moves SDL_RenderPresent's vsync block off the game thread.
 *
 * Two condition variables, reused for every handshake purpose (D11 — no new
 * subsystem, each waiter loops on its own predicate so sharing is safe):
 *   g_present_ready_cond: game -> present. Signaled when a new frame is
 *     submitted (g_frame_pending) or a quiesce is requested/released.
 *   g_present_done_cond:  present -> game. Signaled on THREE occasions:
 *     (1) a submitted frame is dequeued (clears g_frame_pending — the D11
 *         submit-side wait), (2) PresentUpload finishes for the dequeued
 *         frame (sets g_present_upload_done — the pixel-buffer-safety wait,
 *         see present.h's buffer-ownership note: this is what lets the game
 *         thread redraw g_pixels/etc. WITHOUT double-buffering them), and
 *         (3) a quiesce request is acknowledged (g_present_quiesced).
 *
 * Buffer ownership (M5 plan): the FrameSlot metadata is double-buffered
 * (g_frame_slots[2]); the raw pixel buffers are NOT — safety for those comes
 * from (2) above, not copying. */
static SDL_Thread *g_present_thread;
static SDL_Mutex *g_present_mutex;
static SDL_Condition *g_present_ready_cond;
static SDL_Condition *g_present_done_cond;
static bool g_present_running;
static bool g_present_thread_active;  /* false: headless / no renderer — synchronous fallback */

static FrameSlot g_frame_slots[2];
static int g_frame_last_idx;       /* game-thread-only: alternates 0/1 */
static int g_frame_pending_idx;    /* valid while g_frame_pending */
static bool g_frame_pending;
static bool g_present_upload_done = true;
static int g_present_last_presented_idx = -1;  /* present-thread-only */

static bool g_present_quiesce_requested;
static bool g_present_quiesced;

/* D8: quiesce, not a command queue. Parks the present thread after it
 * finishes whatever it's mid-doing, so the caller (game/main thread) can
 * safely run code that mutates the renderer/window/g_ppu wholesale
 * (geometry changes, fullscreen toggle, savestate load, screenshot capture)
 * without racing the present thread's reads. */
static void PresentThread_Quiesce(void) {
  if (!g_present_thread_active) return;
  SDL_LockMutex(g_present_mutex);
  g_present_quiesce_requested = true;
  SDL_SignalCondition(g_present_ready_cond);
  while (!g_present_quiesced && g_present_running)
    SDL_WaitCondition(g_present_done_cond, g_present_mutex);
  SDL_UnlockMutex(g_present_mutex);
}

static void PresentThread_Resume(void) {
  if (!g_present_thread_active) return;
  SDL_LockMutex(g_present_mutex);
  g_present_quiesce_requested = false;
  SDL_SignalCondition(g_present_ready_cond);
  SDL_UnlockMutex(g_present_mutex);
}

static uint64_t FrameLimitIntervalNs(void);  /* defined with the display code */

/* Pace the present thread to the Frame limit (Refresh rate = Limit). Called
 * with no lock held, immediately before SDL_RenderPresent; sleeps out any time
 * left in the target interval since the previous present. A no-op in Vsync
 * (SDL blocks) and Unlimited (interval 0) modes. */
static void PresentThrottle(uint64_t *last_present_ns) {
  uint64_t interval = FrameLimitIntervalNs();
  uint64_t now = SDL_GetTicksNS();
  if (interval && *last_present_ns && now - *last_present_ns < interval) {
    SDL_DelayNS(interval - (now - *last_present_ns));
    now = SDL_GetTicksNS();
  }
  *last_present_ns = now;
}

static int SDLCALL PresentThreadFn(void *userdata) {
  (void)userdata;
  uint64_t last_present_ns = 0;
  /* M7: present-thread-local scroll history for interpolation (present.h's
   * FrameSlot comment explains why this must NOT be a pointer into
   * g_frame_slots[] — that would race the game thread's next submission).
   * Only this thread ever touches it, so it needs no lock. */
  DioramaScrollSnapshot prev_scroll = {0};
  SDL_LockMutex(g_present_mutex);
  while (g_present_running) {
    if (g_present_quiesce_requested) {
      g_present_quiesced = true;
      SDL_SignalCondition(g_present_done_cond);
      while (g_present_quiesce_requested && g_present_running)
        SDL_WaitCondition(g_present_ready_cond, g_present_mutex);
      g_present_quiesced = false;
      /* M7: a quiesce (settings change, diorama toggle, savestate load) can
       * span an arbitrary wall-clock gap and/or a scene change. prev_scroll
       * still holds whatever was captured before the quiesce — invalidate
       * it so the first frame after resuming shows curr as-is (no
       * interpolation) instead of computing a bogus jump delta against
       * stale/unrelated pre-quiesce scroll data. */
      memset(&prev_scroll, 0, sizeof(prev_scroll));
      continue;
    }
    if (!g_frame_pending) {
      /* D11/§2.5, extended for M7: timed wait. On timeout (no new frame
       * submitted), re-composite + re-present the last slot — §2.5's "keep
       * the window alive while paused" path. With the "Scroll interpolation"
       * setting on (kSettingCat_Graphics, off by default — see the
       * PresentComposite comment on the BG2/HDMA vibration bug), poll at
       * ~4ms so this also becomes the steady-state redraw path on a >60Hz
       * display, recomputing the interpolation alpha fresh each time.
       * B1a (followup doc): "Uncapped framerate" also wants this ~4ms
       * cadence — without vsync (see the SDL_SetRenderVSync read at boot),
       * SDL_RenderPresent no longer blocks the loop until the display's next
       * refresh, so polling at the old 16ms idle cadence would just throttle
       * the very re-present rate this setting exists to unlock. Without
       * either setting there is no benefit to redrawing identical content
       * faster than the original 16ms idle cadence, so stick with that.
       * Reading g_settings directly is fine here (this is main.c, not
       * present.c — no D6 boundary). */
      bool signaled = SDL_WaitConditionTimeout(
          g_present_ready_cond, g_present_mutex,
          (g_settings.gpu_interp_enabled ||
           g_settings.refresh_mode != kRefreshMode_Vsync)
              ? 4 : 16);
      if (!g_present_running || g_present_quiesce_requested) continue;
      if (!signaled && !g_frame_pending && g_present_last_presented_idx >= 0) {
        int idx = g_present_last_presented_idx;
        SDL_UnlockMutex(g_present_mutex);
        /* Re-present the SAME slot with a freshly-computed alpha — curr
         * hasn't changed, so prev_scroll must NOT be touched here. Updating
         * it after every idle repaint (as an earlier version of this code
         * did) collapses prev==curr the moment a tick goes idle, snapping
         * the extrapolated shift back to zero for one repaint and then
         * re-extrapolating on the next — a visible forward/back "vibration"
         * on every idle repaint. prev_scroll only ever advances in the
         * dequeue path below, when a genuinely new tick was captured. */
        PresentComposite(&g_frame_slots[idx], &prev_scroll);
        PresentThrottle(&last_present_ns);
        if (g_renderer) SDL_RenderPresent(g_renderer);
        SDL_LockMutex(g_present_mutex);
      }
      continue;
    }

    int idx = g_frame_pending_idx;
    g_frame_pending = false;
    SDL_SignalCondition(g_present_done_cond);  /* D11: unblocks the submit wait */
    SDL_UnlockMutex(g_present_mutex);

    FrameSlot *slot = &g_frame_slots[idx];
    static int perf_on = -1;
    if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;
    uint32 upload_t0 = perf_on ? SDL_GetTicks() : 0;
    PresentUpload(slot);

    SDL_LockMutex(g_present_mutex);
    g_present_upload_done = true;
    SDL_SignalCondition(g_present_done_cond);  /* unblocks the pre-draw wait */
    SDL_UnlockMutex(g_present_mutex);

    uint32 composite_t0 = perf_on ? SDL_GetTicks() : 0;
    PresentComposite(slot, &prev_scroll);
    uint32 vsync_t0 = perf_on ? SDL_GetTicks() : 0;
    PresentThrottle(&last_present_ns);
    if (g_renderer) SDL_RenderPresent(g_renderer);
    FrameSlot_ExtractScrollSnapshot(slot, &prev_scroll);
    if (perf_on) {
      uint32 now = SDL_GetTicks();
      uint32 upload_ms = composite_t0 - upload_t0;
      uint32 present_ms = vsync_t0 - composite_t0;
      uint32 vsync_ms = now - vsync_t0;
      static uint32 win_start, up_sum, up_max, pr_sum, pr_max, vs_sum, vs_max;
      static int win_frames;
      up_sum += upload_ms; if (upload_ms > up_max) up_max = upload_ms;
      pr_sum += present_ms; if (present_ms > pr_max) pr_max = present_ms;
      vs_sum += vsync_ms; if (vsync_ms > vs_max) vs_max = vsync_ms;
      win_frames++;
      if (!win_start) win_start = now;
      if (now - win_start >= 1000) {
        fprintf(stderr,
                "[present-perf] frames=%d upload-ms avg=%.1f max=%u "
                "present-ms avg=%.1f max=%u vsync-wait avg=%.1f max=%u\n",
                win_frames, (double)up_sum / win_frames, up_max,
                (double)pr_sum / win_frames, pr_max,
                (double)vs_sum / win_frames, vs_max);
        win_start = now; up_sum = 0; up_max = 0;
        pr_sum = 0; pr_max = 0; vs_sum = 0; vs_max = 0; win_frames = 0;
      }
    }
    g_present_last_presented_idx = idx;

    SDL_LockMutex(g_present_mutex);
  }
  SDL_UnlockMutex(g_present_mutex);
  return 0;
}

/* Call before every RtlDrawPpuFrame(): blocks (briefly — PresentUpload is
 * sub-millisecond per [present-perf]) until the present thread has finished
 * reading the pixel buffers this call is about to overwrite. No-op with no
 * present thread (headless/synchronous fallback). */
static void WaitForPixelBuffersFree(void) {
  if (!g_present_thread_active) return;
  SDL_LockMutex(g_present_mutex);
  while (!g_present_upload_done && g_present_running)
    SDL_WaitCondition(g_present_done_cond, g_present_mutex);
  SDL_UnlockMutex(g_present_mutex);
}

/* Call after every RtlDrawPpuFrame(): hands the just-drawn frame to the
 * present thread (or, with none running, presents synchronously — the old
 * M5.2 path, kept for headless-with-video and as a safety fallback). */
static void SubmitFrameToPresent(void) {
  if (!g_present_thread_active) {
    if (!g_renderer || !g_texture) return;
    static int perf_on = -1;
    if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;
    FrameSlot slot;
    FrameSlot_Capture(&slot);
    uint32 render_t0 = perf_on ? SDL_GetTicks() : 0;
    PresentUpload(&slot);
    PresentComposite(&slot, NULL);
    uint32 vsync_t0 = perf_on ? SDL_GetTicks() : 0;
    SDL_RenderPresent(g_renderer);
    if (perf_on) {
      uint32 now = SDL_GetTicks();
      uint32 render_ms = vsync_t0 - render_t0;
      uint32 vsync_ms = now - vsync_t0;
      static uint32 win_start, render_sum, render_max, vsync_sum, vsync_max;
      static int win_frames;
      render_sum += render_ms; if (render_ms > render_max) render_max = render_ms;
      vsync_sum += vsync_ms; if (vsync_ms > vsync_max) vsync_max = vsync_ms;
      win_frames++;
      if (!win_start) win_start = now;
      if (now - win_start >= 1000) {
        fprintf(stderr,
                "[present-perf] frames=%d present-ms avg=%.1f max=%u "
                "vsync-wait avg=%.1f max=%u (no present thread)\n",
                win_frames, (double)render_sum / win_frames, render_max,
                (double)vsync_sum / win_frames, vsync_max);
        win_start = now; render_sum = 0; render_max = 0;
        vsync_sum = 0; vsync_max = 0; win_frames = 0;
      }
    }
    return;
  }

  SDL_LockMutex(g_present_mutex);
  while (g_frame_pending && g_present_running)
    SDL_WaitCondition(g_present_done_cond, g_present_mutex);  /* D11 */
  if (!g_present_running) { SDL_UnlockMutex(g_present_mutex); return; }
  int idx = 1 - g_frame_last_idx;
  FrameSlot_Capture(&g_frame_slots[idx]);
  g_frame_last_idx = idx;
  g_frame_pending_idx = idx;
  g_frame_pending = true;
  g_present_upload_done = false;
  SDL_SignalCondition(g_present_ready_cond);
  SDL_UnlockMutex(g_present_mutex);
}

/* Returns true if a redraw actually happened (so the caller knows whether to
 * submit a fresh frame or let the present thread's own idle timeout keep
 * re-presenting the last one — §2.5). */
static bool RedrawPausedFrameIfNeeded(void) {
  if ((g_paused || SettingsOverlay_IsOpen()) && g_paused_redraw_pending) {
    WaitForPixelBuffersFree();
    RtlDrawPpuFrame();
    g_paused_redraw_pending = false;
    return true;
  }
  return false;
}

/* Differential-oracle capture: emit per-frame WRAM changes as JSONL in the
 * exact shape snesref (tools/oracle) emits, so the two traces can be diffed
 * to find the first divergence (frame + WRAM byte). Enabled by AR_WRAM_TRACE
 * (output path); range via AR_TRACE_LO/HI (default full 128KB). */
static FILE *g_wram_trace;
static uint8_t g_wram_prev[0x20000];
static bool g_wram_primed;
static uint32_t g_wram_lo = 0x00000, g_wram_hi = 0x1ffff;

static void WramTraceCallback(uint32_t frame, const uint8_t *wram) {
  /* AR_DUMP_ACT=1: each action-stage frame ($18==01), overwrite a full 128KB
   * WRAM snapshot to saves/recomp_act1.bin. After the walk-off crash the file
   * holds the LAST pre-crash action frame's object table — for oracle diffing
   * without needing to know the exact crash frame number. */
  static int dump_act = -1;
  if (dump_act == -1) dump_act = getenv("AR_DUMP_ACT") ? 1 : 0;
  if (dump_act && wram[0x18] == 0x01) {
    static int first_done;
    char pth[320];
    if (!first_done) { first_done = 1;
      RunDirFile(pth, sizeof pth, "recomp_act1_first.bin");
      FILE *ff = fopen(pth, "wb");
      if (ff) { fwrite(wram, 1, 0x20000, ff); fclose(ff);
        fprintf(stderr, "[dump-act] FIRST action frame %u -> recomp_act1_first.bin\n", frame); } }
    RunDirFile(pth, sizeof pth, "recomp_act1.bin");
    FILE *df = fopen(pth, "wb");
    if (df) { fwrite(wram, 1, 0x20000, df); fclose(df); }
  }
  /* AR_DUMP_AT_GF=N: dump full WRAM exactly when game-frame $0088==N, to
   * saves/recomp_at.bin — for frame-exact recomp-vs-oracle diffing. */
  static long dump_at_gf = -2;
  if (dump_at_gf == -2) { const char *e = getenv("AR_DUMP_AT_GF"); dump_at_gf = e ? atol(e) : -1; }
  if (dump_at_gf >= 0) {
    unsigned gf = (unsigned)wram[0x88] | ((unsigned)wram[0x89] << 8);
    if ((long)gf == dump_at_gf) {
      char pth[320]; RunDirFile(pth, sizeof pth, "recomp_at.bin");
      FILE *gf_f = fopen(pth, "wb");
      if (gf_f) { fwrite(wram, 1, 0x20000, gf_f); fclose(gf_f);
        fprintf(stderr, "[dump-at-gf] gf=%u -> recomp_at.bin\n", gf); } } }
  /* AR_VRAMDUMP_GF=g1,g2,...: headless FULL snapshot (WRAM+VRAM+CGRAM+OAM) at
   * each listed game-frame, from a single replay run. This is the recomp-internal
   * VRAM diff engine for the lair-seal corruption: capture a clean pre-seal frame
   * and the corrupt frame in ONE run, then diff the .vram.bin files — no
   * scene-divergence or scratch-noise confound (unlike the snes9x oracle).
   * Writes saves/snapshots/vd_gf<N>.{wram,vram,cgram,oam}.bin. */
  {
    static const char *vd_list = (const char *)-1;
    if (vd_list == (const char *)-1) vd_list = getenv("AR_VRAMDUMP_GF");
    if (vd_list && vd_list[0]) {
      unsigned gf = (unsigned)wram[0x88] | ((unsigned)wram[0x89] << 8);
      /* scan the comma list for a match; dump once per frame value */
      const char *p = vd_list;
      while (*p) {
        unsigned want = (unsigned)strtoul(p, NULL, 0);
        if (want == gf) {
          static unsigned last_dumped = 0xffffffffu;
          if (gf != last_dumped) {
            last_dumped = gf;
            extern void ActRaiser_FullSnapshot(const char *prefix);
            char pfx[320], snapdir[320];
            RunDirFile(snapdir, sizeof snapdir, "snapshots");
#ifndef _WIN32
            mkdir(snapdir, 0755);
#endif
            RunDirFile(pfx, sizeof pfx, "snapshots/vd_gf%u", gf);
            ActRaiser_FullSnapshot(pfx);
            { extern Ppu *g_ppu;
              if (g_ppu) fprintf(stderr, "[ppureg] gf=%u bgmode=$%02x bgsc=[%02x %02x %02x %02x] bgTileAdr=$%04x\n",
                gf, g_ppu->bgmode, g_ppu->bgXsc[0], g_ppu->bgXsc[1],
                g_ppu->bgXsc[2], g_ppu->bgXsc[3], g_ppu->bgTileAdr); }
            fprintf(stderr, "[vramdump] gf=%u -> %s.{wram,vram,cgram,oam}.bin\n",
                    gf, pfx);
          }
          break;
        }
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
      }
    }
  }
  /* AR_MX_OUT=<file>: per-game-frame CPU m/x capture for the snes9x CPU-flag
   * oracle (tools/oracle/diff_mx.py). Emits "gframe m x" from the SNES cpu state
   * at the frame-end yield, compared against snesref's SNESREF_MX_OUT (read from
   * snes9x's ICPU.Opcodes) to catch the first decode-time m/x divergence in the
   * boss->sim transition. Keyed on game-frame $0088 to align with the oracle. */
  {
    static FILE *mxf = NULL; static int mx_tried;
    if (!mx_tried) { mx_tried = 1; const char *p = getenv("AR_MX_OUT");
      if (p && p[0]) mxf = fopen(p, "w"); }
    if (mxf) {
      extern CpuState g_cpu;
      unsigned gf = (unsigned)wram[0x88] | ((unsigned)wram[0x89] << 8);
      /* "gframe m x g18 g1a" — $18/$1A let the differ auto-anchor on the
       * boss->sim transition independently in each run (no shared $0088). */
      fprintf(mxf, "%u %d %d %u %u\n", gf, g_cpu.m_flag & 1, g_cpu.x_flag & 1,
              wram[0x18], wram[0x1a]);
      if ((frame % 30) == 0) fflush(mxf);
    }
  }
  if (!g_wram_trace) return;
  if (!g_wram_primed) {
    memcpy(g_wram_prev + g_wram_lo, wram + g_wram_lo, g_wram_hi - g_wram_lo + 1);
    g_wram_primed = true;
    return;
  }
  for (uint32_t a = g_wram_lo; a <= g_wram_hi; a++) {
    if (wram[a] != g_wram_prev[a]) {
      fprintf(g_wram_trace, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
              frame, a, g_wram_prev[a], wram[a]);
      g_wram_prev[a] = wram[a];
    }
  }
  if ((frame % 30) == 0) fflush(g_wram_trace);
}

static void WramTraceInit(void) {
  const char *path = getenv("AR_WRAM_TRACE");
  /* AR_DUMP_ACT / AR_DUMP_AT_GF / AR_MX_OUT alone also need the callback. */
  if ((!path || !path[0]) &&
      (getenv("AR_DUMP_ACT") || getenv("AR_DUMP_AT_GF") || getenv("AR_MX_OUT") ||
       getenv("AR_VRAMDUMP_GF"))) {
    g_framedump_callback = WramTraceCallback;
    return;
  }
  if (!path || !path[0]) return;
  const char *v;
  if ((v = getenv("AR_TRACE_LO")) && v[0]) g_wram_lo = (uint32_t)strtoul(v, NULL, 0);
  if ((v = getenv("AR_TRACE_HI")) && v[0]) g_wram_hi = (uint32_t)strtoul(v, NULL, 0);
  if (g_wram_hi > 0x1ffff) g_wram_hi = 0x1ffff;
  g_wram_trace = fopen(path, "w");
  if (!g_wram_trace) { fprintf(stderr, "AR_WRAM_TRACE: cannot open %s\n", path); return; }
  g_framedump_callback = WramTraceCallback;
  fprintf(stderr, "[wram-trace] -> %s  range=[0x%05x,0x%05x]\n", path, g_wram_lo, g_wram_hi);
}

/* Phase-2 live-settings probe. AR_SETTING_SET=key=value applies one descriptor
 * mutation when the logical game frame reaches AR_SETTING_AT_GF (default 0).
 * It uses the exact API the overlay will call, making headless next-frame
 * enforcement tests possible without adding a temporary setting-specific
 * hotkey. Diagnostic control only; it is intentionally not a registry row. */
static void ApplyScheduledSettingChange(void) {
  static int initialized, pending;
  static unsigned target_gf;
  static char key[64], value[256];
  if (!initialized) {
    initialized = 1;
    const char *spec = getenv("AR_SETTING_SET");
    const char *at = getenv("AR_SETTING_AT_GF");
    target_gf = at && at[0] ? (unsigned)strtoul(at, NULL, 0) : 0;
    if (spec && spec[0]) {
      const char *equals = strchr(spec, '=');
      size_t key_len = equals ? (size_t)(equals - spec) : 0;
      if (equals && key_len > 0 && key_len < sizeof(key)) {
        memcpy(key, spec, key_len);
        key[key_len] = 0;
        snprintf(value, sizeof(value), "%s", equals + 1);
        pending = 1;
      } else {
        fprintf(stderr, "[settings] invalid AR_SETTING_SET='%s' (want key=value)\n",
                spec);
      }
    }
  }
  if (!pending) return;
  /* Power-on WRAM is intentionally filled with $55, including the game's
   * logical-frame word. Wait until at least one emulated frame has run so a
   * small target is not mistaken as already reached by the $5555 fill. */
  extern int snes_frame_counter;
  if (snes_frame_counter <= 0) return;
  extern uint8 g_ram[0x20000];
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
  if (gf < target_gf) return;
  pending = 0;
  const SettingDesc *desc = Settings_Find(key);
  SettingChangeResult result = desc
      ? Settings_SetText(desc, value)
      : kSettingChange_Rejected;
  fprintf(stderr, "[settings] gf=%u %s=%s -> %s%s\n", gf, key, value,
          Settings_ChangeResultName(result), desc ? "" : " (unknown key)");
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
  /* §2.9(c)/D8: this does a full render pass + SDL_RenderReadPixels on
   * whichever thread calls it (F2, AR_SHOT_*) — quiesce the present thread
   * first so the two don't touch g_renderer/g_texture/etc. concurrently. */
  PresentThread_Quiesce();
  FrameSlot ppm_slot;
  bool have_ppm_slot = false;
  SDL_Surface *argb = NULL;
  if (g_renderer && g_hud_bg_texture) {
    FrameSlot_Capture(&ppm_slot);
    PresentUpload(&ppm_slot);
    PresentComposite(&ppm_slot, NULL);
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
  PresentThread_Resume();
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
  extern uint8 g_ram[0x20000];
  extern void ActRaiser_FullSnapshot(const char *prefix);
  static int snap_n;
  char snapdir[320];
  RunDirFile(snapdir, sizeof snapdir, "snapshots");
#ifndef _WIN32
  mkdir(snapdir, 0755);
#endif
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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
  extern uint8 g_ram[0x20000];
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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
    /* §2.9(b)/D8: rewrites all of g_ppu (VRAM/CGRAM/OAM/regs) — quiesce so
     * the present thread never reads it mid-tear. */
    PresentThread_Quiesce();
    RtlSaveLoad(kSaveLoad_Load, 0);
    PresentThread_Resume();
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
    if (!Settings_Save("settings.ini")) {
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

/* Point the presentation at the current display mode's framebuffer sub-rect
 * and size the window to that mode's display aspect. Host textures retain
 * maximum capacity; g_snes_width selects the live PPU pitch, and 4:3 presents
 * only the authentic centre 256 columns. */
/* Windowed / borderless-desktop / exclusive fullscreen. SDL3 desktop
 * fullscreen is FullscreenMode == NULL; a non-NULL mode requests a real
 * (exclusive) video mode. */
static void ApplyWindowMode(void) {
  if (!g_window) return;
  switch (g_settings.window_mode) {
    case kWindowMode_Windowed:
      SDL_SetWindowFullscreen(g_window, false);
      break;
    case kWindowMode_Exclusive: {
      SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
      SDL_SetWindowFullscreenMode(g_window,
                                  SDL_GetDesktopDisplayMode(display));
      SDL_SetWindowFullscreen(g_window, true);
      break;
    }
    case kWindowMode_Borderless:
    default:
      SDL_SetWindowFullscreenMode(g_window, NULL);
      SDL_SetWindowFullscreen(g_window, true);
      break;
  }
}

/* Publish the window's current display refresh rate so the Refresh rate row
 * can show "Vsync NHz" instead of a bare "Vsync". Re-query whenever the window
 * changes display or the display's mode changes. */
static void UpdateHostRefreshHz(void) {
  if (!g_window) {
    Settings_SetHostRefreshHz(0);
    return;
  }
  SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
  if (!mode) mode = SDL_GetDesktopDisplayMode(display);
  Settings_SetHostRefreshHz(mode ? (int)(mode->refresh_rate + 0.5f) : 0);
}

/* Vsync tracks Refresh rate: on only in Vsync mode, off for Unlimited and
 * Limit (Limit paces the present thread by wall clock instead). */
static void ApplyRefreshVsync(void) {
  if (!g_renderer) return;
  SDL_SetRenderVSync(g_renderer,
                     g_settings.refresh_mode == kRefreshMode_Vsync ? 1 : 0);
}

/* Minimum nanoseconds between presents when Refresh rate is Limit; 0 means no
 * limit. Read by the present thread's pacing. */
static uint64_t FrameLimitIntervalNs(void) {
  if (g_settings.refresh_mode != kRefreshMode_Limit) return 0;
  int fps = g_settings.frame_limit_fps;
  if (fps < 1) fps = 1;
  return 1000000000ull / (uint64_t)fps;
}

static void ApplyDisplayPresentation(void) {
  if (!g_window || !g_renderer) return;
  int vis_w = Settings_VisibleWidth();
  int scale = g_settings.window_scale ? g_settings.window_scale : 3;
  int win_h = g_snes_height * scale;
  int win_w = vis_w * scale;

  if (g_settings.display_mode == kDisplayMode_43) {
    /* 256px at the 7:6 CRT PAR displays as 4:3. */
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      win_w = (win_h * 4 + 1) / 3;
  } else if (g_ws_active && g_active_pixel_aspect == kPixelAspect_Crt43) {
    win_w = (win_h * g_active_aspect_x + g_active_aspect_y / 2) /
            g_active_aspect_y;
  }

  /* Logical size encodes the pixel stretch so SDL letterboxes correctly if the
   * user resizes the window themselves. Mirrors the boot-time setup. LETTERBOX
   * reproduces SDL2 SDL_RenderSetLogicalSize's aspect-preserving behavior. */
  if (g_ws_active && !g_settings.ignore_aspect_ratio) {
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      SDL_SetRenderLogicalPresentation(g_renderer, vis_w * 7, g_snes_height * 6,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
    else
      SDL_SetRenderLogicalPresentation(g_renderer, vis_w, g_snes_height,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
  }
  SDL_SetWindowSize(g_window, win_w, win_h);
}

static void ResolveVideoGeometry(bool runtime_change) {
  g_active_aspect_x = Settings_ExtendedAspectX();
  g_active_aspect_y = Settings_ExtendedAspectY();
  g_active_pixel_aspect = g_settings.pixel_aspect;

  int extra = 0;
  if (g_widescreen_runtime_allowed &&
      g_active_aspect_x && g_active_aspect_y) {
    bool crt_par = g_active_pixel_aspect == kPixelAspect_Crt43;
    long num = 224L * g_active_aspect_x * (crt_par ? 6 : 7);
    long den = 7L * g_active_aspect_y;
    int internal_w = (int)((num + den - 1) / den);
    extra = internal_w > 256 ? (internal_w - 256 + 1) / 2 : 0;
    if (extra > kWsExtraMax) extra = kWsExtraMax;
  }

  g_ws_display_extra = extra;
  /* Diorama tilts reveal area beyond the displayed frame; render the full
   * margin so those columns carry real game content instead of black. The
   * display crop (Settings_VisibleWidth) stays at the aspect-derived width. */
  if (g_settings.diorama_mode && extra > 0)
    extra = kWsExtraMax;

  g_ws_extra = extra;
  g_ws_active = extra > 0;
  g_snes_width = 256 + 2 * extra;
  g_new_ppu = g_settings.new_renderer || g_ws_active;

  if (runtime_change) {
    /* 4:3 cannot retain a wide presentation profile. Returning to a wide
     * ratio selects the validated FULL policy rather than reviving stale
     * per-layer flags from the previous 4:3 interval. */
    Settings_SetDisplayMode(g_ws_active ? kDisplayMode_WideFull
                                        : kDisplayMode_43);
    memset(g_pixels, 0, sizeof(g_pixels));
    memset(g_hud_bg_pixels, 0, sizeof(g_hud_bg_pixels));
    memset(g_hud_obj_pixels, 0, sizeof(g_hud_obj_pixels));
    ActRaiser_RebindPpuOutputSurfaces();
    ApplyDisplayPresentation();
    g_paused_redraw_pending = true;
  }

  fprintf(stderr,
          "[video-geometry] %s %s -> %d extra columns/side "
          "(render width %d, %s PPU)\n",
          g_active_aspect_x
              ? (g_active_aspect_y == 9 ? "16:9" : "16:10")
              : "4:3",
          g_active_pixel_aspect == kPixelAspect_Crt43
              ? "4:3-PAR" : "square-PAR",
          g_ws_extra, g_snes_width,
          g_new_ppu ? "new" : "legacy");
}

static void OnRuntimeSettingChanged(const SettingDesc *desc,
                                    SettingChangeResult result) {
  (void)result;
  /* D8: several branches below mutate the renderer/window wholesale
   * (SDL_SetWindowFullscreen, ResolveVideoGeometry -> RebindPpuOutputSurfaces
   * + ApplyDisplayPresentation's SDL_SetRenderLogicalPresentation/
   * SDL_SetWindowSize — §2.9(a)). Quiesce the present thread for the whole
   * dispatch rather than surgically picking branches; it's cheap (settings
   * changes are rare, human-triggered) and simpler than re-deriving which
   * branches are render-safe on every future edit here. */
  PresentThread_Quiesce();
  if (desc->field == &g_settings.audio_master_volume)
    SDL_SetAtomicInt(&g_audio_master_percent, g_settings.audio_master_volume);
  if (desc->field == &g_settings.audio_enabled)
    ApplyAudioEnabled();
  if (desc->field == &g_settings.music_replacements)
    MusicReplacements_ApplySetting();
  if (desc->field == &g_settings.scene_inspector &&
      !g_settings.scene_inspector)
    CloseSceneInspectorSelection();
  if (desc->field == &g_settings.window_mode && g_window) {
    ApplyWindowMode();
    UpdateHostRefreshHz();  /* exclusive fullscreen can change the mode */
  }
  /* B1a (followup doc): live-apply without a restart — mirrors the boot-time
   * SDL_SetRenderVSync read near SDL_CreateRenderer. Refresh rate owns vsync
   * now; the frame-limit interval is polled per-present by the present thread,
   * so a Limit-FPS change needs no explicit apply here. */
  if ((desc->field == &g_settings.refresh_mode ||
       desc->field == &g_settings.uncapped_framerate) && g_renderer)
    ApplyRefreshVsync();
  if (desc->field == &g_settings.extended_aspect ||
      desc->field == &g_settings.pixel_aspect) {
    ResolveVideoGeometry(true);
    PresentThread_Resume();
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
   * presentation/window dimensions. */
  if (desc->field == &g_settings.display_mode ||
      desc->field == &g_settings.window_scale ||
      desc->field == &g_settings.ignore_aspect_ratio ||
      desc->category == kSettingCat_Widescreen)
    ApplyDisplayPresentation();
  if (desc->category == kSettingCat_Display ||
      desc->category == kSettingCat_Widescreen ||
      Settings_CategoryIsSim3D(desc->category))
    g_paused_redraw_pending = true;
  PresentThread_Resume();
}

static void ApplyRendererLogicalSize(void) {
  if (!g_renderer) return;
  if (g_ws_active && !g_settings.ignore_aspect_ratio) {
    int vis_w = Settings_VisibleWidth();
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      SDL_SetRenderLogicalPresentation(g_renderer, vis_w * 7, g_snes_height * 6,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
    else
      SDL_SetRenderLogicalPresentation(g_renderer, vis_w, g_snes_height,
                                       SDL_LOGICAL_PRESENTATION_LETTERBOX);
  } else {
    SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
  }
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
  in->hud_scale_percent = g_settings.hud_scale_percent;
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
 * the mode is armed (ResolveVideoGeometry), so the geometry must be
 * re-derived and the PPU surfaces rebound at the new pitch; the display crop
 * and window size are deliberately unaffected. */
void Diorama_OnModeChanged(void) {
  if (!g_settings.diorama_mode) g_diorama_frame_active = false;
  if (!g_ppu) return;   /* pre-boot settings load */
  /* D8: memsets the live pixel buffers and rebinds PPU output surfaces —
   * quiesce first (same hazard class as OnRuntimeSettingChanged). */
  PresentThread_Quiesce();
  ResolveVideoGeometry(false);
  memset(g_pixels, 0, sizeof(g_pixels));
  memset(g_hud_bg_pixels, 0, sizeof(g_hud_bg_pixels));
  memset(g_hud_obj_pixels, 0, sizeof(g_hud_obj_pixels));
  ActRaiser_RebindPpuOutputSurfaces();
  g_paused_redraw_pending = true;
  PresentThread_Resume();
}

/* B4-vellean (followup doc): self-calibrating velocity normalizer — REVISED
 * TWICE after live measurement (AR_DYNCAM_LOG captures, 2026-07-21):
 *   1st capture: the doc's literal "permanent running max, seeded 256" was
 *   dominated by a too-high floor (ordinary run/walk PlayerVelocityX never
 *   exceeded ~1-2 raw units — the 256 floor never got superseded, yaw lean
 *   read 0.004-0.008 all session) and by a single early outlier on Y (one
 *   big fall — likely the stage-entry drop-in, not a real jump — spiked the
 *   running max once; a monotonic max can only grow, so every ordinary jump
 *   afterward normalized against that outlier and read near-zero).
 *   2nd capture (after switching to a decaying PEAK follower, ~10s
 *   half-life): fixed X (yaw now reaches ±0.5 during normal running), but Y
 *   was STILL dead — one -1.000 spike at the very start (4 frames, matching
 *   the same drop-in event), then near-zero for the rest of a ~56s session
 *   with plenty of real jumps in it. A peak-follower is fundamentally the
 *   wrong shape for this: ONE frame can set the entire session's scale, no
 *   matter how fast it decays, if that one frame is a scripted event (a
 *   drop-in) rather than representative gameplay physics.
 * Fix: normalize against a recent-activity AVERAGE (exponential moving
 * average of |v|, ~0.8s time constant) instead of a peak, scaled by
 * kNormMultiple so "typical recent motion" reads as a fraction of full
 * lean and a burst clearly above that reads as more. A single-frame outlier
 * — however large — only nudges the average by kEmaAlpha of its excess, so
 * it can't singlehandedly desensitize anything; sustained real motion (a
 * multi-frame jump arc, continuous running) dominates the average the way
 * it should. This is the auto-gain-control shape (RMS/average-following),
 * not peak-following, and it's what "self-calibrates near real top speed"
 * actually needs when scripted one-off events share the same WRAM signal as
 * real gameplay motion. */
static float g_diorama_velx_avg = 4.0f;
static float g_diorama_vely_avg = 4.0f;

static float NormalizeReactiveVelocity(int16_t v, float *avg) {
  static const float kFloor = 4.0f;
  static const float kEmaAlpha = 0.02f;      /* ~0.8s time constant @ 60Hz */
  static const float kNormMultiple = 3.0f;   /* "full lean" = 3x recent avg */
  float av = fabsf((float)v);
  *avg += (av - *avg) * kEmaAlpha;
  float ref = *avg * kNormMultiple;
  if (ref < kFloor) ref = kFloor;
  float norm = (float)v / ref;
  if (norm > 1.0f) norm = 1.0f;
  if (norm < -1.0f) norm = -1.0f;
  return norm;
}

/* B4-kick (followup doc): rising-edge detection for the three event
 * triggers. Game-thread-only state (FrameSlot_Capture's exclusive caller) —
 * present.c only ever sees the resulting one-shot FrameSlot flags. */
static bool g_diorama_prev_boost;
static int16_t g_diorama_prev_vely;
static uint8_t g_diorama_prev_hp;

/* Sim-town reactive camera. Separate averages from the action-stage pair
 * above: the two modes measure different actors moving at different scales,
 * and sharing an accumulator would make every town entry re-calibrate against
 * whatever the last action stage was doing. */
static float g_sim_velx_avg = 4.0f;
static float g_sim_vely_avg = 4.0f;
static uint8_t g_sim_prev_hp;
static bool g_sim_prev_in_town;

/* Sim world-record planar velocities. The catalogue keeps every world record
 * on one flat map, so these are the whole of the angel's motion -- there is no
 * third axis to read and none is implied by the projection. */
enum {
  kSimRecordVelocityX = 0x1A,
  kSimRecordVelocityY = 0x1C,
};

/* The pose the projection is built from this frame.
 *
 * Free Cam's is the player-owned one the right-drag edits and the reset action
 * restores; Dynamic Cam has its own baseline that the reactive lean works
 * around. Resolved in one place because two Sim3DTuning sites read it, and a
 * camera that differed between them would be a genuinely confusing bug. */
typedef struct SimCameraPose { int pitch_mrad, yaw_mrad, distance_x100; } SimCameraPose;

static SimCameraPose Sim3D_ActivePose(void) {
  if (g_settings.sim3d_camera_mode == kSimCam_Dynamic)
    return (SimCameraPose){
      g_settings.sim3d_dyncam_baseline_tilt_x_mrad,
      g_settings.sim3d_dyncam_baseline_tilt_y_mrad,
      g_settings.sim3d_dyncam_baseline_distance_x100,
    };
  return (SimCameraPose){
    g_settings.sim3d_tilt_x_mrad,
    g_settings.sim3d_tilt_y_mrad,
    g_settings.sim3d_distance_x100,
  };
}

static void CaptureSimDynamicCamera(FrameSlot *dst, bool in_town) {
  dst->sim_camera_mode = g_settings.sim3d_camera_mode;
  dst->sim_dyncam_strength = g_settings.sim3d_reactive_strength;

  /* Outside a town there is no angel record to read: the memory holds
   * whatever the action stage left there. Reporting a neutral camera and
   * resetting the edge state means re-entering a town starts level instead of
   * inheriting a lean from a stale read. */
  if (!in_town) {
    dst->sim_dyncam_lean_yaw = 0.0f;
    dst->sim_dyncam_lean_pitch = 0.0f;
    dst->sim_dyncam_event_hit = false;
    g_sim_prev_in_town = false;
    return;
  }

  int16_t vel_x = (int16_t)ActRaiser_ReadWram16(
      kActRaiserWram_SimAngelRecord + kSimRecordVelocityX);
  int16_t vel_y = (int16_t)ActRaiser_ReadWram16(
      kActRaiserWram_SimAngelRecord + kSimRecordVelocityY);
  dst->sim_dyncam_lean_yaw = NormalizeReactiveVelocity(vel_x, &g_sim_velx_avg);
  dst->sim_dyncam_lean_pitch =
      NormalizeReactiveVelocity(vel_y, &g_sim_vely_avg);

  /* Damage taken, on the frame it applies. Same reasoning as the action
   * stage's revision: an HP decrease is the instant damage lands, whereas an
   * invulnerability flag is set later, once hit-stun begins.
   *
   * The first town frame only seeds the previous value. Entering a town with
   * less HP than the last one ended with is not a hit, and without this the
   * camera jolts on arrival. */
  uint8_t hp = g_ram[kActRaiserWram_AngelCurrentHp];
  dst->sim_dyncam_event_hit = g_sim_prev_in_town && hp < g_sim_prev_hp;
  g_sim_prev_hp = hp;
  g_sim_prev_in_town = true;
}

/* M5 (ar-recomp-threading-impl.md Appendix D5): the sole FrameSlot writer.
 * Reads live g_ppu, g_settings, g_snes_width/height,
 * g_scene_inspector_presentation, g_hd_replacements: legitimate here (this
 * runs on the game thread, immediately after RtlDrawPpuFrame() returns,
 * before the game thread touches any of this state again). present.c must
 * never do this; it only reads the FrameSlot this produces. */
void FrameSlot_Capture(FrameSlot *dst) {
  memset(dst, 0, sizeof(*dst));

  /* D2 publishes only the pitch-zero separated-composite capability, and
   * only after its same-frame CPU oracle found zero differing pixels. */
  SimRenderMetadata_CaptureFrame(
      &dst->sim, g_ram, g_settings.sim3d_mode,
      Settings_Sim3DRequestedFeatures(),
      g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
  int sim_margin_left = 0, sim_margin_right = 0;
  ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right);
  SimCameraPose sim_pose = Sim3D_ActivePose();
  Sim3D_AnnotateFrame(&dst->sim, &(Sim3DTuning){
      .pitch_mrad = sim_pose.pitch_mrad,
      .yaw_mrad = sim_pose.yaw_mrad,
      .distance_x100 = sim_pose.distance_x100,
      .height_scale_x100 = g_settings.sim3d_height_scale_x100,
      .shadow_opacity_pct = g_settings.sim3d_shadow_opacity_pct,
      .height_pop_pct = g_settings.sim3d_height_pop_pct,
      .light_azimuth_deg = g_settings.sim3d_light_azimuth_deg,
      .light_elevation_deg = g_settings.sim3d_light_elevation_deg,
      .shadow_softness_pct = g_settings.sim3d_shadow_softness_pct,
      .rim_strength_pct = g_settings.sim3d_rim_strength_pct,
      .underlay_haze_pct = g_settings.sim3d_underlay_haze_pct,
      .cloud_opacity_pct = g_settings.sim3d_cloud_opacity_pct,
      .cloud_falloff_px = g_settings.sim3d_cloud_falloff_px,
      .cloud_inset_px = g_settings.sim3d_cloud_inset_px,
      .cull_lead_px = g_settings.sim3d_cull_lead_px,
      .cull_haze_pct = g_settings.sim3d_cull_haze_pct,
      .cull_dim_pct = g_settings.sim3d_cull_dim_pct,
      .cull_haze_lead_px = g_settings.sim3d_cull_haze_lead_px,
      .cull_corner_px = g_settings.sim3d_cull_corner_px,
      .underlay_defocus_pct = g_settings.sim3d_underlay_defocus_pct,
      .cloud_altitude_px = g_settings.sim3d_cloud_altitude_px,
      .cloud_drift_pct = g_settings.sim3d_cloud_drift_pct,
      .cull_lift_inset = g_settings.sim3d_cull_lift_inset,
      .backdrop_strength_pct = g_settings.sim3d_backdrop_strength_pct,
      .backdrop_horizon_pct = g_settings.sim3d_backdrop_horizon_pct,
      .sprite_margin_left = sim_margin_left,
      .sprite_margin_right = sim_margin_right });
  /* Accumulation itself happens once a frame at the always-run site below;
   * this only publishes the current canvas state into the slot. */
  dst->sim.town_canvas_serial = SimTownCanvas_Serial();

  dst->snes_width = g_snes_width;
  dst->snes_height = g_snes_height;
  dst->display_mode = g_settings.display_mode;
  dst->pixel_aspect = g_active_pixel_aspect;
  dst->ws_active = g_ws_active;
  dst->ws_extra = g_ws_extra;
  dst->ignore_aspect_ratio = g_settings.ignore_aspect_ratio;
  dst->visible_x0 = Settings_VisibleX0();
  dst->visible_width = Settings_VisibleWidth();
  dst->hud_scale_percent = g_settings.hud_scale_percent;

  dst->diorama_active = g_diorama_frame_active;

  /* M7/§6.1: scroll snapshot for present-time interpolation. */
  dst->timestamp_ns = SDL_GetTicksNS();
  dst->turbo_active = g_turbo != 0;
  dst->interp_setting_enabled = g_settings.gpu_interp_enabled;
  dst->diorama_hud_flat = g_settings.diorama_hud_flat;
  /* B4-split (followup doc): both candidate camera poses, scaled the same
   * way Diorama_SeedCameraFromSettings does (g_diorama_cam and g_settings
   * are kept in lockstep by Diorama_AdjustCamera's write-back and
   * OnRuntimeSettingChanged's re-seed on menu edits, so reading straight
   * from g_settings here is equivalent to reading the live g_diorama_cam). */
  dst->diorama_camera_mode = g_settings.diorama_camera_mode;
  dst->diorama_free_pose = (DioramaCameraPose){
    (float)g_settings.diorama_tilt_x_mrad / 1000.0f,
    (float)g_settings.diorama_tilt_y_mrad / 1000.0f,
    (float)g_settings.diorama_distance_x100 / 100.0f,
  };
  dst->diorama_dyncam_baseline = (DioramaCameraPose){
    (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad / 1000.0f,
    (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad / 1000.0f,
    (float)g_settings.diorama_dyncam_baseline_distance_x100 / 100.0f,
  };
  dst->diorama_reactive_strength = g_settings.diorama_reactive_strength;
  /* B4-vellean (followup doc): same ReadWram16+cast pattern already used for
   * PlayerVelocityX/Y elsewhere (actraiser_rtl.c ~346-349). */
  int16_t vel_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_PlayerVelocityX);
  int16_t vel_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_PlayerVelocityY);
  dst->diorama_dyncam_lean_yaw =
      NormalizeReactiveVelocity(vel_x, &g_diorama_velx_avg);
  dst->diorama_dyncam_lean_pitch =
      NormalizeReactiveVelocity(vel_y, &g_diorama_vely_avg);

  /* B4-kick (followup doc): rising-edge event triggers. Hit was originally
   * the invuln-bit test AR_NO_KNOCKBACK already relies on elsewhere in this
   * file — REVISED (2026-07-21, live report + AR_FRAMELOG/AR_DYNCAM_LOG
   * correlation): that flag consistently lagged the real hit by ~10 game
   * frames (~167ms @ 60Hz) across 3 measured hits, apparently because the
   * game doesn't set it until after the knockback/hit-stun begins, not at
   * the instant damage applies. PlayerHp decreasing IS the instant damage
   * applies, so that's the trigger now — fires exactly on the hit frame,
   * no game-side lag to inherit. Landing has no documented WRAM flag, so
   * it's inferred from velocity: falling with |vely| clearly above the
   * recent-average scale, settling near zero in one tick — reuses
   * g_diorama_vely_avg (just updated above) instead of a guessed magic
   * threshold, same reasoning as B4-vellean's self-calibration. Boost is a
   * 0-to-nonzero read of the raw byte, matching how
   * PlayerInvulnerabilityTimer is read/pinned elsewhere (g_ram[...], not
   * ReadWram16 — it's a single byte). */
  uint8_t hp = g_ram[kActRaiserWram_PlayerHp];
  dst->diorama_dyncam_event_hit = hp < g_diorama_prev_hp;
  g_diorama_prev_hp = hp;

  bool was_falling =
      g_diorama_prev_vely > (int16_t)(g_diorama_vely_avg * 0.5f);
  bool now_settled = abs((int)vel_y) < (int)(g_diorama_vely_avg * 0.15f);
  dst->diorama_dyncam_event_land = was_falling && now_settled;
  g_diorama_prev_vely = vel_y;

  bool boost = g_ram[kActRaiserWram_PlayerBoost] != 0;
  dst->diorama_dyncam_event_boost = boost && !g_diorama_prev_boost;
  g_diorama_prev_boost = boost;

  CaptureSimDynamicCamera(
      dst, ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                      g_ram[kActRaiserWram_CurrentMap]));
  /* B1b (followup doc): the stable game-authored camera in WRAM, read
   * BEFORE HDMA touches the PPU scroll registers — see the long comment on
   * FrameSlot's timestamp_ns field (present.h) for why this replaced
   * g_ppu->hScroll[]/vScroll[]. g_ram is always valid (no g_ppu dependency,
   * unlike the PPU-register read this replaces). */
  dst->bg1_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  dst->bg1_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
  dst->bg2_camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraX);
  dst->bg2_camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraY);

  if (g_ppu) {
    dst->hud_split_height = g_ppu->wsHudSplitHeight;
    dst->hud_left_end = g_ppu->wsHudLeftEnd;
    dst->hud_right_start = g_ppu->wsHudRightStart;
    dst->hud_player_row_y = g_ppu->wsHudPlayerRowY;
    dst->hud_left_only_y = g_ppu->wsHudLeftOnlyY;
    dst->extra_left_right = g_ppu->extraLeftRight;
    dst->inidisp = g_ppu->inidisp;
    dst->bg_mode = (uint8_t)PPU_mode(g_ppu);

    _Static_assert(kFrameSlotOverlaySourceCount == kPpuOverlaySource_Count,
                   "FrameSlot overlay source count must match the PPU's");
    _Static_assert(kFrameSlotOverlay_Bg3 == kPpuOverlaySource_Bg3 &&
                   kFrameSlotOverlay_Obj == kPpuOverlaySource_Obj,
                   "present.h's mirrored overlay source order must match ppu.h");
    _Static_assert(kFrameSlotOverlayFlag_RemoveFromGame ==
                   kPpuOverlayFlag_RemoveFromGame,
                   "present.h's mirrored overlay flag must match ppu.h");
    for (int i = 0; i < kFrameSlotOverlaySourceCount; i++) {
      const PpuOverlayCapture *src = &g_ppu->overlayCaptures[i];
      FrameSlotOverlayCapture *d = &dst->overlay_captures[i];
      d->x0 = src->x0; d->x1 = src->x1;
      d->y0 = src->y0; d->y1 = src->y1;
      d->flags = src->flags;
      d->oamFirst = src->oamFirst; d->oamCount = src->oamCount;
    }

    /* Only needed when an OBJ overlay/HUD icon is active this frame (§2.8
     * cost note). */
    if (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount) {
      _Static_assert(sizeof(dst->oam) == sizeof(g_ppu->oam), "oam size (D18)");
      _Static_assert(sizeof(dst->high_oam) == sizeof(g_ppu->highOam),
                     "highOam size (D18)");
      memcpy(dst->oam, g_ppu->oam, sizeof(dst->oam));
      memcpy(dst->high_oam, g_ppu->highOam, sizeof(dst->high_oam));
      dst->oam_valid = true;
    }

    dst->m7_active = (g_ppu->m7Override.rgba != NULL);
  }

  dst->hd_entry_count = 0;
  for (int i = 0; i < g_hd_replacement_count && i < kHdMaxReplacements; i++) {
    const HdReplacement *e = &g_hd_replacements[i];
    FrameSlotHdEntry *d = &dst->hd_entries[dst->hd_entry_count++];
    d->active = e->active;
    d->source = e->source;
    d->brightness_mod = e->brightness_mod;
    d->texture = e->texture;
  }

  dst->scene_inspector_enabled = g_settings.scene_inspector;
  dst->inspector_selection = g_scene_inspector_presentation;
}

/* M5.2: still called synchronously (no present thread yet — that's M5.3).
 * FrameSlot_Capture + PresentUpload + PresentComposite replace the old
 * single RenderFramebuffer(); this is the checkpoint that proves the
 * present.c extraction is behavior-preserving before concurrency is added. */
static bool PathExists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* A shipped, self-contained bundle places the game binary beside its content
 * (a game-assets/ directory and/or a portable.txt marker). An in-tree
 * developer build lives under build/ with the content at the repo root, so
 * neither marker sits next to the executable. Presence of a marker is the
 * signal to anchor the working directory to the executable's own location. */
static bool RunningAsBundle(void) {
  static const char *const markers[] = {"game-assets", "portable.txt"};
  char probe[1024];
  for (size_t i = 0; i < sizeof markers / sizeof markers[0]; i++)
    if (snesrecomp_exe_dir_path(markers[i], probe, sizeof probe) &&
        PathExists(probe))
      return true;
  return false;
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
  uint32 inputs = g_input_state;
  {
    /* TEMP DEBUG: force a button after N frames to auto-advance the
     * intro without a real keypress, for headless crash repro. */
    static const char *force_env;
    static int force_after = -2;
    static unsigned force_mask = 0;
    static int pulse_frames[16]; static int n_pulses = -1;
    if (force_after == -2) { force_env = getenv("AR_FORCE_INPUT_AFTER");
      force_after = force_env ? atoi(force_env) : -1;
      const char *m = getenv("AR_FORCE_INPUT_MASK");
      force_mask = m ? (unsigned)strtoul(m, NULL, 0) : 0x0001; /* default B */ }
    if (n_pulses == -1) { n_pulses = 0;
      /* AR_FORCE_PULSES="150,210,...": press force_mask for 4 frames as an
       * EDGE (press+release) starting at each listed frame, to drive menus
       * that need distinct button presses (e.g. B skip-swirl, then B select). */
      const char *p = getenv("AR_FORCE_PULSES");
      if (p) for (; *p && n_pulses < 16; ) {
        pulse_frames[n_pulses++] = atoi(p);
        while (*p && *p != ',') p++; if (*p == ',') p++; }
    }
    extern int snes_frame_counter;
    if (force_after >= 0 && snes_frame_counter >= force_after) inputs |= force_mask;
    for (int i = 0; i < n_pulses; i++)
      if (snes_frame_counter >= pulse_frames[i] && snes_frame_counter < pulse_frames[i] + 4)
        inputs |= force_mask;
  }
  /* Differential-oracle input record/replay, keyed by the GAME's logical
   * frame counter $7E:0088 (g_ram[0x88], 16-bit) instead of the host frame.
   * The game-frame advances identically in the recomp and the snes9x oracle
   * for identical input, so a recording made here replays frame-exact in the
   * oracle regardless of how many host frames each spent booting (the old
   * host-frame + offset scheme never aligned because boot timing differs).
   * SNES 12-bit button layout == libretro JOYPAD id order, so one file drives
   * both. File format: repeating 8-byte LE records {uint32 gframe; uint32 inputs}.
   * AR_INPUT_RECORD=path: append one record per host frame.
   * AR_INPUT_REPLAY=path: override `inputs` with the value recorded for the
   * current game-frame (last writer wins when a game-frame repeats). */
  {
    extern uint8 g_ram[];
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    static FILE *rec; static int rec_init;
    static uint32 *rep; static long rep_max = -1; static int rep_init;
    /* End-of-replay marker: the game-frame of the LAST record (the frame the
     * user closed the window / pressed ESC on). The recording itself stores
     * no stop event, so we auto-quit when replay reaches this frame — without
     * it, replay runs off the end of the recording with empty live input.
     * rep_started gates the check past the boot frame (gf == 0x5555 fill
     * before $0088 is initialised inflates the value space). */
    static long rep_last_gf = -1; static int rep_started = 0;
    if (!rep_init) { rep_init = 1; const char *p = getenv("AR_INPUT_REPLAY");
      if (p && p[0]) { FILE *f = fopen(p, "rb");
        if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
          long nrec = n / 8; uint32 *raw = (uint32 *)malloc((size_t)nrec * 8);
          if (raw && fread(raw, 8, (size_t)nrec, f) == (size_t)nrec) {
            for (long i = 0; i < nrec; i++) if ((long)raw[i*2] > rep_max) rep_max = (long)raw[i*2];
            if (nrec > 0) rep_last_gf = (long)raw[(nrec - 1) * 2];
            if (rep_max >= 0) { rep = (uint32 *)calloc((size_t)rep_max + 1, 4);
              if (rep) for (long i = 0; i < nrec; i++) rep[raw[i*2]] = raw[i*2+1]; } }
          free(raw); fclose(f);
          fprintf(stderr, "[input-replay] %ld records, max gf=%ld last gf=%ld from %s\n",
                  nrec, rep_max, rep_last_gf, p); } } }
    if (rep && (long)gf <= rep_max) inputs = rep[gf];
    /* Auto-stop at the end of the recording. AR_REPLAY_NOSTOP=1 disables
     * this so replay runs PAST the last recorded game-frame (holding the
     * last recorded input). Needed to reproduce an in-frame infinite spin:
     * such a freeze never advances $0088, so it is never recorded — the
     * recording ends one frame *before* the hang, and the auto-stop would
     * quit right before the freezing frame executes. */
    static int nostop = -1;
    if (nostop < 0) nostop = getenv("AR_REPLAY_NOSTOP") ? 1 : 0;
    if (rep && rep_last_gf >= 0 && !nostop) {
      if ((long)gf <= rep_last_gf) rep_started = 1;
      if (rep_started && (long)gf >= rep_last_gf) {
        fprintf(stderr, "[input-replay] reached end of recording at gf=%u — stopping\n", gf);
        *stop_running = false;
      }
    }
    if (rep && nostop && (long)gf > rep_max) inputs = rep_last_gf >= 0 ? rep[rep_last_gf] : 0;
    if (!rec_init) { rec_init = 1; const char *p = getenv("AR_INPUT_RECORD");
      if (p && p[0]) { rec = fopen(p, "wb"); fprintf(stderr, "[input-record] -> %s\n", p); } }
    if (rec) { uint32 v[2] = { (uint32)gf, (uint32)inputs }; fwrite(v, 4, 2, rec); fflush(rec); }
    /* AR_GFLOG=1: log (host_frame, gf) every N host frames to compare
     * $0088 advance rate vs the oracle. */
    if (getenv("AR_GFLOG")) {
      extern int snes_frame_counter;
      if ((snes_frame_counter % 100) == 0)
        fprintf(stderr, "[gflog] host=%d gf=%u\n", snes_frame_counter, gf);
    }
    /* Report the first action-region entry game-frame. */
    { static int seen_act = 0;
      if (!seen_act && g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
        seen_act = 1;
        fprintf(stderr, "[act-enter] $18=%02X $19=%02X at game-frame %u\n",
                g_ram[0x18], g_ram[0x19], gf); } }
  }
  return inputs;
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
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      fprintf(stderr, "[perf] fps=%d run-ms avg=%.1f max=%u gf+=%u "
              "apu-catchup calls=%llu cyc=%llu $18=%02x\n",
              win_frames, (double)run_ms_sum / win_frames, run_ms_max,
              (unsigned)(uint16)(gf - last_gf),
              (unsigned long long)(cc - last_cu_calls),
              (unsigned long long)(cy - last_cu_cycles), g_ram[0x18]);
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
 * produced_frame). */
static void DrawAndPresentFrame(bool headless) {
  extern uint8 g_ram[];
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;

  WaitForPixelBuffersFree();
  uint32 perf_draw_t0 = perf_on ? SDL_GetTicks() : 0;
  RtlDrawPpuFrame();
  {
    extern int snes_frame_counter;
    SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram, g_ppu);
    SimFrameData sim;
    SimRenderMetadata_CaptureFrame(
        &sim, g_ram, g_settings.sim3d_mode,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    int sim_margin_left = 0, sim_margin_right = 0;
    ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right);
    SimCameraPose sim_pose = Sim3D_ActivePose();
    Sim3D_AnnotateFrame(&sim, &(Sim3DTuning){
        .pitch_mrad = sim_pose.pitch_mrad,
        .yaw_mrad = sim_pose.yaw_mrad,
        .distance_x100 = sim_pose.distance_x100,
        .height_scale_x100 = g_settings.sim3d_height_scale_x100,
        .shadow_opacity_pct = g_settings.sim3d_shadow_opacity_pct,
        .height_pop_pct = g_settings.sim3d_height_pop_pct,
        .light_azimuth_deg = g_settings.sim3d_light_azimuth_deg,
        .light_elevation_deg = g_settings.sim3d_light_elevation_deg,
        .shadow_softness_pct = g_settings.sim3d_shadow_softness_pct,
        .rim_strength_pct = g_settings.sim3d_rim_strength_pct,
        .underlay_haze_pct = g_settings.sim3d_underlay_haze_pct,
      .cloud_opacity_pct = g_settings.sim3d_cloud_opacity_pct,
      .cloud_falloff_px = g_settings.sim3d_cloud_falloff_px,
      .cloud_inset_px = g_settings.sim3d_cloud_inset_px,
      .cull_lead_px = g_settings.sim3d_cull_lead_px,
      .cull_haze_pct = g_settings.sim3d_cull_haze_pct,
      .cull_dim_pct = g_settings.sim3d_cull_dim_pct,
      .cull_haze_lead_px = g_settings.sim3d_cull_haze_lead_px,
      .cull_corner_px = g_settings.sim3d_cull_corner_px,
      .underlay_defocus_pct = g_settings.sim3d_underlay_defocus_pct,
      .cloud_altitude_px = g_settings.sim3d_cloud_altitude_px,
      .cloud_drift_pct = g_settings.sim3d_cloud_drift_pct,
      .cull_lift_inset = g_settings.sim3d_cull_lift_inset,
      .backdrop_strength_pct = g_settings.sim3d_backdrop_strength_pct,
      .backdrop_horizon_pct = g_settings.sim3d_backdrop_horizon_pct,
      .sprite_margin_left = sim_margin_left,
      .sprite_margin_right = sim_margin_right });
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
              draw_ms_max, g_ram[0x18], g_ram[0x19]);
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
  { unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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

  if (!headless) SubmitFrameToPresent();
}

/* Per-outer-iteration host-side housekeeping (§3.5): polls / one-shot
 * triggers that are not coupled to the emulated tick rate. Runs once per
 * outer iteration regardless of how many ticks the accumulator fired. */
static void RunOuterIterationHousekeeping(void) {
  extern uint8 g_ram[];
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
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      /* $0088 is $5555-filled before the game initialises it; ignore that
       * boot sentinel or every target fires on frame 0. */
      if (gf != 0x5555 && gf >= (unsigned)diorama_at) {
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
    if (!Settings_Save("settings.ini"))
      fprintf(stderr, "[sim3d] failed to persist camera settings\n");
  }

  /* Auto-persist battery SRAM the moment the game writes a save, so progress
   * survives a freeze/force-quit (the clean-exit save-system write never runs
   * if the game hangs). Cheap: only writes when the 8KB SRAM actually changes.
   * SKIPPED during input replay: a replay is keyed on the game-frame counter
   * from a fixed boot state, so letting the replayed run overwrite save.srm
   * mid-playthrough would change the boot state for the NEXT replay and break
   * the frame alignment (the recording then no longer reaches the same spot). */
  if (!getenv("AR_INPUT_REPLAY")) {
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
  if (RunningAsBundle()) {
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
  g_widescreen_runtime_allowed = !headless || ws_headless;
  /* Resolve application and game settings before allocating presentation
   * resources. Known config.ini values were staged by ParseConfigFile;
   * settings.ini overrides them, and real environment variables win last. */
  const char *settings_path = getenv("AR_SETTINGS_PATH");
  if (!settings_path || !settings_path[0]) settings_path = "settings.ini";
  Settings_InitWithFile(settings_path);
  g_active_audio_frequency = Settings_AudioFrequencyHz();
  g_active_audio_samples = g_settings.audio_samples;
  ResolveVideoGeometry(false);

  /* Display presets depend on whether the resolved aspect selected a wide
   * budget. Finalize only after g_ws_active/g_ws_extra are authoritative. */
  Settings_FinalizeDisplayMode();
  SDL_SetAtomicInt(&g_audio_master_percent, g_settings.audio_master_volume);

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

  SDL_InitFlags sdl_flags = SDL_INIT_AUDIO;
  if (video) sdl_flags |= SDL_INIT_VIDEO;
  if (!headless) sdl_flags |= SDL_INIT_GAMEPAD;
  /* SDL3 returns true on success (the SDL2 0-on-success convention flipped). */
  if (!SDL_Init(sdl_flags)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (video) {
    int scale = g_settings.window_scale ? g_settings.window_scale : 3;
    /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
     * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
     * the height), so derive the window from the target ratio, not the
     * framebuffer. Faithful mode keeps the historical width*scale.
     *
     * Must use the DISPLAY crop (Settings_VisibleWidth), not g_snes_width:
     * diorama mode inflates the render width to the full kWsExtraMax margin
     * (ResolveVideoGeometry) while the displayed width stays aspect-derived,
     * so g_snes_width here booted the window wider than the configured screen
     * ratio. Mirrors ApplyDisplayPresentation, which is what fixes it up on
     * any later runtime aspect change. */
    int vis_w = Settings_VisibleWidth();
    int win_w = vis_w * scale;
    if (g_settings.display_mode == kDisplayMode_43) {
      /* 256px at the 7:6 CRT PAR displays as 4:3. */
      if (g_active_pixel_aspect == kPixelAspect_Crt43)
        win_w = (g_snes_height * scale * 4 + 1) / 3;
    } else if (g_ws_active && g_active_pixel_aspect == kPixelAspect_Crt43) {
      win_w = (g_snes_height * scale * g_active_aspect_x +
               g_active_aspect_y / 2) / g_active_aspect_y;
    }
    /* SDL3 merged FULLSCREEN_DESKTOP into FULLSCREEN (borderless desktop is
     * the default fullscreen mode when no exclusive video mode is set).
     * Exclusive fullscreen's video mode is set after window creation by
     * ApplyWindowMode; at boot the flag just requests fullscreen. */
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE |
        (headless_video ? SDL_WINDOW_HIDDEN : 0) |
        (g_settings.window_mode != kWindowMode_Windowed
             ? SDL_WINDOW_FULLSCREEN : 0);
    /* SDL3 SDL_CreateWindow no longer takes an x,y position; it is created at
     * a default (centered) position. */
    g_window = SDL_CreateWindow(
      kWindowTitle,
      win_w, g_snes_height * scale,
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
      ApplyRefreshVsync();

    /* Exclusive fullscreen needs its video mode set after creation; borderless
     * and windowed are already handled by the creation flag. */
    if (!headless_video && g_settings.window_mode == kWindowMode_Exclusive)
      ApplyWindowMode();
    UpdateHostRefreshHz();

    /* Aspect-correct letterboxing via SDL's logical presentation (widescreen
     * only, so faithful mode keeps the historical stretch-to-window behavior).
     * 4:3-PAR: logical w:h = (render_w*7):(224*6) encodes the 7:6 pixel
     * stretch; square: the raw framebuffer dimensions. IgnoreAspectRatio=1
     * restores plain stretching. LETTERBOX matches SDL2's logical-size look. */
    if (g_ws_active && !g_settings.ignore_aspect_ratio) {
      if (g_active_pixel_aspect == kPixelAspect_Crt43)
        SDL_SetRenderLogicalPresentation(g_renderer, vis_w * 7,
            g_snes_height * 6, SDL_LOGICAL_PRESENTATION_LETTERBOX);
      else
        SDL_SetRenderLogicalPresentation(g_renderer, vis_w,
            g_snes_height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }

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
    extern uint8 g_ram[0x20000];
    const char *fenv = getenv("AR_WRAM_FILL");
    int fill = fenv ? (int)strtoul(fenv, NULL, 0) : 0x55;
    memset(g_ram, fill, 0x20000);
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

  /* Load persisted battery save (overrides the fresh-cart fill if present). */
  mkdir("saves", 0755);
  RtlMigrateLegacySram(kActRaiserGameInfo.title);
  {
    extern uint8 *g_sram; extern int g_sram_size;
    SaveError error = {{0}};
    const char *native_path = getenv("AR_SAVE_NATIVE_PATH");
    const char *ini_path = getenv("AR_SAVE_INI_PATH");
    if (!native_path || !native_path[0]) native_path = "saves/save.srm";
    if (!ini_path || !ini_path[0]) ini_path = "saves/save.ini";
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

  WramTraceInit();

  g_audio_mutex = SDL_CreateMutex();
  g_game_thread_id = SDL_GetCurrentThreadID();
  ApplyAudioEnabled();

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

  /* M5.3/D9: spawn the present thread only now — after every boot-time
   * texture creation (base/HUD/diorama textures, the settings-overlay font
   * atlas) has already happened above, and only when there's a renderer to
   * own (video && !headless — §2.7: headless skips the present thread
   * entirely; SubmitFrameToPresent's synchronous fallback still fills
   * g_pixels for PPM captures via AR_HEADLESS_VIDEO). */
  if (video && !headless) {
    g_present_mutex = SDL_CreateMutex();
    g_present_ready_cond = SDL_CreateCondition();
    g_present_done_cond = SDL_CreateCondition();
    if (g_present_mutex && g_present_ready_cond && g_present_done_cond) {
      g_present_running = true;
      g_present_thread = SDL_CreateThread(PresentThreadFn, "present", NULL);
      if (g_present_thread) {
        g_present_thread_active = true;
      } else {
        fprintf(stderr, "[present] SDL_CreateThread failed: %s — "
                "falling back to synchronous present\n", SDL_GetError());
        g_present_running = false;
      }
    } else {
      fprintf(stderr, "[present] mutex/condition creation failed: %s — "
              "falling back to synchronous present\n", SDL_GetError());
    }
  }

  bool running = true;
  uint32 last_tick = SDL_GetTicks();  /* headless-only pacing (§3.6) */

  /* M6/§3.1,§3.3: fixed-timestep accumulator, non-headless only. NTSC rate:
   * 262 scanlines * 1364 master-clock dots / 21.477272 MHz = 60.0988 fps, NOT
   * 60.00 — using 60.00 causes audible audio drift over a long session. */
  static const uint64_t kFrameNs = 16639267;  /* floor(1e9 / 60.0988) */
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
         * mode, can change the refresh rate the Vsync row reports. */
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
          UpdateHostRefreshHz();
          break;
        case SDL_EVENT_KEY_DOWN:
          /* An armed binding row consumes the raw key: it needs the scancode,
           * and it must win over F5/F9/etc. so those stay bindable. */
          if (SettingsOverlay_HandleCaptureEvent(&event)) break;
          if (SettingsOverlay_IsOpen()) {
            bool was_open = true;
            bool consumed = SettingsOverlay_HandleKey(
                event.key.key, true, event.key.repeat != 0);
            if (was_open && !SettingsOverlay_IsOpen()) ClearHeldInput();
            if (consumed) break;
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
              if (result > kSettingChange_Unchanged &&
                  !Settings_Save("settings.ini"))
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
               * OnRuntimeSettingChanged — that observer already quiesces the
               * present thread, calls ApplyDisplayPresentation(), and sets
               * g_paused_redraw_pending for kSettingCat_Display. Doing them
               * again here would re-mutate the renderer outside the
               * observer's quiesce bracket, reintroducing the race this
               * fixes. */
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
          /* While the menu owns the screen the pad drives the menu, not the
           * game — same split the keyboard already had. */
          if (SettingsOverlay_IsOpen()) {
            (void)SettingsOverlay_HandleGamepadEvent(&event);
            break;
          }
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_KEY_UP:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleKey(event.key.key, false,
                                            false);
          else
            HandleInput(event.key.scancode, false);
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
      /* Advance hold-to-accelerate value stepping on the MAIN thread (a
       * settings write from the present thread would deadlock the quiesce).
       * Runs at this loop's ~60Hz cadence while the menu is open; a value it
       * changes sets g_paused_redraw_pending, which the redraw below honors. */
      SettingsOverlay_Tick();
      bool redrew = RedrawPausedFrameIfNeeded();
      /* §2.5: with a present thread running, it re-presents the last slot
       * on its own ~16ms idle timeout — only submit here when something
       * actually changed. Without one (headless-with-video / thread
       * creation failed), keep the old unconditional re-present. */
      if (!headless && (redrew || !g_present_thread_active))
        SubmitFrameToPresent();
      SDL_Delay(16);
      last_time_ns = SDL_GetTicksNS();
      continue;
    }

    ApplyScheduledSettingChange();

    if (headless) {
      /* §3.6: headless keeps the OLD model verbatim — uncapped by default,
       * exactly one tick per outer iteration, no present thread. The
       * oracle/replay tooling depends on this running as fast as the CPU
       * allows. */
      RunOneEmulatedTick(&running);
      RunOuterIterationHousekeeping();
      DrawAndPresentFrame(true);

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
     * thread is no longer paced by SDL_Delay(16) here — the present thread
     * (M5) owns the vsync wait, and this wall-clock accumulator owns the
     * emulated tick rate, decoupled from the display refresh rate. */
    {
      uint64_t now_ns = SDL_GetTicksNS();
      uint64_t dt = now_ns - last_time_ns;
      last_time_ns = now_ns;
      accumulator += dt;
      if (accumulator > kFrameNs * (uint64_t)kMaxCatchupFrames)
        accumulator = kFrameNs * (uint64_t)kMaxCatchupFrames;

      bool produced_frame = false;
      while (accumulator >= kFrameNs) {
        RunOneEmulatedTick(&running);
        accumulator -= kFrameNs;
        produced_frame = true;
      }

      RunOuterIterationHousekeeping();

      if (produced_frame) {
        DrawAndPresentFrame(false);
      } else {
        SDL_Delay(1);
      }
    }
  }

  /* D10: join the present thread immediately after the main loop, BEFORE any
   * teardown touches the renderer/textures/font atlas it may still be
   * reading (SettingsOverlay_Destroy + the DestroyTexture block below run
   * well before SDL_DestroyRenderer — joining first is the only ordering
   * that's safe). */
  if (g_present_thread) {
    SDL_LockMutex(g_present_mutex);
    g_present_running = false;
    g_present_quiesce_requested = false;
    SDL_SignalCondition(g_present_ready_cond);
    SDL_SignalCondition(g_present_done_cond);
    SDL_UnlockMutex(g_present_mutex);
    SDL_WaitThread(g_present_thread, NULL);
    g_present_thread_active = false;
  }

  /* Flush only game-originated battery changes on exit. Deliberate
   * session-only editor changes re-sync the save-system shadow, so using
   * Restart/Exit after one must not turn it into a persistent edit. Skip the
   * flush during replay so a replayed run never mutates the active save (see
   * the auto-persist note above — it would break the next replay's alignment). */
  if (!getenv("AR_INPUT_REPLAY")) {
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

  /* SDL_DestroyAudioStream also closes the bound device (replaces the SDL2
   * SDL_CloseAudio path). */
  if (g_audio_stream) SDL_DestroyAudioStream(g_audio_stream);
  SDL_DestroyMutex(g_audio_mutex);
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
