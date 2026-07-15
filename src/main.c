#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <SDL.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"
#include "types.h"
#include "actraiser_rtl.h"
#include "common_cpu_infra.h"
#include "config.h"
#include "settings.h"
#include "run_dir.h"
#include "util.h"
#include "actraiser_spc_player.h"
#include "snes/snes.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "framedump.h"
#include "widescreen.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_Texture *g_hud_bg_texture;
static SDL_Texture *g_hud_obj_texture;
static uint8 g_paused, g_turbo;
static bool g_paused_redraw_pending;
static uint32 g_input_state;
static int g_snes_width = 256, g_snes_height = 224;
/* Framebuffer sized for the PPU's full widescreen budget (448 wide) so the
 * width can be chosen at startup without reallocation; faithful runs still
 * render/present only the leading 256*4 bytes per row via g_snes_width. */
static uint8_t g_pixels[kPpuBufWidth * 4 * 240];
static uint8_t g_hud_bg_pixels[kPpuBufWidth * 4 * 240];
static uint8_t g_hud_obj_pixels[kPpuBufWidth * 4 * 240];

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
static bool RenderFramebuffer(void);

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

void RtlApuLock(void) {
  if (g_audio_mutex) SDL_LockMutex(g_audio_mutex);
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
  if (SDL_LockMutex(g_audio_mutex)) { memset(stream, 0, len); return; }
  RtlRenderAudio((int16 *)stream, len / 4, 2);
  SDL_UnlockMutex(g_audio_mutex);
}

static void RtlDrawPpuFrame(void) {
  g_rtl_game_info->draw_ppu_frame();
}

/* Runtime presentation settings can change while game execution is paused.
 * Re-render the same emulated PPU state once after such a change; ordinary
 * paused iterations retain that texture, so pause never advances the game or
 * repeatedly replays the scanline/HDMA renderer. */
static void RedrawPausedFrameIfNeeded(void) {
  if (g_paused && g_paused_redraw_pending) {
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

/* Point the presentation at the current display mode's framebuffer sub-rect and
 * size the window to that mode's display aspect. Nothing is reallocated: the
 * PPU always renders at the boot width, and 4:3 simply presents the authentic
 * centre 256 columns instead of showing the margins as black bars. This is the
 * same "allocate at max, select live" mechanism a future user-selectable
 * resolution would use — see docs/settings-system.md §4.2/§10.2. */
static void ApplyDisplayPresentation(void) {
  if (!g_window || !g_renderer) return;
  int vis_w = Settings_VisibleWidth();
  int scale = g_config.window_scale ? g_config.window_scale : 3;
  int win_h = g_snes_height * scale;
  int win_w = vis_w * scale;

  if (g_settings.display_mode == kDisplayMode_43) {
    /* 256px at the 7:6 CRT PAR displays as 4:3. */
    if (g_config.aspect_par_43) win_w = (win_h * 4 + 1) / 3;
  } else if (g_ws_active && g_config.aspect_par_43) {
    win_w = (win_h * g_config.extend_aspect_x
             + g_config.extend_aspect_y / 2) / g_config.extend_aspect_y;
  }

  /* Logical size encodes the pixel stretch so SDL letterboxes correctly if the
   * user resizes the window themselves. Mirrors the boot-time setup. */
  if (g_ws_active && !g_config.ignore_aspect_ratio) {
    if (g_config.aspect_par_43)
      SDL_RenderSetLogicalSize(g_renderer, vis_w * 7, g_snes_height * 6);
    else
      SDL_RenderSetLogicalSize(g_renderer, vis_w, g_snes_height);
  }
  SDL_SetWindowSize(g_window, win_w, win_h);
}

static void OnRuntimeSettingChanged(const SettingDesc *desc,
                                    SettingChangeResult result) {
  (void)result;
  /* HUD scale is a display-category value but must not resize a manually
   * resized window. Only the display profile and geometry policies own the
   * presentation/window dimensions. */
  if (desc->field == &g_settings.display_mode ||
      desc->category == kSettingCat_Widescreen)
    ApplyDisplayPresentation();
  if (desc->category == kSettingCat_Display ||
      desc->category == kSettingCat_Widescreen)
    g_paused_redraw_pending = true;
}

static void ApplyRendererLogicalSize(void) {
  if (!g_renderer) return;
  if (g_ws_active && !g_config.ignore_aspect_ratio) {
    int vis_w = Settings_VisibleWidth();
    if (g_config.aspect_par_43)
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
  if (!g_ws_active || g_config.ignore_aspect_ratio || out_w <= 0 || out_h <= 0)
    return viewport;

  int logical_w = Settings_VisibleWidth() *
                  (g_config.aspect_par_43 ? 7 : 1);
  int logical_h = g_snes_height * (g_config.aspect_par_43 ? 6 : 1);
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

static void RenderHudOverlay(SDL_Rect viewport) {
  if (!g_hud_bg_texture || !g_ppu || !g_ppu->wsHudSplitHeight)
    return;

  int vis_w = Settings_VisibleWidth();
  double scale_y, scale_x;
  if (g_settings.hud_scale_percent == 0) {
    scale_y = (double)viewport.h / g_snes_height;
    scale_x = (double)viewport.w / vis_w;
  } else {
    scale_y = g_settings.hud_scale_percent / 100.0;
    scale_x = scale_y * (g_config.aspect_par_43 ? 7.0 / 6.0 : 1.0);
  }

  int tex_extra = (g_snes_width - 256) / 2;
  int height = g_ppu->wsHudSplitHeight;
  int lower_y = g_ppu->wsHudLeftOnlyY;
  if (lower_y > height) lower_y = height;
  int upper_h = lower_y;
  int upper_dh = ScaledHudPixels(upper_h, scale_y);

  SDL_Rect src = { 0, 0, g_ppu->wsHudLeftEnd, upper_h };
  SDL_Rect dst = { viewport.x, viewport.y,
                   ScaledHudPixels(src.w, scale_x), upper_dh };
  RenderHudChunk(g_hud_bg_texture, src, dst);

  if (g_ppu->wsHudLeftEnd < g_ppu->wsHudRightStart) {
    src.x = tex_extra + g_ppu->wsHudLeftEnd;
    src.w = g_ppu->wsHudRightStart - g_ppu->wsHudLeftEnd;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.x = viewport.x + (viewport.w - dst.w) / 2;
    RenderHudChunk(g_hud_bg_texture, src, dst);
  }

  int right_source_w = 256 - g_ppu->wsHudRightStart;
  int right_dest_w = ScaledHudPixels(right_source_w, scale_x);
  src.x = tex_extra * 2 + g_ppu->wsHudRightStart;
  src.w = right_source_w;
  dst.x = viewport.x + viewport.w - right_dest_w;
  dst.w = right_dest_w;
  RenderHudChunk(g_hud_bg_texture, src, dst);

  if (lower_y < height) {
    src.x = 0;
    src.y = lower_y;
    src.w = 256;
    src.h = height - lower_y;
    dst.x = viewport.x;
    dst.y = viewport.y + ScaledHudPixels(lower_y, scale_y);
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = ScaledHudPixels(src.h, scale_y);
    RenderHudChunk(g_hud_bg_texture, src, dst);
  }

  /* The selected-magic icon is the separately promoted 2x2 OAM signature.
   * Keep it four native pixels before the scaled right group. */
  if (g_hud_obj_texture && g_ppu->wsHudOamCount == 4) {
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
      RenderHudChunk(g_hud_obj_texture, obj_src, obj_dst);
    }
  }
}

static bool RenderFramebuffer(void) {
  if (!g_renderer || !g_texture) return false;
  /* Present only the current mode's sub-rect: the whole framebuffer when
   * wide, the authentic centre 256 in 4:3. The texture allocation never
   * changes while cycling modes. */
  SDL_Rect src = { Settings_VisibleX0(), 0,
                   Settings_VisibleWidth(), g_snes_height };
  SDL_UpdateTexture(g_texture, NULL, g_pixels, g_snes_width * 4);
  if (g_hud_bg_texture)
    SDL_UpdateTexture(g_hud_bg_texture, NULL, g_hud_bg_pixels,
                      g_snes_width * 4);
  if (g_hud_obj_texture)
    SDL_UpdateTexture(g_hud_obj_texture, NULL, g_hud_obj_pixels,
                      g_snes_width * 4);
  ApplyRendererLogicalSize();
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, &src, NULL);

  SDL_Rect viewport = GetPresentationViewport();
  SDL_RenderSetLogicalSize(g_renderer, 0, 0);
  RenderHudOverlay(viewport);
  ApplyRendererLogicalSize();
  return true;
}

static void PresentFramebuffer(void) {
  if (!RenderFramebuffer()) return;
  SDL_RenderPresent(g_renderer);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

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

  const char *rom_path = NULL;
  const char *config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (argv[i][0] != '-') {
      rom_path = argv[i];
    }
  }

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
  if ((!headless || ws_headless) &&
      g_config.extend_aspect_x && g_config.extend_aspect_y) {
    long num = 224L * g_config.extend_aspect_x * (g_config.aspect_par_43 ? 6 : 7);
    long den = 7L * g_config.extend_aspect_y;
    int internal_w = (int)((num + den - 1) / den);
    int extra = internal_w > 256 ? (internal_w - 256 + 1) / 2 : 0;
    if (extra > kWsExtraMax) extra = kWsExtraMax;
    g_ws_extra = extra;
    g_ws_active = extra > 0;
    g_snes_width = 256 + 2 * extra;
    if (g_ws_active && !g_config.new_renderer)
      fprintf(stderr, "[widescreen] NewRenderer=0 unsupported with ExtendedAspectRatio; forcing new PPU\n");
    fprintf(stderr, "[widescreen] %u:%u %s -> %d extra columns/side (render width %d)\n",
            g_config.extend_aspect_x, g_config.extend_aspect_y,
            g_config.aspect_par_43 ? "4:3-PAR" : "square-PAR", g_ws_extra, g_snes_width);
  }
  /* NewRenderer knob (previously parsed but never applied): honor it, except
   * widescreen needs the new PPU path (HUD split/BG3 widen exist only there). */
  g_new_ppu = g_config.new_renderer || g_ws_active;

  /* Seed the live settings from the AR_WS_* env vars (same defaults the gates
   * used before they were migrated), then pick the starting display mode. Must
   * run after the widescreen budget above, which decides g_ws_active. */
  Settings_Init();

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
    int scale = g_config.window_scale ? g_config.window_scale : 3;
    /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
     * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
     * the height), so derive the window from the target ratio, not the
     * framebuffer. Faithful mode keeps the historical g_snes_width*scale. */
    int win_w = g_snes_width * scale;
    if (g_ws_active && g_config.aspect_par_43)
      win_w = (g_snes_height * scale * g_config.extend_aspect_x
               + g_config.extend_aspect_y / 2) / g_config.extend_aspect_y;
    g_window = SDL_CreateWindow(
      kWindowTitle,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      win_w, g_snes_height * scale,
      SDL_WINDOW_RESIZABLE | (headless_video ? SDL_WINDOW_HIDDEN : 0)
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
    if (g_ws_active && !g_config.ignore_aspect_ratio) {
      if (g_config.aspect_par_43)
        SDL_RenderSetLogicalSize(g_renderer, g_snes_width * 7, g_snes_height * 6);
      else
        SDL_RenderSetLogicalSize(g_renderer, g_snes_width, g_snes_height);
    }

    g_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      g_snes_width, g_snes_height);
    if (!g_texture) Die("SDL_CreateTexture failed");

    g_hud_bg_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      g_snes_width, g_snes_height);
    g_hud_obj_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      g_snes_width, g_snes_height);
    if (!g_hud_bg_texture || !g_hud_obj_texture)
      Die("SDL_CreateTexture for HUD overlay failed");
    SDL_SetTextureBlendMode(g_hud_bg_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(g_hud_obj_texture, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  }

  Settings_SetChangeObserver(OnRuntimeSettingChanged);

  g_spc_player = ActRaiserSpcPlayer_Create();

  RtlRegisterGame(&kActRaiserGameInfo);
  Snes *snes = SnesInit(rom_data, (int)rom_size);
  if (!snes) Die("SnesInit failed");

  PpuBeginDrawing(g_ppu, g_pixels, g_snes_width * 4, 0);
  /* Bind explicitly even without video: ppu_init uses malloc, so skipping the
   * call would leave the new optional pointers at their pre-reset values in a
   * pure headless/oracle run. NULL disables extraction deterministically. */
  PpuBeginWidescreenHudOverlay(
      g_ppu,
      g_hud_bg_texture ? g_hud_bg_pixels : NULL,
      g_hud_obj_texture ? g_hud_obj_pixels : NULL,
      g_snes_width * 4);
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
  RtlReadSram();

  WramTraceInit();

  g_audio_mutex = SDL_CreateMutex();
  if (g_config.enable_audio) {
    SDL_AudioSpec want = {0}, have;
    want.freq = g_config.audio_freq ? g_config.audio_freq : 44100;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = g_config.audio_samples ? g_config.audio_samples : 2048;
    want.callback = AudioCallback;
    if (SDL_OpenAudio(&want, &have) == 0) {
      SDL_PauseAudio(0);
    } else {
      fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
    }
  }

  mkdir("saves", 0755);

  /* AR_LOADSTATE=<slot>: load a savestate at boot (before the main loop), so a
   * headless/instrumented run can start from a captured moment instead of
   * replaying from power-on. Runs a few frames first so the game reaches a
   * stable frame boundary, then loads — matches the F7 hotkey path. */
  { const char *ls = getenv("AR_LOADSTATE");
    if (ls && ls[0]) {
      int slot = atoi(ls);
      for (int i = 0; i < 4; i++) { RtlApuLock(); RtlRunFrame(0); RtlApuUnlock(); }
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
          if (event.key.keysym.sym == SDLK_ESCAPE) {
            running = false;
          } else if (event.key.keysym.sym == SDLK_p) {
            g_paused = !g_paused;
          } else if (event.key.keysym.sym == SDLK_t) {
            g_turbo = !g_turbo;
            fprintf(stderr, "[turbo] %s\n", g_turbo ? "ON (8x, AR_TURBO_MULT to change)" : "off");
          } else if (event.key.keysym.sym == SDLK_MINUS ||
                     event.key.keysym.sym == SDLK_KP_MINUS) {
            if (!event.key.repeat) AdjustHudOutputScale(-25);
          } else if (event.key.keysym.sym == SDLK_EQUALS ||
                     event.key.keysym.sym == SDLK_PLUS ||
                     event.key.keysym.sym == SDLK_KP_PLUS) {
            if (!event.key.repeat) AdjustHudOutputScale(25);
          } else if (event.key.keysym.sym == SDLK_F5) {
            RtlSaveLoad(kSaveLoad_Save, 0);
            fprintf(stderr, "State saved.\n");
          } else if (event.key.keysym.sym == SDLK_F7) {
            RtlSaveLoad(kSaveLoad_Load, 0);
            fprintf(stderr, "State loaded.\n");
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
             * map named by AR_WARP=<region_hex><map_hex>. The low byte is $19,
             * not a uniform act number (e.g. Kasandora act 2 is 0303). Press
             * from a transition-capable state; see README + docs/SEAMS.md. */
            extern void ActRaiser_Warp(unsigned region, unsigned map);
            const char *w = getenv("AR_WARP");
            unsigned v = (w && w[0]) ? (unsigned)strtoul(w, NULL, 16) : 0x0101;
            ActRaiser_Warp((v >> 8) & 0xFF, v & 0xFF);
          } else if (event.key.keysym.sym == SDLK_F2) {
            /* On-demand FULL snapshot — each press writes a unique set of files
             * tagged with the game-frame: WRAM + VRAM + CGRAM + OAM (via
             * ActRaiser_FullSnapshot) plus a .ppm screenshot. Lets several
             * moments be grabbed while driving the game manually so the
             * internals (esp. VRAM, where the bridge tiles live) can be watched
             * change over time alongside the picture. */
            /* If F9 and F2 were queued in the same paused host iteration,
             * render the new preset before capturing it. */
            RedrawPausedFrameIfNeeded();
            extern uint8 g_ram[0x20000];
            extern void ActRaiser_FullSnapshot(const char *prefix);
            static int snap_n = 0;
            char snapdir[320];
            RunDirFile(snapdir, sizeof snapdir, "snapshots");
#ifndef _WIN32
            mkdir(snapdir, 0755);  /* keep snapshots out of the
                                    * normal dump_* run data */
#endif
            unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
            char prefix[336];
            RunDirFile(prefix, sizeof prefix, "snapshots/snap_%02d_gf%u",
                       snap_n++, gf);
            ActRaiser_FullSnapshot(prefix);
            char ppm[344]; snprintf(ppm, sizeof ppm, "%s.ppm", prefix);
            FILE *pf = fopen(ppm, "wb");
            if (pf) {
              (void)WriteFramebufferPpm(pf);
              fclose(pf);
            }
            fprintf(stderr, "[snap] F2 -> %s.{wram,vram,cgram,oam,ppm} (gf=%u)\n",
                    prefix, gf);
          } else {
            HandleInput(event.key.keysym.sym, true);
          }
          break;
        case SDL_KEYUP:
          HandleInput(event.key.keysym.sym, false);
          break;
      }
    }

    if (g_paused) {
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
    RtlApuLock();
    bool r = RtlRunFrame(inputs);
    (void)r;
    /* TURBO ('t' toggle): the renderer is created with PRESENTVSYNC, so
     * RenderPresent blocks at 60Hz regardless of the SDL_Delay skip below —
     * which made the original turbo a no-op. Real fast-forward = run extra
     * game frames per RENDERED frame while inside the vsync'd loop. 8x by
     * default (AR_TURBO_MULT overrides). Same input word each sub-frame
     * (level-held buttons repeat; fine for skipping sim waits). Cheats/pins
     * apply inside RtlRunFrame, so they hold during the skipped frames too. */
    if (g_turbo) {
      static int mult = -1;
      if (mult < 0) { const char *e = getenv("AR_TURBO_MULT");
        mult = (e && e[0]) ? atoi(e) : 8; if (mult < 2) mult = 2; }
      for (int tf = 1; tf < mult; tf++) RtlRunFrame(inputs);
    }
    RtlApuUnlock();
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

    /* Auto-persist battery SRAM the moment the game writes a save, so progress
     * survives a freeze/force-quit (the clean-exit RtlWriteSram never runs if
     * the game hangs). Cheap: only writes when the 8KB SRAM actually changes.
     * SKIPPED during input replay: a replay is keyed on the game-frame counter
     * from a fixed boot state, so letting the replayed run overwrite save.srm
     * mid-playthrough would change the boot state for the NEXT replay and break
     * the frame alignment (the recording then no longer reaches the same spot). */
    if (!getenv("AR_INPUT_REPLAY"))
    {
      extern uint8 *g_sram; extern int g_sram_size;
      static uint8 *sram_shadow; static int shadow_size;
      if (g_sram && g_sram_size > 0) {
        if (!sram_shadow || shadow_size != g_sram_size) {
          free(sram_shadow);
          sram_shadow = malloc(g_sram_size); shadow_size = g_sram_size;
          if (sram_shadow) memcpy(sram_shadow, g_sram, g_sram_size);
        } else if (memcmp(sram_shadow, g_sram, g_sram_size) != 0) {
          memcpy(sram_shadow, g_sram, g_sram_size);
          RtlWriteSram();
          fprintf(stderr, "[saves] battery SRAM changed -> wrote saves/save.srm\n");
        }
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

  /* Persist battery save and dump diagnostic state on exit. Skip the SRAM
   * write during replay so a replayed run never mutates save.srm (see the
   * auto-persist note above — it would break the next replay's frame align). */
  if (!getenv("AR_INPUT_REPLAY")) RtlWriteSram();
  DumpDiagState("exit");

  SDL_CloseAudio();
  SDL_DestroyMutex(g_audio_mutex);
  SDL_DestroyTexture(g_hud_obj_texture);
  SDL_DestroyTexture(g_hud_bg_texture);
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  free(rom_data);
  return 0;
}
