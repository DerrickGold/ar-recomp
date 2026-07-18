#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "music_replacements.h"
#include "settings.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

/* Engine seams (snesrecomp-go/runtime). The APU mutex is recursive (SDL), so the
 * handlers below may take it even when the caller already holds it. */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);
extern int RtlGetAudioOutputRate(void);
extern void (*g_rtl_spc_upload_hook)(uint32_t src);
extern void (*g_rtl_apu_port_hook)(uint8_t port, uint8_t val);
extern void (*g_rtl_music_mix_hook)(int16_t *buf, int frames);
extern int g_dsp_voice_mute_srcn_min;

/* ActRaiser's SPC driver: per-song instruments occupy srcn 0x0C and up; the
 * common sample bank (srcn 0x00-0x0B, uploaded once from 06:AC00) carries the
 * SFX, which must survive any song-bank swap and therefore never live in the
 * per-song range. Muting from 0x0C silences music voices only. */
#define MUSIC_MUTE_SRCN_MIN 0x0C

/* Driver command vocabulary on APU port 0 ($2140). Anything other than these
 * controls and idle zero is a "start/resume song N" request. */
#define SPC_CMD_HALT 0xF0
#define SPC_CMD_ATTENTION 0xF1
#define SPC_CMD_PAUSE 0xF2
#define SPC_CMD_UPLOAD 0xFF

MusicReplacement g_music_replacements[kMusicMaxReplacements];
int g_music_replacement_count;

static bool s_musiclog;
static uint32 s_loaded_src; /* most recent SPC image upload source */
static int s_current_song = -1;
/* Native pause ($F2) and host pause (P/settings overlay) are independent.
 * Either one suspends decoding without closing the Vorbis stream or moving
 * its cursor. */
static bool s_driver_paused;
static bool s_host_paused;

static bool PlaybackPaused(void) {
  return s_driver_paused || s_host_paused;
}

/* ---- streamer state (serialised by the APU lock) ----------------------- */

#define MUSIC_MAX_BLOCK_FRAMES 65536 /* 8192 output frames, up to 192 kHz */

static struct {
  const MusicReplacement *session; /* non-NULL = DSP music voices muted */
  stb_vorbis *v;                   /* NULL once a one-shot stream ends */
  uint32 pos;                      /* read cursor, frames */
  uint32 loop_start, loop_end;     /* resolved (loop_end always <= total) */
  unsigned total;                  /* file length, frames */
  bool loop;
  int gain_percent;
  double src_carry;                /* fractional source frames per block */
} s;

/* ---- parsing ------------------------------------------------------------ */

static char *TrimInPlace(char *str) {
  while (*str == ' ' || *str == '\t') str++;
  char *end = str + strlen(str);
  while (end > str && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
    *--end = 0;
  return str;
}

/* BB:AAAA (hex) -> 24-bit source address. */
static bool ParseSrcAddress(const char *value, uint32 *out) {
  unsigned bank = 0, addr = 0;
  char trailing = 0;
  if (sscanf(value, "%x:%x%c", &bank, &addr, &trailing) != 2)
    return false;
  if (bank > 0xff || addr > 0xffff) return false;
  *out = (uint32)((bank << 16) | addr);
  return true;
}

static void ResolveFilePath(const char *manifest_path, const char *value,
                            char *out, size_t out_size) {
  const char *slash = strrchr(manifest_path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(manifest_path, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
  if (value[0] == '/' || !slash) {
    snprintf(out, out_size, "%s", value);
  } else {
    snprintf(out, out_size, "%.*s/%s",
             (int)(slash - manifest_path), manifest_path, value);
  }
}

static bool EntryComplete(const MusicReplacement *entry, const char *path,
                          int line) {
  const char *missing = NULL;
  if (!entry->src) missing = "src";
  else if (!entry->file[0]) missing = "file";
  if (missing)
    fprintf(stderr, "[music-manifest] %s:%d: [music:%s] missing/invalid '%s'"
            " — entry dropped\n", path, line, entry->name, missing);
  return !missing;
}

/* Probe an entry's file: exists -> must decode; read rate/length and the
 * LOOPSTART/LOOPLENGTH(-END) Vorbis comment tags (RPG Maker convention)
 * unless the manifest already set loop points. A missing file is the normal
 * "hook available, audio not provided" state and stays silent. */
static void ProbeEntryFile(MusicReplacement *entry) {
  FILE *probe = fopen(entry->file, "rb");
  if (!probe) return;
  fclose(probe);

  int error = 0;
  stb_vorbis *v = stb_vorbis_open_filename(entry->file, &error, NULL);
  if (!v) {
    fprintf(stderr, "[music-manifest] [music:%s] cannot decode %s "
            "(stb_vorbis error %d)\n", entry->name, entry->file, error);
    return;
  }
  stb_vorbis_info info = stb_vorbis_get_info(v);
  entry->file_rate = (int)info.sample_rate;
  entry->file_frames = stb_vorbis_stream_length_in_samples(v);

  if (!entry->loop_start && !entry->loop_end) {
    stb_vorbis_comment comment = stb_vorbis_get_comment(v);
    uint32 tag_start = 0, tag_len = 0, tag_end = 0;
    for (int i = 0; i < comment.comment_list_length; i++) {
      const char *c = comment.comment_list[i];
      if (!strncasecmp(c, "LOOPSTART=", 10))
        tag_start = (uint32)strtoul(c + 10, NULL, 10);
      else if (!strncasecmp(c, "LOOPLENGTH=", 11))
        tag_len = (uint32)strtoul(c + 11, NULL, 10);
      else if (!strncasecmp(c, "LOOPEND=", 8))
        tag_end = (uint32)strtoul(c + 8, NULL, 10);
    }
    entry->loop_start = tag_start;
    entry->loop_end = tag_len ? tag_start + tag_len : tag_end;
  }
  stb_vorbis_close(v);

  if (entry->file_rate < 8000 || entry->file_rate > 192000 ||
      entry->file_frames == 0) {
    fprintf(stderr, "[music-manifest] [music:%s] unusable file %s "
            "(rate %d, %u frames)\n", entry->name, entry->file,
            entry->file_rate, entry->file_frames);
    return;
  }
  /* Degenerate loop regions fall back to whole-file looping. */
  if (entry->loop_end &&
      (entry->loop_end <= entry->loop_start ||
       entry->loop_end > entry->file_frames)) {
    fprintf(stderr, "[music-manifest] [music:%s] bad loop points %u..%u "
            "(file has %u frames) — looping whole file\n", entry->name,
            entry->loop_start, entry->loop_end, entry->file_frames);
    entry->loop_start = 0;
    entry->loop_end = 0;
  }
  entry->has_audio = true;
}

int MusicReplacements_Load(const char *manifest_path) {
  g_music_replacement_count = 0;
  memset(g_music_replacements, 0, sizeof(g_music_replacements));
  s_musiclog = getenv("AR_MUSICLOG") != NULL;
  FILE *f = fopen(manifest_path, "r");
  if (!f) return 0;

  MusicReplacement pending;
  bool in_entry = false;
  int entry_line = 0;
  char line[1024];
  int line_number = 0;

  #define COMMIT_PENDING() do { \
    if (in_entry && EntryComplete(&pending, manifest_path, entry_line) && \
        g_music_replacement_count < kMusicMaxReplacements) \
      g_music_replacements[g_music_replacement_count++] = pending; \
    in_entry = false; \
  } while (0)

  while (fgets(line, sizeof(line), f)) {
    line_number++;
    char *cursor = TrimInPlace(line);
    if (!cursor[0] || cursor[0] == '#' || cursor[0] == ';') continue;

    if (cursor[0] == '[') {
      COMMIT_PENDING();
      char *close = strchr(cursor, ']');
      if (close) *close = 0;
      if (!strncmp(cursor + 1, "music:", 6)) {
        memset(&pending, 0, sizeof(pending));
        snprintf(pending.name, sizeof(pending.name), "%s", cursor + 7);
        pending.song = kMusicSongAny;
        pending.loop = true;
        pending.gain_percent = 100;
        in_entry = true;
        entry_line = line_number;
      }
      /* [replace:...] and anything else belongs to the HD parser, which
       * owns the unknown-section warning for the shared manifest. */
      continue;
    }
    if (!in_entry) continue;

    char *equals = strchr(cursor, '=');
    if (!equals) continue;
    *equals = 0;
    char *key = TrimInPlace(cursor);
    char *value = TrimInPlace(equals + 1);
    bool ok = true;
    if (!strcmp(key, "src")) {
      ok = ParseSrcAddress(value, &pending.src);
    } else if (!strcmp(key, "file")) {
      ResolveFilePath(manifest_path, value, pending.file,
                      sizeof(pending.file));
    } else if (!strcmp(key, "song")) {
      pending.song = (int)strtol(value, NULL, 0);
      ok = pending.song >= 0 && pending.song <= 0xef;
    } else if (!strcmp(key, "loop")) {
      pending.loop = strtoul(value, NULL, 0) != 0;
    } else if (!strcmp(key, "loop_start")) {
      pending.loop_start = (uint32)strtoul(value, NULL, 0);
    } else if (!strcmp(key, "loop_end")) {
      pending.loop_end = (uint32)strtoul(value, NULL, 0);
    } else if (!strcmp(key, "gain")) {
      pending.gain_percent = (int)strtol(value, NULL, 0);
      ok = pending.gain_percent >= 0 && pending.gain_percent <= 400;
    } else if (!strcmp(key, "when")) {
      ok = HdManifest_ParseWhen(value, pending.conditions, kHdMaxConditions,
                                &pending.condition_count);
    } else {
      fprintf(stderr, "[music-manifest] %s:%d: unknown key '%s' ignored\n",
              manifest_path, line_number, key);
    }
    if (!ok) {
      fprintf(stderr, "[music-manifest] %s:%d: bad value for '%s' — "
              "[music:%s] dropped\n", manifest_path, line_number, key,
              pending.name);
      in_entry = false;
    }
  }
  COMMIT_PENDING();
  #undef COMMIT_PENDING
  fclose(f);

  int with_audio = 0;
  for (int i = 0; i < g_music_replacement_count; i++) {
    MusicReplacement *entry = &g_music_replacements[i];
    ProbeEntryFile(entry);
    if (entry->has_audio) {
      with_audio++;
      fprintf(stderr, "[music-manifest] [music:%s] %s (%d Hz, %u frames, "
              "loop %u..%u)\n", entry->name, entry->file, entry->file_rate,
              entry->file_frames, entry->loop_start,
              entry->loop_end ? entry->loop_end : entry->file_frames);
    }
  }
  fprintf(stderr, "[music-manifest] %d entries, %d with audio\n",
          g_music_replacement_count, with_audio);
  return g_music_replacement_count;
}

/* ---- selection ---------------------------------------------------------- */

const MusicReplacement *MusicReplacements_Select(uint32 src, int song) {
  for (int i = 0; i < g_music_replacement_count; i++) {
    const MusicReplacement *entry = &g_music_replacements[i];
    if (entry->src != src || !entry->has_audio) continue;
    if (entry->song != kMusicSongAny && entry->song != song) continue;
    bool pass = true;
    for (int c = 0; c < entry->condition_count && pass; c++)
      pass = HdManifest_ConditionPasses(&entry->conditions[c]);
    if (pass) return entry;
  }
  return NULL;
}

/* ---- loop-region slicing (pure) ----------------------------------------- */

int MusicLoop_NextRun(uint32 pos, int want, uint32 loop_start,
                      uint32 loop_end, unsigned total, bool *hit_loop_point) {
  (void)loop_start;
  uint32 end = (loop_end && loop_end <= total) ? loop_end : total;
  if (pos >= end) {
    *hit_loop_point = true;
    return 0;
  }
  uint32 room = end - pos;
  int run = want < 0 ? 0 : want;
  if ((uint32)run > room) run = (int)room;
  *hit_loop_point = (uint32)run == room;
  return run;
}

/* ---- streaming ----------------------------------------------------------- */

static void EndSession(const char *why) {
  RtlApuLock();
  if (s.session) {
    fprintf(stderr, "[music] stop [music:%s] (%s)\n", s.session->name, why);
    if (s.v) stb_vorbis_close(s.v);
    memset(&s, 0, sizeof(s));
    g_dsp_voice_mute_srcn_min = -1;
  }
  RtlApuUnlock();
}

static void StartSession(const MusicReplacement *entry, int song) {
  RtlApuLock();
  if (s.v) stb_vorbis_close(s.v);
  int error = 0;
  stb_vorbis *v = stb_vorbis_open_filename(entry->file, &error, NULL);
  if (!v) {
    /* File vanished since the probe: fall back to authentic playback. */
    fprintf(stderr, "[music] [music:%s] open failed at play time (%d) — "
            "authentic\n", entry->name, error);
    if (s.session) g_dsp_voice_mute_srcn_min = -1;
    memset(&s, 0, sizeof(s));
    RtlApuUnlock();
    return;
  }
  s.session = entry;
  s.v = v;
  s.pos = 0;
  s.total = entry->file_frames;
  s.loop = entry->loop;
  s.loop_start = entry->loop_start;
  s.loop_end = entry->loop_end;
  s.gain_percent = entry->gain_percent;
  s.src_carry = 0.0;
  g_dsp_voice_mute_srcn_min = MUSIC_MUTE_SRCN_MIN;
  fprintf(stderr, "[music] src=%02X:%04X song=%02x -> [music:%s] %s\n",
          (unsigned)(entry->src >> 16), (unsigned)(entry->src & 0xffff),
          (unsigned)song, entry->name, entry->file);
  RtlApuUnlock();
}

/* ---- engine hooks -------------------------------------------------------- */

static void OnSpcUpload(uint32_t src) {
  s_loaded_src = src;
  if (s_musiclog)
    fprintf(stderr, "[music] upload src=%02X:%04X\n",
            (unsigned)(src >> 16), (unsigned)(src & 0xffff));
}

static void OnApuPortWrite(uint8_t port, uint8_t val) {
  if (port == 2) {
    if (val && s_musiclog)
      fprintf(stderr, "[music] event id=%02x on port 2\n", val);
    return;
  }
  if (port != 0) return;
  if (s_musiclog)
    fprintf(stderr, "[music] port0=%02x (session=%s song=%02x)\n", val,
            s.session ? s.session->name : "-",
            s_current_song < 0 ? 0xff : (unsigned)s_current_song);
  if (val == SPC_CMD_HALT) {
    /* The driver halts music instantly on $F0 (it precedes every song
     * change and transition); mirror it. */
    s_current_song = -1;
    s_driver_paused = false;
    EndSession("driver halt $F0");
    return;
  }
  if (val == SPC_CMD_PAUSE) {
    bool was_paused = PlaybackPaused();
    s_driver_paused = true;
    if (!was_paused && s.session)
      fprintf(stderr, "[music] pause [music:%s] (driver $F2)\n",
              s.session->name);
    return;
  }
  if (val == SPC_CMD_ATTENTION || val == SPC_CMD_UPLOAD || val == 0x00)
    return;

  /* Anything else on port 0 starts song `val` of the loaded image. */
  int previous_song = s_current_song;
  if (!g_settings.music_replacements) {
    s_current_song = val;
    s_driver_paused = false;
    EndSession("music_replacements off");
    return;
  }
  const MusicReplacement *entry = MusicReplacements_Select(s_loaded_src, val);
  bool resume_same_session = s_driver_paused && s.session == entry &&
                             previous_song == val;
  s_current_song = val;
  s_driver_paused = false;
  if (!entry) {
    /* No playable entry. If a stub for this src exists but has no audio yet,
     * always say which file would engage it — this line is how tracks get
     * identified and named during a normal play session. */
    const MusicReplacement *inert = NULL;
    for (int i = 0; i < g_music_replacement_count && !inert; i++) {
      const MusicReplacement *cand = &g_music_replacements[i];
      if (cand->src == s_loaded_src &&
          (cand->song == kMusicSongAny || cand->song == val))
        inert = cand;
    }
    if (inert)
      fprintf(stderr, "[music] src=%02X:%04X song=%02x authentic — drop %s "
              "to replace ([music:%s])\n", (unsigned)(s_loaded_src >> 16),
              (unsigned)(s_loaded_src & 0xffff), (unsigned)val, inert->file,
              inert->name);
    else if (s_musiclog || s.session)
      fprintf(stderr, "[music] src=%02X:%04X song=%02x authentic "
              "(no manifest entry)\n", (unsigned)(s_loaded_src >> 16),
              (unsigned)(s_loaded_src & 0xffff), (unsigned)val);
    EndSession("authentic song started");
    return;
  }
  if (resume_same_session) {
    if (!PlaybackPaused())
      fprintf(stderr, "[music] resume [music:%s] at frame %u\n",
              s.session->name, s.pos);
    else if (s_musiclog)
      fprintf(stderr, "[music] driver resume [music:%s] deferred by host "
              "pause at frame %u\n", s.session->name, s.pos);
    return;
  }
  StartSession(entry, val);
}

/* Audio thread, APU lock held (RtlRenderAudio's locked region). */
static void MixMusic(int16_t *out, int out_frames) {
  if (!s.session || !s.v || PlaybackPaused() || out_frames <= 0)
    return;

  const MusicReplacement *entry = s.session;
  int output_rate = RtlGetAudioOutputRate();
  if (output_rate <= 0) output_rate = 44100;
  double per_block = ((double)out_frames * entry->file_rate / output_rate) +
                     s.src_carry;
  int src_frames = (int)per_block;
  s.src_carry = per_block - src_frames;
  if (src_frames <= 0) return;
  if (src_frames > MUSIC_MAX_BLOCK_FRAMES) src_frames = MUSIC_MAX_BLOCK_FRAMES;

  static int16_t src[MUSIC_MAX_BLOCK_FRAMES * 2];
  int filled = 0;
  while (filled < src_frames) {
    bool hit = false;
    int run = MusicLoop_NextRun(s.pos, src_frames - filled, s.loop_start,
                                s.loop_end, s.total, &hit);
    int got = 0;
    if (run > 0)
      got = stb_vorbis_get_samples_short_interleaved(
          s.v, 2, src + (size_t)filled * 2, run * 2);
    filled += got;
    s.pos += (uint32)got;
    if (got < run) hit = true; /* decoder came up short: treat as end */
    if (hit) {
      if (!s.loop) {
        /* One-shot ended: keep the session (music voices stay muted — the
         * silent SPC sequencer must not fade back in) but close the file. */
        stb_vorbis_close(s.v);
        s.v = NULL;
        break;
      }
      if (stb_vorbis_seek(s.v, s.loop_start)) {
        s.pos = s.loop_start;
      } else {
        stb_vorbis_close(s.v);
        s.v = NULL;
        break;
      }
      if (got == 0 && run == 0 && filled == 0 && s.pos >= s.total)
        break; /* degenerate file: avoid spinning */
    }
  }
  if (filled <= 0) return;
  memset(src + (size_t)filled * 2, 0,
         (size_t)(src_frames - filled) * 2 * sizeof(int16_t));

  const int gain = s.gain_percent;
  const double step = (double)src_frames / (double)out_frames;
  double pos = 0.0;
  for (int i = 0; i < out_frames; i++) {
    int idx = (int)pos;
    double frac = pos - idx;
    int idx1 = idx + 1;
    if (idx > src_frames - 1) idx = src_frames - 1;
    if (idx1 > src_frames - 1) idx1 = src_frames - 1;
    for (int ch = 0; ch < 2; ch++) {
      int s0 = src[idx * 2 + ch];
      int s1 = src[idx1 * 2 + ch];
      int sample = (int)(s0 + (s1 - s0) * frac);
      sample = (sample * gain) / 100;
      int mixed = out[i * 2 + ch] + sample;
      out[i * 2 + ch] = (int16_t)(mixed < -32768 ? -32768
                                  : (mixed > 32767 ? 32767 : mixed));
    }
    pos += step;
  }

  /* AR_MUSICLOG: once a second, prove frames are actually reaching the mix
   * (post-mix peak) and where the read cursor sits — the counterpart of
   * AR_AUDIODBG, whose peak is measured before this hook runs. */
  if (s_musiclog) {
    static int block_count;
    if ((block_count++ % 60) == 0) {
      int peak = 0;
      for (int i = 0; i < out_frames * 2; i++) {
        int v = out[i] < 0 ? -out[i] : out[i];
        if (v > peak) peak = v;
      }
      fprintf(stderr, "[music] mixing [music:%s] pos=%u/%u peak=%d\n",
              entry->name, s.pos, s.total, peak);
    }
  }
}

void MusicReplacements_InstallHooks(void) {
  s_loaded_src = 0;
  s_current_song = -1;
  s_driver_paused = false;
  s_host_paused = false;
  g_rtl_spc_upload_hook = OnSpcUpload;
  g_rtl_apu_port_hook = OnApuPortWrite;
  g_rtl_music_mix_hook = MixMusic;
}

void MusicReplacements_ApplySetting(void) {
  if (!g_settings.music_replacements) {
    EndSession("enhanced music disabled");
    return;
  }
  if (s_current_song < 0) return;
  const MusicReplacement *entry =
      MusicReplacements_Select(s_loaded_src, s_current_song);
  RtlApuLock();
  bool already_active = s.session == entry;
  RtlApuUnlock();
  if (entry && !already_active)
    StartSession(entry, s_current_song);
  else if (!entry)
    EndSession("no replacement for current song");
}

void MusicReplacements_SetHostPaused(bool paused) {
  RtlApuLock();
  bool was_paused = PlaybackPaused();
  s_host_paused = paused;
  bool now_paused = PlaybackPaused();
  if (was_paused != now_paused && s.session) {
    fprintf(stderr, "[music] host %s [music:%s] at frame %u\n",
            now_paused ? "pause" : "resume", s.session->name, s.pos);
  }
  RtlApuUnlock();
}

bool MusicReplacements_IsPlaybackPaused(void) {
  RtlApuLock();
  bool paused = PlaybackPaused();
  RtlApuUnlock();
  return paused;
}

void MusicReplacements_FormatPlaybackStatus(char *buffer, size_t buffer_size) {
  if (!buffer || !buffer_size) return;
  RtlApuLock();
  if (s_current_song < 0) {
    snprintf(buffer, buffer_size, "MUSIC NONE");
    RtlApuUnlock();
    return;
  }
  const MusicReplacement *identity = s.session;
  for (int i = 0; i < g_music_replacement_count && !identity; i++) {
    const MusicReplacement *candidate = &g_music_replacements[i];
    if (candidate->src == s_loaded_src &&
        (candidate->song == kMusicSongAny ||
         candidate->song == s_current_song))
      identity = candidate;
  }
  if (identity) {
    snprintf(buffer, buffer_size, "MUSIC %.14s $%02X %s%s",
             identity->name, (unsigned)s_current_song,
             s.session ? "ENH" : "AUTH", PlaybackPaused() ? " PAUSED" : "");
  } else {
    snprintf(buffer, buffer_size, "MUSIC %02X:%04X $%02X AUTH%s",
             (unsigned)(s_loaded_src >> 16),
             (unsigned)(s_loaded_src & 0xffff),
             (unsigned)s_current_song, PlaybackPaused() ? " PAUSED" : "");
  }
  RtlApuUnlock();
}

void MusicReplacements_FrameTick(void) {
  if (!g_settings.music_replacements && s.session)
    EndSession("music_replacements off");
}
