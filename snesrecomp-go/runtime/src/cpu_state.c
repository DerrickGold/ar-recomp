/*
 * cpu_state.c — implementations for the v2 runtime CpuState.
 *
 * Address routing for the byte/word memory helpers:
 *   $00-$3F:0000-$1FFF / $7E:0000-$1FFF       -> g_ram (low WRAM mirror)
 *   $7E:0000-$FFFF                            -> g_ram[0x00000-0x0FFFF]
 *   $7F:0000-$FFFF                            -> g_ram[0x10000-0x1FFFF]
 *   $00-$3F:2000-$5FFF / $80-$BF:2000-$5FFF   -> SNES hardware regs
 *                                                (PPU, APU, joypad, DMA)
 *                                                routed via WriteReg/ReadReg
 *   $70-$7D:0000-$7FFF / $F0-$FD:0000-$7FFF   -> LoROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$3F:6000-$7FFF / $80-$BF:6000-$7FFF   -> HiROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$7D:8000-$FFFF / $80-$FF:8000-$FFFF   -> ROM (reads via RomPtr;
 *                                                writes are NOPs)
 *
 * The hardware-register routing is what unblocks boot: every PPU/APU/DMA
 * register write the recompiled code emits goes through WriteReg, so
 * INIDISP / NMITIMEN / OBSEL / DMA setup actually take effect. Without
 * it, $2100 stays at the snes9x default (forced-blank ON) and the
 * screen never lights up.
 *
 * The SRAM routing is what unblocks save/menu: every read against the
 * cart's battery RAM (SMW's VerifySaveFile, save data writes, password
 * tables, etc.) goes through g_sram so save data lives in cart->ram
 * instead of tripping RomPtr-invalid.
 */

#include "cpu_state.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_trace.h"

CpuState g_cpu;

/* Diagnostic accessors for files without the full CpuState definition
 * (e.g. snes.c, which only forward-declares it). */
uint16 ar_cpu_S(void)  { return g_cpu.S; }
uint8  ar_cpu_PB(void) { return g_cpu.PB; }

/* Map a 24-bit logical address onto a g_ram offset. Returns -1 for
 * addresses that are NOT WRAM — the caller routes those to the HW-reg
 * helpers (WriteReg/ReadReg) or to ROM. */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        return (int)addr;
    }
    return -1;
}

/* True when (bank, addr) addresses an SNES hardware register that should
 * be routed through the framework's WriteReg/ReadReg dispatch. The HW
 * register window is $2000-$5FFF in low banks ($00-$3F, $80-$BF). */
static int is_hw_reg(uint8 bank, uint16 addr) {
    if (addr < 0x2000 || addr >= 0x6000) return 0;
    if (bank <= 0x3F) return 1;
    if (bank >= 0x80 && bank <= 0xBF) return 1;
    return 0;
}

/* Map a 24-bit logical address onto a g_sram offset for cart battery
 * RAM. Returns -1 if (bank, addr) is NOT SRAM. Mirrors snes9x's
 * cart_readLorom and cart_readHirom SRAM mappings so save-data
 * accesses route to cart->ram instead of falling through to RomPtr
 * (which would trip the RomPtr-invalid off-rails detector). */
static int cpu_sram_offset(uint8 bank, uint16 addr) {
    if (g_sram_size == 0 || g_sram == NULL) return -1;
    /* LoROM SRAM: banks $70-$7D + $F0-$FD, addr $0000-$7FFF. */
    if (((bank >= 0x70 && bank < 0x7E) || (bank >= 0xF0 && bank < 0xFE))
        && addr < 0x8000) {
        return (int)((((bank & 0xF) << 15) | addr) & (g_sram_size - 1));
    }
    /* HiROM SRAM: banks $00-$3F + $80-$BF, addr $6000-$7FFF. */
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0))
        && addr >= 0x6000 && addr < 0x8000) {
        return (int)((((bank & 0x3F) << 13) | (addr & 0x1FFF))
                     & (g_sram_size - 1));
    }
    return -1;
}

/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter` (RDB_BLOCK_HOOK); v2
 * doesn't emit those, so without this bump g_main_cpu_cycles_estimate
 * stays at 0, snes_catchupApu never advances the SPC, and SMW's
 * "wait for $2140 == $BBAA" poll loop spins forever waiting for a
 * response that the APU can't produce.
 *
 * Per-touch granularity is overshooting reality (real CPU does ~6
 * cycles per insn, far less than 24 per touch) but the SPC handshake
 * doesn't care about precise timing — it just needs *some* cycles to
 * elapse so the IPL ROM runs to the point of writing $BBAA. */
#include <stdio.h>
/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter`; v2 doesn't emit
 * those, so without this bump the SPC never advances and SMW's
 * "wait for $2140 == $BBAA" handshake spins forever.
 *
 * The 256-cycle increment is tuned to roughly match v1's per-block
 * pacing amortised over the recomp's tight CPU read loops. The
 * minimum-cycle floor in snes_catchupApu (snes.c) ensures the SPC
 * actually progresses on each call.
 *
 * Only APU-port touches ($2140-$217F) feed the APU catch-up counter
 * (issue #4): general HW touches massively over-count during load-heavy
 * phases ($2118 decompression spam) and used to convert into SPC-cycle
 * bursts that overflowed the DSP output ring at scene transitions,
 * audibly skipping the music. Handshake loops poll the ports themselves,
 * so APU touches alone keep every upload/ack protocol self-pacing. The
 * all-touch estimate stays for trace timestamps and diagnostics. */
static inline void cpu_pace_cycles(uint16 addr) {
    g_main_cpu_cycles_estimate += 256;
    if (addr >= 0x2140 && addr <= 0x217F)
        g_apu_pace_cycles_estimate += 256;
}

/* Optional debug — disabled in release. Set BUILD_CPU_HW_LOG=1 in the
 * build to enable verbose per-touch logging. */
#define BUILD_CPU_HW_LOG 0
static uint64_t s_hw_touch_count = 0;
static uint16 s_last_hw_addr = 0;
static int s_last_hw_was_read = 0;
static int s_apu_writes_logged = 0;

/* Logger reachable from generated code. Disabled at release. */
void cpu_dbg_funcname(const char *name) {
    (void)name;
#if BUILD_CPU_HW_LOG
    static int n = 0;
    if (n++ < 50) {
        fprintf(stderr, "[func#%d] %s (touch=%llu)\n",
                n, name, (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#endif
}
static void cpu_hw_log(uint16 addr, int is_read, uint16 val) {
    s_last_hw_addr = addr;
    s_last_hw_was_read = is_read;
    if (!is_read && addr >= 0x2140 && addr <= 0x2143) {
        s_apu_writes_logged++;
    }
    s_hw_touch_count++;
#if BUILD_CPU_HW_LOG
    (void)val;
    if (s_hw_touch_count % 1000000 == 0) {
        fprintf(stderr, "[hw-pace] touches=%llu\n", (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#else
    (void)val;
#endif
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        /* AR_STACKPROV over-pop detector: a pull/RTS reads at addr==cpu->S (after
         * the emitted S++). If that slot was NEVER pushed this run, the pop is
         * draining stale memory => a net over-pop — the FIRST one is the actual
         * imbalance bug, upstream of where a later RTS returns to garbage. Name
         * the popping block-PC so we can fix the unbalanced routine directly. */
        if (bank == 0 && addr == cpu->S) {
            extern int ar_strace_active(void);
            extern void ar_strace_op(const char *, uint16, uint8, uint16);
            if (ar_strace_active()) ar_strace_op("POP", addr, cpu->ram[off], cpu->S);
            extern int ar_stackprov_enabled(void);
            if (ar_stackprov_enabled()) {
                extern uint32_t g_stack_pusher[];
                if (g_stack_pusher[addr] == 0) {
                    extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
                    extern int snes_frame_counter;
                    static uint32_t seen[64]; static int nseen;
                    uint32_t blk = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
                    int dup = 0;
                    for (int i = 0; i < nseen; i++) if (seen[i] == blk) { dup = 1; break; }
                    if (!dup && nseen < 64) {
                        seen[nseen++] = blk;
                        extern uint8 g_ram[0x20000];
                        unsigned _gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
                        fprintf(stderr, "[overpop] pop at block $%06X read NEVER-PUSHED "
                                "stack slot $%04X (S=$%04X, f=%d gf=%u) — unbalanced "
                                "pop/RTS draining stale stack. (AR_XFLIP_GF=%u to find "
                                "the x-leak on this frame.)\n", blk, addr, cpu->S,
                                snes_frame_counter, _gf, _gf);
                        /* Walk the block ring back to the most recent block where x
                         * flipped 0->1 — that block (the SEP/PLP in it) is the x-leak
                         * that selected a garbage (wrong-width) variant upstream of
                         * this drain. NOTE: don't gate on the LIVE cpu->x_flag — the
                         * crashing block ($9284's `...PLP;RTS`) has already PLP'd x
                         * back to 0 by the time we're here; the leaked-x history lives
                         * in the ring's per-block-entry x-bit (aux bit17). Also print
                         * the recent x/m trail so a benign late SEP isn't mistaken for
                         * the leak. (aux: bit17=x_flag, bit16=m_flag.) */
                        extern uint32_t g_ar_blk_aux[];
                        {
                            unsigned cur = (g_ar_blk_idx - 1u) & 1023u;
                            int found = 0;
                            for (int k = 1; k < 600; k++) {
                                unsigned i0 = (cur - (unsigned)k) & 1023u;
                                unsigned i1 = (cur - (unsigned)k + 1u) & 1023u;
                                int x0 = (g_ar_blk_aux[i0] >> 17) & 1;
                                int x1 = (g_ar_blk_aux[i1] >> 17) & 1;
                                if (x0 == 0 && x1 == 1) {
                                    fprintf(stderr, "[overpop]   x flipped 0->1: block "
                                            "$%06X (m=%u) -> $%06X (the SEP/PLP that "
                                            "leaked x is in $%06X).\n",
                                            g_ar_blk_ring[i0],
                                            (g_ar_blk_aux[i0] >> 16) & 1,
                                            g_ar_blk_ring[i1], g_ar_blk_ring[i0]);
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found)
                                fprintf(stderr, "[overpop]   no x 0->1 flip in last 600 "
                                        "blocks (x leak older, or not an x-leak).\n");
                            /* Recent block trail with per-block x/m so the flip is
                             * visible in context. */
                            fprintf(stderr, "[overpop]   recent trail (newest last):\n");
                            for (int k = 16; k >= 0; k--) {
                                unsigned i = (cur - (unsigned)k) & 1023u;
                                fprintf(stderr, "[overpop]     $%06X x=%u m=%u\n",
                                        g_ar_blk_ring[i], (g_ar_blk_aux[i] >> 17) & 1,
                                        (g_ar_blk_aux[i] >> 16) & 1);
                            }
                        }
                        fflush(stderr);
                    }
                }
            }
        }
        /* AR_READ0019=1 (2026-07-01, temporary probe): unconditional read
         * watch on $0019. Every WRAM-write mechanism (cpu_write8/16,
         * IndirWriteByte/Word, DMA via snes_write) has been instrumented
         * and shows ZERO writes setting $0019 to 0xA1, yet AR_SIMTRACE
         * shows it being read as 0xA1 mid-frame. Logging the read side
         * directly (with block PC via g_last_recomp_func) settles whether
         * the value is really there, and whichever read call actually
         * observes it narrows down what's between the (apparently absent)
         * write and this read. */
        if (off == 0x19 && getenv("AR_READ0019")) {
            static int n;
            if (n++ < 20000) {
                extern int snes_frame_counter; extern const char *g_last_recomp_func;
                extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                fprintf(stderr, "[read0019] $0019=%02x f=%d cur=%s m=%u x=%u A=%04x X=%04x D=%04x PB=%02x DB=%02x stk:",
                        cpu->ram[off], snes_frame_counter,
                        g_last_recomp_func ? g_last_recomp_func : "?",
                        (unsigned)cpu->m_flag, (unsigned)cpu->x_flag, cpu->A, cpu->X, cpu->D,
                        cpu->PB, cpu->DB);
                for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
                    fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                fprintf(stderr, "\n");
            }
        }
        return cpu->ram[off];
    }
    if (is_hw_reg(bank, addr)) {
      if (addr >= 0x2100 && addr <= 0x2133) {
        static int dbg_done;
        if (!dbg_done) { dbg_done = 1;
          fprintf(stderr, "[cpu_read8 PPU] bank=%02X addr=%04X | A=%04X X=%04X "
                  "Y=%04X D=%04X DB=%02X PB=%02X S=%04X | m_flag=%d x_flag=%d P=%02X\n",
                  bank, addr, cpu->A, cpu->X, cpu->Y, cpu->D, cpu->DB,
                  cpu->PB, cpu->S, cpu->m_flag, cpu->x_flag, cpu->P);
        }
      }
      cpu_pace_cycles(addr); cpu_hw_log(addr, 1, 0); return ReadReg(addr); }
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) return g_sram[sram];
    /* ROM read. RomPtr requires the global g_rom pointer to be live. */
    return *RomPtr(((uint32)bank << 16) | addr);
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000)
        return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off + 1] << 8);
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(addr); cpu_hw_log(addr, 1, 0); return ReadRegWord(addr); }
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        /* Compose word from two byte fetches. If the high byte crosses
         * out of SRAM (e.g. word read at $70:$7FFF), fall through to
         * cpu_read8 for that byte so the boundary is handled by the
         * same routing logic. */
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        uint8 hi = (sram_hi >= 0)
            ? g_sram[sram_hi]
            : cpu_read8(cpu, bank, (uint16)(addr + 1));
        return (uint16)g_sram[sram_lo] | ((uint16)hi << 8);
    }
    /* ROM word read. */
    const uint8 *p = RomPtr(((uint32)bank << 16) | addr);
    return (uint16)p[0] | ((uint16)p[1] << 8);
}

void ar_indirect_suppressed_log(CpuState *cpu, uint32 site_pc24,
                                 uint8 bank, uint16 table_base, uint16 x_reg) {
    if (!getenv("AR_INDIRLOG")) return;
    static uint32_t seen[128]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == site_pc24) return;
    if (nseen < 128) seen[nseen++] = site_pc24;

    extern int snes_frame_counter;
    uint32 eff = (uint32)table_base + x_reg;
    fprintf(stderr, "[indirlog] site=$%06X table=$%02X:%04X (eff=$%04X, X=$%04X) "
            "m=%u x=%u f=%d\n", site_pc24, bank, table_base, eff & 0xFFFFu, x_reg,
            (unsigned)cpu->m_flag, (unsigned)cpu->x_flag, snes_frame_counter);

    int off = cpu_ram_offset(bank, (uint16)eff);
    if (off >= 0 && off + 1 < 0x20000) {
        uint16 target = (uint16)cpu->ram[off] | ((uint16)cpu->ram[off + 1] << 8);
        fprintf(stderr, "[indirlog]   -> WRAM, live table entry = $%04X "
                "(would-be target $%02X:%04X)\n", target, bank, target);
    } else if (is_hw_reg(bank, (uint16)eff)) {
        /* Deliberately NOT read via ReadReg — this address was never
         * touched by any real dispatch before now (the JSR itself is
         * suppressed), so sampling it here would be a brand-new read
         * side effect. Report the classification only. */
        fprintf(stderr, "[indirlog]   -> SNES hardware-register space "
                "($2000-$5FFF) — NOT a real static/WRAM table. A "
                "\"JSR (abs,X)\" landing here is almost certainly a "
                "decode artifact (wrong entry m/x desyncing operand "
                "bytes), not a genuine unauthorised dispatch table.\n");
    } else {
        const uint8 *p = RomPtr(((uint32)bank << 16) | (uint16)eff);
        uint16 target = (uint16)p[0] | ((uint16)p[1] << 8);
        fprintf(stderr, "[indirlog]   -> ROM, static table entry = $%04X "
                "(would-be target $%02X:%04X)\n", target, bank, target);
    }
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        uint8 old = cpu->ram[off];
        /* AR_WATCH0019=1 (2026-07-01, temporary probe): unconditional --
         * fires on EVERY write to $0019, changed or not. AR_WATCHOBJ only
         * logs on value CHANGE (old != v); if something writes 0xA1 to $19
         * every frame and it's already 0xA1 (e.g. restored once from the
         * SRAM save via a raw memcpy at boot, bypassing all per-byte write
         * instrumentation, then rewritten identically every frame after),
         * AR_WATCHOBJ would correctly stay silent forever after the first
         * transition. This settles it either way. */
        if (off == 0x19 && getenv("AR_WATCH0019")) {
            static int n;
            if (n++ < 200) {
                extern int snes_frame_counter; extern const char *g_last_recomp_func;
                extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                fprintf(stderr, "[watch0019] $0019=%02x (was %02x) f=%d cur=%s stk:",
                        v, old, snes_frame_counter, g_last_recomp_func ? g_last_recomp_func : "?");
                for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
                    fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                fprintf(stderr, "\n");
            }
        }
        /* AR_STACKPROV pusher-provenance: in emitted push code the byte is
         * written to cpu->S BEFORE S is decremented, so addr==cpu->S uniquely
         * marks a stack push. Stamp the current block-PC as this slot's pusher
         * so a later bad-RTS can name who left the corrupt return frame. */
        if (bank == 0 && addr == cpu->S) {
            extern int ar_strace_active(void);
            extern void ar_strace_op(const char *, uint16, uint8, uint16);
            if (ar_strace_active()) ar_strace_op("PUSH", addr, v, cpu->S);
            extern int ar_stackprov_enabled(void);
            if (ar_stackprov_enabled()) {
                extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
                extern uint32_t g_stack_pusher[]; extern unsigned g_stack_pusher_frame[];
                extern int snes_frame_counter;
                g_stack_pusher[addr] = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
                g_stack_pusher_frame[addr] = (unsigned)snes_frame_counter;
            }
        }
        /* AR_WATCH18: trace game-mode byte $7E:0018 changes (overworld $18=0
         * vs action stage $18=1) — find who drives the mode transition and
         * what the display state is at that moment. */
        if (off == 0x18 && old != v && getenv("AR_WATCH18")) {
            extern int snes_frame_counter; extern uint8 g_ram[0x20000];
            extern const char *g_last_recomp_func;
            extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
            (void)g_recomp_stack; (void)g_recomp_stack_top;
            uint16 s = cpu->S;
            fprintf(stderr, "[$18] f=%d %02x->%02x $19=%02x by=%s PB=%02x S=%04x snstk:",
                    snes_frame_counter, old, v, g_ram[0x19],
                    g_last_recomp_func ? g_last_recomp_func : "?", cpu->PB, s);
            for (int i = 1; i <= 18; i++)
                fprintf(stderr, " %02x", g_ram[(uint16)(s + i)]);
            fprintf(stderr, "\n");
        }
        /* AR_WATCHOBJ=<hexaddr>: trip when WRAM offset [addr,addr+0x3f) (one
         * object slot) is written — logs writer func + recomp stack + frame, to
         * find the spawner of an object (and why a sibling slot is never written). */
        if (getenv("AR_WATCHOBJ")) {
            static long wo = -2;
            if (wo == -2) { const char *e = getenv("AR_WATCHOBJ"); wo = e ? (long)strtoul(e, NULL, 16) : -1; }
            if (wo >= 0 && off >= wo && off < wo + 0x40 && old != v) {
                extern int snes_frame_counter; extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                extern const char *g_last_recomp_func;
                extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
                static int n;
                if (n++ < 8000) {
                    uint32_t last_blk = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
                    int d14off = cpu_ram_offset(0x7E, (uint16)(cpu->D + 0x0014));
                    int d16off = cpu_ram_offset(0x7E, (uint16)(cpu->D + 0x0016));
                    uint16 d14 = (d14off >= 0) ? ((uint16)cpu->ram[d14off] | ((uint16)cpu->ram[d14off+1] << 8)) : 0xffff;
                    uint16 d16 = (d16off >= 0) ? ((uint16)cpu->ram[d16off] | ((uint16)cpu->ram[d16off+1] << 8)) : 0xffff;
                    fprintf(stderr, "[wobj] $%04x=%02x (was %02x) f=%d PB=%02x blkpc=$%06X X=$%04X DB=$%02X D14=$%04X D16=$%04X cur=%s stk:",
                            off, v, old, snes_frame_counter, cpu->PB, last_blk, cpu->X, cpu->DB, d14, d16,
                            g_last_recomp_func ? g_last_recomp_func : "?");
                    for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
                        fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                    fprintf(stderr, "\n");
                }
            }
        }
        cpu->ram[off] = v;
        { extern int ar_trace_active(void);
          extern void ar_trace_wram(uint32_t, uint16_t, uint16_t, int);
          if (ar_trace_active()) ar_trace_wram((uint32_t)off, (uint16_t)old, (uint16_t)v, 1); }
        cpu_trace_wram_write_check(cpu, bank, addr, off,
                                   (uint16)old, (uint16)v, 1);
        /* Also route through the dedicated 1M-entry WRAM-only ring so
         * writes survive when the main cpu_trace ring gets buried by
         * unrelated events (e.g. a BCS-self-loop block firing millions
         * of BLOCK events). IndirWriteByte/Word (common_rtl.h) already
         * does this for indirect stores; mirror it here for the
         * cpu_write8/16 path. */
#if SNESRECOMP_REVERSE_DEBUG
        extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        debug_on_wram_write_byte((uint32_t)off, old, v);
#endif
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(addr); cpu_hw_log(addr, 0, v); WriteReg(addr, v); return; }
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) { g_sram[sram] = v; return; }
    /* ROM / unmapped write: drop. */
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000) {
        uint16 old = (uint16)cpu->ram[off]
                   | ((uint16)cpu->ram[off + 1] << 8);
        /* AR_WATCH14=1 (2026-07-01, temporary probe): traces the actual STA
         * $0014/$0016 write inside bank_01_ADAD (the position scratch pair
         * that AR_WATCHOBJ found frozen at read-time despite varying per-
         * object source data) -- is the WRITE itself already frozen (bug is
         * upstream, e.g. the subtracted D:0094/D:0096 reference), or does
         * something clobber it between write and read (shouldn't be
         * possible per static read -- no calls in between -- but verify)? */
        if ((off == 0x14 || off == 0x16) && getenv("AR_WATCH14")) {
            extern int snes_frame_counter; extern const char *g_last_recomp_func;
            static int n;
            if (n++ < 4000)
                fprintf(stderr, "[watch14] $%04x=%04x (was %04x) f=%d X=$%04X PB=%02x cur=%s\n",
                        off, v, old, snes_frame_counter, cpu->X, cpu->PB,
                        g_last_recomp_func ? g_last_recomp_func : "?");
        }
        cpu->ram[off]     = (uint8)(v & 0xFF);
        cpu->ram[off + 1] = (uint8)(v >> 8);
        /* AR_WATCH0019=1 (2026-07-01, temporary probe): see cpu_write8's
         * copy of this block for why -- unconditional, catches a same-
         * value rewrite AR_WATCHOBJ's on-change filter would hide. A
         * 16-bit write at off==0x18 also touches 0x19 (the high byte). */
        if ((off == 0x19 || off == 0x18) && getenv("AR_WATCH0019")) {
            static int n;
            if (n++ < 200) {
                extern int snes_frame_counter; extern const char *g_last_recomp_func;
                extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                fprintf(stderr, "[watch0019-16] off=$%04x v=%04x (was %04x) f=%d cur=%s stk:",
                        off, v, old, snes_frame_counter, g_last_recomp_func ? g_last_recomp_func : "?");
                for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
                    fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                fprintf(stderr, "\n");
            }
        }
        if (getenv("AR_WATCHOBJ")) {
            static long wo = -2;
            if (wo == -2) { const char *e = getenv("AR_WATCHOBJ"); wo = e ? (long)strtoul(e, NULL, 16) : -1; }
            if (wo >= 0 && off >= wo && off < wo + 0x40 && old != v) {
                extern int snes_frame_counter; extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                extern const char *g_last_recomp_func;
                static int n;
                if (n++ < 8000) {
                    fprintf(stderr, "[wobj] $%04x=%04x (was %04x) f=%d PB=%02x cur=%s stk:", off, v, old, snes_frame_counter, cpu->PB, g_last_recomp_func ? g_last_recomp_func : "?");
                    for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 6; i--)
                        fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                    fprintf(stderr, "\n");
                }
            }
        }
        /* AR_WATCH16=<hex>: trip when this 16-bit value is written to WRAM —
         * who corrupts an object's $12 handler pointer to a data-table addr
         * ($AB3C action-level freeze). Logs the dest addr, the writing recomp
         * fn, m/x, and a short call stack. */
        if (getenv("AR_WATCH16")) {
            static int wv = -2;
            if (wv == -2) { const char *e = getenv("AR_WATCH16"); wv = e ? (int)strtoul(e, NULL, 16) : -1; }
            if (wv >= 0 && v == (uint16)wv) {
                extern int snes_frame_counter;
                extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                extern const char *g_last_recomp_func;
                int top = g_recomp_stack_top;
                fprintf(stderr, "[watch16] v=%04x -> %02x:%04x (off=%05x) by=%s m=%u x=%u f=%d stack:",
                        v, bank, addr, off, g_last_recomp_func ? g_last_recomp_func : "?",
                        (unsigned)cpu->m_flag, (unsigned)cpu->x_flag, snes_frame_counter);
                for (int i = top - 1; i >= 0 && i >= top - 8; i--)
                    fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                fprintf(stderr, "\n");
            }
        }
        { extern int ar_trace_active(void);
          extern void ar_trace_wram(uint32_t, uint16_t, uint16_t, int);
          if (ar_trace_active()) ar_trace_wram((uint32_t)off, old, v, 2); }
        cpu_trace_wram_write_check(cpu, bank, addr, off, old, v, 2);
#if SNESRECOMP_REVERSE_DEBUG
        extern void debug_on_wram_write_word(uint32_t, uint16_t, uint16_t);
        debug_on_wram_write_word((uint32_t)off, old, v);
#endif
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(addr); cpu_hw_log(addr, 0, v); WriteRegWord(addr, v); return; }
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        g_sram[sram_lo] = (uint8)(v & 0xFF);
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        if (sram_hi >= 0) g_sram[sram_hi] = (uint8)(v >> 8);
        else cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
        return;
    }
    /* ROM / unmapped write: drop. */
}

/* ── PEI-trampoline dispatch helper (2026-05-24, narrow detector) ──────
 *
 * Called from _emit_return on trampoline-flagged Returns when the
 * runtime balance check (cpu->S != _entry_s) fires. The caller has
 * already popped the topmost frame from cpu->S and computed
 * (PB, PC+1) as `pc24`. We binary-search g_dispatch_table for an
 * entry matching `pc24` and, if found, call the variant fnptr for
 * the runtime (m, x) flags.
 *
 * Not-found case: pc24 doesn't correspond to a known function entry.
 * Returning NORMAL lets the host C call stack unwind back through the
 * chain of `return cpu_dispatch_pc(...)` tail calls to the original
 * site, which then resumes naturally.
 */

/* Diagnostic ring for dispatch events — instrumentation added during
 * MMX Dr Light "sprite vanish" diagnosis (2026-05-24). Each entry
 * records (pc24, mx_idx, found, frame) for one cpu_dispatch_pc call.
 * Always-on (small fixed allocation, no perf concern). TCP cmd
 * `dispatch_log_get` dumps the ring. */
typedef struct DispatchLogEntry {
    uint32_t pc24;
    uint32_t source_pc24;
    const char *func_name;
    uint8_t  mx_idx;     /* (m<<1)|x */
    uint8_t  found;      /* 1 if entry found in table, 0 if miss */
    uint8_t  mirror;     /* 1 if found only via LoROM bank-mirror lookup */
    uint8_t  pad;
    uint32_t frame;
} DispatchLogEntry;

#define DISPATCH_LOG_CAP 1024
static DispatchLogEntry g_dispatch_log[DISPATCH_LOG_CAP];
static unsigned g_dispatch_log_idx;  /* monotonic; modulo via CAP for storage */

extern int snes_frame_counter;  /* common_rtl.c — game frame number */
extern const char *g_last_recomp_func;

static void _dispatch_log_record(uint32 pc24, uint32 source_pc24,
                                 unsigned mx_idx,
                                 int found, int via_mirror) {
    unsigned slot = g_dispatch_log_idx % DISPATCH_LOG_CAP;
    g_dispatch_log[slot].pc24 = pc24;
    g_dispatch_log[slot].source_pc24 = source_pc24;
    g_dispatch_log[slot].func_name = g_last_recomp_func;
    g_dispatch_log[slot].mx_idx = (uint8_t)mx_idx;
    g_dispatch_log[slot].found = (uint8_t)(found ? 1 : 0);
    g_dispatch_log[slot].mirror = (uint8_t)(via_mirror ? 1 : 0);
    g_dispatch_log[slot].pad = 0;
    g_dispatch_log[slot].frame = (uint32_t)snes_frame_counter;
    g_dispatch_log_idx++;
}

unsigned cpu_dispatch_log_count(void) {
    return g_dispatch_log_idx;
}

const DispatchLogEntry *cpu_dispatch_log_at(unsigned i) {
    if (i >= g_dispatch_log_idx) return NULL;
    if (g_dispatch_log_idx > DISPATCH_LOG_CAP &&
        i < g_dispatch_log_idx - DISPATCH_LOG_CAP) return NULL;
    return &g_dispatch_log[i % DISPATCH_LOG_CAP];
}

/* On-exit post-mortem dump of the dispatch ring to a standalone JSON file.
 * The TCP `dispatch_log_get` command needs the debug server connected; this
 * is the offline equivalent — the readable record of the last DISPATCH_LOG_CAP
 * runtime dispatches feeding into a crash. Each `found:0` entry is a target
 * that missed the AOT table (resolve lead). Adapted from upstream snesrecomp's
 * CpuDispatchLogDumpJson; emits hex pc24/source for grep-ability. */
void CpuDispatchLogWriteFile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned total = g_dispatch_log_idx;
    unsigned n = total < DISPATCH_LOG_CAP ? total : DISPATCH_LOG_CAP;
    unsigned start = total - n;
    fprintf(f, "{\n  \"dispatch_log\": {\"total\": %u, \"shown\": %u, "
               "\"events\": [\n", total, n);
    for (unsigned i = 0; i < n; i++) {
        const DispatchLogEntry *e = &g_dispatch_log[(start + i) % DISPATCH_LOG_CAP];
        const char *nm = e->func_name ? e->func_name : "(none)";
        fprintf(f,
            "%s    {\"i\":%u,\"pc24\":\"%06X\",\"source_pc24\":\"%06X\","
            "\"func\":\"%s\",\"mx\":%u,\"found\":%u,\"mirror\":%u,\"frame\":%u}",
            (i ? ",\n" : ""), start + i,
            (unsigned)e->pc24, (unsigned)e->source_pc24, nm,
            (unsigned)e->mx_idx, (unsigned)e->found,
            (unsigned)e->mirror, (unsigned)e->frame);
    }
    fprintf(f, "\n  ]}\n}\n");
    fclose(f);
}

static RecompReturn (*_cpu_dispatch_lookup(CpuState *cpu, uint32 pc24))(CpuState *) {
    unsigned lo = 0;
    unsigned hi = g_dispatch_table_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32 mid_pc = g_dispatch_table[mid].pc24;
        if (mid_pc == pc24) {
            unsigned idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
            return g_dispatch_table[mid].variant[idx];
        }
        if (mid_pc < pc24) lo = mid + 1;
        else               hi = mid;
    }
    return NULL;
}

/* One dispatch step: resolve pc24 to a function variant and invoke it. May
 * return RECOMP_RETURN_TAILCALL (the invoked dispatched frame asked to tail-
 * dispatch to g_tailcall_pc24); the driving loop in cpu_dispatch_pc_from
 * consumes that and iterates, so a computed-jump loop runs flat instead of
 * nesting the C/recomp stack (which overflowed RECOMP_STACK_DEPTH=64 and
 * produced over-unwinding SKIP_N — ActRaiser action-stage black playfield). */
static RecompReturn _cpu_dispatch_once(CpuState *cpu, uint32 pc24,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24) {
    pc24 &= 0xFFFFFFu;
    source_pc24 &= 0xFFFFFFu;
    unsigned mx_idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    int via_mirror = 0;
    /* AR_F5BE_HANDLERS=1 (2026-07-02, temporary probe): the ActRaiser town
     * per-frame handler dispatcher $03:F5BE calls out via a PHX/PHY/PHA/SEP/
     * RTS trick whose real target/return-continuation set can't be reliably
     * reconstructed by static ROM analysis (the per-town table layout at
     * $03:F5ED doesn't match a simple fixed-stride array -- naive re-scan
     * produces garbage). This is the generic dispatch trampoline every
     * mismatched RTS funnels through, so logging here captures ground truth
     * with zero engine/decode risk: every dispatch whose source is F5BE's
     * exit RTS site ($03F5E2) IS a handler call; log its target + cpu->S +
     * frame so the real handler set and per-town table shape can be read
     * off directly instead of guessed. See DEBUG.md / SEAMS.md town-handler
     * subsystem notes. */
    if (getenv("AR_F5BE_HANDLERS") && (source_pc24 == 0x03F5E2u || source_pc24 == 0x03F5E3u)) {
        extern int snes_frame_counter;
        fprintf(stderr, "[f5be] src=%06X -> target=%06X S=%04X m=%u x=%u f=%d\n",
                source_pc24, pc24, cpu->S, cpu->m_flag & 1, cpu->x_flag & 1,
                snes_frame_counter);
        fflush(stderr);
    }
    RecompReturn (*fp)(CpuState *) = _cpu_dispatch_lookup(cpu, pc24);
    if (fp == NULL) {
        /* LoROM bank-mirror fallback: $00-$3F and $80-$BF share bytes.
         * Cfg may declare a function in one bank while the trampoline
         * popped (PB:PC) lands on the mirror. Try the other bank
         * before giving up — matches set_name_resolver's alias. */
        uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
        if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
            fp = _cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u);
            if (fp != NULL) via_mirror = 1;
        }
    }
    _dispatch_log_record(pc24, source_pc24, mx_idx, fp != NULL, via_mirror);
    /* AR_B127LOG: trace the dispatch to $02:B127 (the m-flag-misdecode handler).
     * B127 must run M=1 (8-bit); if mx_idx has m=0 it misdecodes LDA #$1480. Logs
     * the m/x flags, the source PC that dispatched it, and the recomp call stack. */
    if (pc24 == 0x02B127u && getenv("AR_B127LOG")) {
        extern int snes_frame_counter; extern int g_recomp_stack_top;
        extern const char *g_recomp_stack[];
        fprintf(stderr, "[b127] ->%06x from %06x mx=%u (m=%u x=%u) S=%04x f=%d\n",
                pc24, source_pc24, mx_idx, (unsigned)cpu->m_flag,
                (unsigned)cpu->x_flag, cpu->S, snes_frame_counter);
        for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 10; i--)
            fprintf(stderr, "[b127]   [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    }
    /* AR_B898LOG (2026-06-30): trace EVERY dispatch that resolves to $01:B898
     * (mode-agnostic, hit-or-miss -- unlike AR_DISPMISSALL which is gated to
     * action-stage misses only, neither of which fits our case: B898 is a
     * REGISTERED function, so a dispatch to it is a HIT, and we're in sim
     * mode). Names the source PC / call stack for the x=1 anomaly (933C's
     * OWN direct call to B898 is proven clean via AR_CALLMX -- this must be a
     * SEPARATE dispatch, most likely via cpu_dispatch_pc_from's flat-dispatch
     * loop landing on B898's entry address from an unrelated miss elsewhere;
     * this log names exactly which source RTS/dispatch produces it). */
    if (pc24 == 0x01B898u && getenv("AR_B898LOG")) {
        extern int snes_frame_counter; extern int g_recomp_stack_top;
        extern const char *g_recomp_stack[];
        static unsigned long n;
        if (n++ < 4000) {
            fprintf(stderr, "[b898log] ->%06x from %06x mx=%u (m=%u x=%u) found=%d "
                    "via_mirror=%d S=%04x top=%d f=%d\n",
                    pc24, source_pc24, mx_idx, (unsigned)cpu->m_flag,
                    (unsigned)cpu->x_flag, fp != NULL, via_mirror, cpu->S,
                    g_recomp_stack_top, snes_frame_counter);
            for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 10; i--)
                fprintf(stderr, "[b898log]   [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        }
    }
    /* AR_1EHIT: trace every $1E,X object-handler dispatch (source $8668 =
     * $8661/$8657's LDA $1E,X; PHA; RTS). The newly-registered $8657 yield
     * continuations run here; the last one before a hang/corruption is the
     * culprit. Logs target, m/x, found, SNES S, recomp depth, frame. */
    if (source_pc24 == 0x008668u && getenv("AR_1EHIT")) {
        extern int snes_frame_counter; extern int g_recomp_stack_top;
        static unsigned long n;
        if (n++ < 4000)
            fprintf(stderr, "[1e] ->%06x mx=%u found=%d S=%04x top=%d f=%d\n",
                    pc24, mx_idx, fp != NULL, cpu->S, g_recomp_stack_top,
                    snes_frame_counter);
    }
    /* AR_8966X: trace the $8915 object-loop continuation ($8966). Logs the loop
     * index X and the object status word read at $00,X each iteration, so an X
     * walk-off (past the object table into $2xxx PPU regs) or a corrupted
     * sentinel is visible, plus the source (who re-entered the loop) + depth. */
    if (pc24 == 0x008966u && getenv("AR_8966X")) {
        extern int snes_frame_counter; extern int g_recomp_stack_top;
        extern uint8 g_ram[0x20000];
        static unsigned long n;
        unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
        const char *gfe = getenv("AR_8966X_GF");
        long wantgf = gfe ? atol(gfe) : -1;
        uint16 x = cpu->X;
        uint16 sw = (uint16)(g_ram[x & 0x1FFFF] | (g_ram[(x + 1) & 0x1FFFF] << 8));
        if (n++ < 4000 && (wantgf < 0 || (long)gf == wantgf))
            fprintf(stderr, "[8966] X=%04x [$00,X]=%04x S=%04x m=%u src=%06x top=%d f=%d\n",
                    x, sw, cpu->S, (unsigned)cpu->m_flag, source_pc24, g_recomp_stack_top, snes_frame_counter);
    }
    /* AR_8966X: also log every object-handler dispatch (source $8965 = $895C's
     * LDA #$8965;PHA; LDA $12,X;DEC;PHA;RTS). Shows X AT HANDLER ENTRY + the
     * handler addr; pairing with the next [8966] X (handler must preserve X)
     * names the handler that corrupts the loop index. */
    if (source_pc24 == 0x008965u && getenv("AR_8966X")) {
        extern int snes_frame_counter;
        extern uint8 g_ram[0x20000];
        static unsigned long n;
        unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
        const char *gfe = getenv("AR_8966X_GF");
        long wantgf = gfe ? atol(gfe) : -1;
        if (n++ < 4000 && (wantgf < 0 || (long)gf == wantgf))
            fprintf(stderr, "[hdlr] ->%06x X=%04x S=%04x m=%u f=%d\n",
                    pc24, cpu->X, cpu->S, (unsigned)cpu->m_flag, snes_frame_counter);
    }
    /* TEMP DIAGNOSTIC AR_DISP8465: log every dispatch that targets $00:8465
     * (the misdecoded NMI-enable routine) — reveals WHO dispatches to it and in
     * what M/X state, since the steady-state freeze enters 8465_M0X0 at top
     * level (not via 82E2's JSR). */
    if ((pc24 == 0x008465u || pc24 == 0x008466u) && getenv("AR_DISP8465")) {
        extern int snes_frame_counter;
        static unsigned long n;
        if ((n++ % 600) == 0)
            fprintf(stderr, "[disp] ->%06x from %06x m=%u x=%u S=%04x found=%d f=%d (n=%lu)\n",
                    pc24, source_pc24, (unsigned)cpu->m_flag, (unsigned)cpu->x_flag,
                    cpu->S, fp != NULL, snes_frame_counter, n);
    }
    if (fp == NULL) {
        /* Some games use a verified RAM-handler loop whose table entries are
         * tiny BRA/BRL trampolines embedded in data. Per-game policy decides
         * which source sites may use this recovery: first follow a branch to a
         * registered shared handler; otherwise pop the verified continuation
         * frame and resume the loop so width-restoring epilogues still run. */
        if (g_rtl_game_info && g_rtl_game_info->recover_dispatch_miss &&
            g_rtl_game_info->recover_dispatch_miss(source_pc24, pc24)) {
            uint8  tb = (uint8)((pc24 >> 16) & 0xFF);
            uint16 ta = (uint16)(pc24 & 0xFFFF);
            uint8  op = cpu_read8(cpu, tb, ta);
            uint32 followed = 0xFFFFFFFFu;
            if (op == 0x80) {            /* BRA rel8 */
                int8 d = (int8)cpu_read8(cpu, tb, (uint16)(ta + 1));
                followed = ((uint32)tb << 16) | (uint16)(ta + 2 + d);
            } else if (op == 0x82) {     /* BRL rel16 */
                uint16 d = (uint16)(cpu_read8(cpu, tb, (uint16)(ta + 1))
                          | (cpu_read8(cpu, tb, (uint16)(ta + 2)) << 8));
                followed = ((uint32)tb << 16) | (uint16)(ta + 3 + d);
            }
            RecompReturn (*ffp)(CpuState *) = NULL;
            if (followed != 0xFFFFFFFFu) {
                ffp = _cpu_dispatch_lookup(cpu, followed);
                if (ffp == NULL) {
                    uint8 fb = (uint8)((followed >> 16) & 0xFF);
                    if (fb < 0x40 || (fb >= 0x80 && fb < 0xC0))
                        ffp = _cpu_dispatch_lookup(cpu, followed ^ 0x800000u);
                }
            }
            if (getenv("AR_DISPMISS")) {
                extern int snes_frame_counter;
                static unsigned long n;
                if (n++ < 400)
                    fprintf(stderr,
                        "[dispmiss] handler->%06x op=%02x bra->%06x resolved=%d m=%u x=%u S=%04x f=%d\n",
                        pc24, op, (followed == 0xFFFFFFFFu) ? 0u : followed,
                        ffp != NULL, (unsigned)cpu->m_flag, (unsigned)cpu->x_flag,
                        cpu->S, snes_frame_counter);
            }
            if (ffp != NULL) {
                /* BRA/BRL is non-destructive (no stack change). Dispatch the
                 * shared handler in place of the unregistered trampoline; its
                 * own RTS pops the $8965 continuation and resumes the loop. */
                cpu->host_return_valid = 0;
                return ffp(cpu);
            }
            /* Graceful fallback: emulate the handler's RTS without running it,
             * then dispatch the continuation supplied on the SNES stack. */
            {
                uint16 sp = cpu->S;
                uint16 cl = cpu_read8(cpu, 0x00, (uint16)(sp + 1));
                uint16 ch = cpu_read8(cpu, 0x00, (uint16)(sp + 2));
                uint32 cont = (uint32)((((ch << 8) | cl) + 1) & 0xFFFFu);
                cpu->S = (uint16)(sp + 2);
                RecompReturn (*cfp)(CpuState *) = _cpu_dispatch_lookup(cpu, cont);
                if (cfp != NULL) {
                    cpu->host_return_valid = 0;
                    return cfp(cpu);
                }
                /* An unregistered continuation falls through to generic unwind. */
            }
        }
        /* Generic RTS/RTL-follow (2026-07-02): a computed dispatch (PHA/RTS
         * jump table) often lands on a bare RTS/RTL -- a "no-op handler"
         * table entry, or a pushed continuation whose only job is to pop the
         * next frame. E.g. ActRaiser's town dispatcher $03:86FD passes its
         * continuation in A (#$8711), so every handler chain returns through
         * the bare RTS at $03:8712, then another at $03:86FC -- none of which
         * are function entries. Statically registering every 1-byte RTS hop
         * is whack-a-mole (each town/state variant adds more); instead,
         * emulate the RTS chain: pop the next return frame and retry the
         * lookup. Flags are untouched (RTS/RTL don't affect m/x), so this is
         * semantically exact. Mirrors the BRA/BRL follow above; hop-capped
         * against garbage chains, and any pops are undone by the absolute
         * S-restore in the generic unwind below if the chain still misses. */
        {
            int hops = 0;
            uint32 fpc = pc24;
            while (fp == NULL && hops < 8) {
                uint8  tb = (uint8)((fpc >> 16) & 0xFF);
                uint16 ta = (uint16)(fpc & 0xFFFF);
                uint8  op = cpu_read8(cpu, tb, ta);
                if (op == 0x60) {          /* RTS: pop 2, target+1 (same bank) */
                    uint16 lo = cpu_read8(cpu, 0x00, (uint16)(cpu->S + 1));
                    uint16 hi = cpu_read8(cpu, 0x00, (uint16)(cpu->S + 2));
                    cpu->S = (uint16)(cpu->S + 2);
                    fpc = ((uint32)tb << 16) |
                          (uint16)((((hi << 8) | lo) + 1) & 0xFFFFu);
                } else if (op == 0x6B) {   /* RTL: pop 3, target+1 (banked) */
                    uint16 lo = cpu_read8(cpu, 0x00, (uint16)(cpu->S + 1));
                    uint16 hi = cpu_read8(cpu, 0x00, (uint16)(cpu->S + 2));
                    uint8  bk = cpu_read8(cpu, 0x00, (uint16)(cpu->S + 3));
                    cpu->S = (uint16)(cpu->S + 3);
                    fpc = ((uint32)bk << 16) |
                          (uint16)((((hi << 8) | lo) + 1) & 0xFFFFu);
                } else {
                    break;
                }
                hops++;
                fp = _cpu_dispatch_lookup(cpu, fpc);
                if (fp == NULL) {
                    uint8 fb = (uint8)((fpc >> 16) & 0xFF);
                    if (fb < 0x40 || (fb >= 0x80 && fb < 0xC0))
                        fp = _cpu_dispatch_lookup(cpu, fpc ^ 0x800000u);
                }
            }
            if (fp != NULL) {
                _dispatch_log_record(fpc, source_pc24, mx_idx, 1, 0);
                cpu->host_return_valid = 0;
                return fp(cpu);
            }
        }
        /* Not found: the popped (PB:PC) is a normal mid-caller return addr,
         * not a known function entry. Unwind by restoring cpu->S to the value
         * the caller expects after THIS function returns and returning NORMAL.
         * The caller passes entry_s_for_miss_restore = entry_s + frame_size
         * (the S after this function pops its own return frame) — so a
         * balanced hrv=0 callee returns with its frame correctly popped, and a
         * PEI trampoline discards its residual params up to that point. Passing
         * bare entry_s here would under-pop by frame_size and leak the caller's
         * frame on every miss (the heavy-load DMA-queue-corruption softlock;
         * cf. MMX Dr Light "sprite vanish" 2026-05-24). */
        /* AR_DISPMISSALL: log EVERY dispatch miss while in the action stage
         * ($18==01), with its source — catches computed-dispatch misses to
         * unemitted routines (e.g. bank-02 fade routines $AB30/$AB6B) that the
         * $8965-gated AR_DISPMISS never saw. A miss to the action-stage fade-in
         * routine would silently skip it -> black playfield. */
        if (getenv("AR_DISPMISSALL")) {
            extern int snes_frame_counter; extern uint8 g_ram[0x20000];
            if (g_ram[0x18] == 0x01) {
                static unsigned long n;
                if (n++ < 50000)
                    fprintf(stderr, "[missall] ->%06x from %06x m=%u x=%u f=%d\n",
                            pc24, source_pc24, (unsigned)cpu->m_flag,
                            (unsigned)cpu->x_flag, snes_frame_counter);
            }
        }
        /* Dispatch-miss tripwire (default ON, deduped + capped). Reaching here
         * means an RTS/RTL/computed dispatch popped a (PB:PC) that is NOT a
         * registered function entry, so we host-unwind to the lexical caller.
         * That is correct for an ordinary mid-caller return — but it is ALSO
         * exactly how an RTS-TRICK to an intra-function label goes silently
         * wrong: the unwind resumes the WRONG pc carrying whatever m/x the trick
         * left set. That is the $03:9156 act->sim transition crash (dispatch
         * 039B59 -> 039B22 had no entry -> host-unwound to $8053:$80B6 at m=1 ->
         * $AC8E ran its garbage M1X0 variant -> SNES stack underflow -> $2133
         * PPU-reg scribble -> abort). It cost a long multi-tool hunt; this names
         * the offending computed target on the FIRST run instead. The fix for a
         * real one is to register the target as a cfg `func ... entry_mx:m,x`.
         * Warn once per (source,target) pair so benign repeats stay quiet;
         * suppress entirely with AR_NODISPWARN=1, add the call stack with
         * AR_DISPWARN=1. See DEBUG.md "Dispatch-miss / RTS-trick" section. */
        {
            /* Unified AR_TRACE dispmiss channel — every miss in-window, no dedup. */
            { extern int ar_trace_active(void);
              extern void ar_trace_dispmiss(uint32_t, uint32_t);
              if (ar_trace_active()) ar_trace_dispmiss(source_pc24, pc24); }
            static int warn = -1, warnall = -1;
            if (warn < 0) warn = getenv("AR_NODISPWARN") ? 0 : 1;
            if (warnall < 0) warnall = getenv("AR_DISPWARN") ? 1 : 0;
            /* By default only flag the DANGEROUS subset: a miss while the SNES
             * stack is relocated out of page 0/1 (S >= $0200) — the RTS-trick /
             * computed-dispatch signature. Ordinary mid-caller returns unwind
             * here too but with S in page 1; flagging those would bury the
             * signal under benign noise and fill the cap. AR_DISPWARN=1 removes
             * the gate (shows every miss) and adds the call stack. */
            extern int ar_stackprov_enabled(void);
            if ((warn && (warnall || cpu->S >= 0x0200)) || ar_stackprov_enabled()) {
                static struct { uint32_t s, t; } seen[128];
                static int nseen, capped;
                int dup = 0;
                for (int i = 0; i < nseen; i++)
                    if (seen[i].s == source_pc24 && seen[i].t == pc24) { dup = 1; break; }
                if (!dup && nseen < 128) {
                    seen[nseen].s = source_pc24; seen[nseen].t = pc24; nseen++;
                    extern int snes_frame_counter;
                    fprintf(stderr,
                        "[dispatch-miss] %06X -> %06X has no entry; host-unwinding "
                        "(m=%u x=%u S=%04X f=%d). If control/flags are wrong after this, "
                        "register %06X as a cfg `func` (RTS-trick/computed target).\n",
                        source_pc24, pc24, cpu->m_flag & 1, cpu->x_flag & 1, cpu->S,
                        snes_frame_counter, pc24);
                    if (warnall) {
                        extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
                        for (int i = g_recomp_stack_top - 1;
                             i >= 0 && i >= g_recomp_stack_top - 8; i--)
                            fprintf(stderr, "[dispatch-miss]   [%d] %s\n", i,
                                    g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                    }
                    /* AR_STACKPROV: name who pushed the corrupt return frame. The
                     * RTS that produced this bad target popped its 2 bytes from
                     * just below the current S, so dump the pusher-PC of the slots
                     * around S. A slot tagged NEVER-PUSHED means the RTS read stale
                     * memory it never wrote => S itself is wrong (bad relocation),
                     * not a bad push — the opposite fix. */
                    {
                        extern int ar_stackprov_enabled(void);
                        if (ar_stackprov_enabled()) {
                            extern uint32_t g_stack_pusher[];
                            extern unsigned g_stack_pusher_frame[];
                            fprintf(stderr,
                                "[stackprov] return-frame provenance around S=$%04X "
                                "(bad target %06X, f=%d):\n", cpu->S, pc24,
                                snes_frame_counter);
                            for (int o = -2; o <= 5; o++) {
                                uint16 a = (uint16)(cpu->S + o);
                                uint8 b = cpu_read8(cpu, 0x00, a);
                                uint32_t pp = g_stack_pusher[a];
                                if (pp)
                                    fprintf(stderr,
                                        "[stackprov]   $%04X = $%02X  pushed-by PC "
                                        "$%06X (f=%u)%s\n", a, b, pp,
                                        g_stack_pusher_frame[a],
                                        (o == -1 || o == 0) ? "  <- return frame" : "");
                                else
                                    fprintf(stderr,
                                        "[stackprov]   $%04X = $%02X  NEVER PUSHED "
                                        "(stale/wrong-S)%s\n", a, b,
                                        (o == -1 || o == 0) ? "  <- return frame" : "");
                            }
                        }
                    }
                    fflush(stderr);
                } else if (!dup && !capped) {
                    capped = 1;
                    fprintf(stderr, "[dispatch-miss] (cap 128 reached; further unique "
                                    "misses suppressed — set AR_NODISPWARN=1 to silence)\n");
                    fflush(stderr);
                }
            }
        }
        cpu->S = entry_s_for_miss_restore;
        return RECOMP_RETURN_NORMAL;
    }
    /* Option-1: a dispatched entry has no paired host-C caller. The target
     * runs with host_return_valid=0 so its RTS/RTL re-dispatches on the
     * popped PC rather than host-returning into this dispatch frame. The
     * chain unwinds when a dispatch misses (S restored above) -> NORMAL. */
    /* ── Dispatch recursion guard (2026-07-06, the closure-loop enabler) ──
     * A WRONG `func` registration of a mid-loop construct continuation (the
     * B8C2 class) turns a benign miss-unwind into one nested re-entry per
     * record -> C-stack overflow. Cap live dispatches of the SAME target:
     * past the cap, fall back to exactly what an UNREGISTERED target would do
     * (S restore + NORMAL unwind) and warn once naming the cfg line to remove.
     * This makes a bad registration self-healing (old behavior + diagnostic
     * instead of a crash), which is what lets the static closure loop
     * (find_rts_webs --suggest) append candidates without a manual shape
     * check being life-or-death. Legit 65816 code never self-nests 24 deep
     * through the dispatcher (RECOMP_STACK_DEPTH itself is only 64). */
    {
        #define AR_DISP_LIVE_MAX 256
        #define AR_DISP_RECURSION_CAP 24
        static uint32 s_disp_live[AR_DISP_LIVE_MAX];
        static int s_disp_live_top;
        int live = 0;
        for (int i = 0; i < s_disp_live_top; i++)
            if (s_disp_live[i] == pc24) live++;
        if (live >= AR_DISP_RECURSION_CAP) {
            static uint32 warned[16]; static int nwarned;
            int seen = 0;
            for (int i = 0; i < nwarned; i++) if (warned[i] == pc24) seen = 1;
            if (!seen) {
                if (nwarned < 16) warned[nwarned++] = pc24;
                extern int snes_frame_counter;
                fprintf(stderr, "[dispatch-recursion] target %06X live x%d — "
                        "unwinding instead of re-entering (f=%d src=%06X). A cfg "
                        "`func` at this pc is likely a MID-LOOP construct "
                        "continuation (B8C2 class) — remove it; see DEBUG.md §1 ⚠️.\n",
                        pc24, live, snes_frame_counter, source_pc24);
                fflush(stderr);
            }
            { extern int ar_trace_active(void);
              extern void ar_trace_dispmiss(uint32_t, uint32_t);
              if (ar_trace_active()) ar_trace_dispmiss(source_pc24, pc24); }
            cpu->S = entry_s_for_miss_restore;
            return RECOMP_RETURN_NORMAL;
        }
        cpu->host_return_valid = 0;
        if (s_disp_live_top < AR_DISP_LIVE_MAX) {
            s_disp_live[s_disp_live_top++] = pc24;
            RecompReturn _r = fp(cpu);
            s_disp_live_top--;
            return _r;
        }
        return fp(cpu);   /* tracking saturated — dispatch untracked */
    }
}

/* Trampoline driving loop. A dispatched frame that tail-dispatches a computed
 * jump returns RECOMP_RETURN_TAILCALL (target stashed in g_tailcall_*) instead
 * of recursively calling this function; we consume it here and iterate, so the
 * call chain runs flat. Real results (NORMAL / SKIP_N) pass straight through. */
extern uint32_t g_tailcall_pc24;
extern uint16_t g_tailcall_miss_s;
extern uint32_t g_tailcall_src24;
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24) {
    /* AR_RTSLOG=<hex source pc>: trace the RTS-dispatch chain from a specific
     * RTS site (e.g. AR_RTSLOG=0x039b59 for $03:9156's stack-relocating RTS-
     * trick). Logs each hop's target PC, the m/x flags, and S so we can see
     * exactly where the chain lands and where m diverges from hardware. */
    static long rtslog = -2;
    if (rtslog == -2) { const char *e = getenv("AR_RTSLOG");
        rtslog = e ? (long)strtoul(e, NULL, 0) : -1; }
    int trace = (rtslog >= 0 && source_pc24 == (uint32)rtslog);
    int hop = 0;
    for (;;) {
        if (trace) {
            extern int snes_frame_counter;
            fprintf(stderr, "[rtslog] from=%06X hop=%d -> dispatch pc=%06X  m=%u x=%u S=%04X entry_s=%04X f=%d\n",
                    source_pc24, hop++, pc24 & 0xFFFFFF, cpu->m_flag & 1, cpu->x_flag & 1,
                    cpu->S, entry_s_for_miss_restore, snes_frame_counter);
            fflush(stderr);
        }
        RecompReturn r = _cpu_dispatch_once(cpu, pc24,
                                            entry_s_for_miss_restore, source_pc24);
        if (r != RECOMP_RETURN_TAILCALL) {
            if (trace) fprintf(stderr, "[rtslog] from=%06X final r=%d m=%u S=%04X\n",
                               source_pc24, (int)r, cpu->m_flag & 1, cpu->S);
            return r;
        }
        pc24 = g_tailcall_pc24;
        entry_s_for_miss_restore = g_tailcall_miss_s;
        source_pc24 = g_tailcall_src24;
    }
}

RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                              uint16 entry_s_for_miss_restore) {
    return cpu_dispatch_pc_from(cpu, pc24, entry_s_for_miss_restore, 0xFFFFFFu);
}

/* Read-only probe: would a dispatch to pc24 find a function entry?
 * Mirrors cpu_dispatch_pc_from's lookup + LoROM bank-mirror fallback so
 * the RTS-decision trace can classify a popped PC as DISPATCH (entry
 * exists) vs MISS_UNWIND (host-return continuation) without side effects. */
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    pc24 &= 0xFFFFFFu;
    if (_cpu_dispatch_lookup(cpu, pc24) != NULL) return 1;
    uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0))
        if (_cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u) != NULL) return 1;
    return 0;
}

void cpu_state_init(CpuState *cpu, uint8 *ram) {
    cpu->A = 0;
    /* No cpu->B init — B is derived from (A >> 8) and has no separate state. */
    cpu->X = 0;
    cpu->Y = 0;
    cpu->S = 0x01FF;
    cpu->D = 0;
    cpu->DB = 0;
    cpu->PB = 0;
    /* Reset state per 65816 spec: emulation=1, M=X=I=1 (P=0x34). */
    cpu->P = CPU_P_M | CPU_P_X | CPU_P_I;
    cpu->m_flag = 1;
    cpu->x_flag = 1;
    cpu->emulation = 1;
    cpu->host_return_valid = 0;
    cpu->_flag_N = 0;
    cpu->_flag_V = 0;
    cpu->_flag_Z = 0;
    cpu->_flag_C = 0;
    cpu->_flag_I = 1;
    cpu->_flag_D = 0;
    cpu->ram = ram;
    /* NLR pending-skip is NOT on CpuState — it's a function-local in
     * each emitted v2 function. See cpu_state.h for design rationale. */
}
