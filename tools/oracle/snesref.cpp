/* snesref (macOS port) — minimal SDL2 libretro frontend used as the
 * differential oracle for the ActRaiser recompilation.
 *
 * Loads a libretro SNES core (e.g. snes9x_libretro.dylib), runs a ROM on a
 * known-good interpreter, and logs per-frame WRAM changes as JSONL — the
 * exact shape the recomp side emits — so the two traces can be diffed to
 * pinpoint the first divergence (frame + WRAM byte).
 *
 *   snesref <core.dylib> <rom.sfc>
 *
 * Ported from third_party/snesrecomp/tools/snesref/frontend.cpp:
 *   - Win32 LoadLibrary/GetProcAddress -> dlopen/dlsym
 *   - MMX-specific trace addresses -> configurable full-WRAM range
 *   - added SNESREF_HEADLESS (no window, run uncapped) for CI/deterministic
 *     capture, and SNESREF_FORCE_B_AFTER (scripted B press) mirroring the
 *     recomp's AR_FORCE_INPUT_AFTER.
 *
 * Env vars:
 *   SNESREF_TRACE_FILE    output JSONL (default oracle_trace.jsonl)
 *   SNESREF_TRACE_LO/HI   WRAM byte range to watch (hex, default 0..0x1ffff)
 *   SNESREF_QUIT_FRAMES   stop after N frames
 *   SNESREF_FORCE_B_AFTER hold the B button from frame N onward
 *   SNESREF_HEADLESS      1 = no window, run as fast as possible
 *   SNESREF_WAV           audio dump path (default snesref_audio.wav)
 */
#include <SDL.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "libretro.h"

// ---- core function pointers ----
static void* g_core;
#define LR(sym) static decltype(&sym) p_##sym;
LR(retro_init) LR(retro_deinit) LR(retro_api_version)
LR(retro_get_system_info) LR(retro_get_system_av_info)
LR(retro_set_environment) LR(retro_set_video_refresh)
LR(retro_set_audio_sample) LR(retro_set_audio_sample_batch)
LR(retro_set_input_poll) LR(retro_set_input_state)
LR(retro_set_controller_port_device)
LR(retro_load_game) LR(retro_unload_game) LR(retro_run)
LR(retro_serialize_size) LR(retro_serialize) LR(retro_unserialize)
LR(retro_get_memory_data) LR(retro_get_memory_size)
#undef LR

template<class T> static void bind(T& fn, const char* name) {
    fn = (T)dlsym(g_core, name);
    if (!fn) { fprintf(stderr, "missing core symbol: %s\n", name); exit(2); }
}

// ---- config (env) ----
static bool     g_headless = false;
static long     g_force_b_after = -1;   // hold B from this frame onward
static uint32_t g_trace_lo = 0x00000;
static uint32_t g_trace_hi = 0x1ffff;

// ---- video state ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static SDL_GameController* g_pad = nullptr;

static void open_first_pad() {
    if (g_pad || g_headless) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { printf("[controller: %s]\n", SDL_GameControllerName(g_pad)); fflush(stdout); return; }
        }
    }
}

// ---- WRAM trace: diff a configurable byte range each frame, emit changes ----
static FILE*    g_log;
static uint8_t* g_prev = nullptr;     // snapshot of [g_trace_lo, g_trace_hi]
static bool     g_primed = false;
static uint32_t g_frame = 0;

static void emit(uint32_t addr, uint8_t o, uint8_t n) {
    if (!g_log) { const char* p=getenv("SNESREF_TRACE_FILE"); g_log=fopen(p&&p[0]?p:"oracle_trace.jsonl","a"); if(!g_log) return; }
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%05x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}

static void trace_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || sz <= g_trace_hi) return;
    uint32_t span = g_trace_hi - g_trace_lo + 1;
    if (!g_prev) g_prev = (uint8_t*)malloc(span);
    if (!g_primed) {
        memcpy(g_prev, ram + g_trace_lo, span);
        g_primed = true; return;
    }
    for (uint32_t i = 0; i < span; i++) {
        uint8_t v = ram[g_trace_lo + i];
        if (v != g_prev[i]) { emit(g_trace_lo + i, g_prev[i], v); g_prev[i] = v; }
    }
    if (g_log && (g_frame % 30) == 0) fflush(g_log);
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if(data) *(bool*)data=false; return true;
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w==g_tex_w && (int)h==g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt==RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt==RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                        SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w=w; g_tex_h=h;
}
static void maybe_shot(const void* data, unsigned w, unsigned h, size_t pitch);
static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    maybe_shot(data, w, h, pitch);
    if (g_headless || !g_ren) return;
    if (data && w && h) {
        ensure_texture(w,h);
        SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch);
    }
    SDL_RenderClear(g_ren);
    if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}

// ---- audio capture (always-on WAV dump) ----
static FILE*    g_wav;
static uint64_t g_wav_sample_frames;
static uint32_t g_wav_rate = 32040;

static void wav_open(const char* path, double rate) {
    g_wav = fopen(path, "wb");
    if (!g_wav) { fprintf(stderr, "cannot open %s\n", path); return; }
    g_wav_rate = (uint32_t)(rate + 0.5);
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, g_wav);
}
static void wav_close() {
    if (!g_wav) return;
    uint32_t data_bytes = (uint32_t)(g_wav_sample_frames * 4);
    uint32_t riff = 36 + data_bytes, fmt32 = 16, brate = g_wav_rate * 4;
    uint16_t pcm = 1, ch = 2, balign = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav);
    fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt32, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
    fwrite(&g_wav_rate, 4, 1, g_wav); fwrite(&brate, 4, 1, g_wav);
    fwrite(&balign, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data_bytes, 4, 1, g_wav);
    fclose(g_wav); g_wav = nullptr;
    printf("[wav closed: %llu frames @ %u Hz]\n",
           (unsigned long long)g_wav_sample_frames, g_wav_rate);
}
static void  cb_audio_sample(int16_t l, int16_t r) {
    if (g_wav) { int16_t s[2] = {l, r}; fwrite(s, 4, 1, g_wav); g_wav_sample_frames++; }
}
static size_t cb_audio_batch(const int16_t* data, size_t frames) {
    if (g_wav && data && frames) { fwrite(data, 4, frames, g_wav); g_wav_sample_frames += frames; }
    return frames;
}
static void  cb_input_poll(void) {}

// Game-frame-indexed input replay (SNESREF_INPUT_REPLAY=<file>; recomp
// AR_INPUT_RECORD format: repeating 8-byte LE records {uint32 gframe; uint32 inputs},
// SNES 12-bit layout == libretro JOYPAD id order). Keyed by the game's logical
// frame counter $7E:0088 (WRAM offset 0x88, 16-bit), which advances identically
// here and in the recomp for identical input — so the recording aligns frame-exact
// without any host-frame offset.
static uint32_t* g_replay = nullptr;   // dense, indexed by game-frame
static long g_replay_max = -1;
static const uint8_t* g_sysram = nullptr;
static long g_recomp_entry_gf = -1;    // SNESREF_ENTRY_GF: recomp's $18==01 gf
static long g_oracle_entry_gf = -1;    // this run's $18==01 gf (detected)

/* SNESREF_SHOT_AT_GF=N: dump the framebuffer to a PPM once game-frame ($7E:0088)
 * reaches N — works headless, so we can compare the oracle's actual screen vs the
 * recomp's for VRAM/PPU-rendering bugs the WRAM oracle can't see. */
static void maybe_shot(const void* data, unsigned w, unsigned h, size_t pitch) {
    static long target = -2; static int done = 0;
    if (target == -2) { const char* e = getenv("SNESREF_SHOT_AT_GF"); target = (e && e[0]) ? atol(e) : -1; }
    if (target < 0 || done || !data || !g_sysram) return;
    long gf = (long)((unsigned)g_sysram[0x88] | ((unsigned)g_sysram[0x89] << 8));
    if (gf < target) return;
    done = 1;
    const char* path = getenv("SNESREF_SHOT_FILE"); if (!path || !path[0]) path = "oracle_shot.ppm";
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* rowp = (const uint8_t*)data + y * pitch;
        for (unsigned x = 0; x < w; x++) {
            uint8_t r,g,b;
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t*)rowp)[x]; r=(p>>16)&0xff; g=(p>>8)&0xff; b=p&0xff;
            } else if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = ((const uint16_t*)rowp)[x]; r=((p>>11)&0x1f)<<3; g=((p>>5)&0x3f)<<2; b=(p&0x1f)<<3;
            } else {
                uint16_t p = ((const uint16_t*)rowp)[x]; r=((p>>10)&0x1f)<<3; g=((p>>5)&0x1f)<<3; b=(p&0x1f)<<3;
            }
            fputc(r,f); fputc(g,f); fputc(b,f);
        }
    }
    fclose(f);
    fprintf(stderr, "[shot] oracle framebuffer at gf=%ld -> %s (%ux%u)\n", gf, path, w, h);
}

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port!=0 || device!=RETRO_DEVICE_JOYPAD) return 0;
    if (g_replay) {
        if (!g_sysram) g_sysram = (const uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        if (g_sysram && id < 12) {
            long gf = (long)((unsigned)g_sysram[0x88] | ((unsigned)g_sysram[0x89] << 8));
            // Re-anchor at action-stage entry: the recomp and snes9x do NOT share
            // an absolute $0088 clock through boot/menus (different boot lengths +
            // menu timing), so pre-entry replay is best-effort by absolute gf —
            // enough to navigate into the stage. Once $18==01 is reached, translate
            // to the recording's stage-relative frame so gameplay input lands
            // frame-exact. SNESREF_ENTRY_GF = the recomp's entry gf (its [act-enter]).
            if (g_recomp_entry_gf >= 0) {
                if (g_oracle_entry_gf < 0 && g_sysram[0x18] == 0x01) g_oracle_entry_gf = gf;
                if (g_oracle_entry_gf >= 0) gf = g_recomp_entry_gf + (gf - g_oracle_entry_gf);
            }
            if (gf >= 0 && gf <= g_replay_max) return (g_replay[gf] >> id) & 1;
        }
        return 0;
    }
    // scripted B press (matches recomp AR_FORCE_INPUT_AFTER)
    if (id==RETRO_DEVICE_ID_JOYPAD_B && g_force_b_after>=0 && (long)g_frame>=g_force_b_after) return 1;
    if (g_headless) return 0;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc; SDL_GameControllerButton gb;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B:      sc=SDL_SCANCODE_Z;      gb=SDL_CONTROLLER_BUTTON_A; break;
        case RETRO_DEVICE_ID_JOYPAD_Y:      sc=SDL_SCANCODE_A;      gb=SDL_CONTROLLER_BUTTON_X; break;
        case RETRO_DEVICE_ID_JOYPAD_A:      sc=SDL_SCANCODE_X;      gb=SDL_CONTROLLER_BUTTON_B; break;
        case RETRO_DEVICE_ID_JOYPAD_X:      sc=SDL_SCANCODE_S;      gb=SDL_CONTROLLER_BUTTON_Y; break;
        case RETRO_DEVICE_ID_JOYPAD_L:      sc=SDL_SCANCODE_C;      gb=SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_R:      sc=SDL_SCANCODE_V;      gb=SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc=SDL_SCANCODE_RETURN; gb=SDL_CONTROLLER_BUTTON_START; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc=SDL_SCANCODE_RSHIFT; gb=SDL_CONTROLLER_BUTTON_BACK; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc=SDL_SCANCODE_UP;     gb=SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc=SDL_SCANCODE_DOWN;   gb=SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc=SDL_SCANCODE_LEFT;   gb=SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc=SDL_SCANCODE_RIGHT;  gb=SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        default: return 0;
    }
    if (ks[sc]) return 1;
    if (g_pad && SDL_GameControllerGetButton(g_pad, gb)) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: snesref <core.dylib> <rom.sfc>\n"); return 1; }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];

    { const char* v;
      g_headless    = (v=getenv("SNESREF_HEADLESS")) && v[0] && v[0]!='0';
      if ((v=getenv("SNESREF_FORCE_B_AFTER")) && v[0]) g_force_b_after = atol(v);
      if ((v=getenv("SNESREF_TRACE_LO"))      && v[0]) g_trace_lo = (uint32_t)strtoul(v,nullptr,0);
      if ((v=getenv("SNESREF_TRACE_HI"))      && v[0]) g_trace_hi = (uint32_t)strtoul(v,nullptr,0);
      if ((v=getenv("SNESREF_ENTRY_GF"))      && v[0]) g_recomp_entry_gf = atol(v);
      if ((v=getenv("SNESREF_INPUT_REPLAY"))  && v[0]) {
          FILE* f=fopen(v,"rb");
          if (f) { fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
              long nrec=n/8; uint32_t* raw=(uint32_t*)malloc((size_t)nrec*8);
              if (raw && fread(raw,8,(size_t)nrec,f)==(size_t)nrec) {
                  for (long i=0;i<nrec;i++) if ((long)raw[i*2]>g_replay_max) g_replay_max=(long)raw[i*2];
                  if (g_replay_max>=0) { g_replay=(uint32_t*)calloc((size_t)g_replay_max+1,4);
                      if (g_replay) for (long i=0;i<nrec;i++) g_replay[raw[i*2]]=raw[i*2+1]; } }
              free(raw); fclose(f);
              printf("[input-replay] %ld records, max gf=%ld from %s\n", nrec, g_replay_max, v); }
          else fprintf(stderr,"[input-replay] cannot open %s\n", v);
      }
    }

    g_core = dlopen(corePath, RTLD_NOW | RTLD_LOCAL);
    if (!g_core) { fprintf(stderr,"dlopen failed: %s\n", dlerror()); return 2; }
    bind(p_retro_init,"retro_init"); bind(p_retro_deinit,"retro_deinit");
    bind(p_retro_api_version,"retro_api_version");
    bind(p_retro_get_system_info,"retro_get_system_info");
    bind(p_retro_get_system_av_info,"retro_get_system_av_info");
    bind(p_retro_set_environment,"retro_set_environment");
    bind(p_retro_set_video_refresh,"retro_set_video_refresh");
    bind(p_retro_set_audio_sample,"retro_set_audio_sample");
    bind(p_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    bind(p_retro_set_input_poll,"retro_set_input_poll");
    bind(p_retro_set_input_state,"retro_set_input_state");
    bind(p_retro_set_controller_port_device,"retro_set_controller_port_device");
    bind(p_retro_load_game,"retro_load_game"); bind(p_retro_unload_game,"retro_unload_game");
    bind(p_retro_run,"retro_run");
    bind(p_retro_serialize_size,"retro_serialize_size");
    bind(p_retro_serialize,"retro_serialize"); bind(p_retro_unserialize,"retro_unserialize");
    bind(p_retro_get_memory_data,"retro_get_memory_data");
    bind(p_retro_get_memory_size,"retro_get_memory_size");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    printf("core: %s %s  need_fullpath=%d\n", si.library_name?si.library_name:"?",
           si.library_version?si.library_version:"?", si.need_fullpath);

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f=fopen(romPath,"rb"); if(!f){ fprintf(stderr,"cannot open rom %s\n",romPath); return 3; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        rom.resize(n); fread(rom.data(),1,n,f); fclose(f);
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    /* SNESREF_SRAM_IN=<file>: load a battery SRAM image into the core's SAVE_RAM
     * (valid only after retro_load_game) so the oracle starts from the SAME saved
     * game as the recomp (saves/save.srm). Lets the reference reach Act 1 from the
     * user's save for object-table / WRAM diffing. */
    if (const char* sin = getenv("SNESREF_SRAM_IN")) {
        uint8_t* sram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        FILE* f = fopen(sin, "rb");
        if (f && sram && sz) {
            size_t rd = fread(sram, 1, sz, f); fclose(f);
            printf("[sram-in] loaded %zu/%zu bytes from %s\n", rd, sz, sin);
        } else { fprintf(stderr, "[sram-in] FAILED to load %s\n", sin); if (f) fclose(f); }
    }

    /* Dump the power-on WRAM so the recomp can init g_ram identically (its
     * g_ram starts at 0x00, snes9x uses a fill pattern — without matching,
     * the game's RAM-clear shows as a huge spurious diff). */
    if (const char* wp0 = getenv("SNESREF_WRAM0")) {
        uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        FILE* f = fopen(wp0, "wb");
        if (f && ram) { fwrite(ram, 1, sz, f); fclose(f);
            printf("[wram0 dumped: %zu bytes -> %s]\n", sz, wp0); }
    }

    retro_system_av_info av; memset(&av,0,sizeof av); p_retro_get_system_av_info(&av);
    int vw=(int)av.geometry.base_width, vh=(int)av.geometry.base_height;
    if(vw<=0)vw=256; if(vh<=0)vh=224;
    printf("core timing: fps=%.4f sample_rate=%.2f  trace=[0x%05x,0x%05x] headless=%d\n",
           av.timing.fps, av.timing.sample_rate, g_trace_lo, g_trace_hi, g_headless);

    { const char* wp = getenv("SNESREF_WAV");
      wav_open(wp && wp[0] ? wp : "snesref_audio.wav",
               av.timing.sample_rate > 0 ? av.timing.sample_rate : 32040.0); }
    long quit_frames = 0;
    { const char* qf = getenv("SNESREF_QUIT_FRAMES"); if (qf && qf[0]) quit_frames = atol(qf); }

    SDL_SetMainReady();
    Uint32 sdl_flags = SDL_INIT_AUDIO;
    if (!g_headless) sdl_flags |= SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
    if (SDL_Init(sdl_flags) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
    if (!g_headless) {
        open_first_pad();
        g_win = SDL_CreateWindow("snesref (libretro oracle)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*2, vh*2, SDL_WINDOW_RESIZABLE);
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(g_ren, vw, vh);
        printf("RUN. KB: arrows=DPad Z=B X=A A=Y S=X C=L V=R Enter=Start RShift=Select | Esc=quit\n");
    } else {
        printf("RUN headless: capturing %ld frames -> %s\n", quit_frames,
               getenv("SNESREF_TRACE_FILE")?getenv("SNESREF_TRACE_FILE"):"oracle_trace.jsonl");
    }
    fflush(stdout);

    bool running=true;
    Uint64 freq=SDL_GetPerformanceFrequency(), prev=SDL_GetPerformanceCounter();
    const double target = (double)freq / 60.098;
    while (running) {
        if (!g_headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT) running=false;
                else if (e.type==SDL_CONTROLLERDEVICEADDED) open_first_pad();
                else if (e.type==SDL_KEYDOWN && e.key.repeat==0 && e.key.keysym.scancode==SDL_SCANCODE_ESCAPE) running=false;
            }
        }
        p_retro_run();
        g_frame++;
        trace_tick();
        /* Report when the action stage ($7E:0018==01) is first entered, with the
         * game-frame $0088 — lets a round-trip confirm the recomp/oracle share the
         * same gf clock (gf-keyed input replay only aligns if they do). */
        { static int seen_act = 0;
          if (!g_sysram) g_sysram = (const uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
          if (!seen_act && g_sysram && g_sysram[0x18] == 0x01) { seen_act = 1;
            unsigned gf = (unsigned)g_sysram[0x88] | ((unsigned)g_sysram[0x89] << 8);
            fprintf(stderr, "[act-enter] $18=01 at game-frame %u (host frame %u)\n", gf, g_frame); } }
        /* SNESREF_DUMP_AT_GF=N: dump full WRAM when $0088==N -> oracle_at.bin. */
        { static long dump_gf = -2; static int dumped = 0;
          if (dump_gf == -2) { const char* e = getenv("SNESREF_DUMP_AT_GF"); dump_gf = e ? atol(e) : -1; }
          if (dump_gf >= 0 && !dumped) {
            if (!g_sysram) g_sysram = (const uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            if (g_sysram) { long gf = (long)((unsigned)g_sysram[0x88] | ((unsigned)g_sysram[0x89] << 8));
              if (gf == dump_gf) { dumped = 1;
                const char* out = getenv("SNESREF_DUMP_AT_FILE"); if (!out) out = "tools/oracle/oracle_at.bin";
                FILE* f = fopen(out, "wb"); if (f) { fwrite(g_sysram, 1, 0x20000, f); fclose(f);
                  fprintf(stderr, "[dump-at-gf] gf=%ld -> %s\n", gf, out); } } } } }
        if (quit_frames > 0 && g_frame >= (uint32_t)quit_frames) running = false;
        if (!g_headless) {
            for (;;) {
                Uint64 now=SDL_GetPerformanceCounter();
                double el=(double)(now-prev);
                if (el>=target) { prev=now; break; }
                double rem_ms=(target-el)*1000.0/(double)freq;
                if (rem_ms>1.5) SDL_Delay((Uint32)(rem_ms-1.0));
            }
        }
    }
    if (g_log) fflush(g_log);
    /* SNESREF_WRAM_END=<file>: dump full 128KB WRAM at end-of-run, so the
     * oracle's actual runtime WRAM (e.g. $7E0018 game-mode) can be inspected
     * directly rather than inferred from the per-frame change diff. */
    if (const char* wpe = getenv("SNESREF_WRAM_END")) {
        uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        FILE* f = fopen(wpe, "wb");
        if (f && ram) { fwrite(ram, 1, sz, f); fclose(f);
            fprintf(stderr, "[wram-end] dumped %zu bytes -> %s\n", sz, wpe); }
    }
    /* SNESREF_SRAM_END=<file>: dump cartridge SAVE RAM (battery SRAM) so the
     * recomp's SRAM init/fill convention can be matched to the reference. */
    if (const char* spe = getenv("SNESREF_SRAM_END")) {
        uint8_t* sram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        FILE* f = fopen(spe, "wb");
        if (f && sram) { fwrite(sram, 1, sz, f); fclose(f);
            fprintf(stderr, "[sram-end] dumped %zu bytes -> %s\n", sz, spe); }
    }
    wav_close();
    p_retro_unload_game(); p_retro_deinit();
    SDL_Quit(); dlclose(g_core);
    return 0;
}
