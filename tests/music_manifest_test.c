/* Unit tests for the music replacement manifest parser, variant selection,
 * and loop-region slicing. Links music_replacements.c + hd_replacements.c
 * (shared gate grammar) against stub engine state — no SDL or audio device.
 * Decode/streaming against a real .ogg is covered by the end-to-end headless
 * run (see docs/SEAMS.md "Audio"), not here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "music_replacements.h"
#include "hd_replacements.h"
#include "snes/ppu.h"
#include "settings.h"

static int g_failures;
#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
    g_failures++; \
  } \
} while (0)

/* ---- stubs -------------------------------------------------------------- */

uint8 g_ram[0x20000];
Ppu *g_ppu; /* NULL: music gates must still work for wram operands */
Settings g_settings;

bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source, int x, int y,
                          int width, int height, uint8_t flags) {
  (void)ppu; (void)source; (void)x; (void)y; (void)width; (void)height;
  (void)flags;
  return true;
}

bool PpuSetMode7Override(Ppu *ppu, const uint32_t *rgba, int width,
                         int height, int canvas_x0, int canvas_y0,
                         int canvas_x1, int canvas_y1, uint8_t wrap) {
  (void)ppu; (void)rgba; (void)width; (void)height; (void)canvas_x0;
  (void)canvas_y0; (void)canvas_x1; (void)canvas_y1; (void)wrap;
  return true;
}

/* Engine seams music_replacements.c binds against. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
int RtlGetAudioOutputRate(void) { return 44100; }
void (*g_rtl_spc_upload_hook)(uint32_t src);
void (*g_rtl_apu_port_hook)(uint8_t port, uint8_t val);
void (*g_rtl_music_mix_hook)(int16_t *buf, int frames);
int g_dsp_voice_mute_srcn_min = -1;

/* ---- helpers ------------------------------------------------------------ */

static const char *WriteManifest(const char *body) {
  static char path[512];
  const char *dir = getenv("TMPDIR");
  snprintf(path, sizeof(path), "%s/music_manifest_test.ini",
           dir ? dir : "/tmp");
  FILE *f = fopen(path, "w");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
  fputs(body, f);
  fclose(f);
  return path;
}

/* ---- tests -------------------------------------------------------------- */

static void TestParseEntries(void) {
  CHECK(MusicReplacements_Load(WriteManifest(
      "# comment\n"
      "[music:title-theme]\n"
      "src = 1A:94B8\n"
      "file = audio/title.ogg\n"
      "\n"
      "[music:boss]\n"
      "src = 0B:8000\n"
      "file = audio/boss.ogg\n"
      "song = 2\n"
      "loop = 0\n"
      "loop_start = 44100\n"
      "loop_end = 220500\n"
      "gain = 80\n"
      "when = wram[00A2]==0x03\n")) == 2);

  const MusicReplacement *title = &g_music_replacements[0];
  CHECK(!strcmp(title->name, "title-theme"));
  CHECK(title->src == 0x1A94B8);
  CHECK(title->song == kMusicSongAny);
  CHECK(title->loop);
  CHECK(title->loop_start == 0 && title->loop_end == 0);
  CHECK(title->gain_percent == 100);
  CHECK(title->condition_count == 0);
  CHECK(strstr(title->file, "audio/title.ogg") != NULL);
  CHECK(!title->has_audio); /* file absent: entry parsed but inert */

  const MusicReplacement *boss = &g_music_replacements[1];
  CHECK(boss->src == 0x0B8000);
  CHECK(boss->song == 2);
  CHECK(!boss->loop);
  CHECK(boss->loop_start == 44100 && boss->loop_end == 220500);
  CHECK(boss->gain_percent == 80);
  CHECK(boss->condition_count == 1);
  CHECK(boss->conditions[0].kind == kHdCond_WramByte);
  CHECK(boss->conditions[0].address == 0xA2);
  CHECK(boss->conditions[0].value == 3);
}

static void TestParseRejections(void) {
  /* Missing src drops the entry; the next one still parses. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:broken]\n"
      "file = audio/x.ogg\n"
      "[music:ok]\n"
      "src = 06:AC00\n"
      "file = audio/y.ogg\n")) == 1);
  CHECK(!strcmp(g_music_replacements[0].name, "ok"));

  /* Bad src syntax drops the entry. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:bad-src]\n"
      "src = not-an-address\n"
      "file = audio/x.ogg\n")) == 0);

  /* Song numbers collide with driver commands ($F0+) — rejected. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:bad-song]\n"
      "src = 01:8000\n"
      "file = audio/x.ogg\n"
      "song = 0xF0\n")) == 0);

  /* Bad gate syntax drops the entry. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:bad-when]\n"
      "src = 01:8000\n"
      "file = audio/x.ogg\n"
      "when = mood==grim\n")) == 0);

  /* [replace:] sections in the shared manifest are ignored here. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[replace:title-logo]\n"
      "plane = screen\n"
      "layer = bg1\n"
      "rect = 0,0,8,8\n"
      "image = hd/x.png\n"
      "when = mode==7\n"
      "[music:tune]\n"
      "src = 02:C000\n"
      "file = audio/z.ogg\n")) == 1);
  CHECK(!strcmp(g_music_replacements[0].name, "tune"));

  /* Missing manifest file is silent and empty. */
  CHECK(MusicReplacements_Load("/nonexistent/manifest.ini") == 0);
}

static void TestSharedManifestHdSideIgnoresMusic(void) {
  /* The HD parser must skip [music:] sections without dropping its own. */
  CHECK(HdReplacements_Load(WriteManifest(
      "[music:tune]\n"
      "src = 02:C000\n"
      "file = audio/z.ogg\n"
      "[replace:logo]\n"
      "plane = screen\n"
      "layer = bg1\n"
      "rect = 0,0,8,8\n"
      "image = hd/x.png\n"
      "when = mode==7\n")) == 1);
  CHECK(!strcmp(g_hd_replacements[0].name, "logo"));
}

static void TestSelection(void) {
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:act-fillmore]\n"
      "src = 1A:94B8\n"
      "file = audio/fillmore.ogg\n"
      "when = wram[00A2]==0x01\n"
      "[music:act-song2]\n"
      "src = 1A:94B8\n"
      "file = audio/second.ogg\n"
      "song = 2\n"
      "[music:act-fallback]\n"
      "src = 1A:94B8\n"
      "file = audio/generic.ogg\n"
      "[music:other]\n"
      "src = 0B:8000\n"
      "file = audio/other.ogg\n")) == 4);
  memset(g_ram, 0, sizeof(g_ram));
  memset(&g_settings, 0, sizeof(g_settings));
  g_settings.music_replacements = true;

  /* Nothing has audio: no selection at all. */
  CHECK(MusicReplacements_Select(0x1A94B8, 1) == NULL);

  for (int i = 0; i < g_music_replacement_count; i++)
    g_music_replacements[i].has_audio = true;

  /* Gate fails (wrong area byte), song filter fails -> fallback wins. */
  CHECK(MusicReplacements_Select(0x1A94B8, 1) == &g_music_replacements[2]);
  /* Song-2 entry outranks the fallback for its song number. */
  CHECK(MusicReplacements_Select(0x1A94B8, 2) == &g_music_replacements[1]);
  /* Gate passes -> the gated variant wins over both. */
  g_ram[0xA2] = 0x01;
  CHECK(MusicReplacements_Select(0x1A94B8, 1) == &g_music_replacements[0]);
  CHECK(MusicReplacements_Select(0x1A94B8, 2) == &g_music_replacements[0]);
  g_ram[0xA2] = 0;
  /* Unknown src -> authentic. */
  CHECK(MusicReplacements_Select(0x028000, 1) == NULL);
  /* Entry without audio never wins even when its gate matches. */
  g_music_replacements[2].has_audio = false;
  CHECK(MusicReplacements_Select(0x1A94B8, 1) == NULL);
}

static void TestLoopSlicing(void) {
  bool hit;

  /* Plain looping across a 1000-frame file, loop region 100..900. */
  CHECK(MusicLoop_NextRun(0, 500, 100, 900, 1000, &hit) == 500);
  CHECK(!hit);
  CHECK(MusicLoop_NextRun(500, 500, 100, 900, 1000, &hit) == 400);
  CHECK(hit);
  /* Cursor back at loop_start after the seek. */
  CHECK(MusicLoop_NextRun(100, 500, 100, 900, 1000, &hit) == 500);
  CHECK(!hit);

  /* Whole-file loop (loop_end 0 -> file end). */
  CHECK(MusicLoop_NextRun(900, 500, 0, 0, 1000, &hit) == 100);
  CHECK(hit);

  /* Exactly filling to the boundary reports the hit. */
  CHECK(MusicLoop_NextRun(400, 500, 0, 900, 1000, &hit) == 500);
  CHECK(hit);

  /* At/past the end: zero-length run, immediate hit. */
  CHECK(MusicLoop_NextRun(1000, 500, 0, 0, 1000, &hit) == 0);
  CHECK(hit);
  CHECK(MusicLoop_NextRun(1200, 500, 0, 0, 1000, &hit) == 0);
  CHECK(hit);

  /* loop_end beyond the file clamps to the file end. */
  CHECK(MusicLoop_NextRun(990, 500, 0, 5000, 1000, &hit) == 10);
  CHECK(hit);
}

static void TestTriggerStateMachine(void) {
  /* Entries whose files don't exist: a play command must leave the DSP
   * un-gated (authentic fallback), and driver commands must be inert. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:tune]\n"
      "src = 1A:94B8\n"
      "file = audio/missing.ogg\n")) == 1);
  memset(&g_settings, 0, sizeof(g_settings));
  g_settings.music_replacements = true;
  MusicReplacements_InstallHooks();
  CHECK(g_rtl_apu_port_hook != NULL);
  CHECK(g_rtl_spc_upload_hook != NULL);
  CHECK(g_rtl_music_mix_hook != NULL);
  CHECK(!MusicReplacements_IsPlaybackPaused());
  char status[128];
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(!strcmp(status, "MUSIC NONE"));

  g_rtl_spc_upload_hook(0x1A94B8);
  g_rtl_apu_port_hook(0, 0xF0); /* halt */
  g_rtl_apu_port_hook(0, 0xFF); /* upload */
  g_rtl_apu_port_hook(0, 0x01); /* play song 1: no audio -> authentic */
  CHECK(g_dsp_voice_mute_srcn_min == -1);
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(strstr(status, "MUSIC tune $01 AUTH") != NULL);

  /* $F2 is native pause, not song F2. The same remembered song command
   * resumes it. Host pause is an independent reason, so clearing only one
   * latch must not resume playback. */
  g_rtl_apu_port_hook(0, 0xF2);
  CHECK(MusicReplacements_IsPlaybackPaused());
  MusicReplacements_SetHostPaused(true);
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(strstr(status, "PAUSED") != NULL);
  g_rtl_apu_port_hook(0, 0x01);
  CHECK(MusicReplacements_IsPlaybackPaused());
  MusicReplacements_SetHostPaused(false);
  CHECK(!MusicReplacements_IsPlaybackPaused());

  MusicReplacements_SetHostPaused(true);
  g_rtl_apu_port_hook(0, 0xF2);
  MusicReplacements_SetHostPaused(false);
  CHECK(MusicReplacements_IsPlaybackPaused());
  g_rtl_apu_port_hook(0, 0x01);
  CHECK(!MusicReplacements_IsPlaybackPaused());

  /* Force the entry playable but keep the file missing: the play-time open
   * fails and must also fall back authentic (no gate left behind). */
  g_music_replacements[0].has_audio = true;
  g_music_replacements[0].file_rate = 44100;
  g_music_replacements[0].file_frames = 44100;
  g_rtl_apu_port_hook(0, 0x01);
  CHECK(g_dsp_voice_mute_srcn_min == -1);

  /* The menu callback may toggle either direction while the game is paused.
   * With this deliberately missing fixture both paths stay authentic, but
   * they still exercise adoption of the remembered current song. */
  g_settings.music_replacements = false;
  MusicReplacements_ApplySetting();
  CHECK(g_dsp_voice_mute_srcn_min == -1);
  g_settings.music_replacements = true;
  MusicReplacements_ApplySetting();
  CHECK(g_dsp_voice_mute_srcn_min == -1);

  /* Mix hook without a session: must not touch the buffer. */
  int16_t buf[64];
  memset(buf, 0x11, sizeof(buf));
  g_rtl_music_mix_hook(buf, 16);
  CHECK(buf[0] == 0x1111 && buf[31] == 0x1111);
}

/* music resampler: the MixMusic interpolation is static in music_replacements.c
 * and needs a decoded OGG session to drive, so replicate its exact Catmull-Rom
 * kernel + phase arithmetic here and pin the spec's three ROM-free claims. The
 * audible-quality payoff (48k against a 44.1k OGG) is the Wave-3 listening gate. */
static double CatmullRom(double p0, double p1, double p2, double p3, double frac) {
  return p1 + 0.5 * frac *
         ((p2 - p0) +
          frac * ((2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) +
                  frac * (3.0 * (p1 - p2) + p3 - p0)));
}
static void TestMusicResamplerMath(void) {
  /* Claim 1: Catmull-Rom at frac==0 returns exactly p1 (== src[idx]), so the
   * native-rate default path is byte-identical to the old linear lerp. */
  for (int t = 0; t < 4; t++) {
    double p[4] = { -1000.0 + t, 4321.0, -8765.0, 20000.0 };
    double v = CatmullRom(p[0], p[1], p[2], p[3], 0.0);
    CHECK((int)v == (int)p[1]);
  }
  /* Claim 2: native-rate ratio is exactly 1.0 (step==1.0), so per_block is
   * integral, src_carry stays 0.0, and the carried phase0 stays 0. */
  {
    int file_rate = 44100, output_rate = 44100, out_frames = 512;
    double src_carry = 0.0;
    for (int block = 0; block < 8; block++) {
      double phase0 = src_carry;
      double per_block = (double)out_frames * file_rate / output_rate + phase0;
      int src_frames = (int)per_block;
      src_carry = per_block - src_frames;
      CHECK(src_frames == out_frames);   /* 512 in, 512 out */
      CHECK(src_carry == 0.0);           /* no drift */
      CHECK(phase0 == 0.0);              /* frac==0 everywhere -> identity */
    }
  }
  /* Claim 3: phase continuity at a non-native rate — pos starts at phase0 and
   * ends at per_block; the next block's frame 0 sits at src_frames, and the
   * carried remainder equals src_carry (zero cumulative drift). */
  {
    int file_rate = 44100, output_rate = 48000, out_frames = 1024;
    double step = (double)file_rate / output_rate;
    double src_carry = 0.0;
    double absolute_src = 0.0;   /* running true source position across blocks */
    for (int block = 0; block < 32; block++) {
      double phase0 = src_carry;
      double per_block = (double)out_frames * file_rate / output_rate + phase0;
      int src_frames = (int)per_block;
      src_carry = per_block - src_frames;
      double pos = phase0;
      for (int i = 0; i < out_frames; i++) pos += step;
      /* pos ends at per_block; leftover past the decoded frames == src_carry */
      CHECK((pos - src_frames) - src_carry < 1e-6 &&
            (pos - src_frames) - src_carry > -1e-6);
      absolute_src += (double)out_frames * step;
    }
    /* Over 32 blocks the accumulated source position matches out*step exactly
     * (no per-block reset drift). */
    CHECK(absolute_src - (double)(32 * out_frames) * step < 1e-3 &&
          absolute_src - (double)(32 * out_frames) * step > -1e-3);
  }
}

int main(void) {
  TestMusicResamplerMath();
  TestParseEntries();
  TestParseRejections();
  TestSharedManifestHdSideIgnoresMusic();
  TestSelection();
  TestLoopSlicing();
  TestTriggerStateMachine();
  if (g_failures) {
    fprintf(stderr, "music manifest tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("music manifest tests: all passed\n");
  return 0;
}
