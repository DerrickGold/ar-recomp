/* ar_trace — unified single-run runtime trace.
 *
 * One env-gated JSONL stream that correlates EVERY layer in ONE run, so
 * debugging no longer needs a dozen small targeted probes that each see only
 * one layer (and miss paths like the atomic WriteVramWord $2118 write that
 * bypasses the $2118-byte handlers).
 *
 * Every event carries a monotonic `seq` (global order — survives the fact that
 * the game-frame $0088 clock is NON-MONOTONIC near cutscenes), the host frame
 * `hf`, the game frame `gf`, the current recomp function + last block PC, and a
 * per-channel payload.
 *
 * Enable:  AR_TRACE=<path.jsonl>
 * Window:  AR_TRACE_HF_LO / AR_TRACE_HF_HI   (host-frame range; default: all —
 *          set a window, the full run is huge)
 * Channels:AR_TRACE_CH=func,vram,vmadd,reg,dma,mx   (default: all)
 * VRAM:    AR_TRACE_VLO / AR_TRACE_VHI        (word-addr filter for `vram`)
 * Func:    AR_TRACE_FUNC=<substr>             (only funcs whose name contains it)
 *
 * Slice/pivot the output locally with tools/trace_slice.py.
 */
#ifndef AR_TRACE_H
#define AR_TRACE_H

#include <stdint.h>

/* Channel bits (also the AR_TRACE_CH names). */
#define AR_TR_FUNC  0x01
#define AR_TR_VRAM  0x02
#define AR_TR_VMADD 0x04
#define AR_TR_REG   0x08
#define AR_TR_DMA   0x10
#define AR_TR_MX    0x20
#define AR_TR_CALL     0x40
#define AR_TR_DISPMISS 0x80    /* computed-dispatch miss (RTS-trick root event) */
#define AR_TR_GARBAGE  0x100   /* entered a known-misdecode "garbage" variant */
#define AR_TR_WRAM     0x200   /* WRAM writes (range-gated AR_TRACE_WLO/WHI) */
#define AR_TR_STACK    0x400   /* stack-page ($01xx) pushes — provenance */
#define AR_TR_HWREAD   0x800   /* control-flow hardware reads ($4210/joypad/APU) */
#define AR_TR_PPUMEM   0x1000  /* CGRAM ($2122) + OAM ($2104) writes */
#define AR_TR_FRAME    0x2000  /* NMI / vblank-yield frame markers */

/* Fast gate: is tracing enabled AND currently inside the host-frame window?
 * Cheap enough to call on hot paths (single compare after one-time init). */
int  ar_trace_active(void);
/* True if a specific channel is selected (call after ar_trace_active). */
int  ar_trace_ch(int chbit);

/* Choke-point emitters. All are no-ops when tracing is off / out of window. */
/* Function entry. m/x = runtime; em/ex = the dispatched variant's flags — this
 * matches by construction (the call switch picks the variant by runtime m/x),
 * so `misdecode` here only catches IRQ/NMI-style entries, NOT a self-consistent
 * m-leak. For that, use ar_trace_call (below). */
void ar_trace_func(uint32_t pc24, const char *name, int m, int x, int em, int ex);

/* CALL SITE (from ar_call_mx_check, in every JSR/JSL prologue). em/ex = the
 * DECODER'S STATIC expectation at this site; m/x = runtime. A mismatch means m/x
 * leaked between the enclosing fn's entry and this call — THE signal that finds
 * an ambiguous-exit m-leak (the lair-seal `$9D4D` case). This is `AR_CALLMX`
 * folded into the one-run trace. */
void ar_trace_call(uint32_t pc24, const char *name, int m, int x, int em, int ex);
void ar_trace_vram(uint16_t vaddr, uint16_t val, const char *path); /* path: b18/b19/word */
void ar_trace_vmadd(uint16_t vmadd, const char *how);
void ar_trace_reg(uint16_t reg, uint8_t val);
void ar_trace_dma(int chan, uint8_t bAdr, uint8_t aBank, uint16_t aAdr,
                  uint32_t size, int fromB);
/* Computed-dispatch miss (an RTS-trick/return-target with no registered entry —
 * the #1 root event for unregistered-handler bugs). */
void ar_trace_dispmiss(uint32_t from_pc, uint32_t to_pc);
/* Execution entered a known-misdecode "garbage" variant (split-immediate BRK). */
void ar_trace_garbage(uint32_t pc24, const char *name, int m, int x);
/* WRAM write (g_ram offset). Range-gated by AR_TRACE_WLO/WHI. Writes into the
 * stack page ($0100-$01FF) also emit a `stack` push event (value + who). */
void ar_trace_wram(uint32_t off, uint16_t old, uint16_t val, int width);
/* Control-flow-gating hardware register read ($4210/$4212/$4016-7/$4218/$2140-3).
 * The value a spin/branch keyed on — answers "why did the loop exit / stall". */
void ar_trace_hwread(uint16_t reg, uint8_t val);
/* PPU-memory write outside VRAM: CGRAM (palette) / OAM (sprites). */
void ar_trace_ppumem(const char *mem, uint16_t addr, uint16_t val);
/* Frame boundary marker (NMI fire / vblank yield). `what` = "nmi" | "vblank". */
void ar_trace_frame(const char *what);
/* Force-dump the AR_TRACE_WATCH ring now (called by the watchdog on a hang). */
void ar_trace_flush(const char *reason);

#endif /* AR_TRACE_H */
