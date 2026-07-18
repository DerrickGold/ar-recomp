/* ar_trace — unified single-run runtime trace. See ar_trace.h. */
#include "ar_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"    /* g_cpu — runtime m/x for the prefix */

/* Externs from the runner (no new headers needed). */
extern int          snes_frame_counter;      /* host/game frame tick */
extern unsigned char g_ram[0x20000];          /* WRAM; $0088 = game frame */
extern const char  *g_last_recomp_func;       /* current recomp function */
extern uint32_t     g_ar_blk_ring[];          /* block-PC ring */
extern unsigned     g_ar_blk_idx;

/* ---- config (parsed once) ---- */
static int      s_init = 0;
static FILE    *s_fp = NULL;
static long     s_hf_lo = -1, s_hf_hi = -1;   /* host-frame window */
/* Default channel mask: EVERYTHING EXCEPT the two highest-volume channels
 * (wram, stack), which need an address range (AR_TRACE_WLO/WHI) anyway. The
 * root-event finders (dispmiss, garbage) are ON by default — the whole point is
 * to never again miss a signal because a channel wasn't selected. */
#define AR_TR_DEFAULT (0x3FFF & ~(AR_TR_WRAM | AR_TR_STACK))
static int      s_ch = AR_TR_DEFAULT;         /* channel mask */
static unsigned s_vlo = 0, s_vhi = 0x7fff;    /* vram word-addr filter */
static unsigned s_wlo = 0, s_whi = 0x1ffff;   /* wram g_ram-offset filter */
static char     s_func_sub[48] = "";          /* func-name substring filter */
static uint64_t s_seq = 0;

/* ---- always-on anomaly-capture ("watch") mode ----
 * AR_TRACE_WATCH=<dir-or-prefix>: instead of a fixed host-frame window, keep a
 * rolling in-memory RING of the last N trace lines and, when an anomaly fires
 * (a dispatch-miss with S<$0200, a garbage-variant, or an m/x leak), auto-dump
 * the ring + the next K lines to `<prefix>_hf<frame>_<kind>.jsonl`. This is the
 * "log everything as it happens, no replay needed" mode — you play manually and
 * the tool has ALREADY captured the window around whatever went wrong. */
static int      s_watch = 0;
static char     s_watch_pfx[256];
static int      s_ring_n = 4096;              /* AR_TRACE_RING */
static int      s_post_k = 400;               /* lines captured AFTER an anomaly */
#define AR_LINE_MAX 240
static char   (*s_ring)[AR_LINE_MAX];         /* heap ring of formatted lines */
static int      s_ring_head = 0, s_ring_fill = 0;
static char     s_linebuf[1024];
static int      s_linelen = 0;
static int      s_pending_anom = 0;           /* set by an emitter before its line */
static const char *s_anom_kind = NULL;
static FILE    *s_anom_fp = NULL;
static int      s_post_countdown = -1;
static uint32_t s_dumped_keys[512];           /* dedup (kind-hash ^ target) */
static int      s_ndumped = 0;
static int      s_anom_count = 0;

static int already_dumped(uint32_t key) {
  for (int i = 0; i < s_ndumped; i++) if (s_dumped_keys[i] == key) return 1;
  if (s_ndumped < 512) s_dumped_keys[s_ndumped++] = key;
  return 0;
}

/* Called once per completed trace line (from the funopen write callback). */
static void ring_push_and_maybe_dump(const char *line) {
  /* store in ring */
  strncpy(s_ring[s_ring_head], line, AR_LINE_MAX - 1);
  s_ring[s_ring_head][AR_LINE_MAX - 1] = 0;
  s_ring_head = (s_ring_head + 1) % s_ring_n;
  if (s_ring_fill < s_ring_n) s_ring_fill++;

  /* if we're capturing the aftermath of an anomaly, append + count down */
  if (s_post_countdown >= 0 && s_anom_fp) {
    fputs(line, s_anom_fp);
    if (--s_post_countdown < 0) { fclose(s_anom_fp); s_anom_fp = NULL; }
  }

  /* a fresh anomaly on this line → open a dump, flush the ring, start aftermath */
  if (s_pending_anom && s_post_countdown < 0) {
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    (void)gf;
    char path[320];
    snprintf(path, sizeof path, "%s_hf%d_%s%d.jsonl",
             s_watch_pfx, snes_frame_counter, s_anom_kind, s_anom_count++);
    s_anom_fp = fopen(path, "w");
    if (s_anom_fp) {
      /* dump ring oldest->newest */
      int idx = (s_ring_head - s_ring_fill + s_ring_n) % s_ring_n;
      for (int i = 0; i < s_ring_fill; i++) {
        fputs(s_ring[idx], s_anom_fp);
        idx = (idx + 1) % s_ring_n;
      }
      s_post_countdown = s_post_k;
      fprintf(stderr, "[ar_trace] ANOMALY '%s' hf=%d -> %s (ring=%d +%d)\n",
              s_anom_kind, snes_frame_counter, path, s_ring_fill, s_post_k);
    }
  }
  s_pending_anom = 0;
}

/* funopen write callback: accumulate chars until newline, then push the line. */
static int watch_write(void *cookie, const char *buf, int len) {
  (void)cookie;
  for (int i = 0; i < len; i++) {
    char c = buf[i];
    if (s_linelen < (int)sizeof(s_linebuf) - 1) s_linebuf[s_linelen++] = c;
    if (c == '\n') {
      s_linebuf[s_linelen] = 0;
      ring_push_and_maybe_dump(s_linebuf);
      s_linelen = 0;
    }
  }
  return len;
}

static void parse_channels(const char *csv) {
  if (!csv || !csv[0]) { s_ch = AR_TR_DEFAULT; return; }
  s_ch = 0;
  if (strstr(csv, "func"))  s_ch |= AR_TR_FUNC;
  if (strstr(csv, "vram"))  s_ch |= AR_TR_VRAM;
  if (strstr(csv, "vmadd")) s_ch |= AR_TR_VMADD;
  if (strstr(csv, "reg"))   s_ch |= AR_TR_REG;
  if (strstr(csv, "dma"))   s_ch |= AR_TR_DMA;
  if (strstr(csv, "mx"))    s_ch |= AR_TR_MX;
  if (strstr(csv, "call"))     s_ch |= AR_TR_CALL;
  if (strstr(csv, "dispmiss")) s_ch |= AR_TR_DISPMISS;
  if (strstr(csv, "garbage"))  s_ch |= AR_TR_GARBAGE;
  if (strstr(csv, "wram"))     s_ch |= AR_TR_WRAM;
  if (strstr(csv, "stack"))    s_ch |= AR_TR_STACK;
  if (strstr(csv, "hwread"))   s_ch |= AR_TR_HWREAD;
  if (strstr(csv, "ppumem"))   s_ch |= AR_TR_PPUMEM;
  if (strstr(csv, "frame"))    s_ch |= AR_TR_FRAME;
}

static void trace_init(void) {
  s_init = 1;
  const char *watch = getenv("AR_TRACE_WATCH");
  const char *path = getenv("AR_TRACE");
  /* A targeted windowed capture (AR_TRACE=<file>) beats ambient watch mode:
   * dev-config.ini keeps AR_TRACE_WATCH always-on, and a deliberate env
   * AR_TRACE for one run must not be silently ignored. */
  if (path && path[0]) watch = NULL;
  if (watch && watch[0]) {
    /* always-on anomaly-capture mode: s_fp is a memory stream feeding the ring. */
    s_watch = 1;
    strncpy(s_watch_pfx, watch, sizeof s_watch_pfx - 1);
    const char *r = getenv("AR_TRACE_RING"), *k = getenv("AR_TRACE_POST");
    if (r) s_ring_n = (int)strtoul(r, NULL, 0);
    if (k) s_post_k = (int)strtoul(k, NULL, 0);
    if (s_ring_n < 16) s_ring_n = 16;
    s_ring = calloc((size_t)s_ring_n, AR_LINE_MAX);
    s_fp = s_ring ? funopen(NULL, NULL, watch_write, NULL, NULL) : NULL;
    if (!s_fp) { fprintf(stderr, "[ar_trace] watch: alloc/stream failed\n"); return; }
    setvbuf(s_fp, NULL, _IONBF, 0);
    const char *chenv = getenv("AR_TRACE_CH");
    if (chenv && chenv[0]) parse_channels(chenv);
    else s_ch = AR_TR_FUNC | AR_TR_CALL | AR_TR_DISPMISS | AR_TR_GARBAGE
              | AR_TR_FRAME | AR_TR_VMADD | AR_TR_REG;  /* lean control-flow default */
    fprintf(stderr, "[ar_trace] WATCH mode -> %s_hf<frame>_<anom>.jsonl  "
            "ring=%d post=%d ch=0x%x\n", s_watch_pfx, s_ring_n, s_post_k, s_ch);
    return;
  }
  if (!path || !path[0]) { s_fp = NULL; return; }
  s_fp = fopen(path, "w");
  if (!s_fp) { fprintf(stderr, "[ar_trace] cannot open %s\n", path); return; }
  const char *a = getenv("AR_TRACE_HF_LO"), *b = getenv("AR_TRACE_HF_HI");
  s_hf_lo = a ? atol(a) : -1;
  s_hf_hi = b ? atol(b) : -1;
  parse_channels(getenv("AR_TRACE_CH"));
  const char *vl = getenv("AR_TRACE_VLO"), *vh = getenv("AR_TRACE_VHI");
  if (vl) s_vlo = (unsigned)strtoul(vl, NULL, 0);
  if (vh) s_vhi = (unsigned)strtoul(vh, NULL, 0);
  const char *wl = getenv("AR_TRACE_WLO"), *wh = getenv("AR_TRACE_WHI");
  if (wl) s_wlo = (unsigned)strtoul(wl, NULL, 0);
  if (wh) s_whi = (unsigned)strtoul(wh, NULL, 0);
  const char *fs = getenv("AR_TRACE_FUNC");
  if (fs) { strncpy(s_func_sub, fs, sizeof s_func_sub - 1); }
  fprintf(stderr, "[ar_trace] -> %s  hf=[%ld,%ld] ch=0x%02x vram=[$%04x,$%04x] func~'%s'\n",
          path, s_hf_lo, s_hf_hi, s_ch, s_vlo, s_vhi, s_func_sub);
}

int ar_trace_active(void) {
  if (!s_init) trace_init();
  if (!s_fp) return 0;
  if (s_watch) return 1;               /* no window in watch mode — always recording */
  int hf = snes_frame_counter;
  if (s_hf_lo >= 0 && hf < s_hf_lo) return 0;
  if (s_hf_hi >= 0 && hf > s_hf_hi) return 0;
  return 1;
}

int ar_trace_ch(int chbit) { return (s_ch & chbit) != 0; }

/* Shared prefix: {"seq":N,"hf":H,"gf":G,"blk":"0xNNNNNN","fn":"...", ...  */
extern CpuState g_cpu;

static void emit_prefix(const char *ch) {
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
  uint32_t blk = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
  const char *fn = g_last_recomp_func ? g_last_recomp_func : "?";
  /* mnow/xnow/S/DB = the LIVE cpu state at this event — a single trace shows
   * where m/x flips (exit-mx leak), where S drifts (stack corruption), and where
   * DB is wrong (wrong-bank pointer/DMA) without a second run. */
  fprintf(s_fp, "{\"seq\":%llu,\"hf\":%d,\"gf\":%u,\"ch\":\"%s\",\"blk\":\"%06X\","
          "\"mnow\":%d,\"xnow\":%d,\"S\":\"%04X\",\"DB\":\"%02X\",\"PB\":\"%02X\",\"fn\":\"%s\"",
          (unsigned long long)s_seq++, snes_frame_counter, gf, ch, blk,
          g_cpu.m_flag & 1, g_cpu.x_flag & 1, g_cpu.S, g_cpu.DB, g_cpu.PB, fn);
}

static int func_ok(const char *name) {
  if (!s_func_sub[0]) return 1;
  return name && strstr(name, s_func_sub) != NULL;
}

void ar_trace_func(uint32_t pc24, const char *name, int m, int x, int em, int ex) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_FUNC)) return;
  if (!func_ok(name)) return;
  emit_prefix("func");
  int bad = ((m & 1) != (em & 1)) || ((x & 1) != (ex & 1));
  fprintf(s_fp, ",\"pc\":\"%06X\",\"m\":%d,\"x\":%d,\"em\":%d,\"ex\":%d,\"misdecode\":%d}\n",
          pc24, m & 1, x & 1, em & 1, ex & 1, bad);
}

void ar_trace_call(uint32_t pc24, const char *name, int m, int x, int em, int ex) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_CALL)) return;
  if (!func_ok(name)) return;
  int bad = ((m & 1) != (em & 1)) || ((x & 1) != (ex & 1));
  /* Anomaly: an m/x leak (runtime != decoder-expected) at a call site. Skip the
   * benign NMI/IRQ-handler entries. Dedup per call-site PC. */
  if (s_watch && bad && name && !strstr(name, "Handler")
      && !already_dumped(0x1EA00000u ^ pc24)) {
    s_pending_anom = 1; s_anom_kind = "leak";
  }
  emit_prefix("call");
  fprintf(s_fp, ",\"site\":\"%06X\",\"m\":%d,\"x\":%d,\"em\":%d,\"ex\":%d,\"leak\":%d}\n",
          pc24, m & 1, x & 1, em & 1, ex & 1, bad);
}

void ar_trace_vram(uint16_t vaddr, uint16_t val, const char *path) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_VRAM)) return;
  vaddr &= 0x7fff;
  if (vaddr < s_vlo || vaddr > s_vhi) return;
  emit_prefix("vram");
  fprintf(s_fp, ",\"va\":\"%04X\",\"val\":\"%04X\",\"path\":\"%s\"}\n", vaddr, val, path);
}

void ar_trace_vmadd(uint16_t vmadd, const char *how) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_VMADD)) return;
  emit_prefix("vmadd");
  fprintf(s_fp, ",\"vmadd\":\"%04X\",\"how\":\"%s\"}\n", vmadd, how);
}

void ar_trace_reg(uint16_t reg, uint8_t val) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_REG)) return;
  emit_prefix("reg");
  fprintf(s_fp, ",\"reg\":\"%04X\",\"val\":\"%02X\"}\n", reg, val);
}

void ar_trace_dma(int chan, uint8_t bAdr, uint8_t aBank, uint16_t aAdr,
                  uint32_t size, int fromB) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_DMA)) return;
  emit_prefix("dma");
  fprintf(s_fp, ",\"dch\":%d,\"bAdr\":\"%02X\",\"src\":\"%02X:%04X\",\"size\":%u,\"fromB\":%d}\n",
          chan, bAdr, aBank, aAdr, size, fromB ? 1 : 0);
}

void ar_trace_dispmiss(uint32_t from_pc, uint32_t to_pc) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_DISPMISS)) return;
  /* Anomaly trigger: an RTS-trick continuation miss with S<$0200 — the class the
   * stderr tripwire hides (the $9D8E lair-seal bug). Dedup per target. */
  if (s_watch && g_cpu.S < 0x0200 && !already_dumped(0xD1500000u ^ to_pc)) {
    s_pending_anom = 1; s_anom_kind = "dispmiss";
  }
  emit_prefix("dispmiss");
  fprintf(s_fp, ",\"from\":\"%06X\",\"to\":\"%06X\"}\n", from_pc, to_pc);
}

void ar_trace_garbage(uint32_t pc24, const char *name, int m, int x) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_GARBAGE)) return;
  if (s_watch && !already_dumped(0x6A160000u ^ pc24)) {
    s_pending_anom = 1; s_anom_kind = "garbage";
  }
  emit_prefix("garbage");
  fprintf(s_fp, ",\"pc\":\"%06X\",\"m\":%d,\"x\":%d,\"variant\":\"%s\"}\n",
          pc24, m & 1, x & 1, name ? name : "?");
}

void ar_trace_wram(uint32_t off, uint16_t old, uint16_t val, int width) {
  if (!ar_trace_active()) return;
  /* stack-page write ($0100-$01FF) = a push; emit provenance even if the wram
   * address filter excludes it (S-corruption hunts want every push). */
  if (ar_trace_ch(AR_TR_STACK) && off >= 0x100 && off < 0x200) {
    emit_prefix("stack");
    fprintf(s_fp, ",\"off\":\"%05X\",\"old\":\"%04X\",\"val\":\"%04X\",\"w\":%d}\n",
            off, old, val, width);
  }
  if (!ar_trace_ch(AR_TR_WRAM)) return;
  if (off < s_wlo || off > s_whi) return;
  emit_prefix("wram");
  fprintf(s_fp, ",\"off\":\"%05X\",\"old\":\"%04X\",\"val\":\"%04X\",\"w\":%d}\n",
          off, old, val, width);
}

void ar_trace_hwread(uint16_t reg, uint8_t val) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_HWREAD)) return;
  emit_prefix("hwread");
  fprintf(s_fp, ",\"reg\":\"%04X\",\"val\":\"%02X\"}\n", reg, val);
}

void ar_trace_ppumem(const char *mem, uint16_t addr, uint16_t val) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_PPUMEM)) return;
  emit_prefix("ppumem");
  fprintf(s_fp, ",\"mem\":\"%s\",\"addr\":\"%04X\",\"val\":\"%04X\"}\n",
          mem ? mem : "?", addr, val);
}

void ar_trace_frame(const char *what) {
  if (!ar_trace_active() || !ar_trace_ch(AR_TR_FRAME)) return;
  emit_prefix("frame");
  fprintf(s_fp, ",\"what\":\"%s\"}\n", what ? what : "?");
}

/* Force-dump the watch ring NOW (e.g. from the watchdog on a hang — there's no
 * "aftermath" to capture since the game is frozen, so just write the lead-up). */
void ar_trace_flush(const char *reason) {
  if (!s_watch || !s_ring || s_ring_fill == 0) return;
  char path[320];
  snprintf(path, sizeof path, "%s_hf%d_%s%d.jsonl",
           s_watch_pfx, snes_frame_counter, reason ? reason : "flush", s_anom_count++);
  FILE *f = fopen(path, "w");
  if (!f) return;
  int idx = (s_ring_head - s_ring_fill + s_ring_n) % s_ring_n;
  for (int i = 0; i < s_ring_fill; i++) { fputs(s_ring[idx], f); idx = (idx + 1) % s_ring_n; }
  fclose(f);
  fprintf(stderr, "[ar_trace] flush '%s' hf=%d -> %s (%d lines)\n",
          reason ? reason : "?", snes_frame_counter, path, s_ring_fill);
}
