#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sfx_census.h"
#include "run_dir.h"

/* Engine seams. The APU mutex is recursive, so handlers may take it even when
 * the caller already holds it (both of ours are called with it held). */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);
extern void (*g_rtl_apu_port_hook)(uint8_t port, uint8_t val);
extern void (*g_dsp_voice_kon_hook)(int ch, uint8_t srcn, uint16_t decodeOffset,
                                    int volL, int volR, uint16_t pitch);
extern uint64_t snes_apu_cycle_count(void);

/* Per-song instruments start here; anything below is the shared bank that
 * carries the SFX. Key-ons at or above this are music and are not candidates
 * for correlation. Kept in sync with music_replacements.c by intent, not by
 * #include — the census must keep working if that gate is ever retuned. */
#define SFX_SRCN_MAX 0x0C

/* A request and the key-on it caused are separated by: the NMI's even-frame
 * mailbox drain, the scheduled port-write delay, and the driver's own poll
 * latency. Two game frames of APU cycles is comfortably past all three while
 * staying short enough that unrelated key-ons rarely fall inside. Correlation
 * is inherently heuristic — the protocol carries no tag — so the report prints
 * the evidence (how many key-ons landed in-window per id) rather than
 * pretending each mapping is certain. */
#define SFX_CORRELATE_WINDOW_CYCLES 68266u  /* ~2 frames @ 2.048 MHz */

#define SFX_MAX_FNS 6      /* distinct calling functions tracked per id */
#define SFX_MAX_SRCN 8     /* distinct srcn observed per id */

typedef struct {
  const char *name;
  unsigned count;
} SfxFnRef;

typedef struct {
  unsigned requests;
  unsigned correlated;      /* key-ons attributed to this id */
  unsigned unmatched;       /* requests that produced no key-on in-window */

  SfxFnRef fns[SFX_MAX_FNS];
  int fn_count;
  unsigned fn_overflow;

  uint8_t srcn[SFX_MAX_SRCN];
  unsigned srcn_count[SFX_MAX_SRCN];
  int srcn_n;
  unsigned srcn_overflow;

  uint8_t voice_mask;
  uint16_t pitch_min, pitch_max;
  int vol_l_min, vol_l_max;   /* the driver's own pan for this effect */
  int vol_r_min, vol_r_max;

  uint16_t last_x, last_y;    /* CPU X/Y at the last request: the actor handle */
  unsigned first_frame, last_frame;
} SfxEntry;

static SfxEntry s_sfx[256];
static int s_enabled = -1;

/* The single request awaiting correlation. The mailbox holds one id at a time
 * and the NMI clears it on drain, so a depth-1 slot matches the protocol; a
 * second request before the first is claimed simply supersedes it (and the
 * first is counted unmatched). */
static struct {
  int id;                   /* -1 = empty */
  uint64_t deadline;        /* apu cycle after which the request is stale */
  unsigned claimed;
} s_pending = { -1, 0, 0 };

/* Key-ons seen while no request was outstanding — these are the driver's own
 * music/ambience in the shared bank, i.e. exactly the voices that a naive
 * srcn-based music/SFX split would misclassify. Counted per srcn so the report
 * can call them out. */
static unsigned s_orphan_kon[SFX_SRCN_MAX];
static unsigned s_orphan_total;

static void (*s_prev_port_hook)(uint8_t port, uint8_t val);

static int CensusEnabled(void) {
  if (s_enabled < 0) s_enabled = getenv("AR_SFXCENSUS") ? 1 : 0;
  return s_enabled;
}

static void NotePending(unsigned *unmatched_out) {
  if (s_pending.id >= 0 && !s_pending.claimed) {
    s_sfx[s_pending.id].unmatched++;
    if (unmatched_out) (*unmatched_out)++;
  }
}

/* ---- CPU side ------------------------------------------------------------ */

void SfxCensus_OnRequest(uint8_t id, const char *fn, unsigned game_frame,
                         uint16_t cpu_x, uint16_t cpu_y) {
  if (!CensusEnabled()) return;
  SfxEntry *e = &s_sfx[id];

  /* id 0 is the idle/clear post, not a sound: the game writes it constantly
   * (754 posts vs 12 key-ons in the first real capture) and the NMI forwards
   * zero as "nothing pending". Record it for caller attribution, but never arm
   * the correlation window with it — a dead id left pending claims whatever
   * unrelated key-on happens next and corrupts the orphan accounting. */
  if (id != 0) {
    RtlApuLock();
    NotePending(NULL);
    s_pending.id = id;
    s_pending.claimed = 0;
    s_pending.deadline = snes_apu_cycle_count() + SFX_CORRELATE_WINDOW_CYCLES;
    RtlApuUnlock();
  }

  if (!e->requests) e->first_frame = game_frame;
  e->last_frame = game_frame;
  e->requests++;
  e->last_x = cpu_x;
  e->last_y = cpu_y;

  if (fn) {
    int i;
    for (i = 0; i < e->fn_count; i++)
      if (e->fns[i].name == fn || strcmp(e->fns[i].name, fn) == 0) {
        e->fns[i].count++;
        break;
      }
    if (i == e->fn_count) {
      if (e->fn_count < SFX_MAX_FNS) {
        e->fns[e->fn_count].name = fn;   /* static string from the recompiler */
        e->fns[e->fn_count].count = 1;
        e->fn_count++;
      } else {
        e->fn_overflow++;
      }
    }
  }
}

/* ---- APU port side ------------------------------------------------------- */

/* Chained: music_replacements.c owns this seam for the port-0 song protocol,
 * so the census must forward rather than displace it. Installed after music,
 * so s_prev_port_hook is music's handler. */
static void OnApuPortWrite(uint8_t port, uint8_t val) {
  if (s_prev_port_hook) s_prev_port_hook(port, val);
  if (port != 3 || val == 0) return;
  /* Port 3 carries the high byte of the NMI's packed mailbox store = the BRK
   * sound id. Refresh the correlation deadline from the moment the driver can
   * actually see the request, which is more accurate than the BRK timestamp
   * (the request sat in the mailbox until the next even frame). */
  if (s_pending.id == (int)val) {
    s_pending.deadline = snes_apu_cycle_count() + SFX_CORRELATE_WINDOW_CYCLES;
  } else {
    /* Delivered an id the CPU side never recorded (or recorded then
     * superseded): still correlate it, just without caller attribution. */
    NotePending(NULL);
    s_pending.id = val;
    s_pending.claimed = 0;
    s_pending.deadline = snes_apu_cycle_count() + SFX_CORRELATE_WINDOW_CYCLES;
  }
}

/* ---- DSP side ------------------------------------------------------------ */

static void OnVoiceKeyOn(int ch, uint8_t srcn, uint16_t decodeOffset,
                         int volL, int volR, uint16_t pitch) {
  (void)decodeOffset;
  if (srcn >= SFX_SRCN_MAX) return;   /* music voice, not a census candidate */

  uint64_t now = snes_apu_cycle_count();
  if (s_pending.id < 0 || now > s_pending.deadline) {
    if (s_pending.id >= 0 && !s_pending.claimed) {
      s_sfx[s_pending.id].unmatched++;
      s_pending.id = -1;
    }
    s_orphan_kon[srcn]++;
    s_orphan_total++;
    return;
  }

  SfxEntry *e = &s_sfx[s_pending.id];
  e->correlated++;
  s_pending.claimed++;
  e->voice_mask |= (uint8_t)(1u << ch);

  if (!e->pitch_min || pitch < e->pitch_min) e->pitch_min = pitch;
  if (pitch > e->pitch_max) e->pitch_max = pitch;
  if (e->correlated == 1) {
    e->vol_l_min = e->vol_l_max = volL;
    e->vol_r_min = e->vol_r_max = volR;
  } else {
    if (volL < e->vol_l_min) e->vol_l_min = volL;
    if (volL > e->vol_l_max) e->vol_l_max = volL;
    if (volR < e->vol_r_min) e->vol_r_min = volR;
    if (volR > e->vol_r_max) e->vol_r_max = volR;
  }

  int i;
  for (i = 0; i < e->srcn_n; i++)
    if (e->srcn[i] == srcn) { e->srcn_count[i]++; break; }
  if (i == e->srcn_n) {
    if (e->srcn_n < SFX_MAX_SRCN) {
      e->srcn[e->srcn_n] = srcn;
      e->srcn_count[e->srcn_n] = 1;
      e->srcn_n++;
    } else {
      e->srcn_overflow++;
    }
  }
}

/* ---- lifecycle ----------------------------------------------------------- */

void SfxCensus_Init(void) {
  if (!CensusEnabled()) return;
  s_prev_port_hook = g_rtl_apu_port_hook;
  g_rtl_apu_port_hook = OnApuPortWrite;
  g_dsp_voice_kon_hook = OnVoiceKeyOn;
  fprintf(stderr, "[sfx-census] enabled — correlating BRK sound requests with "
                  "shared-bank (srcn < %02x) key-ons\n", SFX_SRCN_MAX);
}

static void WriteReport(FILE *f) {
  unsigned total_req = 0, total_corr = 0, ids = 0;
  for (int i = 0; i < 256; i++) {
    if (!s_sfx[i].requests && !s_sfx[i].correlated) continue;
    ids++;
    total_req += s_sfx[i].requests;
    total_corr += s_sfx[i].correlated;
  }

  fprintf(f, "# ActRaiser SFX census\n");
  fprintf(f, "# %u distinct sound ids, %u requests, %u correlated key-ons\n",
          ids, total_req, total_corr);
  fprintf(f, "# srcn = shared-bank sample the driver keyed for this id.\n"
             "# volL/volR = the driver's own pan (the baseline any positional\n"
             "#   panning has to compose with, not replace).\n"
             "# x/y = CPU X/Y at the last request — the actor handle the sound\n"
             "#   came from, for resolving an origin later.\n\n");

  fprintf(f, "# id 00 = idle/clear post, counted but never correlated.\n\n");
  fprintf(f, "%-4s %-8s %-10s %-18s %-11s %-13s %-9s %s\n",
          "id", "requests", "keyons", "srcn(count)", "voices",
          "volL/volR", "pitch", "callers");
  for (int i = 0; i < 256; i++) {
    SfxEntry *e = &s_sfx[i];
    if (!e->requests && !e->correlated) continue;

    char srcn_buf[64] = "-";
    int off = 0;
    for (int k = 0; k < e->srcn_n; k++)
      off += snprintf(srcn_buf + off, sizeof srcn_buf - off, "%s%02x(%u)",
                      k ? "," : "", e->srcn[k], e->srcn_count[k]);
    if (e->srcn_overflow)
      snprintf(srcn_buf + off, sizeof srcn_buf - off, ",+%u", e->srcn_overflow);

    char vox[16] = "-";
    if (e->voice_mask) {
      int o = 0;
      for (int c = 0; c < 8; c++)
        if (e->voice_mask & (1u << c))
          o += snprintf(vox + o, sizeof vox - o, "%s%d", o ? "," : "", c);
    }

    char vol[24] = "-";
    if (e->correlated)
      snprintf(vol, sizeof vol, "%d-%d/%d-%d", e->vol_l_min, e->vol_l_max,
               e->vol_r_min, e->vol_r_max);

    char pitch[16] = "-";
    if (e->correlated)
      snprintf(pitch, sizeof pitch, "%04x-%04x", e->pitch_min, e->pitch_max);

    fprintf(f, "%02x   %-8u %-10u %-18s %-11s %-13s %-9s ",
            i, e->requests, e->correlated, srcn_buf, vox, vol, pitch);
    for (int k = 0; k < e->fn_count; k++)
      fprintf(f, "%s%s(%u)", k ? " " : "", e->fns[k].name, e->fns[k].count);
    if (e->fn_overflow) fprintf(f, " +%u more", e->fn_overflow);
    if (!e->fn_count) fprintf(f, "(no CPU-side request seen)");
    fprintf(f, "\n");
    if (e->unmatched)
      fprintf(f, "     ^ %u request(s) produced no key-on in-window "
                 "(masked by an already-sounding voice, or silent id)\n",
              e->unmatched);
    if (e->requests)
      fprintf(f, "     ^ last actor context: X=%04x Y=%04x  frames %u..%u\n",
              e->last_x, e->last_y, e->first_frame, e->last_frame);
  }

  fprintf(f, "\n# Shared-bank key-ons with NO outstanding request (%u total).\n"
             "# These are music or driver ambience living in the SFX bank — the\n"
             "# voices a plain srcn-threshold music/SFX split gets wrong.\n",
          s_orphan_total);
  for (int i = 0; i < SFX_SRCN_MAX; i++)
    if (s_orphan_kon[i])
      fprintf(f, "#   srcn %02x: %u key-ons\n", i, s_orphan_kon[i]);
}

void SfxCensus_Report(void) {
  if (!CensusEnabled()) return;
  unsigned any = s_orphan_total;
  for (int i = 0; i < 256 && !any; i++)
    any = s_sfx[i].requests + s_sfx[i].correlated;
  if (!any) {
    fprintf(stderr, "[sfx-census] nothing captured\n");
    return;
  }

  char path[512];
  RunDirFile(path, sizeof path, "sfx_census.txt");
  FILE *f = fopen(path, "w");
  if (f) {
    WriteReport(f);
    fclose(f);
    fprintf(stderr, "[sfx-census] wrote %s\n", path);
  }
  WriteReport(stderr);
}
