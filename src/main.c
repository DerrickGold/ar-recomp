#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <SDL.h>

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
#include "scene_inspector.h"
#include "scene_asset_dump.h"
#include "save_system.h"
#include "hd_replacements.h"
#include "music_replacements.h"
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

/* HD art substitution (hd_replacements.c manifest entries). PNG only;
 * decoded once at startup. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_Texture *g_hud_bg_texture;
static SDL_Texture *g_hud_obj_texture;
static uint8 g_paused, g_turbo;
static bool g_scene_inspector_owns_pause;
typedef enum InspectorPresentationKind {
  kInspectorPresentation_Base,
  kInspectorPresentation_HudBg,
  kInspectorPresentation_HudObj,
} InspectorPresentationKind;
typedef struct InspectorPresentationSelection {
  InspectorPresentationKind kind;
  double source_x;
  double source_y;
  int output_x;
  int output_y;
  int output_width;
  int output_height;
} InspectorPresentationSelection;
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
static uint8_t g_pixels[kPpuBufWidth * 4 * 240];
static uint8_t g_hud_bg_pixels[kPpuBufWidth * 4 * 240];
static uint8_t g_hud_obj_pixels[kPpuBufWidth * 4 * 240];
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
static uint8_t *g_m7_overlay_pixels;
static SDL_Texture *g_m7_texture;

/* Widescreen master switch + per-side extra-column budget — the definitions
 * for the runner's widescreen.h externs (each game defines them; 0/false =
 * authentic 256-wide, all PPU margin machinery inert). Set once at startup
 * from ExtendedAspectRatio/AspectPAR in config.ini; per-frame policy lives in
 * ActRaiser_ApplyWidescreenPolicy (actraiser_rtl.c). */
bool g_ws_active;
int g_ws_extra;

extern Snes *g_snes;
extern Ppu *g_ppu;
struct SpcPlayer *g_spc_player;

extern const RtlGameInfo kActRaiserGameInfo;

bool g_new_ppu = true;

static SDL_mutex *g_audio_mutex;
/* The audio callback runs on SDL's audio thread, while settings mutate on the
 * main/game thread. Keep the callback's one live input in an SDL atomic mirror
 * instead of racing on g_settings. */
static SDL_atomic_t g_audio_master_percent;
static bool g_audio_open;
static bool RenderFramebuffer(void);
static void RebindPpuOutputSurfaces(void);

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
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
static SDL_threadID g_game_thread_id;

void RtlApuLock(void) {
  if (!g_audio_mutex) return;
  extern int ApuProfEnabled(void);
  if (ApuProfEnabled()) {
    extern uint64_t g_apuprof_lockwait_ns, g_apuprof_audiowait_max_ns;
    extern uint64_t audio_trace_wall_ns(void);
    if (SDL_TryLockMutex(g_audio_mutex) == 0) return;
    uint64_t t0 = audio_trace_wall_ns();
    SDL_LockMutex(g_audio_mutex);
    uint64_t waited = audio_trace_wall_ns() - t0;
    if (g_game_thread_id != 0 && SDL_ThreadID() == g_game_thread_id)
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

static void HandleInput(int keyCode, bool pressed) {
  uint32 bit = 0;
  switch (keyCode) {
    case SDLK_UP:     bit = 0x0010; break;
    case SDLK_DOWN:   bit = 0x0020; break;
    case SDLK_LEFT:   bit = 0x0040; break;
    case SDLK_RIGHT:  bit = 0x0080; break;
    case SDLK_RETURN: bit = 0x0008; break;
    case SDLK_RSHIFT: bit = 0x0004; break;
    case SDLK_z:      bit = 0x0001; break;
    case SDLK_x:      bit = 0x0100; break;
    case SDLK_a:      bit = 0x0002; break;
    case SDLK_s:      bit = 0x0200; break;
    case SDLK_q:      bit = 0x0400; break;
    case SDLK_w:      bit = 0x0800; break;
    default: return;
  }
  if (pressed)
    g_input_state |= bit;
  else
    g_input_state &= ~bit;
}

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  /* AR_APUPROF: time this acquire — it is the audio thread's outer lock
   * (RtlRenderAudio's internal RtlApuLock calls are recursive re-entries and
   * can never block), so any starvation of the callback shows up here. */
  extern int ApuProfEnabled(void);
  if (ApuProfEnabled()) {
    extern uint64_t g_apuprof_audiowait_max_ns;
    extern uint64_t audio_trace_wall_ns(void);
    uint64_t t0 = audio_trace_wall_ns();
    if (SDL_LockMutex(g_audio_mutex)) { memset(stream, 0, len); return; }
    uint64_t waited = audio_trace_wall_ns() - t0;
    if (waited > g_apuprof_audiowait_max_ns)
      g_apuprof_audiowait_max_ns = waited;
  } else if (SDL_LockMutex(g_audio_mutex)) { memset(stream, 0, len); return; }
  RtlRenderAudio((int16 *)stream, len / 4, 2);
  int volume = SDL_AtomicGet(&g_audio_master_percent);
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  if (volume != 100) {
    int16 *samples = (int16 *)stream;
    const int sample_count = len / (int)sizeof(*samples);
    for (int i = 0; i < sample_count; i++)
      samples[i] = (int16)(((int32)samples[i] * volume) / 100);
  }
  SDL_UnlockMutex(g_audio_mutex);
}

static bool OpenHostAudio(void) {
  if (g_audio_open) return true;
  SDL_AudioSpec want = {0}, have;
  want.freq = g_active_audio_frequency > 0
      ? g_active_audio_frequency : 44100;
  want.format = AUDIO_S16;
  want.channels = 2;
  want.samples = g_active_audio_samples > 0
      ? (Uint16)g_active_audio_samples : 2048;
  want.callback = AudioCallback;
  if (SDL_OpenAudio(&want, &have) != 0) {
    fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
    return false;
  }
  RtlSetAudioOutputRate(have.freq);
  fprintf(stderr, "[audio] opened %d Hz, %u-frame buffer "
                  "(requested %d Hz, %u frames)\n",
          have.freq, (unsigned)have.samples, want.freq,
          (unsigned)want.samples);
  g_audio_open = true;
  return true;
}

static void ApplyAudioEnabled(void) {
  if (g_settings.audio_enabled) {
    if (OpenHostAudio()) SDL_PauseAudio(0);
  } else if (g_audio_open) {
    SDL_PauseAudio(1);
  }
}

static void RtlDrawPpuFrame(void) {
  g_rtl_game_info->draw_ppu_frame();
}

/* Runtime presentation settings can change while game execution is paused.
 * Re-render the same emulated PPU state once after such a change; ordinary
 * paused iterations retain that texture, so pause never advances the game or
 * repeatedly replays the scanline/HDMA renderer. */
static void RedrawPausedFrameIfNeeded(void) {
  if ((g_paused || SettingsOverlay_IsOpen()) && g_paused_redraw_pending) {
    RtlDrawPpuFrame();
    g_paused_redraw_pending = false;
  }
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
  if (g_renderer && g_hud_bg_texture && RenderFramebuffer()) {
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(g_renderer, &out_w, &out_h);
    size_t pitch = (size_t)out_w * 4;
    uint8_t *pixels = out_w > 0 && out_h > 0
        ? (uint8_t *)malloc(pitch * (size_t)out_h) : NULL;
    if (pixels && SDL_RenderReadPixels(g_renderer, NULL,
                                      SDL_PIXELFORMAT_ARGB8888,
                                      pixels, (int)pitch) == 0) {
      fprintf(pf, "P6\n%d %d\n255\n", out_w, out_h);
      for (int y = 0; y < out_h; y++) {
        const uint8_t *row = pixels + (size_t)y * pitch;
        for (int x = 0; x < out_w; x++) {
          fputc(row[x * 4 + 2], pf);
          fputc(row[x * 4 + 1], pf);
          fputc(row[x * 4 + 0], pf);
        }
      }
      free(pixels);
      return (SDL_Point){ out_w, out_h };
    }
    free(pixels);
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
  g_input_state = 0;
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
    RtlSaveLoad(kSaveLoad_Load, 0);
    fprintf(stderr, "State loaded.\n");
  } else if (!strcmp(desc->key, "warp_now")) {
    PerformWarp();
  } else if (!strcmp(desc->key, "take_snapshot")) {
    TakeFullSnapshot();
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

/* Point the presentation at the current display mode's framebuffer sub-rect
 * and size the window to that mode's display aspect. Host textures retain
 * maximum capacity; g_snes_width selects the live PPU pitch, and 4:3 presents
 * only the authentic centre 256 columns. */
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
   * user resizes the window themselves. Mirrors the boot-time setup. */
  if (g_ws_active && !g_settings.ignore_aspect_ratio) {
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      SDL_RenderSetLogicalSize(g_renderer, vis_w * 7, g_snes_height * 6);
    else
      SDL_RenderSetLogicalSize(g_renderer, vis_w, g_snes_height);
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
    RebindPpuOutputSurfaces();
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
  if (desc->field == &g_settings.audio_master_volume)
    SDL_AtomicSet(&g_audio_master_percent, g_settings.audio_master_volume);
  if (desc->field == &g_settings.audio_enabled)
    ApplyAudioEnabled();
  if (desc->field == &g_settings.music_replacements)
    MusicReplacements_ApplySetting();
  if (desc->field == &g_settings.scene_inspector &&
      !g_settings.scene_inspector)
    CloseSceneInspectorSelection();
  if (desc->field == &g_settings.fullscreen && g_window)
    SDL_SetWindowFullscreen(g_window, g_settings.fullscreen
        ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (desc->field == &g_settings.extended_aspect ||
      desc->field == &g_settings.pixel_aspect) {
    ResolveVideoGeometry(true);
    return;
  }
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
      desc->category == kSettingCat_Widescreen)
    g_paused_redraw_pending = true;
}

static void ApplyRendererLogicalSize(void) {
  if (!g_renderer) return;
  if (g_ws_active && !g_settings.ignore_aspect_ratio) {
    int vis_w = Settings_VisibleWidth();
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      SDL_RenderSetLogicalSize(g_renderer, vis_w * 7, g_snes_height * 6);
    else
      SDL_RenderSetLogicalSize(g_renderer, vis_w, g_snes_height);
  } else {
    SDL_RenderSetLogicalSize(g_renderer, 0, 0);
  }
}

static SDL_Rect GetPresentationViewport(void) {
  int out_w = 0, out_h = 0;
  SDL_GetRendererOutputSize(g_renderer, &out_w, &out_h);
  SDL_Rect viewport = { 0, 0, out_w, out_h };
  if (!g_ws_active || g_settings.ignore_aspect_ratio ||
      out_w <= 0 || out_h <= 0)
    return viewport;

  int logical_w = Settings_VisibleWidth() *
                  (g_active_pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  int logical_h = g_snes_height *
                  (g_active_pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  if ((int64_t)out_w * logical_h > (int64_t)out_h * logical_w) {
    viewport.w = (int)((int64_t)out_h * logical_w / logical_h);
    viewport.x = (out_w - viewport.w) / 2;
  } else {
    viewport.h = (int)((int64_t)out_w * logical_h / logical_w);
    viewport.y = (out_h - viewport.h) / 2;
  }
  return viewport;
}

static void AdjustHudOutputScale(int delta_percent) {
  const SettingDesc *desc = Settings_Find("hud_scale_percent");
  if (!desc) return;
  int current = g_settings.hud_scale_percent;
  if (!current && g_renderer) {
    SDL_Rect viewport = GetPresentationViewport();
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

static int ScaledHudPixels(int pixels, double scale) {
  int result = (int)(pixels * scale + 0.5);
  return result > 0 ? result : 1;
}

static void RenderHudChunk(SDL_Texture *texture, SDL_Rect src, SDL_Rect dst) {
  if (!texture || src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
    return;
  SDL_RenderCopy(g_renderer, texture, &src, &dst);
}

typedef struct HudPresentationChunk {
  SDL_Texture *texture;
  SDL_Rect texture_source;
  SDL_Rect screen_source;
  SDL_Rect output_destination;
  InspectorPresentationKind inspector_kind;
  int inspector_x_bias;
} HudPresentationChunk;

enum { kHudPresentationChunkCapacity = 5 };

static void AddHudPresentationChunk(HudPresentationChunk *chunks,
                                    int *count,
                                    SDL_Texture *texture,
                                    SDL_Rect texture_source,
                                    SDL_Rect screen_source,
                                    SDL_Rect output_destination,
                                    InspectorPresentationKind kind,
                                    int inspector_x_bias) {
  if (!chunks || !count || *count >= kHudPresentationChunkCapacity ||
      !texture || texture_source.w <= 0 || texture_source.h <= 0 ||
      screen_source.w <= 0 || screen_source.h <= 0 ||
      output_destination.w <= 0 || output_destination.h <= 0)
    return;
  chunks[(*count)++] = (HudPresentationChunk){
    texture,
    texture_source,
    screen_source,
    output_destination,
    kind,
    inspector_x_bias,
  };
}

/* One geometry description drives both compositing and hit-testing. This is
 * essential when the HUD uses an independent scale or its right group is
 * translated to the widescreen edge. */
static int BuildHudPresentationChunks(
    SDL_Rect viewport, HudPresentationChunk *chunks) {
  if (!g_hud_bg_texture || !g_ppu || !g_ppu->wsHudSplitHeight)
    return 0;

  int count = 0;
  int vis_w = Settings_VisibleWidth();
  double scale_y, scale_x;
  if (g_settings.hud_scale_percent == 0) {
    scale_y = (double)viewport.h / g_snes_height;
    scale_x = (double)viewport.w / vis_w;
  } else {
    scale_y = g_settings.hud_scale_percent / 100.0;
    scale_x = scale_y *
        (g_active_pixel_aspect == kPixelAspect_Crt43 ? 7.0 / 6.0 : 1.0);
  }

  int tex_extra = (g_snes_width - 256) / 2;
  int height = g_ppu->wsHudSplitHeight;
  int lower_y = g_ppu->wsHudLeftOnlyY;
  if (lower_y > height) lower_y = height;
  int upper_h = lower_y;
  int upper_dh = ScaledHudPixels(upper_h, scale_y);

  SDL_Rect src = { tex_extra, 0, g_ppu->wsHudLeftEnd, upper_h };
  SDL_Rect dst = { viewport.x, viewport.y,
                   ScaledHudPixels(src.w, scale_x), upper_dh };
  AddHudPresentationChunk(
      chunks, &count, g_hud_bg_texture, src,
      (SDL_Rect){ 0, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, -g_ppu->extraLeftRight);

  if (g_ppu->wsHudLeftEnd < g_ppu->wsHudRightStart) {
    src.x = tex_extra + g_ppu->wsHudLeftEnd;
    src.w = g_ppu->wsHudRightStart - g_ppu->wsHudLeftEnd;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.x = viewport.x + (viewport.w - dst.w) / 2;
    AddHudPresentationChunk(
        chunks, &count, g_hud_bg_texture, src,
        (SDL_Rect){ g_ppu->wsHudLeftEnd, 0, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, 0);
  }

  int right_source_w = 256 - g_ppu->wsHudRightStart;
  int right_dest_w = ScaledHudPixels(right_source_w, scale_x);
  src.x = tex_extra + g_ppu->wsHudRightStart;
  src.w = right_source_w;
  dst.x = viewport.x + viewport.w - right_dest_w;
  dst.w = right_dest_w;
  AddHudPresentationChunk(
      chunks, &count, g_hud_bg_texture, src,
      (SDL_Rect){ g_ppu->wsHudRightStart, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, g_ppu->extraLeftRight);

  if (lower_y < height) {
    src.x = tex_extra;
    src.y = lower_y;
    src.w = 256;
    src.h = height - lower_y;
    dst.x = viewport.x;
    dst.y = viewport.y + ScaledHudPixels(lower_y, scale_y);
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = ScaledHudPixels(src.h, scale_y);
    AddHudPresentationChunk(
        chunks, &count, g_hud_bg_texture, src,
        (SDL_Rect){ 0, lower_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, -g_ppu->extraLeftRight);
  }

  /* Action's selected-magic icon and simulation's hourglass are separately
   * validated four-slot OAM signatures. Keep either promoted HUD icon four
   * native pixels before the scaled right group. */
  const PpuOverlayCapture *obj_capture =
      &g_ppu->overlayCaptures[kPpuOverlaySource_Obj];
  if (g_hud_obj_texture && obj_capture->oamCount == 4) {
    int x = (g_ppu->oam[0] & 0xff) | ((g_ppu->highOam[0] & 1) << 8);
    int y = g_ppu->oam[0] >> 8;
    if (x < 256) {
      SDL_Rect obj_src = { tex_extra + x, y, 16, 16 };
      SDL_Rect obj_dst = {
        viewport.x + viewport.w - right_dest_w -
            ScaledHudPixels(20, scale_x),
        viewport.y + ScaledHudPixels(y, scale_y),
        ScaledHudPixels(16, scale_x),
        ScaledHudPixels(16, scale_y),
      };
      AddHudPresentationChunk(
          chunks, &count, g_hud_obj_texture, obj_src,
          (SDL_Rect){ x, y, 16, 16 }, obj_dst,
          kInspectorPresentation_HudObj, 0);
    }
  }
  return count;
}

static void RenderHudOverlay(SDL_Rect viewport) {
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, chunks);
  for (int i = 0; i < count; i++)
    RenderHudChunk(chunks[i].texture, chunks[i].texture_source,
                   chunks[i].output_destination);
}

static int InspectorScreenToOutputX(SDL_Rect viewport, double screen_x) {
  int visible_left = Settings_VisibleX0() - g_ws_extra;
  return viewport.x +
      (int)((screen_x - visible_left) * viewport.w /
            Settings_VisibleWidth() + 0.5);
}

static int InspectorScreenToOutputY(SDL_Rect viewport, double screen_y) {
  return viewport.y +
      (int)(screen_y * viewport.h / g_snes_height + 0.5);
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

static bool FindSelectedHudChunk(SDL_Rect viewport,
                                 HudPresentationChunk *selected) {
  if (g_scene_inspector_presentation.kind == kInspectorPresentation_Base)
    return false;
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, chunks);
  for (int i = count - 1; i >= 0; i--) {
    const SDL_Rect source = chunks[i].screen_source;
    if (chunks[i].inspector_kind != g_scene_inspector_presentation.kind ||
        g_scene_inspector_presentation.source_x < source.x ||
        g_scene_inspector_presentation.source_x >= source.x + source.w ||
        g_scene_inspector_presentation.source_y < source.y ||
        g_scene_inspector_presentation.source_y >= source.y + source.h)
      continue;
    if (selected) *selected = chunks[i];
    return true;
  }
  return false;
}

static int HudSourceToOutputX(const HudPresentationChunk *chunk,
                              double source_x) {
  return chunk->output_destination.x +
      (int)((source_x - chunk->screen_source.x) *
            chunk->output_destination.w / chunk->screen_source.w + 0.5);
}

static int HudSourceToOutputY(const HudPresentationChunk *chunk,
                              double source_y) {
  return chunk->output_destination.y +
      (int)((source_y - chunk->screen_source.y) *
            chunk->output_destination.h / chunk->screen_source.h + 0.5);
}

static bool HudHighlightToOutput(const HudPresentationChunk *chunk,
                                 int x0, int y0, int x1, int y1,
                                 SDL_Rect *output) {
  if (!chunk || !output) return false;
  x0 -= chunk->inspector_x_bias;
  x1 -= chunk->inspector_x_bias;
  const SDL_Rect source = chunk->screen_source;
  if (x0 < source.x) x0 = source.x;
  if (y0 < source.y) y0 = source.y;
  if (x1 > source.x + source.w) x1 = source.x + source.w;
  if (y1 > source.y + source.h) y1 = source.y + source.h;
  if (x1 <= x0 || y1 <= y0) return false;
  int output_x0 = HudSourceToOutputX(chunk, x0);
  int output_y0 = HudSourceToOutputY(chunk, y0);
  int output_x1 = HudSourceToOutputX(chunk, x1);
  int output_y1 = HudSourceToOutputY(chunk, y1);
  *output = (SDL_Rect){
    output_x0, output_y0,
    output_x1 - output_x0, output_y1 - output_y0,
  };
  return output->w > 0 && output->h > 0;
}

static void RenderSceneInspector(SDL_Rect viewport) {
  if (!g_settings.scene_inspector || !SceneInspector_HasSelection()) return;
  int x = 0, y = 0;
  if (!SceneInspector_GetPoint(&x, &y)) return;
  HudPresentationChunk hud_chunk;
  bool hud_selection = FindSelectedHudChunk(viewport, &hud_chunk);
  int projected_px = hud_selection
      ? HudSourceToOutputX(
          &hud_chunk, g_scene_inspector_presentation.source_x)
      : InspectorScreenToOutputX(
          viewport, g_scene_inspector_presentation.source_x);
  int projected_py = hud_selection
      ? HudSourceToOutputY(
          &hud_chunk, g_scene_inspector_presentation.source_y)
      : InspectorScreenToOutputY(
          viewport, g_scene_inspector_presentation.source_y);
  int output_width = 0, output_height = 0;
  SDL_GetRendererOutputSize(g_renderer, &output_width, &output_height);
  bool same_output = output_width ==
                         g_scene_inspector_presentation.output_width &&
                     output_height ==
                         g_scene_inspector_presentation.output_height;
  int px = same_output ? g_scene_inspector_presentation.output_x
                       : projected_px;
  int py = same_output ? g_scene_inspector_presentation.output_y
                       : projected_py;
  int anchor_dx = px - projected_px;
  int anchor_dy = py - projected_py;

  SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  SDL_GetRenderDrawBlendMode(g_renderer, &old_blend);
  SDL_GetRenderDrawColor(g_renderer, &old_r, &old_g, &old_b, &old_a);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 255, 192, 32, 255);
  SDL_RenderDrawLine(g_renderer, px - 7, py, px + 7, py);
  SDL_RenderDrawLine(g_renderer, px, py - 7, px, py + 7);

  int x0, y0, x1, y1;
  if (SceneInspector_GetHighlight(&x0, &y0, &x1, &y1)) {
    SDL_Rect rect;
    bool have_rect = hud_selection &&
        HudHighlightToOutput(&hud_chunk, x0, y0, x1, y1, &rect);
    if (!hud_selection) {
      rect = (SDL_Rect){
        InspectorScreenToOutputX(viewport, x0),
        InspectorScreenToOutputY(viewport, y0),
        InspectorScreenToOutputX(viewport, x1) -
            InspectorScreenToOutputX(viewport, x0),
        InspectorScreenToOutputY(viewport, y1) -
            InspectorScreenToOutputY(viewport, y0),
      };
      have_rect = rect.w > 0 && rect.h > 0;
    }
    if (have_rect) {
      /* Preserve the exact renderer-output click anchor. Native source
       * sampling intentionally quantizes to one SNES pixel; without this
       * correction, that discarded fraction visibly shifts both annotations
       * when the game or promoted HUD is scaled by a non-integer factor. */
      rect.x += anchor_dx;
      rect.y += anchor_dy;
      SDL_RenderDrawRect(g_renderer, &rect);
    }
  }
  SDL_SetRenderDrawBlendMode(g_renderer, old_blend);
  SDL_SetRenderDrawColor(g_renderer, old_r, old_g, old_b, old_a);
  SettingsOverlay_RenderDebugPanel(
      "SCENE INSPECTOR", SceneInspector_PanelText(), (SDL_Point){ px, py });
}

static bool WindowPointToOutput(int event_x, int event_y,
                                int *output_x, int *output_y) {
  if (!g_window || !g_renderer) return false;
  int window_width = 0, window_height = 0;
  int output_width = 0, output_height = 0;
  SDL_GetWindowSize(g_window, &window_width, &window_height);
  if (SDL_GetRendererOutputSize(g_renderer, &output_width, &output_height) ||
      window_width <= 0 || window_height <= 0 ||
      output_width <= 0 || output_height <= 0)
    return false;
  int logical_width = 0, logical_height = 0;
  SDL_RenderGetLogicalSize(g_renderer, &logical_width, &logical_height);
  if (logical_width > 0 && logical_height > 0) {
    /* SDL_RenderSetLogicalSize filters absolute mouse events before they are
     * queued: event.button/motion x/y are ALREADY logical coordinates. Map
     * that logical point directly through the same physical viewport used by
     * the game. Treating it as a window pixel (or passing it through
     * SDL_RenderWindowToLogical again) scales it twice and sends far-edge
     * clicks outside the viewport. */
    SDL_Rect viewport = GetPresentationViewport();
    if (output_x)
      *output_x = viewport.x +
          (int)(((int64_t)event_x * viewport.w + logical_width / 2) /
                logical_width);
    if (output_y)
      *output_y = viewport.y +
          (int)(((int64_t)event_y * viewport.h + logical_height / 2) /
                logical_height);
    return true;
  }

  /* Without a logical renderer size SDL leaves the event in window-client
   * coordinates. Only this fallback needs the high-DPI window/output ratio. */
  if (output_x)
    *output_x = (int)(((int64_t)event_x * output_width +
                       window_width / 2) / window_width);
  if (output_y)
    *output_y = (int)(((int64_t)event_y * output_height +
                       window_height / 2) / window_height);
  return true;
}

static bool InspectWindowPoint(int window_x, int window_y) {
  int output_x = 0, output_y = 0;
  if (!WindowPointToOutput(window_x, window_y, &output_x, &output_y))
    return false;
  SDL_Rect viewport = GetPresentationViewport();
  bool had_selection = SceneInspector_HasSelection();
  bool was_paused = g_paused != 0;
  int output_width = 0, output_height = 0;
  SDL_GetRendererOutputSize(g_renderer, &output_width, &output_height);

  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int chunk_count = BuildHudPresentationChunks(viewport, chunks);
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
  g_input_state = 0;
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
    if (texture && SDL_UpdateTexture(texture, NULL, rgba, w * 4) == 0) {
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
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

static void RebindPpuOutputSurfaces(void) {
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

/* Composite the Mode-7 override surface over the game frame (the engine
 * already removed the substituted pixels there). Drawn before the OBJ/HUD
 * overlays so promoted sprites stay above substituted scenery. Brightness
 * was applied by the engine sampler; forced blank renders nothing because
 * the per-line clears run before the early-out. */
static void RenderMode7Overlay(SDL_Rect viewport) {
  if (!g_m7_texture || !g_ppu || !g_ppu->m7Override.rgba) return;
  /* Upload only the sub-rect RenderCopy samples (the visible columns); the
   * source row pitch stays the full surface width. Identical in wide-full
   * mode; a 4:3 view of a mode-7 substitution skips the margin columns. */
  SDL_Rect src = { Settings_VisibleX0() * kHdMode7Scale, 0,
                   Settings_VisibleWidth() * kHdMode7Scale,
                   g_snes_height * kHdMode7Scale };
  SDL_UpdateTexture(g_m7_texture, &src,
                    g_m7_overlay_pixels + (size_t)src.x * 4,
                    g_snes_width * kHdMode7Scale * 4);
  SDL_RenderCopy(g_renderer, g_m7_texture, &src, &viewport);
}

/* Draw every active HD replacement over the region its capture removed this
 * frame. Master brightness is resolved on the host texture so INIDISP fades
 * apply to the substituted art; forced blank suppresses it entirely,
 * matching the authentic layer. */
static void RenderHdReplacements(SDL_Rect viewport) {
  if (!g_ppu || (g_ppu->inidisp & 0x80)) return;

  /* Screen space -> framebuffer -> visible sub-rect -> viewport. Promoted
   * scenes are pillarboxed, so authentic x=0 sits g_ws_extra columns into
   * the framebuffer regardless of display mode. */
  int vis_w = Settings_VisibleWidth();
  int vis_x0 = Settings_VisibleX0();
  int extra = (g_snes_width - 256) / 2;
  double scale_x = (double)viewport.w / vis_w;
  double scale_y = (double)viewport.h / g_snes_height;

  for (int i = 0; i < g_hd_replacement_count; i++) {
    const HdReplacement *entry = &g_hd_replacements[i];
    if (!entry->active || !entry->texture) continue;
    const PpuOverlayCapture *capture =
        &g_ppu->overlayCaptures[entry->source];
    if (capture->x1 <= capture->x0 || capture->y1 <= capture->y0 ||
        !(capture->flags & kPpuOverlayFlag_RemoveFromGame))
      continue;
    int dx0 = (int)((capture->x0 + extra - vis_x0) * scale_x + 0.5);
    int dx1 = (int)((capture->x1 + extra - vis_x0) * scale_x + 0.5);
    int dy0 = (int)(capture->y0 * scale_y + 0.5);
    int dy1 = (int)(capture->y1 * scale_y + 0.5);
    SDL_Rect dst = { viewport.x + dx0, viewport.y + dy0,
                     dx1 - dx0, dy1 - dy0 };
    if (dst.w <= 0 || dst.h <= 0) continue;

    SDL_Texture *texture = (SDL_Texture *)entry->texture;
    Uint8 mod = entry->brightness_mod
        ? (Uint8)((g_ppu->inidisp & 0xf) * 255 / 15) : 255;
    SDL_SetTextureColorMod(texture, mod, mod, mod);
    SDL_RenderCopy(g_renderer, texture, NULL, &dst);
  }
}

static bool RenderFramebuffer(void) {
  if (!g_renderer || !g_texture) return false;
  /* Present only the current mode's sub-rect: the whole framebuffer when
   * wide, the authentic centre 256 in 4:3. The texture allocation never
   * changes while cycling modes. */
  SDL_Rect src = { Settings_VisibleX0(), 0,
                   Settings_VisibleWidth(), g_snes_height };
  SDL_Rect upload = { 0, 0, g_snes_width, g_snes_height };
  SDL_UpdateTexture(g_texture, &upload, g_pixels, g_snes_width * 4);
  /* The HUD planes are only sampled by RenderHudOverlay, and only while a
   * widescreen HUD split is active; both captures are top-anchored. Upload
   * just the rows the split can sample instead of three full-height planes
   * every frame. Rows beyond the upload keep stale texture content, which is
   * fine: sampling is bounded by the CURRENT split height and OAM signature,
   * so a disabled or shorter split never reads them. */
  if (g_ppu && g_ppu->wsHudSplitHeight) {
    int split_rows = g_ppu->wsHudSplitHeight;
    if (g_hud_bg_texture) {
      int rows = g_ppu->overlayCaptures[kPpuOverlaySource_Bg3].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, g_snes_width, rows };
      SDL_UpdateTexture(g_hud_bg_texture, &hud, g_hud_bg_pixels,
                        g_snes_width * 4);
    }
    if (g_hud_obj_texture) {
      int rows = g_ppu->overlayCaptures[kPpuOverlaySource_Obj].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, g_snes_width, rows };
      SDL_UpdateTexture(g_hud_obj_texture, &hud, g_hud_obj_pixels,
                        g_snes_width * 4);
    }
  }
  ApplyRendererLogicalSize();
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, &src, NULL);

  SDL_Rect viewport = GetPresentationViewport();
  SDL_RenderSetLogicalSize(g_renderer, 0, 0);
  RenderMode7Overlay(viewport);
  RenderHudOverlay(viewport);
  RenderHdReplacements(viewport);
  RenderSceneInspector(viewport);
  SettingsOverlay_Render(viewport);
  ApplyRendererLogicalSize();
  return true;
}

static void PresentFramebuffer(void) {
  if (!RenderFramebuffer()) return;
  SDL_RenderPresent(g_renderer);
}

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
  Settings_InitWithFile("settings.ini");
  g_active_audio_frequency = Settings_AudioFrequencyHz();
  g_active_audio_samples = g_settings.audio_samples;
  ResolveVideoGeometry(false);

  /* Display presets depend on whether the resolved aspect selected a wide
   * budget. Finalize only after g_ws_active/g_ws_extra are authoritative. */
  Settings_FinalizeDisplayMode();
  SDL_AtomicSet(&g_audio_master_percent, g_settings.audio_master_volume);

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

  Uint32 sdl_flags = SDL_INIT_AUDIO;
  if (video) sdl_flags |= SDL_INIT_VIDEO;
  if (!headless) sdl_flags |= SDL_INIT_GAMECONTROLLER;
  if (SDL_Init(sdl_flags) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (video) {
    int scale = g_settings.window_scale ? g_settings.window_scale : 3;
    /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
     * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
     * the height), so derive the window from the target ratio, not the
     * framebuffer. Faithful mode keeps the historical g_snes_width*scale. */
    int win_w = g_snes_width * scale;
    if (g_ws_active && g_active_pixel_aspect == kPixelAspect_Crt43)
      win_w = (g_snes_height * scale * g_active_aspect_x +
               g_active_aspect_y / 2) / g_active_aspect_y;
    Uint32 window_flags = SDL_WINDOW_RESIZABLE |
        (headless_video ? SDL_WINDOW_HIDDEN : 0) |
        (g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    g_window = SDL_CreateWindow(
      kWindowTitle,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      win_w, g_snes_height * scale,
      window_flags
    );
    if (!g_window) Die("SDL_CreateWindow failed");

    g_renderer = SDL_CreateRenderer(g_window, -1, headless_video
      ? SDL_RENDERER_SOFTWARE
      : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) Die("SDL_CreateRenderer failed");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* Aspect-correct letterboxing via SDL's logical size (widescreen only, so
     * faithful mode keeps the historical stretch-to-window behavior).
     * 4:3-PAR: logical w:h = (render_w*7):(224*6) encodes the 7:6 pixel
     * stretch; square: the raw framebuffer dimensions. IgnoreAspectRatio=1
     * restores plain stretching. */
    if (g_ws_active && !g_settings.ignore_aspect_ratio) {
      if (g_active_pixel_aspect == kPixelAspect_Crt43)
        SDL_RenderSetLogicalSize(g_renderer, g_snes_width * 7, g_snes_height * 6);
      else
        SDL_RenderSetLogicalSize(g_renderer, g_snes_width, g_snes_height);
    }

    g_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPpuBufWidth, g_snes_height);
    if (!g_texture) Die("SDL_CreateTexture failed");

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

    LoadHdReplacements();

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  }

  if (!SettingsOverlay_Init(g_renderer, rom_data, rom_size))
    Die("SDL font atlas creation for settings overlay failed");
  SettingsOverlay_SetInspectorInfoProvider(FormatInspectorInfo);

  Settings_SetChangeObserver(OnRuntimeSettingChanged);
  Settings_SetActionObserver(OnSettingsAction);

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

  g_spc_player = ActRaiserSpcPlayer_Create();

  RtlRegisterGame(&kActRaiserGameInfo);
  Snes *snes = SnesInit(rom_data, (int)rom_size);
  if (!snes) Die("SnesInit failed");

  BindHdReplacementSurfaces();
  RebindPpuOutputSurfaces();
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
  g_game_thread_id = SDL_ThreadID();
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

  bool running = true;
  uint32 last_tick = SDL_GetTicks();

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT:
          running = false;
          break;
        case SDL_KEYDOWN:
          if (SettingsOverlay_IsOpen()) {
            bool was_open = true;
            bool consumed = SettingsOverlay_HandleKey(
                event.key.keysym.sym, true, event.key.repeat != 0);
            if (was_open && !SettingsOverlay_IsOpen()) g_input_state = 0;
            if (consumed) break;
          }
          /* The settings UI is host-owned and safe in every emulated state.
           * Escape/F1 are not SNES inputs, so consume them before HandleInput
           * and clear held joypad state before freezing game advancement. */
          if (!event.key.repeat &&
              (event.key.keysym.sym == SDLK_ESCAPE ||
               event.key.keysym.sym == SDLK_F1)) {
            g_input_state = 0;
            SettingsOverlay_Open();
          } else if (event.key.keysym.sym == SDLK_p) {
            if (SceneInspector_HasSelection()) {
              bool inspector_owned_pause = g_scene_inspector_owns_pause;
              CloseSceneInspectorSelection();
              if (!inspector_owned_pause) TogglePause();
            } else {
              TogglePause();
            }
          } else if (event.key.keysym.sym == SDLK_t) {
            ToggleTurbo();
          } else if (event.key.keysym.sym == SDLK_F3) {
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
          } else if (event.key.keysym.sym == SDLK_MINUS ||
                     event.key.keysym.sym == SDLK_KP_MINUS) {
            if (!event.key.repeat) AdjustHudOutputScale(-25);
          } else if (event.key.keysym.sym == SDLK_EQUALS ||
                     event.key.keysym.sym == SDLK_PLUS ||
                     event.key.keysym.sym == SDLK_KP_PLUS) {
            if (!event.key.repeat) AdjustHudOutputScale(25);
          } else if (event.key.keysym.sym == SDLK_F5) {
            (void)OnSettingsAction(Settings_Find("save_state"));
          } else if (event.key.keysym.sym == SDLK_F7) {
            (void)OnSettingsAction(Settings_Find("load_state"));
          } else if (event.key.keysym.sym == SDLK_F9) {
            /* Cycle 4:3 -> widescreen RAW -> widescreen FULL, for capturing
             * before/after comparison shots without a settings UI. Requires
             * booting with ExtendedAspectRatio set: the wide framebuffer and
             * window are sized once at boot, so an authentic-booted run has no
             * margins to reveal and stays pinned to 4:3. Shift+F9 retains the
             * long-standing diagnostic dump command. Ignore key-repeat so one
             * physical press advances exactly one preset. */
            if (event.key.repeat) {
              /* no-op */
            } else if (event.key.keysym.mod & KMOD_SHIFT) {
              DumpDiagState("hotkey");
            } else if (!g_ws_active) {
              fprintf(stderr, "[display] F9 needs ExtendedAspectRatio "
                      "(e.g. 16:9) in config.ini; staying 4:3\n");
            } else {
              int m = Settings_CycleDisplayMode();
              ApplyDisplayPresentation();
              g_paused_redraw_pending = true;
              fprintf(stderr, "[display] mode %d/%d -> %s\n", m + 1,
                      kDisplayMode_PresetCount, Settings_DisplayModeName(m));
            }
          } else if (event.key.keysym.sym == SDLK_F6) {
            /* Level warp: stage the game's own sim->act transition to the raw
             * registry target seeded by AR_WARP=<region_hex><map_hex>. The low byte is $19,
             * not a uniform act number (e.g. Kasandora act 2 is 0303). Press
             * from a transition-capable state; see README + docs/SEAMS.md. */
            PerformWarp();
          } else if (event.key.keysym.sym == SDLK_F2) {
            /* On-demand FULL snapshot — each press writes a unique set of files
             * tagged with the game-frame: WRAM + VRAM + CGRAM + OAM (via
             * ActRaiser_FullSnapshot) plus a .ppm screenshot. Lets several
             * moments be grabbed while driving the game manually so the
             * internals (esp. VRAM, where the bridge tiles live) can be watched
             * change over time alongside the picture. */
            /* If F9 and F2 were queued in the same paused host iteration,
             * render the new preset before capturing it. */
            TakeFullSnapshot();
          } else {
            HandleInput(event.key.keysym.sym, true);
          }
          break;
        case SDL_TEXTINPUT:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleText(event.text.text);
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
              CloseSceneInspectorSelection();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
              int output_x = 0, output_y = 0;
              if (!WindowPointToOutput(event.button.x, event.button.y,
                                       &output_x, &output_y) ||
                  !SettingsOverlay_BeginDebugPanelDrag(
                      output_x, output_y))
                (void)InspectWindowPoint(event.button.x, event.button.y);
            }
          }
          break;
        case SDL_MOUSEMOTION:
          if (SettingsOverlay_IsDebugPanelDragging()) {
            int output_x = 0, output_y = 0;
            if (WindowPointToOutput(event.motion.x, event.motion.y,
                                    &output_x, &output_y))
              SettingsOverlay_DragDebugPanel(output_x, output_y);
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (event.button.button == SDL_BUTTON_LEFT)
            SettingsOverlay_EndDebugPanelDrag();
          break;
        case SDL_KEYUP:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleKey(event.key.keysym.sym, false,
                                            false);
          else
            HandleInput(event.key.keysym.sym, false);
          break;
      }
    }

    if (g_host_lifecycle_request != kHostLifecycle_None) {
      running = false;
      continue;
    }

    /* Host-owned pauses do not issue the game's native SPC $F2 command. Keep
     * the HD decoder aligned explicitly; its independent driver-pause latch
     * still prevents resume until both pause reasons have cleared. */
    MusicReplacements_SetHostPaused(
        g_paused || SettingsOverlay_IsOpen());

    if (g_paused || SettingsOverlay_IsOpen()) {
      RedrawPausedFrameIfNeeded();
      if (!headless) PresentFramebuffer();
      SDL_Delay(16);
      continue;
    }

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
          running = false;
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
    ApplyScheduledSettingChange();

    /* AR_PERF=1: once-per-second frame-time budget lines. Separates the two
     * "it feels slow" classes in one run: host-bound (fps < 60; inspect both
     * run-ms here and draw-ms below) vs. pacing (fps=60, both budgets tiny, but
     * on-screen action crawls because logical updates span extra host frames;
     * see the $4210 3-read-wait class). An APU-port spin cycling the SPC under
     * the lock appears here as run-ms plus a large apu-catchup delta; host
     * widescreen/compositor work appears in [draw-perf]. */
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
    /* TURBO ('t' toggle): the renderer is created with PRESENTVSYNC, so
     * RenderPresent blocks at 60Hz regardless of the SDL_Delay skip below —
     * which made the original turbo a no-op. Real fast-forward = run extra
     * game frames per RENDERED frame while inside the vsync'd loop. 8x by
     * default (the registry-backed AR_TURBO_MULT setting overrides). Same input word each sub-frame
     * (level-held buttons repeat; fine for skipping sim waits). Cheats/pins
     * apply inside RtlRunFrame, so they hold during the skipped frames too. */
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
      extern uint8 g_ram[];
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

    uint32 perf_draw_t0 = perf_on ? SDL_GetTicks() : 0;
    RtlDrawPpuFrame();
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
    { extern uint8 g_ram[];
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
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

    if (!headless) PresentFramebuffer();

    if (headless) {
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
    } else if (!g_turbo) {
      uint32 now = SDL_GetTicks();
      uint32 elapsed = now - last_tick;
      if (elapsed < 16)
        SDL_Delay(16 - elapsed);
      last_tick = SDL_GetTicks();
    }
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

  SDL_CloseAudio();
  SDL_DestroyMutex(g_audio_mutex);
  for (int i = 0; i < g_hd_replacement_count; i++) {
    if (g_hd_replacements[i].texture)
      SDL_DestroyTexture((SDL_Texture *)g_hd_replacements[i].texture);
    free(g_hd_replacements[i].pixels);
  }
  SDL_DestroyTexture(g_m7_texture);
  SettingsOverlay_Destroy();
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
