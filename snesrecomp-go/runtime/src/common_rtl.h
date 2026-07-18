#pragma once
#include "types.h"
#include "snes/snes_regs.h"
#include "debug_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct SimpleHdma {
  const uint8 *table;
  const uint8 *indir_ptr;
  uint8 rep_count;
  uint8 mode;
  uint8 ppu_addr;
  uint8 indir_bank;
} SimpleHdma;


typedef struct Dma Dma;
typedef struct DmaChannel DmaChannel;
typedef struct Ppu Ppu;
typedef struct CpuState CpuState;

void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc);
void SimpleHdma_DoLine(SimpleHdma *c);

extern uint8 g_ram[0x20000];
extern uint8 *g_sram;
extern int g_sram_size;
extern const uint8 *g_rom;
extern Ppu *g_ppu;
extern Dma *g_dma;
extern uint8 g_snesrecomp_last_hdmaen;

// Main-CPU cycle estimate for APU pacing. Incremented per RDB_BLOCK_HOOK.
// See common_rtl.c rtl_accumulate_apu_catchup() for usage.
extern uint64_t g_main_cpu_cycles_estimate;
// APU-port-touch-only estimate; the only counter that feeds APU catch-up.
// See the definition in common_rtl.c for the issue #4 rationale.
extern uint64_t g_apu_pace_cycles_estimate;
extern uint64_t g_apu_last_sync_cycles;
void rtl_accumulate_apu_catchup(void);

#define GET_BYTE(p) (*(uint8*)(p))

extern int snes_frame_counter;

#include "spc_player.h"

void MemCpy(void *dst, const void *src, int size);
bool Unreachable();

#if defined(_DEBUG)
// Gives better warning messages but non inlined on tcc
static inline uint16 GET_WORD(const uint8 *p) { return *(uint16 *)(p); }
static inline const uint8 *RomFixedPtr(uint32_t addr) { return &g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff]; }
#else
#define GET_WORD(p) (*(uint16*)(p))
#define RomFixedPtr(addr) (&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff])
#endif

#define GET_BYTE(p) (*(uint8*)(p))
#define SET_WORD(p, v) (*(uint16*)(p) = (uint16)(v))

// Construct a LongPtr from a 16-bit lo word and 8-bit bank byte.
// Used by the DP aliasing fix: local pointer variables replace g_ram reads.
static inline LongPtr MAKE_LONG(uint16 lo, uint8 bank) {
  LongPtr lp;
  *(uint16 *)&lp = lo;
  ((uint8 *)&lp)[2] = bank;
  return lp;
}

uint8 *RomPtr(uint32_t addr);
uint8 *MvnPtr(uint8_t bank, uint16_t addr);

static inline uint8 *RomPtr_RAM(uint16_t addr) { assert(addr < 0x2000); return g_ram + addr; }
static inline const uint8 *RomPtr_00(uint16_t addr) { return RomPtr(0x000000 | addr); }
static inline const uint8 *RomPtr_01(uint16_t addr) { return RomPtr(0x010000 | addr); }
static inline const uint8 *RomPtr_02(uint16_t addr) { return RomPtr(0x020000 | addr); }
static inline const uint8 *RomPtr_03(uint16_t addr) { return RomPtr(0x030000 | addr); }
static inline const uint8 *RomPtr_04(uint16_t addr) { return RomPtr(0x040000 | addr); }
static inline const uint8 *RomPtr_05(uint16_t addr) { return RomPtr(0x050000 | addr); }
static inline const uint8 *RomPtr_06(uint16_t addr) { return RomPtr(0x060000 | addr); }
static inline const uint8 *RomPtr_07(uint16_t addr) { return RomPtr(0x070000 | addr); }
static inline const uint8 *RomPtr_08(uint16_t addr) { return RomPtr(0x080000 | addr); }
static inline const uint8 *RomPtr_09(uint16_t addr) { return RomPtr(0x090000 | addr); }
static inline const uint8 *RomPtr_0A(uint16_t addr) { return RomPtr(0x0a0000 | addr); }
static inline const uint8 *RomPtr_0B(uint16_t addr) { return RomPtr(0x0b0000 | addr); }
static inline const uint8 *RomPtr_0C(uint16_t addr) { return RomPtr(0x0c0000 | addr); }
static inline const uint8 *RomPtr_0D(uint16_t addr) { return RomPtr(0x0d0000 | addr); }
static inline const uint8 *RomPtr_0E(uint16_t addr) { return RomPtr(0x0e0000 | addr); }
static inline const uint8 *RomPtr_0F(uint16_t addr) { return RomPtr(0x0f0000 | addr); }
static inline const uint8 *RomPtr_11(uint16_t addr) { return RomPtr(0x110000 | addr); }
static inline const uint8 *RomPtr_12(uint16_t addr) { return RomPtr(0x120000 | addr); }
// Extended ROM banks (used in data banks and bank mirrors)
static inline const uint8 *RomPtr_18(uint16_t addr) { return RomPtr(0x180000 | addr); }
static inline const uint8 *RomPtr_1D(uint16_t addr) { return RomPtr(0x1d0000 | addr); }
static inline const uint8 *RomPtr_20(uint16_t addr) { return RomPtr(0x200000 | addr); }
static inline const uint8 *RomPtr_28(uint16_t addr) { return RomPtr(0x280000 | addr); }
static inline const uint8 *RomPtr_37(uint16_t addr) { return RomPtr(0x370000 | addr); }
static inline const uint8 *RomPtr_38(uint16_t addr) { return RomPtr(0x380000 | addr); }
static inline const uint8 *RomPtr_39(uint16_t addr) { return RomPtr(0x390000 | addr); }
static inline const uint8 *RomPtr_40(uint16_t addr) { return RomPtr(0x400000 | addr); }
static inline const uint8 *RomPtr_42(uint16_t addr) { return RomPtr(0x420000 | addr); }
static inline const uint8 *RomPtr_44(uint16_t addr) { return RomPtr(0x440000 | addr); }
static inline const uint8 *RomPtr_48(uint16_t addr) { return RomPtr(0x480000 | addr); }
static inline const uint8 *RomPtr_4B(uint16_t addr) { return RomPtr(0x4b0000 | addr); }
static inline const uint8 *RomPtr_66(uint16_t addr) { return RomPtr(0x660000 | addr); }
static inline const uint8 *RomPtr_6B(uint16_t addr) { return RomPtr(0x6b0000 | addr); }
static inline const uint8 *RomPtr_6D(uint16_t addr) { return RomPtr(0x6d0000 | addr); }
static inline const uint8 *RomPtr_7B(uint16_t addr) { return RomPtr(0x7b0000 | addr); }
// High bank mirrors ($80+) and upper data banks
static inline const uint8 *RomPtr_82(uint16_t addr) { return RomPtr(0x820000 | addr); }
static inline const uint8 *RomPtr_87(uint16_t addr) { return RomPtr(0x870000 | addr); }
static inline const uint8 *RomPtr_89(uint16_t addr) { return RomPtr(0x890000 | addr); }
static inline const uint8 *RomPtr_8A(uint16_t addr) { return RomPtr(0x8a0000 | addr); }
static inline const uint8 *RomPtr_8C(uint16_t addr) { return RomPtr(0x8c0000 | addr); }
static inline const uint8 *RomPtr_90(uint16_t addr) { return RomPtr(0x900000 | addr); }
static inline const uint8 *RomPtr_94(uint16_t addr) { return RomPtr(0x940000 | addr); }
static inline const uint8 *RomPtr_A0(uint16_t addr) { return RomPtr(0xa00000 | addr); }
static inline const uint8 *RomPtr_A8(uint16_t addr) { return RomPtr(0xa80000 | addr); }
static inline const uint8 *RomPtr_AE(uint16_t addr) { return RomPtr(0xae0000 | addr); }
static inline const uint8 *RomPtr_B7(uint16_t addr) { return RomPtr(0xb70000 | addr); }
static inline const uint8 *RomPtr_C9(uint16_t addr) { return RomPtr(0xc90000 | addr); }
static inline const uint8 *RomPtr_D6(uint16_t addr) { return RomPtr(0xd60000 | addr); }
static inline const uint8 *RomPtr_F8(uint16_t addr) { return RomPtr(0xf80000 | addr); }
static inline const uint8 *RomPtrWithBank(uint8 bank, uint16_t addr) { return RomPtr((bank << 16) | addr); }
// WRAM banks — $7E:xxxx → g_ram[addr], $7F:xxxx → g_ram[0x10000 + addr]
static inline uint8 *RomPtr_7E(uint16_t addr) { return g_ram + addr; }
static inline uint8 *RomPtr_7F(uint16_t addr) { return g_ram + 0x10000 + addr; }
static inline const uint8 *RomPtr_10(uint16_t addr) { return RomPtr(0x100000 | addr); }
static inline const uint8 *RomPtr_17(uint16_t addr) { return RomPtr(0x170000 | addr); }
static inline const uint8 *RomPtr_1B(uint16_t addr) { return RomPtr(0x1b0000 | addr); }
static inline const uint8 *RomPtr_1C(uint16_t addr) { return RomPtr(0x1c0000 | addr); }
static inline const uint8 *RomPtr_80(uint16_t addr) { return RomPtr(0x000000 | addr); }

void WriteReg(uint16 reg, uint8 value);
void WriteRegWord(uint16 reg, uint16 value);
uint16 ReadRegWord(uint16 reg);
uint8 ReadReg(uint16 reg);
uint8_t *IndirPtr_Slow(LongPtr ptr, uint16 offs);

// 16-bit-indirect-via-DP resolution. The addressing modes `(dp)`,
// `(dp),Y`, `(dp,X)` and `(dp,S),Y` all fetch a 2-byte pointer from
// DP and combine it with the data bank register (DB) to form the
// full 24-bit effective address. Use this instead of raw
// `g_ram[ptr_lo | ptr_hi<<8]` — that silently assumes DB=\$7E and
// returns garbage when DB is a ROM bank (typical for in-ROM
// data-table loads).
uint8_t *IndirPtrDB(uint8 dp_addr, uint16 offs);
static inline uint8_t *IndirPtr(LongPtr ptr, uint16 offs) {
  uint32 a = (*(uint32 *)&ptr & 0xffffff) + offs;
  uint8 bank = (uint8)(a >> 16);
  if (bank >= 0x7e && bank <= 0x7f)
    return &g_ram[a & 0x1ffff];
  if ((a & 0xffff) < 0x2000)
    return &g_ram[a & 0x1ffff];
  return RomPtr(a);
}
/* AR_WATCHOBJ/AR_WATCH16 gap fix (2026-07-01): cpu_write8/cpu_write16
 * (cpu_state.c) are the only writers those two watches ever saw -- any
 * store through an indexed/indirect addressing mode (`STA (dp),Y`, `STA
 * [dp],Y`, `STA abs,X` when the effective address happens to land in a
 * watched range) goes through IndirWriteByte/IndirWriteWord instead,
 * which wrote straight to g_ram with zero instrumentation outside the
 * (normally-off) SNESRECOMP_REVERSE_DEBUG trace build. That left a real
 * blind spot: a value that's only ever written indirectly is invisible
 * to both watches no matter what you set them to. Found chasing a
 * sim-mode freeze where AR_WATCHOBJ=0 caught nothing on $0019 despite
 * AR_SIMTRACE proving it holds a stray 0xA1 every frame -- the write
 * had to be indirect. Mirrors cpu_write8's exact watch bodies so the
 * two watches behave identically regardless of which store form hits
 * the target address. */
static inline void IndirWatchByte(uint8_t *dst, uint8_t old_val, uint8_t value) {
  if (dst < g_ram || dst >= g_ram + 0x20000) return;
  uint32_t off = (uint32_t)(dst - g_ram);
  { extern int ar_trace_active(void);
    extern void ar_trace_wram(uint32_t, uint16_t, uint16_t, int);
    if (ar_trace_active()) ar_trace_wram(off, old_val, value, 1); }
  if (getenv("AR_WATCHOBJ")) {
    static long wo = -2;
    if (wo == -2) { const char *e = getenv("AR_WATCHOBJ"); wo = e ? (long)strtoul(e, NULL, 16) : -1; }
    if (wo >= 0 && off >= (uint32_t)wo && off < (uint32_t)wo + 0x40 && old_val != value) {
      extern int snes_frame_counter; extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
      extern const char *g_last_recomp_func;
      static int n;
      if (n++ < 8000) {
        fprintf(stderr, "[wobj-ind] $%04x=%02x (was %02x) f=%d cur=%s stk:",
                off, value, old_val, snes_frame_counter,
                g_last_recomp_func ? g_last_recomp_func : "?");
        for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
          fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        fprintf(stderr, "\n");
      }
    }
  }
}

static inline void IndirWriteByte(LongPtr ptr, uint16 offs, uint8 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  uint8_t old_val = 0;
  static int need_old = -1;
  if (need_old < 0)
    need_old = (getenv("AR_WATCHOBJ") != NULL) || (getenv("AR_TRACE") != NULL);
  if (need_old && dst >= g_ram && dst < g_ram + 0x20000) old_val = dst[0];
#if SNESRECOMP_REVERSE_DEBUG
  // Only fire the WRAM hook if the write actually landed in WRAM.
  // dst may point into ROM for in-ROM data-table writes (a NOP in practice
  // since ROM is read-only, but the ptr math still lands there).
  // Read old BEFORE the store so the Tier-1 log can emit old/new.
  if (dst >= g_ram && dst < g_ram + 0x20000) {
    uint8_t old_val_dbg = dst[0];
    dst[0] = value;
    debug_on_wram_write_byte((uint32_t)(dst - g_ram), old_val_dbg, value);
  } else {
    dst[0] = value;
  }
#else
  dst[0] = value;
#endif
  if (need_old) IndirWatchByte(dst, old_val, value);
}

static inline void IndirWatchWord(uint8_t *dst, uint16_t old_val, uint16_t value) {
  if (dst < g_ram || dst >= g_ram + 0x20000) return;
  uint32_t off = (uint32_t)(dst - g_ram);
  { extern int ar_trace_active(void);
    extern void ar_trace_wram(uint32_t, uint16_t, uint16_t, int);
    if (ar_trace_active()) ar_trace_wram(off, old_val, value, 2); }
  if (getenv("AR_WATCHOBJ")) {
    static long wo = -2;
    if (wo == -2) { const char *e = getenv("AR_WATCHOBJ"); wo = e ? (long)strtoul(e, NULL, 16) : -1; }
    if (wo >= 0 && off >= (uint32_t)wo && off < (uint32_t)wo + 0x40 && old_val != value) {
      extern int snes_frame_counter; extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
      extern const char *g_last_recomp_func;
      static int n;
      if (n++ < 8000) {
        fprintf(stderr, "[wobj-ind] $%04x=%04x (was %04x) f=%d cur=%s stk:",
                off, value, old_val, snes_frame_counter,
                g_last_recomp_func ? g_last_recomp_func : "?");
        for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
          fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        fprintf(stderr, "\n");
      }
    }
  }
  if (getenv("AR_WATCH16")) {
    static int wv = -2;
    if (wv == -2) { const char *e = getenv("AR_WATCH16"); wv = e ? (int)strtoul(e, NULL, 16) : -1; }
    if (wv >= 0 && value == (uint16_t)wv) {
      extern int snes_frame_counter; extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
      extern const char *g_last_recomp_func;
      int top = g_recomp_stack_top;
      fprintf(stderr, "[watch16-ind] v=%04x -> off=%05x by=%s f=%d stack:",
              value, off, g_last_recomp_func ? g_last_recomp_func : "?", snes_frame_counter);
      for (int i = top - 1; i >= 0 && i >= top - 8; i--)
        fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
      fprintf(stderr, "\n");
    }
  }
}

// 16-bit word store through a 24-bit DP pointer. Native counterpart of
// `STA [dp]` / `STA [dp],Y` emitted when M=0 (A-16). Writes the low byte
// at the effective address and the high byte one byte later; the pair is
// always contiguous in the target region (WRAM or ROM-mirror).
static inline void IndirWriteWord(LongPtr ptr, uint16 offs, uint16 value) {
  uint8_t *dst = IndirPtr(ptr, offs);
  uint16_t old_val = 0;
  static int need_old = -1;
  if (need_old < 0)
    need_old = (getenv("AR_WATCHOBJ") != NULL) || (getenv("AR_WATCH16") != NULL)
             || (getenv("AR_TRACE") != NULL);
  if (need_old && dst >= g_ram && dst < g_ram + 0x20000)
    old_val = (uint16_t)dst[0] | ((uint16_t)dst[1] << 8);
#if SNESRECOMP_REVERSE_DEBUG
  if (dst >= g_ram && dst < g_ram + 0x20000) {
    uint16_t old_val_dbg = (uint16_t)dst[0] | ((uint16_t)dst[1] << 8);
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    debug_on_wram_write_word((uint32_t)(dst - g_ram), old_val_dbg, value);
  } else {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
  }
#else
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
#endif
  if (need_old) IndirWatchWord(dst, old_val, value);
}

// Tier-1 wrappers for the direct IndirPtrDB([0] = val) pattern the generator
// emits for STA (dp),Y / STA (dp,X) / STA (dp). Equivalent to the raw
// pattern when SNESRECOMP_REVERSE_DEBUG=0; adds the hook when =1.
#if SNESRECOMP_REVERSE_DEBUG
static inline void rdb_indir_dbx_store8(uint8 dp_addr, uint16 offs, uint8 value) {
  uint8_t *dst = IndirPtrDB(dp_addr, offs);
  if (dst >= g_ram && dst < g_ram + 0x20000) {
    uint8_t old_val = dst[0];
    dst[0] = value;
    debug_on_wram_write_byte((uint32_t)(dst - g_ram), old_val, value);
  } else {
    dst[0] = value;
  }
}
static inline void rdb_indir_dbx_store16(uint8 dp_addr, uint16 offs, uint16 value) {
  uint8_t *dst = IndirPtrDB(dp_addr, offs);
  if (dst >= g_ram && dst < g_ram + 0x20000) {
    uint16_t old_val = *(uint16_t *)dst;
    *(uint16_t *)dst = value;
    debug_on_wram_write_word((uint32_t)(dst - g_ram), old_val, value);
  } else {
    *(uint16_t *)dst = value;
  }
}
#endif

void RtlReset(int mode);

enum {
  kSaveLoad_Save = 1,
  kSaveLoad_Load = 2,
};

void RtlSaveLoad(int cmd, int slot);
void RtlApuLock();
void RtlApuUnlock();
/* Set/query the actual host-device rate used by RtlRenderAudio. The runner's
 * S-DSP always produces at 32.04 kHz; this rate controls only the final
 * resampling boundary and therefore must never change emulated pitch. */
void RtlSetAudioOutputRate(int hz);
int RtlGetAudioOutputRate(void);
void RtlRenderAudio(int16 *audio_buffer, int samples, int channels);
bool RtlUploadSpcImageFromDp(CpuState *cpu);
bool RtlRunFrame(uint32 inputs);
void RtlReadSram();
void RtlWriteSram();
// Copy a legacy saves/<legacy_title>.srm forward to the generic saves/save.srm
// (idempotent). Call before the launcher so its SAVES panel reflects the carried-
// forward save; RtlReadSram also calls it on boot as a fallback.
void RtlMigrateLegacySram(const char *legacy_title);
void RtlSaveSnapshot(const char *filename);
bool RtlLoadSnapshot(const char *filename);

void RtlApuWrite(uint16 adr, uint8 val);


enum {
  kJoypadL_A = 0x80,
  kJoypadL_X = 0x40,
  kJoypadL_L = 0x20,
  kJoypadL_R = 0x10,

  kJoypadH_B = 0x80,
  kJoypadH_Y = 0x40,
  kJoypadH_Select = 0x20,
  kJoypadH_Start = 0x10,

  kJoypadH_Up = 0x8,
  kJoypadH_Down = 0x4,
  kJoypadH_Left = 0x2,
  kJoypadH_Right = 0x1,

  kJoypadH_AnyDir = 0xf,
};
