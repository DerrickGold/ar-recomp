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
Settings g_settings;

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
  (void)requested_abi_version;
  return NULL; /* WRAM-only music gates do not require a live PPU. */
}

/* Engine seams music_replacements.c binds against. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
int RtlGetAudioOutputRate(void) { return 44100; }
int g_dsp_voice_mute_srcn_min = -1;
static bool s_music_bus_muted;
void dsp_setMusicBusMuted(bool muted) { s_music_bus_muted = muted; }

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


/* Selection is by specificity, not position. Position deciding on its own made
 * a manifest depend on an invariant nothing enforced: an ungated catch-all
 * written above a gated variant shadowed it completely, and the builder GUI
 * rewrites its [music:song-NN] entry wherever that section already sits -- so a
 * correct hand-authored split could stop applying the first time someone
 * dropped a file into the catch-all slot, with no diagnostic either way. */
static void TestGatedVariantsBeatAnUngatedEntryInAnyOrder(void) {
  const char *catch_all_first =
      "[music:song-09]\nsrc = 0E:F69F\nfile = audio/act2.ogg\n"
      "[music:act2-fillmore]\nsrc = 0E:F69F\nwhen = wram[0018]==0x01\n"
      "file = audio/act2-fillmore.ogg\n"
      "[music:act2-kasandora]\nsrc = 0E:F69F\nwhen = wram[0018]==0x03\n"
      "file = audio/act2-kasandora.ogg\n";
  const char *catch_all_last =
      "[music:act2-fillmore]\nsrc = 0E:F69F\nwhen = wram[0018]==0x01\n"
      "file = audio/act2-fillmore.ogg\n"
      "[music:act2-kasandora]\nsrc = 0E:F69F\nwhen = wram[0018]==0x03\n"
      "file = audio/act2-kasandora.ogg\n"
      "[music:song-09]\nsrc = 0E:F69F\nfile = audio/act2.ogg\n";

  for (int variant = 0; variant < 2; variant++) {
    const char *body = variant ? catch_all_last : catch_all_first;
    CHECK(MusicReplacements_Load(WriteManifest(body)) == 3);
    memset(g_ram, 0, sizeof(g_ram));
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.music_replacements = true;
    for (int i = 0; i < g_music_replacement_count; i++)
      g_music_replacements[i].has_audio = true;

    /* The act spans several rooms; only the region byte gates the variant. */
    for (int map = 1; map <= 8; map++) {
      g_ram[0x19] = (uint8)map;
      g_ram[0x18] = 0x01;
      CHECK(!strcmp(MusicReplacements_Select(0x0EF69F, 1)->name, "act2-fillmore"));
      g_ram[0x18] = 0x03;
      CHECK(!strcmp(MusicReplacements_Select(0x0EF69F, 1)->name, "act2-kasandora"));
    }
    /* Any other region falls through to the catch-all. */
    g_ram[0x18] = 0x05;
    CHECK(!strcmp(MusicReplacements_Select(0x0EF69F, 1)->name, "song-09"));

    /* A gated entry whose file is absent must not shadow the catch-all: it is
     * more specific, but there is nothing to play. */
    g_ram[0x18] = 0x01;
    for (int i = 0; i < g_music_replacement_count; i++)
      if (!strcmp(g_music_replacements[i].name, "act2-fillmore"))
        g_music_replacements[i].has_audio = false;
    CHECK(!strcmp(MusicReplacements_Select(0x0EF69F, 1)->name, "song-09"));
  }

  /* Overlapping gates are the one case position still decides, because
   * specificity cannot separate them. */
  CHECK(MusicReplacements_Load(WriteManifest(
      "[music:broad]\nsrc = 0E:F69F\nwhen = wram[0018]==0x01\nfile = a.ogg\n"
      "[music:narrow]\nsrc = 0E:F69F\nwhen = wram[0018]==0x01, wram[0019]==0x02\n"
      "file = b.ogg\n")) == 2);
  for (int i = 0; i < g_music_replacement_count; i++)
    g_music_replacements[i].has_audio = true;
  g_ram[0x18] = 0x01;
  g_ram[0x19] = 0x02;
  CHECK(!strcmp(MusicReplacements_Select(0x0EF69F, 1)->name, "broad"));
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
  CHECK(!MusicReplacements_IsPlaybackPaused());
  char status[128];
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(!strcmp(status, "MUSIC NONE"));

  MusicReplacements_OnSpcUpload(0x1A94B8);
  MusicReplacements_OnApuPortWrite(0, 0xF0); /* halt */
  MusicReplacements_OnApuPortWrite(0, 0xFF); /* upload */
  MusicReplacements_OnApuPortWrite(0, 0x01); /* play song 1: no audio -> authentic */
  CHECK(g_dsp_voice_mute_srcn_min == -1);
  CHECK(!s_music_bus_muted);
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(strstr(status, "MUSIC tune $01 AUTH") != NULL);

  /* $F2 is native pause, not song F2. The same remembered song command
   * resumes it. Host pause is an independent reason, so clearing only one
   * latch must not resume playback. */
  MusicReplacements_OnApuPortWrite(0, 0xF2);
  CHECK(MusicReplacements_IsPlaybackPaused());
  MusicReplacements_SetHostPaused(true);
  MusicReplacements_FormatPlaybackStatus(status, sizeof(status));
  CHECK(strstr(status, "PAUSED") != NULL);
  MusicReplacements_OnApuPortWrite(0, 0x01);
  CHECK(MusicReplacements_IsPlaybackPaused());
  MusicReplacements_SetHostPaused(false);
  CHECK(!MusicReplacements_IsPlaybackPaused());

  MusicReplacements_SetHostPaused(true);
  MusicReplacements_OnApuPortWrite(0, 0xF2);
  MusicReplacements_SetHostPaused(false);
  CHECK(MusicReplacements_IsPlaybackPaused());
  MusicReplacements_OnApuPortWrite(0, 0x01);
  CHECK(!MusicReplacements_IsPlaybackPaused());

  /* Force the entry playable but keep the file missing: the play-time open
   * fails and must also fall back authentic (no gate left behind). */
  g_music_replacements[0].has_audio = true;
  g_music_replacements[0].file_rate = 44100;
  g_music_replacements[0].file_frames = 44100;
  MusicReplacements_OnApuPortWrite(0, 0x01);
  CHECK(g_dsp_voice_mute_srcn_min == -1);
  CHECK(!s_music_bus_muted);

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
  MusicReplacements_MixOutput(buf, 16);
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
  /* Claim 3: phase continuity at a non-native rate — the carry recurrence
   * (phase0 = carry; per_block = out*ratio + phase0; src_frames = (int);
   * carry = fraction) never drifts from the ideal resample ratio. Asserted
   * against an INDEPENDENT oracle: after every block, the integer source
   * frames decoded so far plus the live carry must equal blocks*out*ratio by
   * direct multiplication. (An earlier version summed step out_frames times
   * and compared it with the recurrence built from the same product — both
   * sides were the same arithmetic, so it could never fail.) */
  {
    int file_rate = 44100, output_rate = 48000, out_frames = 1024;
    double step = (double)file_rate / output_rate;   /* 0.91875 */
    double src_carry = 0.0;
    long long total_decoded = 0;   /* integer source frames consumed */
    for (int block = 0; block < 32; block++) {
      double phase0 = src_carry;
      double per_block = (double)out_frames * file_rate / output_rate + phase0;
      int src_frames = (int)per_block;
      src_carry = per_block - src_frames;
      total_decoded += src_frames;
      /* The carried phase is always a proper fraction... */
      CHECK(src_carry >= 0.0 && src_carry < 1.0);
      /* ...the decode count only wobbles between floor/ceil of the ideal
       * per-block consumption (940/941 here) — a carry reset would pin it
       * at the floor forever... */
      int ideal_floor = (int)((double)out_frames * step);
      CHECK(src_frames == ideal_floor || src_frames == ideal_floor + 1);
      /* ...and decoded + carry tracks the ideal source position with zero
       * cumulative drift. A per-block carry reset diverges by block 2
       * (30080 + 0 vs 30105.6 by block 32). */
      double ideal = (double)(block + 1) * out_frames * step;
      double actual = (double)total_decoded + src_carry;
      CHECK(actual - ideal < 1e-6 && actual - ideal > -1e-6);
    }
  }
}

/* F5 (2026-07-26 handback): "hd music loops and plays first few seconds of the
 * previous song before transitioning" when a boss fight starts.
 *
 * The boss go-signal chain ($00:A3FE -> $00:A410) issues a play command with NO
 * preceding $F0 and no new upload, so
 * the upload-derived identity still names the OUTGOING song: selection resolves
 * to the session already playing and the old restart rewound it to frame 0.
 *
 * MusicPlay_IsRedundantRestart is the decision, extracted pure so it is
 * testable without an audio device (no .ogg encoder exists on the authoring
 * machine, and `has_audio` requires a real decodable file).
 *
 * HOW THIS FAILS AGAINST THE OLD CODE: the pre-fix path had no such guard —
 * every play command reaching an entry called StartSession unconditionally. Any
 * implementation that returns false for the live-session case (i.e. deletes the
 * guard, or drops the `live == selected` term) fails TheRedundantCase below. */
static void TestRedundantRestartGuard(void) {
  /* Two distinct entries; identity is the pointer, as in the caller. */
  MusicReplacement act, boss;
  memset(&act, 0, sizeof(act));
  memset(&boss, 0, sizeof(boss));

  /* THE BUG: the boss chain's play command resolves to the act track that is
   * already streaming. Restarting it is what replayed its opening. */
  CHECK(MusicPlay_IsRedundantRestart(&act, &act, true));

  /* A genuine song change must still start: different entry. */
  CHECK(!MusicPlay_IsRedundantRestart(&act, &boss, true));

  /* Nothing playing yet ($F0 ran EndSession, or first play of the session):
   * must start. This is the common case and a false positive here would mean
   * music never begins. */
  CHECK(!MusicPlay_IsRedundantRestart(NULL, &boss, false));
  CHECK(!MusicPlay_IsRedundantRestart(NULL, &boss, true));

  /* A FINISHED ONE-SHOT keeps its session but closes the decoder (MixMusic
   * does this deliberately so the muted SPC sequencer cannot fade back in).
   * Re-triggering it IS legitimate, so the stream state — not the mere
   * presence of a session — has to decide. Dropping the `stream_open` term
   * would make a one-shot un-retriggerable. */
  CHECK(!MusicPlay_IsRedundantRestart(&act, &act, false));

  /* Degenerate: no entry selected. The caller returns earlier in that case,
   * but the predicate must not claim a redundant restart regardless. */
  CHECK(!MusicPlay_IsRedundantRestart(&act, NULL, true));
  CHECK(!MusicPlay_IsRedundantRestart(NULL, NULL, true));
  CHECK(!MusicPlay_IsRedundantRestart(NULL, NULL, false));
}

int main(void) {
  TestMusicResamplerMath();
  TestParseEntries();
  TestParseRejections();
  TestSharedManifestHdSideIgnoresMusic();
  TestSelection();
  TestGatedVariantsBeatAnUngatedEntryInAnyOrder();
  TestLoopSlicing();
  TestRedundantRestartGuard();
  TestTriggerStateMachine();
  if (g_failures) {
    fprintf(stderr, "music manifest tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("music manifest tests: all passed\n");
  return 0;
}
