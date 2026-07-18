/* MSU-1 hardware core. See msu1.h for the model and the threading
 * contract. Self-contained: no SDL dependency — it borrows the APU mutex
 * (RtlApuLock/RtlApuUnlock) to serialise its state against the audio
 * thread, the same lock the S-DSP ports already use. */
#include "msu1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>   /* FindFirstFileA / FindNextFileA — MSVC has no <dirent.h> */
#else
#include <dirent.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

/* The audio thread already holds this around RtlRenderAudio; the CPU
 * thread takes it for register accesses. SDL mutexes are recursive, so
 * nesting is safe. Declared here to avoid coupling to common_rtl.h. */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);

#define MSU1_RATE            44100
#define MSU1_MAX_MIX_FRAMES  65536
#define MSU1_PCM_HEADER      8                  /* "MSU1" + uint32 loop point */
#define MSU1_REVISION        0x01
#define MSU1_PATH_MAX        1024

/* Status register ($2000 read) bit layout. */
#define MSU1_ST_DATA_BUSY    0x80
#define MSU1_ST_AUDIO_BUSY   0x40
#define MSU1_ST_AUDIO_REPEAT 0x20
#define MSU1_ST_AUDIO_PLAY   0x10
#define MSU1_ST_AUDIO_ERROR  0x08   /* track missing / load failed */

static const char kMsuId[6] = { 'S', '-', 'M', 'S', 'U', '1' };

static struct {
    bool   armed;        /* env said "on" */
    bool   auto_base;    /* derive base from ROM path */
    bool   have_base;
    char   base[MSU1_PATH_MAX];

    /* Audio channel */
    FILE  *track;        /* current <base>-<N>.pcm, positioned at the read cursor */
    uint32_t loop_point; /* loop restart, in sample-frames from data start */
    long   data_start;   /* byte offset of frame 0 (8 for MSU1 header, 0 if raw) */
    bool   playing;
    bool   repeat;
    bool   audio_error;  /* last track select missing/invalid */
    uint8_t volume;      /* 0..255 linear */
    uint16_t track_lo;   /* latched low byte of $2004, committed on $2005 */
    double audio_src_carry; /* fractional 44.1 kHz frames across callbacks */

    /* Data channel */
    FILE  *data;         /* <base>.msu, opened lazily */
    bool   data_tried;   /* attempted to open data file */
    uint8_t seek[4];     /* latched $2000-$2003, committed on $2003 */
} g;

/* ── lifecycle ───────────────────────────────────────────────────────── */

static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* True if `name` matches "<base>-<N>.pcm" (case-insensitive .pcm) with N a
 * run of digits. On success copies the "<base>" part into `out`. The base
 * itself may contain '-' (e.g. "...the_-_A_Link...") since we only strip the
 * FINAL "-<digits>.pcm". */
static int msu_match_track_name(const char *name, char *out, size_t out_sz) {
    size_t L = strlen(name);
    if (L < 7) return 0;                                   /* "x-1.pcm" min */
    if (!(  (name[L-4] == '.') &&
            (name[L-3]=='p'||name[L-3]=='P') &&
            (name[L-2]=='c'||name[L-2]=='C') &&
            (name[L-1]=='m'||name[L-1]=='M') )) return 0;
    size_t i = L - 4;                                      /* index of '.' */
    if (i == 0 || !isdigit((unsigned char)name[i-1])) return 0;
    while (i > 0 && isdigit((unsigned char)name[i-1])) i--; /* skip digits */
    if (i == 0 || name[i-1] != '-') return 0;             /* need the '-' */
    size_t base_len = i - 1;
    if (base_len == 0 || base_len >= out_sz) return 0;
    memcpy(out, name, base_len);
    out[base_len] = '\0';
    return 1;
}

/* Permissive base resolution: if g.base names a directory, scan it and adopt
 * the pack base ("<dir>/<name>") shared by the most "<name>-<N>.pcm" files —
 * so SNESRECOMP_MSU1 can point at a folder regardless of the pack's exact
 * name. No-op when g.base is already a file prefix. */
static void msu_resolve_base_if_dir(void) {
    struct stat st;
    if (stat(g.base, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    char dir[MSU1_PATH_MAX];
    strncpy(dir, g.base, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    size_t dlen = strlen(dir);
    while (dlen && (dir[dlen-1] == '/' || dir[dlen-1] == '\\')) dir[--dlen] = '\0';

    /* Tally candidate bases; keep the one backing the most tracks. The
     * directory enumeration is the only platform-specific part — MSVC has no
     * <dirent.h>, so Windows uses FindFirstFileA/FindNextFileA. The per-entry
     * tally below is shared. */
    struct { char base[MSU1_PATH_MAX]; int count; } cand[16];
    int ncand = 0;
    char nb[MSU1_PATH_MAX];

#ifdef _WIN32
    char pat[MSU1_PATH_MAX];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
#endif
        if (msu_match_track_name(name, nb, sizeof(nb))) {
            int found = 0;
            for (int k = 0; k < ncand; k++) {
                if (strcmp(cand[k].base, nb) == 0) { cand[k].count++; found = 1; break; }
            }
            if (!found && ncand < 16) {
                strcpy(cand[ncand].base, nb);
                cand[ncand].count = 1;
                ncand++;
            }
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif

    int best = -1;
    for (int k = 0; k < ncand; k++)
        if (best < 0 || cand[k].count > cand[best].count) best = k;
    if (best < 0) return;   /* no "<name>-<N>.pcm" here — leave base as-is */

    snprintf(g.base, sizeof(g.base), "%s/%s", dir, cand[best].base);
    printf("[MSU-1] pack base auto-detected: %s (%d tracks)\n",
           g.base, cand[best].count);
    fflush(stdout);
}

void msu1_init(void) {
    memset(&g, 0, sizeof(g));
    g.volume = 255;  /* full unless the game says otherwise */
    const char *e = getenv("SNESRECOMP_MSU1");
    if (!e || !*e) return;                 /* default-OFF, silent, byte-identical */
    g.armed = true;
    if (ci_eq(e, "1") || ci_eq(e, "on") || ci_eq(e, "auto") || ci_eq(e, "true")) {
        g.auto_base = true;                /* base supplied later via ROM path */
    } else {
        strncpy(g.base, e, sizeof(g.base) - 1);
        g.have_base = true;
        msu_resolve_base_if_dir();         /* allow pointing at a folder */
    }
    printf("[MSU-1] enabled: base='%s'\n",
           g.have_base ? g.base : "(awaiting ROM path)");
    fflush(stdout);
}

void msu1_set_rom_path(const char *rom_path) {
    if (!g.armed || !g.auto_base || g.have_base || !rom_path || !*rom_path)
        return;
    strncpy(g.base, rom_path, sizeof(g.base) - 1);
    /* Strip the extension, but only if the dot follows the last path
     * separator (don't chop "my.dir/rom"). */
    char *dot = strrchr(g.base, '.');
    char *sep = strrchr(g.base, '/');
    char *bsl = strrchr(g.base, '\\');
    if (bsl > sep) sep = bsl;
    if (dot && (!sep || dot > sep)) *dot = '\0';
    g.have_base = true;
}

bool msu1_enabled(void) {
    return g.armed && g.have_base;
}

/* ── audio track loading ─────────────────────────────────────────────── */

static void msu_close_track(void) {
    if (g.track) { fclose(g.track); g.track = NULL; }
}

/* Open <base>-<track>.pcm and leave the file positioned at the first
 * sample. Standard MSU files carry an 8-byte "MSU1" + loop-point header;
 * permissively, a file lacking the magic is treated as raw 44.1 kHz stereo
 * PCM from byte 0 (loop to start). Selecting a track stops playback (spec). */
static void msu_load_track(uint16_t track) {
    msu_close_track();
    g.playing = false;
    g.audio_error = false;
    g.audio_src_carry = 0.0;

    char fn[MSU1_PATH_MAX + 32];
    snprintf(fn, sizeof(fn), "%s-%u.pcm", g.base, (unsigned)track);
    FILE *f = fopen(fn, "rb");
    if (!f) { g.audio_error = true; return; }

    uint8_t hdr[MSU1_PCM_HEADER];
    if (fread(hdr, 1, MSU1_PCM_HEADER, f) == MSU1_PCM_HEADER &&
        hdr[0] == 'M' && hdr[1] == 'S' && hdr[2] == 'U' && hdr[3] == '1') {
        g.loop_point = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        g.data_start = MSU1_PCM_HEADER;   /* positioned past header at frame 0 */
    } else {
        /* Header-less raw PCM: rewind to byte 0, no loop point. */
        g.loop_point = 0;
        g.data_start = 0;
        fseek(f, 0, SEEK_SET);
    }
    g.track = f;
}

/* ── register interface (CPU thread) ─────────────────────────────────── */

uint8_t msu1_read(uint16_t reg) {
    if (!msu1_enabled()) return 0;
    RtlApuLock();
    uint8_t out = 0;
    switch (reg & 0x7) {
        case 0x0: /* $2000 status */
            out = (uint8_t)((g.repeat      ? MSU1_ST_AUDIO_REPEAT : 0) |
                            (g.playing     ? MSU1_ST_AUDIO_PLAY   : 0) |
                            (g.audio_error ? MSU1_ST_AUDIO_ERROR  : 0) |
                            (MSU1_REVISION & 0x07));
            break;
        case 0x1: /* $2001 data read port (auto-increment via file pos) */
            if (!g.data && !g.data_tried) {
                g.data_tried = true;
                char dn[MSU1_PATH_MAX + 8];
                snprintf(dn, sizeof(dn), "%s.msu", g.base);
                g.data = fopen(dn, "rb");
            }
            if (g.data) {
                int c = fgetc(g.data);
                out = (c == EOF) ? 0 : (uint8_t)c;
            }
            break;
        default:  /* $2002-$2007 identification "S-MSU1" */
            out = (uint8_t)kMsuId[(reg & 0x7) - 0x2];
            break;
    }
    RtlApuUnlock();
    return out;
}

void msu1_write(uint16_t reg, uint8_t val) {
    if (!msu1_enabled()) return;
    RtlApuLock();
    switch (reg & 0x7) {
        case 0x0: case 0x1: case 0x2: /* $2000-$2002 data seek bytes (latch) */
            g.seek[reg & 0x7] = val;
            break;
        case 0x3: { /* $2003 commits the 32-bit data seek */
            g.seek[3] = val;
            uint32_t off = (uint32_t)g.seek[0] | ((uint32_t)g.seek[1] << 8) |
                           ((uint32_t)g.seek[2] << 16) | ((uint32_t)g.seek[3] << 24);
            if (!g.data && !g.data_tried) {
                g.data_tried = true;
                char dn[MSU1_PATH_MAX + 8];
                snprintf(dn, sizeof(dn), "%s.msu", g.base);
                g.data = fopen(dn, "rb");
            }
            if (g.data) fseek(g.data, (long)off, SEEK_SET);
            break;
        }
        case 0x4: /* $2004 track number low byte (latch) */
            g.track_lo = val;
            break;
        case 0x5: /* $2005 commits the 16-bit track select + loads it */
            msu_load_track((uint16_t)(g.track_lo | ((uint16_t)val << 8)));
            break;
        case 0x6: /* $2006 volume */
            g.volume = val;
            break;
        case 0x7: /* $2007 control: bit0 play, bit1 repeat */
            g.repeat = (val & 0x2) != 0;
            if (val & 0x1) {
                /* Resume from the current cursor; only a valid track plays. */
                g.playing = (g.track != NULL && !g.audio_error);
            } else {
                g.playing = false;  /* pause, cursor retained */
            }
            break;
    }
    RtlApuUnlock();
}

/* ── audio mix (audio thread, APU lock already held) ─────────────────── */

/* Fill `frames` stereo frames from the track, looping or stopping at EOF.
 * Returns the count actually backed by audio; the remainder is zeroed. */
static int msu_read_frames(int16_t *dst, int frames) {
    int filled = 0;
    bool looped = false;
    while (filled < frames) {
        size_t n = fread(dst + (size_t)filled * 2, 4,
                         (size_t)(frames - filled), g.track);
        filled += (int)n;
        if (filled >= frames) break;
        /* short read => end of file */
        if (g.repeat && !looped) {
            long off = g.data_start + (long)g.loop_point * 4;
            if (fseek(g.track, off, SEEK_SET) == 0) { looped = true; continue; }
            g.playing = false;
            break;
        }
        g.playing = false;   /* no repeat (or empty loop region): stop */
        break;
    }
    if (filled < frames)
        memset(dst + (size_t)filled * 2, 0, (size_t)(frames - filled) * 4);
    return filled;
}

static inline int16_t clamp_s16(int v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

void msu1_mix(int16_t *out, int out_frames, int output_rate) {
    if (!msu1_enabled() || !g.playing || !g.track || out_frames <= 0 ||
        output_rate <= 0)
        return;

    double exact = ((double)out_frames * MSU1_RATE / output_rate) +
                   g.audio_src_carry;
    int src_frames = (int)exact;
    g.audio_src_carry = exact - src_frames;
    if (src_frames <= 0) return;
    if (src_frames > MSU1_MAX_MIX_FRAMES)
        src_frames = MSU1_MAX_MIX_FRAMES;

    static int16_t src[MSU1_MAX_MIX_FRAMES * 2];
    msu_read_frames(src, src_frames);

    if (g.volume == 0)
        return;  /* still advanced the cursor above; just don't mix */

    const int vol = g.volume;  /* 0..255, linear */
    const double step = (double)src_frames / (double)out_frames;
    double pos = 0.0;
    for (int i = 0; i < out_frames; i++) {
        int idx = (int)pos;
        double frac = pos - idx;
        int idx1 = idx + 1;
        if (idx  > src_frames - 1) idx  = src_frames - 1;
        if (idx1 > src_frames - 1) idx1 = src_frames - 1;

        for (int ch = 0; ch < 2; ch++) {
            int s0 = src[idx * 2 + ch];
            int s1 = src[idx1 * 2 + ch];
            int s  = (int)(s0 + (s1 - s0) * frac);   /* linear interpolation */
            s = (s * vol) / 255;
            int o = out[i * 2 + ch] + s;
            out[i * 2 + ch] = clamp_s16(o);
        }
        pos += step;
    }
}
