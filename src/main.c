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
#include "util.h"
#include "actraiser_spc_player.h"
#include "snes/snes.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "framedump.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static uint8 g_paused, g_turbo;
static uint32 g_input_state;
static int g_snes_width = 256, g_snes_height = 224;
static uint8_t g_pixels[256 * 4 * 240];

extern Snes *g_snes;
extern Ppu *g_ppu;
struct SpcPlayer *g_spc_player;

extern const RtlGameInfo kActRaiserGameInfo;

bool g_new_ppu = true;

static SDL_mutex *g_audio_mutex;

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

void OpenGLRenderer_Create(struct RendererFuncs *funcs) {
  (void)funcs;
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

/* Differential-oracle capture: emit per-frame WRAM changes as JSONL in the
 * exact shape snesref (tools/oracle) emits, so the two traces can be diffed
 * to find the first divergence (frame + WRAM byte). Enabled by AR_WRAM_TRACE
 * (output path); range via AR_TRACE_LO/HI (default full 128KB). */
static FILE *g_wram_trace;
static uint8_t g_wram_prev[0x20000];
static bool g_wram_primed;
static uint32_t g_wram_lo = 0x00000, g_wram_hi = 0x1ffff;

static void WramTraceCallback(uint32_t frame, const uint8_t *wram) {
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

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  cpu_trace_init();

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

  Uint32 sdl_flags = SDL_INIT_AUDIO;
  if (!headless) sdl_flags |= SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (SDL_Init(sdl_flags) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (!headless) {
    int scale = g_config.window_scale ? g_config.window_scale : 3;
    g_window = SDL_CreateWindow(
      kWindowTitle,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      g_snes_width * scale, g_snes_height * scale,
      SDL_WINDOW_RESIZABLE
    );
    if (!g_window) Die("SDL_CreateWindow failed");

    g_renderer = SDL_CreateRenderer(g_window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) Die("SDL_CreateRenderer failed");

    g_texture = SDL_CreateTexture(g_renderer,
      SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      g_snes_width, g_snes_height);
    if (!g_texture) Die("SDL_CreateTexture failed");

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  }

  g_spc_player = ActRaiserSpcPlayer_Create();

  RtlRegisterGame(&kActRaiserGameInfo);
  Snes *snes = SnesInit(rom_data, (int)rom_size);
  if (!snes) Die("SnesInit failed");

  PpuBeginDrawing(g_ppu, g_pixels, g_snes_width * 4, 0);

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
          } else if (event.key.keysym.sym == SDLK_F5) {
            RtlSaveLoad(kSaveLoad_Save, 0);
            fprintf(stderr, "State saved.\n");
          } else if (event.key.keysym.sym == SDLK_F7) {
            RtlSaveLoad(kSaveLoad_Load, 0);
            fprintf(stderr, "State loaded.\n");
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
    RtlApuLock();
    bool r = RtlRunFrame(inputs);
    (void)r;
    RtlApuUnlock();

    RtlDrawPpuFrame();

    if (!headless) {
      SDL_UpdateTexture(g_texture, NULL, g_pixels, g_snes_width * 4);
      SDL_RenderClear(g_renderer);
      SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
      SDL_RenderPresent(g_renderer);
    }

    if (headless) {
      extern int snes_frame_counter;
      static int quit_frames = -2;
      if (quit_frames == -2) { const char *q = getenv("AR_QUIT_FRAMES");
        quit_frames = q ? atoi(q) : -1; }
      if (quit_frames > 0 && snes_frame_counter >= quit_frames) running = false;
    } else if (!g_turbo) {
      uint32 now = SDL_GetTicks();
      uint32 elapsed = now - last_tick;
      if (elapsed < 16)
        SDL_Delay(16 - elapsed);
      last_tick = SDL_GetTicks();
    }
  }

  SDL_CloseAudio();
  SDL_DestroyMutex(g_audio_mutex);
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();
  free(rom_data);
  return 0;
}
