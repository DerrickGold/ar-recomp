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

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

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

  g_spc_player = ActRaiserSpcPlayer_Create();

  RtlRegisterGame(&kActRaiserGameInfo);
  Snes *snes = SnesInit(rom_data, (int)rom_size);
  if (!snes) Die("SnesInit failed");

  PpuBeginDrawing(g_ppu, g_pixels, g_snes_width * 4, 0);

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
      if (force_after == -2) { force_env = getenv("AR_FORCE_INPUT_AFTER");
        force_after = force_env ? atoi(force_env) : -1; }
      if (force_after >= 0) {
        extern int snes_frame_counter;
        if (snes_frame_counter >= force_after) inputs |= 0x0001; /* B */
      }
    }
    RtlApuLock();
    bool r = RtlRunFrame(inputs);
    (void)r;
    RtlApuUnlock();

    RtlDrawPpuFrame();

    SDL_UpdateTexture(g_texture, NULL, g_pixels, g_snes_width * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);

    if (!g_turbo) {
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
