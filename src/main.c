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

#include "action/action_obj_apron.h"
#include "snes/ppu.h"
#include "types.h"
#include "actraiser_rtl.h"
#include "common_cpu_infra.h"
#include "config.h"
#include "crt_post.h"
#include "settings.h"
#include "settings_overlay.h"
#include "input_map.h"
#include "dev/scene_inspector.h"
#include "diorama/diorama.h"
#include "diorama/diorama_scroll_math.h"  /* kInterpPhaseNone */
#include "forced_input.h"
#include "save_system.h"
#include "hd_replacement_host.h"
#include "music_replacements.h"
#include "dev/sfx_census.h"
#include "run_dir.h"
#include "launcher.h"
#include "util.h"
#include "actraiser/actraiser_spc_player.h"
#include "actraiser_game.h"
#include "snes/snes.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "widescreen.h"
#include "present.h"
#include "frame_slot.h"
#include "host/host_audio.h"
#include "dev/host_dev_tools.h"
#include "host/host_display.h"
#include "host/host_input.h"
#include "manual/manual_reader.h"
#include "ini_upgrade_apply.h"
#include "input_replay.h"
#include "dev/oracle_trace.h"
#include "portable_paths.h"
#include "randomizer.h"
#include "runtime_settings.h"
#include "runtime_diagnostics.h"
#include "scheduled_settings.h"
#include "user_data_dir.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_render_metadata.h"
#include "sim/sim_world_map_build.h"
#include "sim/sim_visual_patches.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_world_map.h"
#include "sim/sim3d.h"

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
/* InspectorPresentationKind/InspectorPresentationSelection now live in
 * present.h (D4) — shared between this file's InspectWindowPoint (live
 * hit-test) and present.c's renderer (fed from the FrameSlot snapshot). */
/* external: read by FrameSlot_Capture (frame_slot.c) */
InspectorPresentationSelection g_scene_inspector_presentation;
static bool g_window_hidden;  /* true while MINIMIZED or HIDDEN: skip present */
/* external: read by FrameSlot_Capture (frame_slot.c) */
int g_snes_width = kActRaiserAuthenticWidth,
    g_snes_height = kActRaiserAuthenticHeight;
/* Framebuffer sized for the PPU's full widescreen budget (448 wide) so the
 * active width can change live without reallocating storage; each frame uses
 * only the leading g_snes_width*4 bytes per row. Rows follow the same rule on
 * the other axis: capacity for the full vertical margin band, of which a frame
 * uses only 224 + g_ws_extra_top + g_ws_extra_bottom. */
_Static_assert(kHostDisplayFramebufferHeight >= kPpuBufHeight,
               "frame surfaces must hold every row the PPU can render");
uint8_t g_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_bg_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_obj_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];

/* Diorama per-plane capture buffers, indexed by kDioramaPlane_* (engine
 * sources = the priority-0 remainder of each layer, appended entries = the
 * priority-band splits; see diorama_planes.h). Dedicated set separate from
 * the HUD/HD overlay buffers (BG3/OBJ reuse those for the widescreen HUD
 * split, and HD replacements claim per-source capture slots — see §4.3).
 * Allocated lazily on first diorama capture (actraiser_rtl.c) and released at
 * shutdown. BG4 is never drawn in Mode 1, so excluded; the backdrop slot
 * stays NULL (RenderDiorama points it at g_pixels). */
uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
bool g_diorama_dump_pending;
bool g_diorama_frame_active;
SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
SDL_Texture *g_sim_obj_atlas_texture;
SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
SDL_Texture *g_sim3d_flat_texture;
bool g_sim3d_textures_ready;

static void DestroyDioramaTextures(void) {
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    SDL_DestroyTexture(g_diorama_textures[i]);
    g_diorama_textures[i] = NULL;
  }
}

static void CreateDioramaTextures(void) {
  /* Allocated at the PPU's full render-target size on BOTH axes, for the same
   * reason: kPpuBufWidth already covered every widescreen margin without a
   * realloc, and kPpuBufHeight now does the same for the vertical band. Only
   * the leading snes_width x (snes_height + ws_extra_top) region is uploaded
   * each frame; Diorama_Composite's UV window is expressed against these
   * allocated dimensions. */
  uint8_t *zero_fill =
      calloc(1, (size_t)kPpuSurfaceWidth * kPpuBufHeight * 4);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    if (i == kPpuOverlaySource_Bg4)
      continue;
    g_diorama_textures[i] = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kPpuSurfaceWidth, kPpuBufHeight);
    if (!g_diorama_textures[i])
      continue;
    SDL_SetTextureBlendMode(g_diorama_textures[i],
        i == kDioramaPlane_Backdrop ? SDL_BLENDMODE_NONE
                                    : SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_diorama_textures[i], SDL_SCALEMODE_NEAREST);
    if (zero_fill)
      SDL_UpdateTexture(g_diorama_textures[i], NULL, zero_fill,
                        kPpuSurfaceWidth * 4);
  }
  free(zero_fill);
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
/* The vertical transpose of g_ws_extra: scanlines the PPU renders above line 0
 * and below line 223 this frame, already clamped to real world space by
 * ActRaiser_ApplyWidescreenPolicy. Diorama-only (nothing in the flat path is
 * prepared for a non-zero frame origin), and 0 restores authentic 224-line
 * output everywhere.
 *
 * g_ws_extra_bottom stays 0 for now and is not merely unwired: OAM Y is 8-bit
 * with a 256 modulus against a 224-line screen, so a sprite below the screen is
 * indistinguishable from one above it and the game's object coordinates carry
 * no usable data down there. The top band has no such problem -- those
 * positions are already what OAM encodes -- which is why it is the half that
 * ships. See kPpuExtraTopBottom. */
int g_ws_extra_top;
int g_ws_extra_bottom;

extern Snes *g_snes;
extern Ppu *g_ppu;
struct SpcPlayer *g_spc_player;

extern const RtlGameInfo kActRaiserGameInfo;

bool g_new_ppu = true;

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

static void RtlDrawPpuFrame(void) {
  g_rtl_game_info->draw_ppu_frame();
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

  uint32 inputs = HostInput_ComputeGameInputs(stop_running);

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
  if (HostInput_IsTurbo()) {
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
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
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
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
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
  /* Own the developed world tilemap instead of observing $7E:C000, which acts
   * and towns both reuse as unrelated scratch. This runs only on the game
   * thread, after an emulated tick reached a stable frame boundary. */
  SimWorldMap_BuildIfNeeded();
  /* #16: function-scope so the annotated sim outlives the block below and can
   * be published to FrameSlot_Capture around the HostDisplay_SubmitFrame tail. */
  SimFrameData sim;
  {
    extern int snes_frame_counter;
    SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram, g_ppu);
    SimRenderMetadata_CaptureFrame(
        &sim, g_ram, g_settings.sim3d_mode,
        g_settings.sim3d_world_navigation,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&sim, &tuning);
    SimWorldNavigationCapture_Capture(&sim, g_ppu);
    /* This site runs on every frame including headless, unlike
     * FrameSlot_Capture, whose only caller is the dev-tools snapshot path
     * (dev_tools.c) and is gated on a live renderer. */
    Sim3D_RenderTownCanvas(&sim, g_ram, g_ppu);
    sim.town_canvas_serial = SimTownCanvas_Serial();
    Sim3D_LogViewTransition(&sim);
    SceneInspector_SetSimFrameData(&sim);
    /* g_pixels is bound apron-wide; offset past the apron so the trace sees
     * the authentic frame at column 0, as it always has. */
    SimRenderMetadata_TraceFrame(
        (uint32)snes_frame_counter, &sim,
        g_pixels + ActionApron_DisplayOffset(kPpuObjApron),
        g_snes_width, g_snes_height,
        ActionApron_SurfacePitch(g_snes_width, kPpuObjApron));
  }
  /* AR_DIORAMA_DUMP_GF=<gf>[,<gf>...]: arm the Shift+D layer dump from a replay
   * instead of the keyboard, so a diorama frame can be inspected headlessly.
   * The PNGs keep the captured ALPHA, which is what makes F4's half-add
   * annotation verifiable without looking at the screen. Same shape as
   * AR_VRAMDUMP_GF. */
  {
    static const char *dump_list = NULL;
    static bool dump_list_read;
    static unsigned last_dumped_gf = (unsigned)-1;
    if (!dump_list_read) {
      dump_list_read = true;
      dump_list = getenv("AR_DIORAMA_DUMP_GF");
    }
    if (dump_list && dump_list[0]) {
      const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      if (gf != last_dumped_gf) {
        for (const char *at = dump_list; at && *at;) {
          if ((unsigned)strtoul(at, NULL, 0) == gf) {
            last_dumped_gf = gf;
            g_diorama_dump_pending = true;
            break;
          }
          const char *comma = strchr(at, ',');
          at = comma ? comma + 1 : NULL;
        }
      }
    }
  }
  if (g_diorama_dump_pending) {
    HostDevTools_DumpDioramaLayers();
    g_diorama_dump_pending = false;
    if (!g_settings.diorama_mode)
      ActRaiser_RebindPpuOutputSurfaces();
  }
  HostInput_MarkFrameDrawn();
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
  {
    const unsigned gf =
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
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
        const SDL_Point shot_size =
            HostDevTools_WriteFramebufferPpm(pf);
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
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      if (gf >= (unsigned)warp_at) {
        warp_fired = true;
        (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
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
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
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
  Sim3DCamera_FlushSettingsIfDirty();

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

/* One application-level host-pause edge owns both transport layers. The order
 * matters: stop the device before latching the OGG decoder, then release the
 * decoder before resuming the device, so no callback can advance only one
 * source across the edge. */
static void ApplyHostAudioPause(bool paused) {
  if (paused) HostAudio_SetHostPaused(true);
  MusicReplacements_SetHostPaused(paused);
  if (!paused) HostAudio_SetHostPaused(false);
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

  /* Upgrade step, BEFORE anything reads a config file: merge the bundle's
   * shipped defaults into the user's live copies, keeping every value they
   * changed and adding only what is new in this version. A no-op in a developer
   * checkout (no defaults/ directory) and silent when nothing changed.
   *
   * Here rather than only in the builder GUI because run-game starts the game
   * directly, so a GUI-only upgrade would never run for those users. */
  IniUpgrade_ApplyShippedDefaults();

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
     * HostDisplay_WindowPointToOutput already maps window points to output
     * pixels. */
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
     * env var per the usual priority chain.
     *
     * The "gpu" backend is requested WITH PROPERTIES, not by name alone,
     * because SDL picks the underlying GPU backend (Vulkan / Metal / D3D12)
     * partly from the shader formats the app declares it can supply. We ship
     * SPIR-V and MSL (src/shaders/, see diorama.c) and deliberately do NOT
     * claim DXIL, so SDL will choose Vulkan or Metal — backends this build
     * can actually feed — instead of landing on D3D12 and leaving every
     * effect silently disabled. Declaring nothing here, as this code did
     * before, is exactly how the effects came to be macOS-only in practice. */
    g_gpu_shaders_requested = g_settings.gpu_shaders_enabled;
    if (headless_video) {
      g_renderer = SDL_CreateRenderer(g_window, SDL_SOFTWARE_RENDERER);
    } else if (g_gpu_shaders_requested) {
      SDL_PropertiesID renderer_props = SDL_CreateProperties();
      if (renderer_props) {
        SDL_SetStringProperty(renderer_props,
            SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
        SDL_SetPointerProperty(renderer_props,
            SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, g_window);
        SDL_SetBooleanProperty(renderer_props,
            SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(renderer_props,
            SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
        g_renderer = SDL_CreateRendererWithProperties(renderer_props);
        SDL_DestroyProperties(renderer_props);
      } else {
        g_renderer = NULL;
      }
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
      kPpuSurfaceWidth, g_snes_height);
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
      kPpuSurfaceWidth, g_snes_height);
    g_hud_obj_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPpuSurfaceWidth, g_snes_height);
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

    HdReplacementHost_LoadTextures();

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
    CreateDioramaTextures();

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
  /* The world-map image and pure development-builder tables are immutable ROM
   * data. Failure is not fatal: consumers retain the authentic presentation. */
  if (SimWorldMap_Init(rom_data, rom_size))
    SimWorldMapBuild_Init(rom_data, rom_size);
  /* Per-room diorama layer overrides. Absent file is the normal case and leaves
   * every room drawing as built. */
  Diorama_LoadLayerManifest();
  SettingsOverlay_SetInspectorInfoProvider(
      HostDevTools_FormatInspectorInfo);
  /* The layer editor (Settings > Layers, developer-only) edits the override
   * table loaded above and writes the manifest back. Injected rather than called
   * directly from the overlay so that file stays testable without diorama.c --
   * see settings_overlay.h. */
  SettingsOverlay_SetLayerEditorHooks(Diorama_LayerOverrides, Diorama_LiveRoom,
                                      Diorama_SaveLayerManifest);

  /* The in-game manual, injected for the same reason: it owns textures and an
   * image decoder, and the overlay's own test links settings_overlay.c with no
   * renderer at all. Nothing is loaded here -- the availability hook reads and
   * indexes the file when the overlay first needs it; page textures remain lazy
   * until the player opens the reader. */
  static const SettingsOverlayManualHooks kManualHooks = {
    .available = ManualReader_Available,
    .is_open = ManualReader_IsOpen,
    .close = ManualReader_Close,
    .render = ManualReader_Render,
    .handle_key = ManualReader_HandleKey,
    .handle_pad = ManualReader_HandleGamepadEvent,
  };
  SettingsOverlay_SetManualHooks(&kManualHooks);

  RuntimeSettings_Install();
  /* After the action observer is installed: the pad's save/load-state
   * bindings route through it. */
  InputMap_Init();
  HostInput_InstallActionHandler();
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

  /* Keep deterministic visual source-data adjustments below cart_load (which
   * copies rom_data) and above Randomizer_Init (which snapshots the live cart
   * as its non-randomized restore baseline). A signature mismatch is safe but
   * important: it means effects metadata and the running visual script would
   * no longer share the investigated USA-ROM contract. */
  if (!SimVisualPatches_Apply(snes->cart->rom, snes->cart->romSize))
    fprintf(stderr,
            "[sim-visuals] house-fire cadence patch skipped: "
            "unexpected ROM signature\n");

  /* Randomizer: cart_load COPIES the image, so the buffer the game actually
   * reads is the cart's, not rom_data. Register that one and apply before the
   * game coroutine starts. Its pristine snapshot deliberately includes the
   * deterministic visual adjustments above, so later option changes restore
   * a stable non-randomized baseline instead of erasing them. */
  if (Randomizer_Init(snes->cart->rom, snes->cart->romSize))
    Randomizer_Apply();

  HdReplacementHost_BindSurfaces();
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
    bool staged = RuntimeSettings_BuildSaveEditRequest(&edits);
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
  /* A replay must not mutate the player's configuration, for the same reason it
   * refuses to persist SRAM. Set from the same predicate so the two protections
   * cannot drift apart. */
  Settings_SetPersistenceEnabled(!InputReplay_ShouldProtectSaveData());
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
      FrameSlot_ResetActionEffects();
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
          HostInput_RequestPausedRedraw();
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
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
          g_window_hidden = true;
          break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
          g_window_hidden = false;
          HostInput_RequestPausedRedraw();
          break;
        /* GPU device/target reset: STATIC textures lose their contents and
         * must be recreated — both the HD replacements and the settings
         * overlay's atlases (fonts/icons/dialog frame, uploaded once at
         * Init). DEVICE_LOST is unrecoverable. */
        case SDL_EVENT_RENDER_TARGETS_RESET:
        case SDL_EVENT_RENDER_DEVICE_RESET:
          if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            Diorama_ResetRendererResources(g_renderer);
            DestroyDioramaTextures();
            CreateDioramaTextures();
          }
          /* Manual pages are STATIC textures too. Drop their cache before any
           * stale pointer can be mistaken for a resident page; the next manual
           * frame re-decodes from the retained PDF bytes under its normal budget. */
          ManualReader_DestroyTextures();
          HdReplacementHost_ReloadTextures();
          if (!SettingsOverlay_ReloadTextures(rom_data, rom_size))
            fprintf(stderr,
                    "[settings-menu] atlas reload after device reset failed\n");
          /* The sim-3D caches are serial-gated on GAME state, so they would
           * never notice the reset and would keep presenting discarded
           * contents for the rest of the session (a settled town never bumps
           * the underlay serial). Drop them so the next present re-bakes. */
          PresentRendererResources_Reset();
          HostInput_RequestPausedRedraw();
          /* R17/C2: the retained re-present slot copies hd_entries[].texture
           * as raw SDL_Texture* (present.h). The host reload just destroyed
           * and recreated every one of them, so those copies are
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
          /* Steam Input can emit a keyboard mapping and a native gamepad
           * event for one physical control. Auto mode gives the live gamepad
           * event ownership, including host hotkeys, so the synthesized key
           * cannot perform a second action. Key-up is still processed below
           * to ensure a previously accepted key can never stick. */
          if (HostInput_KeyboardIsSuppressed()) break;
          if (SettingsOverlay_IsOpen()) {
            /* Only the menu's active device drives navigation, so one
             * physical press (+ its synthesized twin) moves the menu once. */
            if (HostInput_MenuKeyboardIsActive()) {
              bool was_open = true;
              bool consumed = SettingsOverlay_HandleKey(
                  event.key.key, true, event.key.repeat != 0);
              if (was_open && !SettingsOverlay_IsOpen())
                HostInput_ClearHeld();
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
            HostInput_ClearHeld();
            SettingsOverlay_Open();
          } else if (event.key.key == SDLK_P) {
            if (SceneInspector_HasSelection()) {
              const bool inspector_owned_pause =
                  HostInput_InspectorOwnsPause();
              HostInput_CloseInspectorSelection();
              if (!inspector_owned_pause) HostInput_TogglePause();
            } else {
              HostInput_TogglePause();
            }
          } else if (event.key.key == SDLK_T) {
            HostInput_ToggleTurbo();
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
            if (!event.key.repeat)
              HostDevTools_AdjustHudOutputScale(-25);
          } else if (event.key.key == SDLK_EQUALS ||
                     event.key.key == SDLK_PLUS ||
                     event.key.key == SDLK_KP_PLUS) {
            if (!event.key.repeat)
              HostDevTools_AdjustHudOutputScale(25);
          } else if (event.key.key == SDLK_F5) {
            (void)RuntimeSettings_HandleAction(Settings_Find("save_state"));
          } else if (event.key.key == SDLK_F7) {
            (void)RuntimeSettings_HandleAction(Settings_Find("load_state"));
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
             * from a transition-capable state; see docs/manual.md + docs/SEAMS.md. */
            (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
          } else if (event.key.key == SDLK_F2 || event.key.key == SDLK_C) {
            /* C is the one-hand alias for F2, deliberately NOT gated on
             * `!event.key.repeat`: holding it fires on every key repeat, which
             * is how you sweep a glitch that only shows for a frame or two
             * without knowing its game-frame up front. Each press is still a
             * full snapshot (~21 MB of .ppm alone), so a long hold writes GBs
             * — for a REPLAY prefer the deterministic sweep, which captures
             * exact frames and costs nothing to repeat:
             *   AR_INPUT_REPLAY=<rec> AR_SHOT_EVERY=1 AR_SHOT_FROM=a AR_SHOT_TO=b
             *   AR_INPUT_REPLAY=<rec> AR_VRAMDUMP_GF=g1,g2,...
             * Use the key to find the moment, those to pin it. */
            /* On-demand FULL snapshot — each press writes a unique set of files
             * tagged with the game-frame: WRAM + VRAM + CGRAM + OAM (via
             * ActRaiser_FullSnapshot) plus a .ppm screenshot. Lets several
             * moments be grabbed while driving the game manually so the
             * internals (esp. VRAM, where the bridge tiles live) can be watched
             * change over time alongside the picture. */
            /* If F9 and F2 were queued in the same paused host iteration,
             * render the new preset before capturing it. */
            HostDevTools_TakeFullSnapshot();
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
              HostInput_RequestPausedRedraw();
            }
          } else {
            HostInput_HandleKeyboard(event.key.scancode, true);
          }
          break;
        case SDL_EVENT_TEXT_INPUT:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleText(event.text.text);
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          /* The manual reader is modal and takes the mouse whole -- click to
           * turn, drag to pan, wheel to zoom. Checked before every camera and
           * inspector path below, all of which are gated on the overlay being
           * CLOSED and so would otherwise silently ignore the reader. */
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          /* Diorama owns right-drag (orbit) and middle-click (reset) while it
           * is on screen; §8.7 disables click-inspect in diorama for v1
           * because the flat hit-testing does not follow the tilted planes. */
          if (!SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame()) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              Diorama_SetDragging(true);
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              Diorama_ResetCamera();
          } else if (!SettingsOverlay_IsOpen() &&
                     Sim3DCamera_ControlsAvailable(
                         g_sim3d_textures_ready)) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              Sim3DCamera_SetDragging(true);
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              HostInput_ResetSim3DCamera();
          } else if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
              HostInput_CloseInspectorSelection();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
              /* SDL3 mouse event coordinates are floats; the hit-testing works
               * at SNES-pixel granularity, so truncating to int is exact. */
              int event_x = (int)event.button.x;
              int event_y = (int)event.button.y;
              int output_x = 0, output_y = 0;
              if (!HostDisplay_WindowPointToOutput(
                      event_x, event_y, &output_x, &output_y) ||
                  !SettingsOverlay_BeginDebugPanelDrag(
                      output_x, output_y))
                (void)HostDevTools_InspectWindowPoint(event_x, event_y);
            }
          }
          break;
        case SDL_EVENT_MOUSE_MOTION:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          if (Diorama_IsDragging() && Diorama_IsActiveThisFrame()) {
            Diorama_AdjustCamera(event.motion.xrel * Diorama_DragRadPerPx(),
                                 event.motion.yrel * Diorama_DragRadPerPx(),
                                 0.0f);
          } else if (Sim3DCamera_IsDragging() &&
                     Sim3DCamera_ControlsAvailable(
                         g_sim3d_textures_ready)) {
            HostInput_AdjustSim3DCamera(
                event.motion.xrel * Diorama_DragRadPerPx(),
                event.motion.yrel * Diorama_DragRadPerPx(),
                0.0f);
          } else if (SettingsOverlay_IsDebugPanelDragging()) {
            int output_x = 0, output_y = 0;
            if (HostDisplay_WindowPointToOutput(
                    (int)event.motion.x, (int)event.motion.y,
                    &output_x, &output_y))
              SettingsOverlay_DragDebugPanel(output_x, output_y);
          }
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          /* Wheel up zooms in, i.e. decreases the camera distance. */
          if (!SettingsOverlay_IsOpen() && Diorama_IsActiveThisFrame())
            Diorama_AdjustCamera(0.0f, 0.0f,
                                 -event.wheel.y * Diorama_ZoomStep());
          else if (!SettingsOverlay_IsOpen() &&
                   Sim3DCamera_ControlsAvailable(
                       g_sim3d_textures_ready))
            HostInput_AdjustSim3DCamera(
                0.0f, 0.0f, -event.wheel.y * Diorama_ZoomStep());
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          if (event.button.button == SDL_BUTTON_RIGHT) {
            Diorama_SetDragging(false);
            Sim3DCamera_SetDragging(false);
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
            if (HostInput_MenuGamepadIsActive())
              (void)SettingsOverlay_HandleGamepadEvent(&event);
            break;
          }
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_KEY_UP:
          if (SettingsOverlay_IsOpen()) {
            if (HostInput_MenuKeyboardIsActive())
              (void)SettingsOverlay_HandleKey(event.key.key, false, false);
          } else {
            HostInput_HandleKeyboard(event.key.scancode, false);
          }
          break;
      }
    }

    if (RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None) {
      running = false;
      continue;
    }

    HostInput_ApplyAnalogCamera();

    /* Host-owned pauses do not issue the game's native SPC $F2 command. The
     * coordinator above freezes authentic music, replacement music, and SFX. */
    const bool host_paused =
        HostInput_IsPaused() || SettingsOverlay_IsOpen();
    ApplyHostAudioPause(host_paused);

    if (host_paused) {
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
      HostInput_RedrawPausedFrameIfNeeded();
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
                       HostInput_IsPausedRedrawPending())) {
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
  DumpDiagState(RuntimeSettings_LifecycleRequest() ==
                        kRuntimeLifecycle_Restart
                    ? "restart" : "exit");
  SimPhase0Trace_Close();
  SimRenderMetadata_TraceClose();

  /* Before tearing down audio: the census reads only its own accumulators,
   * but the report should land while the run dir is still current. */
  SfxCensus_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  HostAudio_Shutdown();
  HdReplacementHost_Shutdown();
  PresentRendererResources_Reset();
  Diorama_Shutdown(g_renderer);
  DestroyDioramaTextures();
  SDL_DestroyTexture(g_sim_obj_atlas_texture);
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    SDL_DestroyTexture(g_sim3d_layer_textures[plane]);
  SDL_DestroyTexture(g_sim3d_flat_texture);
  ManualReader_DestroyTextures();
  SettingsOverlay_Destroy();
  /* Release the game coroutine's stack mapping / fiber. Safe here: the game
   * thread is this thread and the main loop has exited, so nothing can be
   * running on that stack. */
  ActRaiser_DestroyGameCoroutine();
  InputMap_Shutdown();
  SDL_DestroyTexture(g_hud_obj_texture);
  SDL_DestroyTexture(g_hud_bg_texture);
  SDL_DestroyTexture(g_texture);
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    free(g_diorama_layer_pixels[plane]);
    g_diorama_layer_pixels[plane] = NULL;
  }
  /* Owns a full-window render target plus a GPU shader and render state, and
   * all three must go before the renderer that created them. */
  CrtPost_Shutdown();
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  free(rom_data);

  if (RuntimeSettings_LifecycleRequest() == kRuntimeLifecycle_Restart) {
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
