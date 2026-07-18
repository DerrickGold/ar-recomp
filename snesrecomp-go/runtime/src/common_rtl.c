#include "common_rtl.h"
#include "common_cpu_infra.h"
#include <setjmp.h>
#include "recomp_hw.h"
#include "framedump.h"
#include "util.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/msu1.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "audio_trace.h"

uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;
uint8 g_snesrecomp_last_hdmaen;

/* Host audio is a presentation concern. The S-DSP's native FIFO always runs
 * at 534 samples/frame * 60 frames/s = 32.04 kHz. Keep a fractional native
 * cursor so changing SDL frequency or callback size cannot change pitch. */
#define RTL_DSP_NATIVE_RATE 32040.0
#define RTL_AUDIO_RESAMPLE_CHUNK 1024
static int s_audio_output_rate = 44100;
static double s_dsp_resample_phase;

// Main-CPU cycle estimate, incremented per RDB_BLOCK_HOOK in debug_on_block_enter.
// Used to pace APU catchup realistically: real SNES is ~3.58 MHz main / ~1.024 MHz APU,
// ratio ~3.5:1. Prior code hardcoded apuCatchupCycles=32 per APU port touch regardless
// of elapsed main-CPU time, which let APU stay artificially synchronized -- SMW boot's
// "wait for APU ack" loops resolved instantly, racing through ~200 frames worth of game
// logic in ~95 frames. Tracking real elapsed cycles makes those waits actually wait.
uint64_t g_main_cpu_cycles_estimate = 0;
// APU-port-touch-only estimate (issue #4). Only touches of $2140-$217F
// count toward APU catch-up pacing. The general estimate above counts
// EVERY HW-reg touch, which wildly over-counts during load-heavy phases:
// graphics decompression hammers $2118/$2122 thousands of times per
// frame, and the next APU-port access used to convert that entire
// fictional backlog into SPC cycles at once. Measured on MMX (audio_trace
// rings, 300 s boot→attract): 237,426 native samples — 7.4 s of SPC
// over-advance — dropped at the output ring, clustered at scene
// transitions, audibly skipping the music forward at every stage load.
// Genuine APU handshakes (boot/bank uploads, command acks) self-pace
// through their own port polls — each poll is an APU touch — so they
// still over-clock the SPC and complete fast; that part is required:
// at hardware-real SPC pace MMX's boot upload blocks a single frame
// for >5 s and trips the per-frame watchdog (measured, 2026-06-09).
uint64_t g_apu_pace_cycles_estimate = 0;
uint64_t g_apu_last_sync_cycles = 0;

/* ---- AR_APUPROF: per-frame APU-stall profiler (diagnostic only) --------
 * The main loop zeroes these before each game frame and prints one line
 * when the frame exceeds the report threshold, attributing the time to
 * lock waits / SPC catch-up / port-handshake spins / HLE uploads / the
 * music-replacement port hook. All counters are game-thread except
 * lockwait, which the game-thread RtlApuLock wrapper alone accumulates. */
int g_apuprof = -1;
uint64_t g_apuprof_lockwait_ns;
uint64_t g_apuprof_catchup_ns;
uint64_t g_apuprof_catchup_cyc;
uint32_t g_apuprof_catchup_calls;
uint32_t g_apuprof_port_reads, g_apuprof_port_writes;
uint64_t g_apuprof_hook_ns;
uint64_t g_apuprof_upload_ns;
uint64_t g_apuprof_sched_lat_max;
/* Worst blocked-acquire on the NON-game thread (i.e. the audio callback)
 * since the last [apuprof] report — the direct starvation measure. Running
 * max, zeroed by the reporter, deliberately NOT by ApuProfFrameReset. */
uint64_t g_apuprof_audiowait_max_ns;
const char *g_apuprof_last_port_func;

int ApuProfEnabled(void) {
  if (g_apuprof < 0) {
    const char *e = getenv("AR_APUPROF");
    g_apuprof = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return g_apuprof;
}

void ApuProfFrameReset(void) {
  g_apuprof_lockwait_ns = 0;
  g_apuprof_catchup_ns = 0;
  g_apuprof_catchup_cyc = 0;
  g_apuprof_catchup_calls = 0;
  g_apuprof_port_reads = 0;
  g_apuprof_port_writes = 0;
  g_apuprof_hook_ns = 0;
  g_apuprof_upload_ns = 0;
  g_apuprof_sched_lat_max = 0;
  g_apuprof_last_port_func = NULL;
}

// FILE-backed SaveLoadInfo. snes_saveload calls back into func() once per
// scalar/blob; we route each call to fread/fwrite. Single magic+version
// header lets future format changes be detected.
#define RTL_SAV_MAGIC   0x52544c53u  /* "RTLS" */
#define RTL_SAV_VERSION 4u  /* v4: dropped Dma.pad[7] blob tail */

typedef struct FileSli {
  SaveLoadInfo base;
  FILE *f;
  bool is_save;
  bool error;
} FileSli;

static void file_sli_func(SaveLoadInfo *sli, void *data, size_t n) {
  FileSli *fs = (FileSli *)sli;
  if (fs->error) return;
  size_t got = fs->is_save ? fwrite(data, 1, n, fs->f)
                           : fread(data, 1, n, fs->f);
  if (got != n) fs->error = true;
}

void RtlReset(int mode) {
  snes_frame_counter = 0;
  g_main_cpu_cycles_estimate = 0;
  g_apu_pace_cycles_estimate = 0;
  g_apu_last_sync_cycles = 0;
  snes_reset(g_snes, true);
  SnesEnterNativeMode();
  ppu_reset(g_ppu);
  if (!(mode & 1))
    memset(g_sram, 0, g_sram_size);

  RtlApuLock();
  s_dsp_resample_phase = 0.0;
  g_spc_player->initialize(g_spc_player);
  RtlApuUnlock();
}

void RtlSetAudioOutputRate(int hz) {
  /* SDL's practical range is narrower, but keeping the runner seam generic is
   * useful to other games. The lower bound also guarantees each 1024-frame
   * resampling chunk fits in the 8192-frame DSP FIFO. */
  if (hz < 8000) hz = 8000;
  if (hz > 384000) hz = 384000;
  s_audio_output_rate = hz;
  s_dsp_resample_phase = 0.0;
}

int RtlGetAudioOutputRate(void) {
  return s_audio_output_rate;
}

bool RtlRunFrame(uint32 inputs) {
  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;
  // Player2
  if ((inputs & 0x30000) == 0x30000) inputs ^= 0x30000;
  if ((inputs & 0xc0000) == 0xc0000) inputs ^= 0xc0000;

  g_snes->input1_currentState = inputs & 0xfff;
  g_snes->input2_currentState = (inputs >> 12) & 0xfff;

  WatchdogFrameStart();
  // Watchdog guard: WatchdogCheck() (called per-block in v2 gen) longjmps
  // here when a frame exceeds 5s, so an infinite loop in recompiled code
  // doesn't freeze the runtime indefinitely. Without this setjmp the
  // longjmp would dereference an uninitialized jmp_buf and crash.
  if (setjmp(g_watchdog_jmp) == 0) {
    g_rtl_game_info->run_frame();
  }
  // If g_watchdog_tripped is set, frame was abandoned mid-execution;
  // continue to the next frame so the user can interrupt cleanly.
  if (g_framedump_callback)
    g_framedump_callback(snes_frame_counter, g_ram);
  {
    extern void debug_server_record_frame(int);
    debug_server_record_frame(snes_frame_counter);
  }

  snes_frame_counter++;
  return false;
}

void RtlSaveSnapshot(const char *filename) {
  FILE *f = fopen(filename, "wb");
  if (!f) {
    printf("Failed fopen for save: %s\n", filename);
    return;
  }
  uint32 hdr[2] = { RTL_SAV_MAGIC, RTL_SAV_VERSION };
  fwrite(hdr, sizeof(hdr), 1, f);
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, true, false };
  snes_saveload(g_snes, &fs.base);
  RtlApuUnlock();
  if (fs.error) printf("Save write error: %s\n", filename);
  fclose(f);
}

bool RtlLoadSnapshot(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f)
    return false;
  uint32 hdr[2];
  if (fread(hdr, sizeof(hdr), 1, f) != 1
      || hdr[0] != RTL_SAV_MAGIC || hdr[1] != RTL_SAV_VERSION) {
    printf("Save file %s: bad magic/version (legacy StateRecorder format no longer supported)\n", filename);
    fclose(f);
    return false;
  }
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, false, false };
  snes_saveload(g_snes, &fs.base);
  RtlApuUnlock();
  fclose(f);
  if (fs.error) {
    printf("Save read error: %s\n", filename);
    return false;
  }
  return true;
}

void RtlSaveLoad(int cmd, int slot) {
  char name[128];
  const char *prefix = g_rtl_game_info->save_name_prefix;
  if (prefix)
    sprintf(name, "saves/%s%d.sav", prefix, slot);
  else
    sprintf(name, "saves/%s_save%d.sav", g_rtl_game_info->title, slot);
  printf("*** %s slot %d: %s\n",
    cmd == kSaveLoad_Save ? "Saving" : "Loading", slot, name);
  if (cmd == kSaveLoad_Save)
    RtlSaveSnapshot(name);
  else
    RtlLoadSnapshot(name);
}


void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  assert(0);
  g_ram[0x1ffff] = 1;
  return false;
}

uint8 *RomPtr(uint32_t addr) {
  uint8_t bank = (uint8_t)(addr >> 16);
  uint16_t lo = (uint16_t)addr;
  bool lorom_rom_window = (lo >= 0x8000) || ((bank & 0x7f) >= 0x40);
  if (bank == 0x7e || bank == 0x7f || !lorom_rom_window) {
    if (!g_fail) g_fail = true;
    /* No printf — the ring buffer + cpu_trace_offrails is the
     * channel for backwards investigation. printf'ing every bad
     * read floods stderr with millions of identical lines. */
    cpu_trace_offrails("RomPtr-invalid", addr);
  }
  /* Compute LoROM offset, then mirror against ACTUAL ROM size. SMW is
   * 512KB but the original `& 0x3fffff` mask assumed 4MB, so reads at
   * high banks (e.g. $FF:0100 — bogus pointer values from data-as-code
   * regions or unmapped ARAM) computed index 0x7F8100, FAR past
   * g_rom's 0x80000 bytes — instant SIGSEGV. The right behaviour:
   * mirror to actual ROM size, matching real SNES bank-mirroring. */
  extern Snes *g_snes;
  uint32_t off = (((addr >> 16) << 15) | (addr & 0x7fff));
  uint32_t rom_size = g_snes && g_snes->cart ? (uint32_t)g_snes->cart->romSize : 0x80000;
  if (rom_size == 0) rom_size = 0x80000;
  return (uint8 *)&g_rom[off % rom_size];
}

// MVN/MVP block-move pointer: resolves (bank, addr) per 65816 LoROM rules.
// Banks $00-$3F and $80-$BF mirror WRAM at $0000-$1FFF; $7E/$7F are WRAM.
// Everything else is ROM (same mapping as RomPtr). Returns a non-const pointer
// because MVN dst writes through this; callers must only dst into WRAM banks.
uint8 *MvnPtr(uint8_t bank, uint16_t addr) {
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  uint32_t full = ((uint32_t)bank << 16) | addr;
  return (uint8 *)&g_rom[(((full >> 16) << 15) | (full & 0x7fff)) & 0x3fffff];
}

// Replay a DMA transfer into g_ppu after the emulator executed it into g_snes->ppu.

static int _writereg_ppu_count = 0;
static int _writereg_dma_count = 0;
void WriteReg(uint16 reg, uint8 value) {
  // Direct dispatch — bypass emulator bus
  // MSU-1 ($2000-$2007). Inert unless a pack is armed, so the open-bus
  // default (no-op + log) is preserved byte-for-byte when disabled.
  if (reg >= 0x2000 && reg < 0x2008) {
    if (msu1_enabled()) msu1_write(reg, value);
    debug_server_on_reg_write(reg, value);
    return;
  }
  if (reg >= 0x2100 && reg < 0x2140) {
    ppu_write(g_ppu, reg & 0xff, value);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    RtlApuWrite(reg, value);
  } else if (reg >= 0x2180 && reg < 0x2184) {
    snes_writeBBus(g_snes, reg & 0xff, value);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    if (reg == 0x420C)
      g_snesrecomp_last_hdmaen = value;
    recomp_write_internal_reg(reg, value);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    dma_write(g_dma, reg, value);
  }
  debug_server_on_reg_write(reg, value);
}


uint8 ReadReg(uint16 reg) {
  // Direct dispatch — bypass emulator bus
  // MSU-1 ($2000-$2007). Returns 0 (open bus) when no pack is armed,
  // matching the prior fall-through behaviour exactly.
  if (reg >= 0x2000 && reg < 0x2008) {
    return msu1_enabled() ? msu1_read(reg) : 0;
  }
  if (reg >= 0x2100 && reg < 0x2140) {
    return ppu_read(g_ppu, reg & 0xff);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    // APU read — route through emulator (real SPC700 outPorts).
    return snes_read(g_snes, reg);
  } else if (reg == 0x2180) {
    return snes_readBBus(g_snes, reg & 0xff);
  } else if (reg == 0x4016 || reg == 0x4017) {
    /* JOYSER0 / JOYSER1 — manual joypad-read serial registers.
     * Routed through snes_readReg so the SNES core can return the
     * controller-presence signature (bit 0 set after the implicit
     * "16 reads done" state). Phase B koopa-stomp investigation
     * (2026-04-24) found these reads were falling through to the
     * default `return 0` and breaking SMW's CheckWhichControllers-
     * ArePluggedIn detection. */
    return snes_readReg(g_snes, reg);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    return recomp_read_internal_reg(reg);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    return dma_read(g_dma, reg);
  }
  return 0;
}

uint16 ReadRegWord(uint16 reg) {
  // APU port quirk: 16-bit CMP $2140 must see a CONSISTENT outPorts
  // snapshot. Two separate ReadReg calls would each catch the APU
  // up — between them the SPC could write only the LO byte (port 0)
  // before host has read HI (port 1), so host sees a torn value. Read
  // both ports atomically (single catchup) for the APU-port range.
  if (reg >= 0x2140 && reg <= 0x217F) {
    extern void rtl_accumulate_apu_catchup(void);
    void RtlApuLock(void); void RtlApuUnlock(void);
    void snes_catchupApu(Snes* snes);
    extern Snes *g_snes;
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    { extern int ApuProfEnabled(void); extern uint32_t g_apuprof_port_reads;
      if (ApuProfEnabled()) g_apuprof_port_reads++; }
    uint8_t lo = g_snes->apu->outPorts[(reg & 0x3)];
    uint8_t hi = g_snes->apu->outPorts[((reg + 1) & 0x3)];
    RtlApuUnlock();
    return (uint16_t)lo | ((uint16_t)hi << 8);
  }
  uint16_t rv = ReadReg(reg);
  rv |= ReadReg(reg + 1) << 8;
  return rv;
}

static void WriteVramWord(Ppu *ppu, uint16 value) {
  uint16_t adr = ppu->vramPointer;
  /* AR_VWORD=1: log atomic 16-bit STA $2118 writes into the $0000 graphics
   * region (the lair-seal corruptor bypasses $2118-byte watches via this path),
   * host-frame gated (AR_HF_LO/HI), with the issuing recomp func + block PC. */
  { static int en=-1; static long lo,hi; if(en<0){en=getenv("AR_VWORD")?1:0;
      const char*a=getenv("AR_HF_LO"),*b=getenv("AR_HF_HI");
      lo=a?atol(a):-1; hi=b?atol(b):-1;}
    if(en && (adr&0x7fff) < 0x1000){
      extern int snes_frame_counter; extern const char *g_last_recomp_func;
      extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
      if(lo<0||(snes_frame_counter>=lo&&(hi<0||snes_frame_counter<=hi))){
        static int nl; if(nl++<20000){
          uint32_t blk=g_ar_blk_ring[(g_ar_blk_idx-1u)&1023u];
          fprintf(stderr,"[vword] hf=%d vram=$%04x val=$%04x blk=$%06X func=%s\n",
            snes_frame_counter, adr&0x7fff, value, blk,
            g_last_recomp_func?g_last_recomp_func:"?"); } } } }
  ppu->vram[adr & 0x7fff] = value;
  /* Unified AR_TRACE — this atomic-word path bypasses ppu WriteReg $2118/$2119,
   * so it was invisible to byte-level VRAM watches; log it on the same channel. */
  { extern int ar_trace_active(void);
    extern void ar_trace_vram(uint16_t vaddr, uint16_t val, const char *path);
    if (ar_trace_active()) ar_trace_vram(adr & 0x7fff, value, "word"); }
  // Atomic 16-bit STA $2118 hits both VRAM bytes at this word; record
  // each as a byte event so the differ can compare against the
  // oracle's REGISTER_2118 + REGISTER_2119 byte sequence.
  uint32_t byte_addr = (uint32_t)(adr & 0x7fff) << 1;
  debug_server_on_vram_write(byte_addr,     (uint8_t)(value & 0xff));
  debug_server_on_vram_write(byte_addr + 1, (uint8_t)(value >> 8));
  ppu->vramPointer += ppu->vramIncrement;
}

void WriteRegWord(uint16 reg, uint16 value) {
  if (reg == 0x2118) {
    // VRAM data port: atomic word write
    WriteVramWord(g_ppu, value);
    return;
  }
  // APU port quirk: 16-bit STA $2140 transfers data via $2141 (hi)
  // and the ack-trigger via $2140 (lo). On real hardware both bytes
  // hit the bus together; SMW's SPC IPL upload protocol reads $2141
  // the moment it sees $2140 change. If we write lo first the IPL
  // latches stale $2141. Order hi-then-lo so $2141 is in place
  // before $2140 fires the trigger.
  if (reg >= 0x2140 && reg <= 0x217F) {
    WriteReg(reg + 1, value >> 8);
    WriteReg(reg, (uint8)value);
    return;
  }
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

uint8 *IndirPtr_Slow(LongPtr ptr, uint16 offs) {
  return IndirPtr(ptr, offs);  /* delegates to inline version in header */
}

/* IndirWriteByte is now inline in common_rtl.h */

// Convert the APU-touch cycle delta into APU cycles (ratio ~3.5:1) and
// accumulate into apuCatchupCycles. Caller holds RtlApuLock and is
// responsible for the snes_catchupApu() call. Sets g_apu_last_sync_cycles
// to the current APU-touch estimate so subsequent calls only see
// incremental work. (See g_apu_pace_cycles_estimate's comment for why
// this deliberately ignores non-APU HW touches.)
//
// Public so snes.c's snes_readBBus (the APU read path) can use the same
// pacing -- both reads and writes need to advance APU.
void rtl_accumulate_apu_catchup(void) {
  uint64_t delta = g_apu_pace_cycles_estimate - g_apu_last_sync_cycles;
  g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
  // 2/7 is about 1/3.5 (main MHz / APU MHz). Floor of zero is fine -- short deltas
  // (back-to-back APU touches with no block hooks between them) just don't
  // advance APU on this pass; cycles accumulate for the next touch.
  g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;

  // Real-time baseline when nothing is draining the DSP output ring
  // (EnableAudio=0, headless runs, the pre-callback boot window, a
  // stalled device). With audio on, the audio thread's RtlRenderAudio
  // top-up advances the SPC at the device's real consumption rate; with
  // no consumer that baseline vanishes and the SPC would advance only on
  // the (possibly rare) APU touches. Inject wall-clock time at the SPC's
  // real rate (~1.024 MHz) so engine state keeps tracking real time and
  // handshakes can never freeze. ADDITIVE to the touch credit, never a
  // limit — handshake over-clock must stay possible (see above).
  // Consumer presence is inferred from sampleRead movement, so this is
  // automatic per game and per moment, no config.
  {
    static uint32_t last_sample_read;
    static uint64_t consume_seen_ms, wall_last_ms;
    Dsp *dsp = g_snes->apu->dsp;
    uint64_t now_ms = audio_trace_wall_ms();
    uint32_t rd = dsp->sampleRead;
    if (rd != last_sample_read) {
      last_sample_read = rd;
      consume_seen_ms = now_ms;
    }
    int consumer_active = consume_seen_ms != 0 &&
                          now_ms - consume_seen_ms < 250;
    uint32_t baseline = 0;
    if (!consumer_active && wall_last_ms != 0) {
      uint64_t elapsed = now_ms - wall_last_ms;
      if (elapsed > 32) elapsed = 32;  /* burst cap: ~32 ms of SPC time */
      baseline = (uint32_t)(elapsed * 1024);  /* SPC ~1.024 MHz */
      g_snes->apuCatchupCycles += (double)baseline;
    }
    wall_last_ms = now_ms;
    audio_trace_on_pace(consumer_active, baseline);
  }
}

/* Game-installable audio observation seams (all optional, NULL by default):
 *   g_rtl_apu_port_hook    every $2140-$2143 write, CPU thread, APU lock held
 *                          (observation only — the write still happens)
 *   g_rtl_spc_upload_hook  after each successful HLE SPC image upload, with
 *                          the 24-bit ROM source address (the song identity)
 *   g_rtl_music_mix_hook   inside RtlRenderAudio's locked region after the
 *                          MSU-1 mix — same contract as msu1_mix
 * Installed by ActRaiser's music_replacements.c for manifest-driven music
 * streaming; other games leave them NULL (zero behavior change). */
void (*g_rtl_apu_port_hook)(uint8_t port, uint8_t val) = NULL;
void (*g_rtl_spc_upload_hook)(uint32_t src) = NULL;
void (*g_rtl_music_mix_hook)(int16_t *buf, int frames) = NULL;

void RtlApuWrite(uint16 adr, uint8 val) {
  /* An out-of-range adr here means corrupted state upstream reached a bogus
   * register write (seen during the post-act transition's bad NMI DMA). Don't
   * hard-abort — that masks the real fault and prevents the diagnostic dump.
   * Log once and ignore so the run limps far enough to capture state. */
  if (adr < APUI00 || adr > APUI03) {
    static int warned;
    if (!warned) { warned = 1;
      fprintf(stderr, "[apu] WARN RtlApuWrite out-of-range adr=$%04x val=%02x (upstream corruption) — ignoring\n", adr, val);
    }
    return;
  }
  // Catch the APU up to the current cycle, then SCHEDULE the port write
  // in APU-sample time rather than mutating inPorts at wall time.
  //
  // Rationale (SMW missed-SFX root cause): the audio thread advances
  // the SPC in whole-callback bursts, so a wall-time port mutation gives
  // the value a lifetime of however many samples happen to be produced
  // before the next mutation — measured ~9 samples (vs the 64 an engine
  // poll needs) whenever the 60.0988 Hz NMI beats across the 60.00 Hz
  // callback phase. Anchoring each write one callback quantum past the
  // CONSUMED clock keeps successive frame writes a full frame apart in
  // the SPC's own execution time, so the engine always polls every
  // value, exactly as on hardware. Steady-state added latency is ~zero:
  // consumed + quantum ~= produced, i.e. the next burst applies it.
  // Serialise with the audio thread via RtlApuLock -- it holds the same
  // lock while cycling the APU in RtlRenderAudio.
  RtlApuLock();
  rtl_accumulate_apu_catchup();
  snes_catchupApu(g_snes);
  audio_trace_on_cpu_port_write((uint8_t)(adr & 0x3), val);
  if (ApuProfEnabled()) {
    extern const char *g_last_recomp_func;
    g_apuprof_port_writes++;
    g_apuprof_last_port_func = g_last_recomp_func;
    if (g_rtl_apu_port_hook) {
      uint64_t t0 = audio_trace_wall_ns();
      g_rtl_apu_port_hook((uint8_t)(adr & 0x3), val);
      g_apuprof_hook_ns += audio_trace_wall_ns() - t0;
    }
  } else if (g_rtl_apu_port_hook)
    g_rtl_apu_port_hook((uint8_t)(adr & 0x3), val);
  if (getenv("AR_APULOG") && (adr & 0xfc) == 0x40) {
    extern int snes_frame_counter;
    fprintf(stderr, "[apu] f=%d WRITE $21%02x <- %02x  (spc.pc=%04x in=%02x%02x%02x%02x out=%02x%02x%02x%02x)\n",
            snes_frame_counter, (unsigned)(adr & 0xff), val,
            g_snes->apu->spc->pc,
            g_snes->apu->inPorts[0], g_snes->apu->inPorts[1], g_snes->apu->inPorts[2], g_snes->apu->inPorts[3],
            g_snes->apu->outPorts[0], g_snes->apu->outPorts[1], g_snes->apu->outPorts[2], g_snes->apu->outPorts[3]);
  }
  {
    /* Write clock: each target advances from the PREVIOUS write's target
     * by the real wall-time gap between the two writes, converted to
     * samples. This preserves hardware-faithful inter-write spacing in
     * the SPC's execution timeline — frame-spaced NMI writes stay ~534
     * samples apart, same-frame double writes keep their ms-scale gap —
     * independent of where the audio thread's burst boundaries fall.
     *
     * (First attempt anchored targets at consumed+quantum; that fails
     * because produced runs AHEAD of consumed by the output-ring fill,
     * so every target was in the past and the floor collapsed
     * consecutive writes onto the same sample — measured as +0-sample
     * command lifetimes, i.e. the original race in a new costume.)
     *
     * Floor at produced: a target in the APU's past applies on the next
     * executed sample. Ceiling at produced + 3 callback quanta bounds
     * worst-case latency and sheds the slow forward drift from the
     * NMI(60.0988 Hz)/callback(60.00 Hz) rate mismatch. Both caps scale
     * with the observed burst granularity (audio_samples in config.ini
     * is user-tunable): a ceiling smaller than the real burst would pin
     * late-window writes to the same target and re-collapse spacing. */
    static uint64_t s_port_clock;     /* previous write's target */
    static uint64_t s_port_clock_ns;  /* wall_ns of previous write */
    /* Per-port history for the minimum-dwell floor below. Statics, like
     * s_port_clock: not reset across RtlReset/upload, which is benign —
     * after a reset `produced` has advanced far past any stale target, so
     * the floor (stale_target + dwell) is already in the past and never
     * engages spuriously. */
    static uint64_t s_port_last_target[4];
    static uint8_t  s_port_last_val[4];
    static uint8_t  s_port_last_valid[4];
    uint64_t quantum = audio_trace_consume_quantum();
    uint64_t now_ns = audio_trace_wall_ns();
    uint64_t produced, consumed;
    audio_trace_sample_clocks(&produced, &consumed);
    uint64_t delta = 0;
    if (s_port_clock_ns != 0)
      delta = (now_ns - s_port_clock_ns) * 32040u / 1000000000u;
    if (delta > 4u * quantum) delta = 4u * quantum;
    uint64_t target = s_port_clock + delta;
    if (target < produced) target = produced;
    if (target > produced + 3u * quantum) target = produced + 3u * quantum;

    /* Minimum per-port dwell — the turbo audio-dropout fix. A level
     * transition fires several DISTINCT values at the same APU port
     * (fade, silence, the new song; or a one-shot command then the NMI's
     * next-frame 0-clear) within a few frames. The wall-clock spacing
     * computed above reproduces hardware timing faithfully at 1x, but
     * turbo runs the game thread uncapped while the SPC still advances at
     * 1x, compressing that spacing below the engine's ~64-sample poll
     * period — so an earlier value is overwritten in inPorts before the
     * engine ever reads it and the command is silently lost (music/SFX
     * drop out; because a surviving fade can zero global output, they do
     * not come back until the next track change, i.e. never within a
     * level). Floor a DISTINCT value's target so the previous distinct
     * value on that port holds the bus for at least APU_PORT_MIN_DWELL
     * produced-samples — one guaranteed engine poll. The drain runs once
     * per produced sample (apu_cycle), so this target spacing becomes
     * apply spacing directly. Bounded by produced + 8*quantum so a
     * pathological sustained burst degrades to today's bounded latency
     * rather than unbounding it. Identical repeats (e.g. repeated
     * 0-clears) need no spacing. No effect at 1x: frame-spaced writes are
     * already ~534 samples apart, far above the floor. */
    {
      int p = (int)(adr & 0x3);
      if (s_port_last_valid[p] && val != s_port_last_val[p]) {
        uint64_t floor = s_port_last_target[p] + APU_PORT_MIN_DWELL;
        uint64_t ceil  = produced + 8u * quantum;
        if (target < floor) target = floor < ceil ? floor : ceil;
      }
      s_port_last_target[p] = target;
      s_port_last_val[p]    = val;
      s_port_last_valid[p]  = 1;
    }

    s_port_clock = target;
    s_port_clock_ns = now_ns;
    if (ApuProfEnabled()) {
      uint64_t lat = target > produced ? target - produced : 0;
      if (lat > g_apuprof_sched_lat_max) g_apuprof_sched_lat_max = lat;
    }
    apu_schedulePortWrite(g_snes->apu, (uint8_t)(adr & 0x3), val, target);
  }
  RtlApuUnlock();
}

/* ActRaiser's SPC-uploader ($02:9964 / $02:9A56) reads the block-stream
 * source through `LDA [$A5],Y` — i.e. the 24-bit source pointer lives at
 * direct-page offset $A5/$A6/$A7, NOT at DP+0 (the SMW convention this HLE
 * was first written for). Reading DP+0 fetched all-zero garbage, so the
 * parser walked $FF-filled ROM, reported a bogus entry of $0000, and the
 * `final_pc != 0` guard then left the SPC PC unset; the SPC ran off into
 * uninitialised ARAM, hit a STOP ($FF) opcode, and halted at boot — no
 * audio ever, and the boss-music handshake at $00:A410 (poll $2140 for the
 * SPC to echo $F1) spun forever -> watchdog. The stream format itself is
 * identical (length/target/data ... 0/entry), so only the pointer offset
 * differs. */
#define AR_SPC_UPLOAD_DP_PTR 0xA5

/* Set by the HLE upload when the engine's resident ARAM uploader needs to be
 * completed but the engine hasn't entered it yet; cleared once completed. See
 * RtlUploadSpcImageFromDpInternal and ar_uploader_complete_tick. */
int g_ar_uploader_complete_pending = 0;

/* Finish the engine's resident uploader the moment it enters the $CC-wait, for
 * the deferred case where the CPU's $9A56 (HLEd) ran before the engine got
 * there. Called once per host frame from the main loop. Jumps to the uploader
 * tail at $0F48 (clear ports, enable timer0, RET), exactly as a real transfer
 * ends — leaving the engine in the correct post-upload state. */
void ar_uploader_complete_tick(void) {
  if (!g_ar_uploader_complete_pending) return;
  RtlApuLock();
  Spc *spc = g_snes->apu->spc;
  const uint8_t *ar = g_snes->apu->ram;
  if (spc->pc >= 0x0F0E && spc->pc <= 0x0F18 &&
      ar[0x0F48] == 0xCD && ar[0x0F49] == 0x31 && ar[0x0F4A] == 0xD8 &&
      ar[0x0F4B] == 0xF1 && ar[0x0F4C] == 0x6F) {
    spc->pc = 0x0F48;
    g_ar_uploader_complete_pending = 0;
    if (getenv("AR_APULOG")) {
      extern int snes_frame_counter;
      fprintf(stderr, "[apu] f=%d UPLOAD(deferred): completed resident uploader -> $0F48 RET\n",
              snes_frame_counter);
    }
  }
  RtlApuUnlock();
}

static bool RtlUploadSpcImageFromDpInternal(CpuState *cpu, bool update_cpu_result) {
  uint64_t prof_t0 = ApuProfEnabled() ? audio_trace_wall_ns() : 0;
  uint16_t dp = (cpu->D + AR_SPC_UPLOAD_DP_PTR) & 0xffff;
  uint16_t data_lo = (uint16_t)g_ram[(dp + 0) & 0xffff]
                   | ((uint16_t)g_ram[(dp + 1) & 0xffff] << 8);
  uint8_t data_bank = g_ram[(dp + 2) & 0xffff];
  if (getenv("AR_APULOG")) {
    extern int snes_frame_counter;
    fprintf(stderr, "[apu] f=%d HLE-ENTRY D=%04x src=%02x:%04x\n",
            snes_frame_counter, cpu->D, data_bank, data_lo);
  }
  const uint8_t *p = RomPtr(((uint32_t)data_bank << 16) | data_lo);
  uint16_t final_pc = 0;
  int block_count = 0;

  bool ulog = getenv("AR_APULOG") != NULL;
  RtlApuLock();
  for (;;) {
    uint16_t n = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    uint16_t target = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    p += 4;
    if (n == 0) {
      final_pc = target;
      break;
    }
    if (ulog && block_count < 16)
      fprintf(stderr, "[apu]   block %d: target=%04x n=%u firstbytes=%02x %02x %02x\n",
              block_count, target, n, p[0], n>1?p[1]:0, n>2?p[2]:0);
    for (uint16_t i = 0; i < n; i++)
      g_snes->apu->ram[(uint16_t)(target + i)] = p[i];
    p += n;
    if (++block_count > 512) {
      RtlApuUnlock();
      fprintf(stderr, "[apu] bad SPC upload stream at %02X:%04X\n",
              data_bank, data_lo);
      if (prof_t0) g_apuprof_upload_ns += audio_trace_wall_ns() - prof_t0;
      return false;
    }
  }

  /* Stage 2 — BRR sample streaming ($02:9964 callers only). The native
   * $9964 is NOT just a wrapper around the $9A56 block-image upload: after
   * JSR $9A56 it re-handshakes with the resident uploader and streams raw
   * sample chunks from a length-prefixed pool at ROM $08:8000 into ARAM.
   * The image terminator's target word doubles as the stage-2 script: its
   * LOW byte is the segment count, and the script's index bytes start at
   * the HIGH byte and continue past the terminator. Each index selects a
   * [len16][data...] chunk from the pool (scanned linearly, LoROM
   * bank-contiguous); chunks land back-to-back in ARAM starting at WRAM
   * $0358 (boot preloads $3000 for the 12-sample common bank; the boot
   * caller then sets $0358 = DP$02+DP$08 = end-of-common for song banks).
   * Verified against the DSP sample directory: title segments land at
   * $3000/$3B01/.../$6E4C = srcn 00-0B exactly; song-7 segments at
   * $795F/.../$B89E = srcn 0C-12 exactly. Without this stage every DIR
   * entry points at zero-filled ARAM -> voices key on but decode silence
   * (the "engine runs, no audio" symptom).
   *
   * The direct-$9A56 path (boot mini-driver upload) has no stage 2; gate on
   * the wrapper name, same trick RtlUploadSpcImageFromDp uses for the
   * return-frame size. */
  {
    extern const char *g_last_recomp_func;
    int is_9964 = g_last_recomp_func && strstr(g_last_recomp_func, "9964");
    int seg_count = final_pc & 0xff;
    if (is_9964 && seg_count > 0) {
      const uint8_t *script = p - 1;      /* terminator target high byte */
      const uint8_t *pool = RomPtr(0x088000);
      uint16_t dest = (uint16_t)(g_ram[0x358] | (g_ram[0x359] << 8));
      uint16_t prev_len = 0, last_len = 0, last_dest = dest;
      for (int s = 0; s < seg_count; s++) {
        uint8_t idx = *script++;
        dest = (uint16_t)(dest + prev_len);
        const uint8_t *q = pool;
        int bad = 0;
        for (int k = 0; k < idx; k++) {
          uint16_t skip = (uint16_t)(q[0] | (q[1] << 8));
          q += 2 + skip;
          if (q - pool > 0x60000) { bad = 1; break; }  /* runaway script */
        }
        if (bad) {
          fprintf(stderr, "[apu] stage2: bad chunk index %u (seg %d), aborting\n",
                  idx, s);
          break;
        }
        uint16_t len = (uint16_t)(q[0] | (q[1] << 8));
        q += 2;
        for (uint32_t i = 0; i < len; i++)
          g_snes->apu->ram[(uint16_t)(dest + i)] = q[i];
        if (ulog)
          fprintf(stderr, "[apu]   stage2 seg %d: chunk %u -> ARAM %04x..%04x (len %#x)\n",
                  s, idx, dest, (uint16_t)(dest + len), len);
        prev_len = len; last_len = len; last_dest = dest;
      }
      /* Native exit state the boot caller depends on ($02:9957 computes the
       * next $0358 as DP$02 + DP$08 = last dest + last length): */
      uint16_t d = cpu->D;
      g_ram[(uint16_t)(d + 0)] = 0;                       /* STZ $00/$01 */
      g_ram[(uint16_t)(d + 1)] = 0;
      g_ram[(uint16_t)(d + 2)] = (uint8_t)last_dest;      /* DP $02 */
      g_ram[(uint16_t)(d + 3)] = (uint8_t)(last_dest >> 8);
      g_ram[(uint16_t)(d + 8)] = (uint8_t)last_len;       /* DP $08 */
      g_ram[(uint16_t)(d + 9)] = (uint8_t)(last_len >> 8);
    }
  }

  /* First-upload vs subsequent-upload semantics differ. The very first
   * upload from CPU after reset goes through the SNES SPC IPL bootROM,
   * which ends with `JMP [$0000+X]` — i.e. the IPL jumps to the entry
   * address provided in the terminator's target field. After that
   * first upload, the IPL is mapped out (romReadable=false) and the
   * loaded SPC engine handles all subsequent CPU upload requests via
   * its own routine (SMW's StandardTransfer at SPC $12F2). That
   * routine just RETs at the end — it does NOT jump to any entry
   * point. The terminator's target field is benign on subsequent
   * uploads.
   *
   * If we unconditionally re-jumped SPC PC to the terminator entry,
   * every music-bank upload would restart APU_Start, zero-clearing
   * the engine's music state ($00-$E7 + ARAM_0386-9) and the
   * just-uploaded music data would never start playing. SFX would
   * still work since they're triggered by inPort writes processed
   * after the restart's re-init, but song state would never persist.
   *
   * Detect "first upload" via apu->romReadable: it's reset to true by
   * apu_reset() and only flipped false here, so on the IPL-phase
   * upload it's still true. */
  if (getenv("AR_APULOG")) {
    extern int snes_frame_counter;
    fprintf(stderr, "[apu] f=%d UPLOAD blocks=%d final_pc=%04x src=%02x:%04x ipl=%d (spc.pc was %04x stopped=%d)\n",
            snes_frame_counter, block_count, final_pc, data_bank, data_lo,
            (int)g_snes->apu->romReadable, g_snes->apu->spc->pc, (int)g_snes->apu->spc->stopped);
  }
  bool ipl_phase = g_snes->apu->romReadable;
  /* The upload supersedes any not-yet-applied scheduled port writes;
   * a stale pre-upload command landing on the freshly cleared ports
   * would replay into the re-initialised engine. */
  apu_clearPortQueue(g_snes->apu);
  memset(g_snes->apu->inPorts, 0, sizeof(g_snes->apu->inPorts));
  memset(g_snes->apu->outPorts, 0, sizeof(g_snes->apu->outPorts));
  if (ipl_phase) {
    g_snes->apu->romReadable = false;
    g_snes->apuCatchupCycles = 0;
    g_snes->apu->cpuCyclesLeft = 0;
    if (final_pc != 0) {
      g_snes->apu->spc->a = 0;
      g_snes->apu->spc->x = 0;
      g_snes->apu->spc->y = 0;
      if (g_snes->apu->spc->sp == 0)
        g_snes->apu->spc->sp = 0xef;
      g_snes->apu->spc->pc = final_pc;
    }
  } else {
    /* Subsequent upload: ActRaiser's resident SPC engine handles these via its
     * OWN IPL-style uploader in ARAM at $0F0E — it CALLs in, raises the $AABB
     * ready signature, and spins at $0F12 (`MOV A,$F4; CMP #$CC; BNE`) waiting
     * for the CPU to send $CC and stream the block image. We bypass that by
     * memcpying the image straight into apu->ram (running the CPU-side $9A56
     * natively over-clocks the SPC and breaks boot), so the $CC stream is never
     * sent and the engine stays parked at $0F12 — it has, since the first music
     * upload. A later play-command handshake ($00:A410 sends $F1 and spins for
     * the echo, $A427 waits for the port to clear) then can't be serviced: at
     * worst a 5 s watchdog/SIGSEGV, at best the engine escapes but mis-handles
     * the sequence (echoes commands instead of acking), so the boss never gets
     * its go-signal. Complete the uploader for it, here at upload time (before
     * any play command exists, so the finalize's port-clear is correct): jump
     * the SPC to its tail at $0F48 (`MOV X,#$31; MOV $F1,X; RET` — clears ports,
     * enables timer0, returns to the engine main loop), exactly as a real $CC
     * transfer would end. Guarded on the actual opcodes + the engine being in
     * the wait, so a different resident driver or non-uploader state is left
     * untouched. */
    Spc *spc = g_snes->apu->spc;
    const uint8_t *ar = g_snes->apu->ram;
    int uploader_present =
        (ar[0x0F48] == 0xCD && ar[0x0F49] == 0x31 && ar[0x0F4A] == 0xD8 &&
         ar[0x0F4B] == 0xF1 && ar[0x0F4C] == 0x6F);
    if (uploader_present && spc->pc >= 0x0F0E && spc->pc <= 0x0F18) {
      /* Engine already parked in the uploader (the normal ordering: it CALLs
       * in, then the CPU's $9A56 — which we are HLEing — streams). Finish it. */
      spc->pc = 0x0F48;
      g_ar_uploader_complete_pending = 0;
      if (getenv("AR_APULOG")) {
        extern int snes_frame_counter;
        fprintf(stderr, "[apu] f=%d UPLOAD: completed resident uploader -> $0F48 RET\n",
                snes_frame_counter);
      }
    } else if (uploader_present) {
      /* The CPU reached $9A56 before the engine entered the uploader (happens
       * when the game thread outruns the SPC, e.g. uncapped/headless). Defer:
       * ar_uploader_complete_tick() (called per frame) finishes it the moment
       * the engine arrives — still well before any play-command handshake, so
       * the finalize's port-clear stays harmless. */
      g_ar_uploader_complete_pending = 1;
    }
  }
  g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
  RtlApuUnlock();

  if (g_rtl_spc_upload_hook)
    g_rtl_spc_upload_hook(((uint32_t)data_bank << 16) | data_lo);
  if (prof_t0) g_apuprof_upload_ns += audio_trace_wall_ns() - prof_t0;

  if (update_cpu_result) {
    cpu->A = (uint16_t)(cpu->A & 0xff00);
    cpu->X = 0;
    cpu->Y = 0;
    cpu->_flag_Z = 1;
    cpu->_flag_N = 0;
    cpu->P = (uint8_t)((cpu->P & ~0x82) | 0x02);
  }
  return true;
}

bool RtlUploadSpcImageFromDp(CpuState *cpu) {
  bool ok = RtlUploadSpcImageFromDpInternal(cpu, false);
  /* The emitted hle_spc_upload wrapper has NO `RTL`/`RTS`, so unlike a real
   * recompiled function it never removes the return frame the call site pushed
   * onto cpu->S — leaking it every call. Harmless at top level (SP resets each
   * frame) but fatal inside the object loop (e.g. the boss-music handler's
   * `PHX; JSL $9964; PLX; RTS`: the leak shifts PLX/RTS -> garbage return ->
   * scribbled $D0-$D5 DMA descriptor -> bad NMI DMA from $2100 -> crash).
   * Pop the matching frame size — and the two routines differ in convention:
   *   $9964 is reached only via JSL (3-byte frame, RTL)   -> pop 3
   *   $9A56 is reached only via same-bank JSR (2-byte, RTS) -> pop 2
   * (verified: 0 `JSL $029A56` sites, 0 `JSR $9964` sites). Popping a blanket 3
   * over-popped $9A56 by 1 byte/call -> stack drift -> SIGSEGV in normal play.
   * Distinguish via g_last_recomp_func, which the wrapper set at entry. */
  extern const char *g_last_recomp_func;
  int pop = (g_last_recomp_func && strstr(g_last_recomp_func, "9A56")) ? 2 : 3;
  cpu->S = (uint16_t)(cpu->S + pop);
  return ok;
}

bool RtlHandleSpcUpload(CpuState *cpu) {
  return RtlUploadSpcImageFromDpInternal(cpu, true);
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);
  if (!audio_buffer || samples <= 0) return;
  /* Cycle the APU in small batches under the lock, releasing between
   * each so the CPU thread (RtlApuWrite / snes_readBBus) can make
   * progress. Earlier code held RtlApuLock for the entire 17 000-cycle
   * loop, which took ~4 ms host time per audio callback. With audio
   * callbacks at ~60 Hz that pinned the CPU thread out of the lock for
   * ~27 % of wall time, and the SMW IPL upload (which touches APU
   * ports thousands of times) ran an order of magnitude slower than
   * the watchdog allowed.
   *
   * 256 SPC cycles per batch is about 64 us host work per acquire, short
   * enough that the CPU thread's RtlApuLock call almost never has to
   * wait through a full audio batch. apu_cycle is single-threaded
   * regardless -- the lock just serialises access to inPorts/outPorts
   * shared with the CPU thread. */
  // Ensure enough native samples are available for each output chunk, then
  // consume according to elapsed host audio time. The audio thread only
  // produces the shortfall
  // the CPU-thread catch-up (snes_catchupApu) hasn't already supplied, so
  // it self-balances: total SPC advance stays at the consumption rate and
  // bursty catch-up production is buffered, not dropped.
  #define DSP_AVAIL(d) ((uint32_t)((d)->sampleWrite - (d)->sampleRead))
  const double native_step = RTL_DSP_NATIVE_RATE / s_audio_output_rate;
  int rendered = 0;
  while (rendered < samples) {
    int chunk = samples - rendered;
    if (chunk > RTL_AUDIO_RESAMPLE_CHUNK)
      chunk = RTL_AUDIO_RESAMPLE_CHUNK;
    uint32_t needed_for_cursor =
        (uint32_t)(s_dsp_resample_phase + chunk * native_step) + 1;
    uint32_t needed_for_interpolation =
        (uint32_t)(s_dsp_resample_phase + (chunk - 1) * native_step) + 2;
    uint32_t needed = needed_for_cursor > needed_for_interpolation
        ? needed_for_cursor : needed_for_interpolation;
    for (;;) {
      RtlApuLock();
      uint32_t available = DSP_AVAIL(g_snes->apu->dsp);
      if (available < needed) {
        audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
        int batch = 256;
        while (batch-- > 0 && DSP_AVAIL(g_snes->apu->dsp) < needed)
          apu_cycle(g_snes->apu);
        audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
        available = DSP_AVAIL(g_snes->apu->dsp);
      }
      if (available >= needed) {
        dsp_getSamplesResampled(g_snes->apu->dsp,
                                audio_buffer + rendered * 2, chunk,
                                native_step, &s_dsp_resample_phase);
        RtlApuUnlock();
        break;
      }
      RtlApuUnlock();
    }
    rendered += chunk;
  }
  #undef DSP_AVAIL

  RtlApuLock();
  /* AR_AUDIODBG: report DSP master volume / mute / peak sample so we can tell
   * whether the engine is producing sound at all (silence = engine not playing
   * vs DSP muted/zero-volume vs samples lost downstream). */
  if (getenv("AR_AUDIODBG")) {
    extern uint64_t snes_apu_cycle_count(void);
    static int n; static uint64_t last_cyc; static uint64_t last_ms;
    extern uint64_t audio_trace_wall_ms(void);
    if ((n++ % 60) == 0) {
      int16 pk = 0;
      for (int i = 0; i < samples * 2; i++) {
        int16 s = audio_buffer[i]; if (s < 0) s = -s; if (s > pk) pk = s;
      }
      Dsp *d = g_snes->apu->dsp;
      uint64_t cyc = snes_apu_cycle_count(), ms = audio_trace_wall_ms();
      uint64_t dcyc = cyc - last_cyc, dms = ms - last_ms;
      /* SPC should run ~1024 cycles/ms (1.024 MHz). cyc/ms far below that =
       * under-cycled engine (slow music/handshakes); count keyed-on voices. */
      int kon = 0; for (int v = 0; v < 8; v++) if (d->channel[v].keyOn || d->channel[v].keyOff==false ) kon += (d->channel[v].keyOn?1:0);
      fprintf(stderr, "[audiodbg] mvol=%d mute=%d peak=%d | SPC %llu cyc/%llu ms = %llu cyc/ms (want ~1024) konPend=%d\n",
              d->masterVolumeL, (int)d->mute, pk,
              (unsigned long long)dcyc, (unsigned long long)dms,
              dms ? (unsigned long long)(dcyc/dms) : 0ull, kon);
      last_cyc = cyc; last_ms = ms;
    }
  }
  /* Mix MSU-1 streaming audio on top of the S-DSP block. Inert (no-op)
   * unless a pack is armed and a track is playing. Runs under the APU
   * lock we already hold, which serialises it against MSU register
   * writes on the CPU thread (msu1_read/msu1_write take the same lock). */
  msu1_mix(audio_buffer, samples, s_audio_output_rate);
  /* Manifest-driven music replacement (ActRaiser music_replacements.c):
   * streams a host file over the (voice-gated) S-DSP mix. Same lock
   * contract as msu1_mix; NULL for games that don't install it. */
  if (g_rtl_music_mix_hook)
    g_rtl_music_mix_hook(audio_buffer, samples);
  RtlApuUnlock();
}

/* The battery-backed SRAM lives at a fixed, game-agnostic path next to the exe
 * (each game has its own directory, so there is no collision). Older builds named
 * it after the game's internal title — which happened to be "smw" for every game
 * (a copy-paste leftover), so Mega Man X / Zelda also wrote saves/smw.srm.
 * RtlMigrateLegacySram copies any such legacy save forward the first time the new
 * generic path is used, so existing players keep their progress. */
#define RTL_SRAM_FILE     "saves/save.srm"
#define RTL_SRAM_BAK_FILE "saves/save.srm.bak"

void RtlMigrateLegacySram(const char *legacy_title) {
  if (!legacy_title || !*legacy_title) return;
  FILE *cur = fopen(RTL_SRAM_FILE, "rb");
  if (cur) { fclose(cur); return; }   /* already on the generic name */
  char legacy[64];
  snprintf(legacy, sizeof(legacy), "saves/%s.srm", legacy_title);
  if (strcmp(legacy, RTL_SRAM_FILE) == 0) return;  /* legacy name IS the generic one */
  FILE *in = fopen(legacy, "rb");
  if (!in) return;                    /* no legacy save to carry forward */
  FILE *out = fopen(RTL_SRAM_FILE, "wb");
  if (!out) { fclose(in); return; }   /* e.g. saves/ not writable */
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  fprintf(stderr, "[saves] migrated legacy %s -> %s\n", legacy, RTL_SRAM_FILE);
}

void RtlReadSram(void) {
  RtlMigrateLegacySram(g_rtl_game_info->title);
  FILE *f = fopen(RTL_SRAM_FILE, "rb");
  if (f) {
    if (fread(g_sram, 1, g_sram_size, f) != g_sram_size)
      fprintf(stderr, "Error reading %s\n", RTL_SRAM_FILE);
    fclose(f);
  }
}

void RtlWriteSram(void) {
  rename(RTL_SRAM_FILE, RTL_SRAM_BAK_FILE);
  FILE *f = fopen(RTL_SRAM_FILE, "wb");
  if (f) {
    fwrite(g_sram, 1, g_sram_size, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write %s\n", RTL_SRAM_FILE);
  }
}

static const uint8 *SimpleHdma_GetPtr(uint32 p) {
  uint8 bank = (uint8)(p >> 16);
  uint16 addr = (uint16)(p & 0xffff);
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  return RomPtr(p);
}

/* 2026-06-30: SimpleHdma_GetPtr's RomPtr() call is bounds-safe at the point
 * of translation (wraps via `% rom_size`), but SimpleHdma_DoLine then walks
 * that pointer forward with raw `c->table++`/`c->table += 2` and never
 * re-validates — a table lacking a proper zero-terminator before the ROM
 * mirror boundary walks the RAW HOST POINTER past the actual malloc'd
 * buffer, an out-of-bounds READ (ASan: heap-buffer-overflow, common_rtl.c
 * SimpleHdma_DoLine, table pointer 0 bytes past the 1MB ROM region). Guard
 * every dereference against BOTH possible backing buffers (g_ram is a fixed
 * array; g_rom's size is cart-dependent, same calc RomPtr uses) and treat an
 * out-of-bounds pointer as the natural end-of-table condition instead of
 * reading past the buffer. */
static bool SimpleHdma_PtrValid(const uint8 *p) {
  if (p >= g_ram && p < g_ram + sizeof(g_ram)) return true;
  extern Snes *g_snes;
  uint32 rom_size = g_snes && g_snes->cart ? (uint32)g_snes->cart->romSize : 0x80000;
  if (rom_size == 0) rom_size = 0x80000;
  if (p >= g_rom && p < g_rom + rom_size) return true;
  return false;
}

void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) {
  if (!dc->hdmaActive) {
    c->table = 0;
    return;
  }
  c->table = SimpleHdma_GetPtr(dc->aAdr | dc->aBank << 16);
  c->rep_count = 0;
  c->mode = dc->mode | dc->indirect << 6;
  c->ppu_addr = dc->bAdr;
  c->indir_bank = dc->indBank;
}

void SimpleHdma_DoLine(SimpleHdma *c) {
  static const uint8 bAdrOffsets[8][4] = {
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1},
    {0, 1, 2, 3},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1}
  };
  static const uint8 transferLength[8] = {
    1, 2, 2, 4, 4, 4, 2, 4
  };

  if (c->table == NULL)
    return;
  bool do_transfer = false;
  if ((c->rep_count & 0x7f) == 0) {
    if (!SimpleHdma_PtrValid(c->table)) { c->table = NULL; return; }
    c->rep_count = *c->table++;
    if (c->rep_count == 0) {
      c->table = NULL;
      return;
    }
    if(c->mode & 0x40) {
      if (!SimpleHdma_PtrValid(c->table) || !SimpleHdma_PtrValid(c->table + 1)) {
        c->table = NULL;
        return;
      }
      c->indir_ptr = SimpleHdma_GetPtr(c->indir_bank << 16 | c->table[0] | c->table[1] * 256);
      c->table += 2;
    }
    do_transfer = true;
  }
  if(do_transfer || c->rep_count & 0x80) {
    for(int j = 0, j_end = transferLength[c->mode & 7]; j < j_end; j++) {
      const uint8 *src = c->mode & 0x40 ? c->indir_ptr : c->table;
      if (!SimpleHdma_PtrValid(src)) { c->table = NULL; break; }
      uint8 v = *src;
      if (c->mode & 0x40) c->indir_ptr++; else c->table++;
      uint16 addr = 0x2100 + c->ppu_addr + bAdrOffsets[c->mode & 7][j];
      ppu_write(g_ppu, addr, v);
      debug_server_on_reg_write(addr, v);
    }
  }
  c->rep_count--;
}
