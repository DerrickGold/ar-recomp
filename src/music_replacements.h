#ifndef MUSIC_REPLACEMENTS_H
#define MUSIC_REPLACEMENTS_H

#include <stdbool.h>
#include "types.h"
#include "hd_replacements.h" /* shared HdCondition gate grammar */

/* Manifest-driven music replacement ([music:<name>] sections of
 * game-assets/manifest.ini): stream an OGG Vorbis file in place of an SPC
 * driver song, keyed off the song's SPC-image upload source address.
 *
 * How a song plays authentically (decoded from $02:B63B / $00:A3FE and an
 * AR_APULOG boot capture — see docs/SEAMS.md "Audio"):
 *   1. CPU writes $F0 to APU port 0 ($2140): driver halts music, acks 0.
 *   2. CPU writes $FF: driver parks in its resident uploader.
 *   3. The $02:9964 HLE memcpys the song image + BRR samples into ARAM.
 *      The image SOURCE ADDRESS uniquely names the song — our identity key.
 *   4. CPU writes the song number (e.g. $01) to port 0: sequencer starts.
 * Port 2 carries per-frame event ids (COP -> $035A), port 3 SFX ids
 * (BRK -> $035B); both are forwarded by the NMI tail at $02:AC33.
 *
 * Replacement model: every port write and upload still happens authentically
 * (no handshake is suppressed, so no soft-lock risk). When a play command
 * names a song with a matching manifest entry whose .ogg exists, the host
 * streams the file into the final mix and mutes the DSP voices whose srcn is
 * >= 0x0C — per-song instruments live there, while SFX use the common sample
 * bank (srcn 00-0B) by design and stay audible. $F0 (halt) stops the stream;
 * a new play command switches it. Missing file / no entry = fully authentic
 * playback.
 *
 * Threading: trigger callbacks run on the game thread (under the APU lock —
 * RtlApuWrite holds it); the mix callback runs on the audio thread inside
 * RtlRenderAudio's locked region. All streamer state is therefore
 * lock-serialised the same way msu1.c is. */

enum {
  kMusicMaxReplacements = 32,
  kMusicSongAny = -1,
};

typedef struct MusicReplacement {
  char name[kHdMaxName];
  uint32 src;            /* SPC image source address (BB:AAAA), the identity */
  int song;              /* driver song number, kMusicSongAny = any */
  char file[kHdMaxPath]; /* resolved relative to the manifest */
  bool loop;             /* default true */
  /* Loop points in sample frames at the FILE's rate. loop_end 0 = end of
   * file. Manifest keys override LOOPSTART/LOOPLENGTH(-END) Vorbis comment
   * tags, which override whole-file looping. */
  uint32 loop_start, loop_end;
  int gain_percent;      /* default 100 */
  HdCondition conditions[kHdMaxConditions];
  int condition_count;

  /* Probe results (filled at load): the file exists and decodes. Entries
   * without audio stay fully inert, mirroring the HD manifest convention. */
  bool has_audio;
  int file_rate;
  unsigned file_frames;
} MusicReplacement;

extern MusicReplacement g_music_replacements[kMusicMaxReplacements];
extern int g_music_replacement_count;

/* Parse [music:] sections of the shared manifest and probe each entry's file
 * (existence, rate, length, loop tags). Returns entries loaded; 0 with no
 * output if the manifest does not exist. */
int MusicReplacements_Load(const char *manifest_path);

/* Install the engine trigger/mix hooks (upload observer, APU port observer,
 * music mix). Call once at startup after Load. Safe headless. */
void MusicReplacements_InstallHooks(void);

/* Apply the live enhanced-music setting. Disabling immediately stops the OGG
 * stream and unmutes the authentic SPC voices; enabling can adopt the song
 * already playing instead of waiting for the next song-change command. */
void MusicReplacements_ApplySetting(void);

/* Suspend/resume the replacement decoder for host-owned pauses (P and the
 * settings overlay). Native in-game pause is tracked independently from the
 * driver's $F2 command. Neither path closes the stream or advances its cursor;
 * playback resumes only after both pause reasons clear. */
void MusicReplacements_SetHostPaused(bool paused);

/* Combined native/host pause state, exposed for diagnostics and tests. */
bool MusicReplacements_IsPlaybackPaused(void);

/* Per-frame safety policy for non-registry writers of g_settings. */
void MusicReplacements_FrameTick(void);

/* First entry matching (src, song) whose gates pass and whose audio loaded;
 * NULL if none. Entries are tried in manifest order, so gated variants
 * placed above an ungated fallback win. Exposed for tests. */
const MusicReplacement *MusicReplacements_Select(uint32 src, int song);

/* Loop-region slicing (pure, exposed for tests): with the read cursor at
 * `pos` (frames) wanting `want` frames, return how many contiguous frames to
 * decode now. When that many frames lands exactly on the loop point (or the
 * end of a non-looping file is reached), *seek_to is set to the frame to
 * seek to before continuing (loop start), or left untouched when no seek is
 * needed. `total` is the file length in frames (loop_end 0 uses it). */
int MusicLoop_NextRun(uint32 pos, int want, uint32 loop_start,
                      uint32 loop_end, unsigned total, bool *hit_loop_point);

#endif /* MUSIC_REPLACEMENTS_H */
