
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include "snes.h"
#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "variables.h"
#include "../common_rtl.h"
#include "../common_cpu_infra.h"
#include "../debug_server.h"
#include "../audio_trace.h"

int snes_frame_counter;
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

uint8_t snes_readReg(Snes* snes, uint16_t adr);
void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val);

Snes* snes_init(uint8_t *ram) {
  Snes* snes = malloc(sizeof(Snes));
  snes->ram = ram;

  snes->cpu = cpu_init();
  snes->apu = apu_init();
  snes->dma = dma_init(snes);
  snes->ppu = ppu_init();
  snes->cart = cart_init(snes);
  snes->input1_currentState = 0;
  snes->input2_currentState = 0;
  return snes;
}

void snes_free(Snes* snes) {
  cpu_free(snes->cpu);
  apu_free(snes->apu);
  dma_free(snes->dma);
  ppu_free(snes->ppu);
  cart_free(snes->cart);
  free(snes);
}

void snes_saveload(Snes *snes, SaveLoadInfo *sli) {
  cpu_saveload(snes->cpu, sli);
  apu_saveload(snes->apu, sli);
  dma_saveload(snes->dma, sli);
  ppu_saveload(snes->ppu, sli);
  cart_saveload(snes->cart, sli);

  sli->func(sli, &snes->hPos, sizeof(*snes) - offsetof(Snes, hPos));
  sli->func(sli, snes->ram, 0x20000);
  sli->func(sli, &snes->ramAdr, 4);

  snes->cpu->e = 0;
}

void snes_reset(Snes* snes, bool hard) {
  cart_reset(snes->cart); // reset cart first, because resetting cpu will read from it (reset vector)
  cpu_reset(snes->cpu);
  apu_reset(snes->apu);
  dma_reset(snes->dma);
  ppu_reset(snes->ppu);
  if (hard)
    memset(snes->ram, 0, 0x20000);
  snes->ramAdr = 0;
  snes->hPos = 0;
  snes->vPos = 0;
  snes->apuCatchupCycles = 0.0;
  snes->hIrqEnabled = false;
  snes->vIrqEnabled = false;
  snes->nmiEnabled = false;
  snes->hTimer = 0x1ff;
  snes->vTimer = 0x1ff;
  snes->inNmi = false;
  snes->inIrq = false;
  snes->inVblank = false;
  snes->autoJoyRead = false;
  snes->autoJoyTimer = 0;
  snes->ppuLatch = false;
  snes->multiplyA = 0xff;
  snes->multiplyResult = 0xfe01;
  snes->divideA = 0xffff;
  snes->divideResult = 0x101;
}

static uint64_t s_catchup_calls = 0;
static uint64_t s_catchup_cycles_total = 0;
uint64_t g_apu_timer0_total_ticks = 0;

void snes_catchupApu(Snes* snes) {
  /* Upper cap is a guard against accumulator runaway after a long
   * stall; SPC runs at ~1 MHz so 10000 cycles is about 10 ms of real
   * SPC time per catchup, plenty to absorb any spike. */
  if (snes->apuCatchupCycles > 10000)
    snes->apuCatchupCycles = 10000;

  /* No artificial minimum. Earlier code floored to 1024 SPC cycles
   * per call, which was needed to brute-force progress while the
   * g_apu_autoack stub short-circuited polls. With autoack ripped,
   * the real SPC IPL handshake must run at hardware-realistic
   * timing: ~3.5 SNES-CPU cycles per SPC cycle, which works out to
   * ~73 SPC cycles per HW-reg touch (cpu_pace_cycles bumps 256 main
   * cycles per touch -> 256 * 2/7 is about 73). Flooring to 1024 made each
   * SMW upload byte take ~3000 SPC cycles instead of ~219, blowing
   * past the 5-second per-frame watchdog before the ~10 KB SPC
   * engine could finish uploading. The audio thread separately
   * cycles the SPC in bulk via RtlRenderAudio (534 samples is about 17 k
   * cycles per audio callback), so the SPC always gets enough
   * time even when the CPU is busy elsewhere. */
  int catchupCycles = (int) snes->apuCatchupCycles;
  if (catchupCycles < 0) catchupCycles = 0;

  /* AR_APUPROF frame attribution (see common_rtl.c). */
  extern int ApuProfEnabled(void);
  extern uint64_t audio_trace_wall_ns(void);
  extern uint64_t g_apuprof_catchup_ns, g_apuprof_catchup_cyc;
  extern uint32_t g_apuprof_catchup_calls;
  uint64_t prof_t0 = ApuProfEnabled() ? audio_trace_wall_ns() : 0;

  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  for(int i = 0; i < catchupCycles; i++) {
    apu_cycle(snes->apu);
  }
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
  if (prof_t0) {
    g_apuprof_catchup_ns += audio_trace_wall_ns() - prof_t0;
    g_apuprof_catchup_cyc += (uint64_t)catchupCycles;
    g_apuprof_catchup_calls++;
  }
  snes->apuCatchupCycles -= (double) catchupCycles;
  if (snes->apuCatchupCycles < 0.0) snes->apuCatchupCycles = 0.0;
  s_catchup_calls++;
  s_catchup_cycles_total += (uint64_t)catchupCycles;
}

void snes_catchup_stats(uint64_t *calls, uint64_t *cycles) {
  if (calls) *calls = s_catchup_calls;
  if (cycles) *cycles = s_catchup_cycles_total;
}

uint8_t snes_readBBus(Snes* snes, uint8_t adr) {
  if(adr < 0x40) {
    return ppu_read(g_ppu, adr);
  }
  if(adr < 0x80) {
    // APU port read ($2140-$217F). Catch the APU up based on the
    // main-CPU cycles elapsed since the last APU sync, and return
    // the live outPort value. RtlApuLock serialises us against the
    // audio thread's render loop, which advances the APU under the
    // same lock.
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(snes);
    { extern int ApuProfEnabled(void); extern uint32_t g_apuprof_port_reads;
      if (ApuProfEnabled()) g_apuprof_port_reads++; }
    /* AR_SPC_SPINFIX=1 (opt-in): resident-uploader deadlock breaker — the
     * boss-music-load fix. ActRaiser's sound engine has its own IPL-style
     * uploader resident in ARAM at $0F0E: it raises the $AABB "ready"
     * signature and spins at $0F12 (`MOV A,$F4; CMP #$CC; BNE`) waiting for
     * the CPU to send $CC and stream a block image. On hardware the CPU's
     * $02:9A56 drives that. We HLE $9A56 (it memcpys the image straight into
     * apu->ram — running it natively over-clocks the SPC and breaks boot), so
     * the $CC stream is NEVER sent and the engine sits in its uploader forever.
     * A later boss command ($00:A410 sends $F1 and spins for the echo) then
     * deadlocks: the SPC is stuck in the uploader (out=$AABB) and can't answer
     * -> 5 s watchdog -> SIGSEGV. Since the data is already in apu->ram, just
     * finalize the engine's uploader for it: when we catch it parked in the
     * $0F12 $CC-wait with the $AABB signature on a poll, jump the SPC to the
     * uploader's tail at $0F48 (`MOV X,#$31; MOV $F1,X; RET` — clears the
     * ports + enables timer0, then returns to the engine main loop), exactly
     * as a completed transfer would. The engine then processes the pending
     * command normally. Threshold keeps it from firing on a momentary pass
     * through the wait. */
    {
      /* Read-path backstop for the resident-uploader deadlock. The PRIMARY fix
       * is in the HLE upload path (common_rtl.c: completes the engine's
       * resident uploader to $0F48 at upload time), which prevents the stuck
       * state entirely — so this never fires in practice. Kept opt-in
       * (AR_SPC_SPINFIX=1) as a safety net for any path that enters the
       * uploader without a following HLE upload. Forces $0F4C (bare RET, no
       * port-clear) since it fires LATE, with a play command already pending. */
      static int s_spinfix = -1;
      if (s_spinfix < 0) s_spinfix = getenv("AR_SPC_SPINFIX") ? 1 : 0;  /* default OFF; HLE path is primary */
      if (s_spinfix) {
        Spc *s = snes->apu->spc;
        static uint32_t stuck;
        /* Detect by PC alone: the $0F12..$0F18 loop is the resident uploader's
         * $CC-wait, and since $9A56 is HLE'd the $CC stream is never sent, so any
         * sustained time here is the deadlock (a real transfer would stream and
         * leave immediately). Jump to $0F4C — the bare RET, SKIPPING the
         * $0F48 `MOV $F1,#$31` which would clear the input ports and wipe the
         * pending command ($F1) the CPU is waiting to have echoed. The engine
         * then returns to its main loop, sees $F1 in inPort0, and echoes it. */
        if (s->pc >= 0x0F12 && s->pc <= 0x0F18) {
          if (++stuck > 2000u) {
            s->pc = 0x0F4C;   /* bare RET, preserve pending input ports */
            stuck = 0;
            if (getenv("AR_APULOG")) {
              extern int snes_frame_counter;
              fprintf(stderr, "[apu] f=%d SPINFIX: forced SPC out of resident uploader -> $0F4C RET (in=%02x%02x out=%02x%02x sp=%02x)\n",
                      snes_frame_counter, snes->apu->inPorts[0], snes->apu->inPorts[1],
                      snes->apu->outPorts[0], snes->apu->outPorts[1], s->sp);
            }
          }
        } else {
          stuck = 0;
        }
      }
    }
    uint8_t v = snes->apu->outPorts[adr & 0x3];
    audio_trace_on_cpu_port_read((uint8_t)(adr & 0x3), v);
    RtlApuUnlock();
    if (getenv("AR_APULOG") && (adr & 0xfc) == 0x40) {  /* $2140-$2143 */
      extern int snes_frame_counter;
      static uint8_t lastv[4] = {0xff,0xff,0xff,0xff};
      if (v != lastv[adr & 3]) {  /* only on change to cut spin spam */
        lastv[adr & 3] = v;
        fprintf(stderr, "[apu] f=%d READ  $21%02x -> %02x\n", snes_frame_counter, (unsigned)(adr & 0xff), v);
      }
      /* During a spin (same value read repeatedly) periodically dump SPC
       * engine state so we can see if the SPC is alive/advancing. */
      if ((adr & 3) == 0) {
        static uint64_t rd_count;
        if ((rd_count++ % 200000) == 0) {
          uint16_t pc = snes->apu->spc->pc;
          uint8_t *r = snes->apu->ram;
          extern uint64_t snes_apu_cycle_count(void);
          fprintf(stderr, "[apu] f=%d SPIN $2140=%02x spc.pc=%04x sp=%02x a=%02x cyc=%d apucyc=%llu code@pc=%02x %02x %02x %02x in=%02x%02x%02x%02x out=%02x%02x%02x%02x romRd=%d\n",
                  snes_frame_counter, v, pc, snes->apu->spc->sp, snes->apu->spc->a,
                  (int)snes->apuCatchupCycles, (unsigned long long)snes_apu_cycle_count(),
                  r[pc], r[(uint16_t)(pc+1)], r[(uint16_t)(pc+2)], r[(uint16_t)(pc+3)],
                  snes->apu->inPorts[0], snes->apu->inPorts[1], snes->apu->inPorts[2], snes->apu->inPorts[3],
                  snes->apu->outPorts[0], snes->apu->outPorts[1], snes->apu->outPorts[2], snes->apu->outPorts[3],
                  (int)snes->apu->romReadable);
        }
      }
      /* AR_SPCDUMP: one-shot dump of the SPC ARAM + regs the first time we
       * see the resident-uploader deadlock (SPC parked at $0Fxx with the
       * $AABB ready signature on the out ports while the CPU spins on a
       * port). Lets us decode the engine's command dispatch + resident
       * uploader offline to build the HLE completion. */
      if (getenv("AR_SPCDUMP")) {
        uint16_t pc = snes->apu->spc->pc;
        if (pc >= 0x0F12 && pc <= 0x0F18) {
          static int done;
          if (!done) {
            done = 1;
            FILE *f = fopen("/tmp/aram.bin", "wb");
            if (f) { fwrite(snes->apu->ram, 1, 0x10000, f); fclose(f); }
            Spc *s = snes->apu->spc;
            fprintf(stderr, "[spcdump] aram->/tmp/aram.bin  pc=%04x sp=%02x a=%02x x=%02x y=%02x p=%d in=%02x%02x%02x%02x out=%02x%02x%02x%02x\n",
                    s->pc, s->sp, s->a, s->x, s->y, (int)s->p,
                    snes->apu->inPorts[0], snes->apu->inPorts[1], snes->apu->inPorts[2], snes->apu->inPorts[3],
                    snes->apu->outPorts[0], snes->apu->outPorts[1], snes->apu->outPorts[2], snes->apu->outPorts[3]);
            fflush(stderr);
          }
        }
      }
    }
    return v;
  }
  if(adr == 0x80) {
    uint8_t ret = snes->ram[snes->ramAdr++];
    snes->ramAdr &= 0x1ffff;
    return ret;
  }

  /* Out-of-range B-bus read. v2 boot path occasionally fires DMA
   * with a misconfigured channel (consequence of an upstream bad
   * ROM read returning garbage that configures the DMA setup). On
   * release builds we silently return 0 instead of crashing so the
   * boot path can keep progressing — the upstream issue is what
   * actually needs fixing. */
  return 0;
}

void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val) {
  if(adr < 0x40) {
    if (adr == 0x00 && getenv("AR_INIDISP")) {  /* $2100 INIDISP */
      extern int snes_frame_counter; extern uint8 g_ram[0x20000];
      extern const char *g_last_recomp_func;
      static uint8_t lastv = 0xfe;
      if (val != lastv) { lastv = val;
        fprintf(stderr, "[inidisp] f=%d $2100<-%02x (bright=%d fblank=%d) $18=%02x by=%s\n",
          snes_frame_counter, val, val & 0xf, (val & 0x80) ? 1 : 0, g_ram[0x18],
          g_last_recomp_func ? g_last_recomp_func : "?");
      }
    }
    ppu_write(g_ppu, adr, val);
    return;
  }
  if(adr < 0x80) {
    if (getenv("AR_APULOG") && (adr & 0xfc) == 0x40) {  /* $2140-$2143 */
      extern int snes_frame_counter;
      fprintf(stderr, "[apu] f=%d WRITE $21%02x <- %02x\n", snes_frame_counter, adr, val);
    }
    RtlApuWrite(0x2100 + adr, val);
    return;
  }
  switch(adr) {
    case 0x80: {
#if SNESRECOMP_REVERSE_DEBUG
      { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        uint32_t wa = snes->ramAdr & 0x1ffffu;
        uint8_t old = snes->ram[wa];
        snes->ram[wa] = val;
        debug_on_wram_write_byte(wa, old, val);
        snes->ramAdr = (wa + 1u) & 0x1ffffu; }
#else
      snes->ram[snes->ramAdr++] = val;
      snes->ramAdr &= 0x1ffff;
#endif
      break;
    }
    case 0x81: {
      snes->ramAdr = (snes->ramAdr & 0x1ff00) | val;
      break;
    }
    case 0x82: {
      snes->ramAdr = (snes->ramAdr & 0x100ff) | (val << 8);
      break;
    }
    case 0x83: {
      snes->ramAdr = (snes->ramAdr & 0x0ffff) | ((val & 1) << 16);
      break;
    }
  }
}

uint16_t SwapInputBits(uint16_t x) {
  uint16_t r = 0;
  for (int i = 0; i < 16; i++, x >>= 1)
    r = r * 2 + (x & 1);
  return r;
}

uint8_t snes_readReg(Snes* snes, uint16_t adr) {
  switch(adr) {
    case 0x4210: {
      if (g_rtl_game_info && g_rtl_game_info->read_rdnmi) {
        int override = g_rtl_game_info->read_rdnmi(snes);
        if (override >= 0)
          return (uint8_t)override;
      }
      uint8_t val = 0x2; // CPU version (4 bit)
      bool nmi = snes->inNmi || snes->forceNmi;
      val |= nmi << 7;
      snes->inNmi = false;
      return val;
    }
    case 0x4211: {
      uint8_t val = snes->inIrq << 7;
      snes->inIrq = false;
      return val;
    }
    case 0x4212: {
      // Static-recomp h-counter model: real hardware updates hPos every
      // dot-clock; recomp has no dot-clock, so each $4212 read advances
      // hPos by a fixed step. Calibrated so a typical busy-wait crosses
      // both edges in ~10-20 reads. Bit 6 = hblank (dots ~1024..1364 of
      // a 1364-dot scanline). See docs/VIRTUAL_HW_CONTRACT.md.
      snes->hPos = (snes->hPos + 64) % 1364;
      uint8_t val = (snes->autoJoyTimer > 0);
      val |= (snes->hPos >= 1024) << 6;
      val |= snes->inVblank << 7;
      return val;
    }
    case 0x4213:
      return snes->ppuLatch << 7; // IO-port
    case 0x4214:
      return snes->divideResult & 0xff;
    case 0x4215:
      return snes->divideResult >> 8;
    case 0x4216:
      return snes->multiplyResult & 0xff;
    case 0x4217:
      return snes->multiplyResult >> 8;
    case 0x4016:  /* JOYSER0 — manual joypad read for controller 1. */
    case 0x4017:  /* JOYSER1 — manual joypad read for controller 2. */
      /* On real SNES, $4016/$4017 are the manual joypad-read serial
       * shift registers. After a strobe write to $4016 (latch), 16
       * sequential reads shift out the controller's 16-bit button
       * state (LSB-first). After 16 reads, subsequent reads return
       * bit 0 = 1 as the "controller present" signature for a
       * standard pad. snes9x's S9xReadJOYSERn (controls.cpp:2917)
       * implements this: in the no-latch state with read_idx>=16
       * it returns `bits | 1`.
       *
       * Recomp's emulation core didn't handle these registers at
       * all — the reads fell through to the default `return 0` path,
       * which made SMW's CheckWhichControllersArePluggedIn at $00:9A74
       * conclude "no controllers connected" and write $0DA0 = 0x00.
       * That single byte then cascaded into ~250 downstream WRAM
       * divergences over the attract demo, contributing to the
       * koopa-stomp visible bug (Mario contacts the koopa from a
       * different angle, dies instead of stomping).
       *
       * For correctness without full strobe-latch tracking, return
       * 0x01 unconditionally — same effect as snes9x's post-latch
       * read past 16 bits with a standard pad attached. */
      if (getenv("AR_JOYLOG") && snes->input1_currentState) { static int n; if(n++<8) fprintf(stderr,"[joy] read $%04X (manual) in1=%04X\n", adr, snes->input1_currentState); }
      return 0x01;
    case 0x4218:
      if (getenv("AR_JOYLOG") && snes->input1_currentState) { static int n; if(n++<8) fprintf(stderr,"[joy] read $4218 -> %02X (in1=%04X)\n", SwapInputBits(snes->input1_currentState)&0xff, snes->input1_currentState); }
      return SwapInputBits(snes->input1_currentState) & 0xff;
    case 0x4219:
      if (getenv("AR_JOYLOG") && snes->input1_currentState) { static int n; if(n++<8) fprintf(stderr,"[joy] read $4219 -> %02X (in1=%04X)\n", SwapInputBits(snes->input1_currentState)>>8, snes->input1_currentState); }
      return SwapInputBits(snes->input1_currentState) >> 8;
    case 0x421a:
      return SwapInputBits(snes->input2_currentState) & 0xff;
    case 0x421b:
      return SwapInputBits(snes->input2_currentState) >> 8;
    case 0x421c:
    case 0x421e:
    case 0x421d:
    case 0x421f:
      return 0;

    default: {
      return 0;
    }
  }
}

void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val) {
  switch(adr) {
    case 0x4200: {
      snes->autoJoyRead = val & 0x1;
      if(!snes->autoJoyRead) snes->autoJoyTimer = 0;
      snes->hIrqEnabled = val & 0x10;
      snes->vIrqEnabled = val & 0x20;
      snes->nmiEnabled = val & 0x80;
      if(!snes->hIrqEnabled && !snes->vIrqEnabled) {
        snes->inIrq = false;
      }
      // TODO: enabling nmi during vblank with inNmi still set generates nmi
      //   enabling virq (and not h) on the vPos that vTimer is at generates irq (?)
      break;
    }
    case 0x4201: {
      if(!(val & 0x80) && snes->ppuLatch) {
        // latch the ppu
        ppu_read(g_ppu, 0x37);
      }
      snes->ppuLatch = val & 0x80;
      break;
    }
    case 0x4202: {
      snes->multiplyA = val;
      break;
    }
    case 0x4203: {
      snes->multiplyResult = snes->multiplyA * val;
      break;
    }
    case 0x4204: {
      snes->divideA = (snes->divideA & 0xff00) | val;
      break;
    }
    case 0x4205: {
      snes->divideA = (snes->divideA & 0x00ff) | (val << 8);
      break;
    }
    case 0x4206: {
      if(val == 0) {
        snes->divideResult = 0xffff;
        snes->multiplyResult = snes->divideA;
      } else {
        snes->divideResult = snes->divideA / val;
        snes->multiplyResult = snes->divideA % val;
      }
      break;
    }
    case 0x4207: {
      snes->hTimer = (snes->hTimer & 0x100) | val;
      break;
    }
    case 0x4208: {
      snes->hTimer = (snes->hTimer & 0x0ff) | ((val & 1) << 8);
      break;
    }
    case 0x4209: {
      snes->vTimer = (snes->vTimer & 0x100) | val;
      break;
    }
    case 0x420a: {
      snes->vTimer = (snes->vTimer & 0x0ff) | ((val & 1) << 8);
      break;
    }
    case 0x420b: {
      dma_startDma(snes->dma, val, false);
      while (dma_cycle(snes->dma)) {}
      break;
    }
    case 0x420c: {
      dma_startDma(snes->dma, val, true);
      break;
    }
    default: {
      break;
    }
  }
}

uint8_t snes_read(Snes* snes, uint32_t adr) {
  uint8_t bank = adr >> 16;
  adr &= 0xffff;
  if(bank == 0x7e || bank == 0x7f) {
    return snes->ram[((bank & 1) << 16) | adr]; // ram
  }
  if(bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) {
    if(adr < 0x2000) {
      return snes->ram[adr]; // ram mirror
    }
    if(adr >= 0x2100 && adr < 0x2200) {
      uint8_t v = snes_readBBus(snes, adr & 0xff); // B-bus
      /* AR_TRACE hwread: APU handshake ports ($2140-$2143) that SPC-upload
       * loops spin on — the value the branch keyed on. */
      if (adr >= 0x2140 && adr <= 0x2143) {
        extern int ar_trace_active(void); extern void ar_trace_hwread(uint16_t, uint8_t);
        if (ar_trace_active()) ar_trace_hwread(adr, v);
      }
      return v;
    }
    if (adr == 0x4016 || adr == 0x4017) {
      // joypad read disabled
      { extern int ar_trace_active(void); extern void ar_trace_hwread(uint16_t, uint8_t);
        if (ar_trace_active()) ar_trace_hwread(adr, 0); }
      return 0;
    }
    if(adr >= 0x4200 && adr < 0x4220 || adr >= 0x4218 && adr < 0x4220) {
      uint8_t v = snes_readReg(snes, adr); // internal registers
      /* vblank/NMI/IRQ status ($4210 RDNMI, $4212 HVBJOY) + auto-joypad
       * ($4218-$421F) — the reads that gate vblank-wait spins and input. */
      if (adr == 0x4210 || adr == 0x4212 || (adr >= 0x4218 && adr < 0x4220)) {
        extern int ar_trace_active(void); extern void ar_trace_hwread(uint16_t, uint8_t);
        if (ar_trace_active()) ar_trace_hwread(adr, v);
      }
      return v;
    }
    if(adr >= 0x4300 && adr < 0x4380) {
      return dma_read(snes->dma, adr); // dma registers
    }
  }
  // read from cart
  return cart_read(snes->cart, bank, adr);
}

/* AR_WATCHOBJ/AR_WATCH0019 for DMA-driven writes (2026-07-01). dma.c's
 * dma_transferByte -> snes_write writes STRAIGHT into snes->ram (which
 * IS g_ram -- see snes_init(g_ram) in common_cpu_infra.c) with zero
 * instrumentation, bypassing cpu_write8/cpu_write16/IndirWriteByte/
 * IndirWriteWord entirely. Found chasing a sim-mode freeze: $0019 read
 * 0xA1 mid-frame with NO write ever logged by any of those four
 * instrumented paths -- DMA is the only write mechanism left that
 * writes WRAM without going through them. Mirrors cpu_write8's watch
 * bodies (unconditional for AR_WATCH0019, on-change for AR_WATCHOBJ). */
static inline void snes_write_watch(uint32_t off, uint8_t old_val, uint8_t val) {
  /* AR_TRACE wram channel — DMA-driven WRAM writes bypass the cpu_write and
   * IndirWrite paths, so hook them here too (completes "who wrote this WRAM"). */
  { extern int ar_trace_active(void); extern void ar_trace_wram(uint32_t, uint16_t, uint16_t, int);
    if (ar_trace_active()) ar_trace_wram(off, old_val, val, 1); }
  if (off == 0x19 && getenv("AR_WATCH0019")) {
    static int n;
    if (n++ < 200) {
      extern int snes_frame_counter; extern const char *g_last_recomp_func;
      fprintf(stderr, "[watch0019-dma] $0019=%02x (was %02x) f=%d cur=%s\n",
              val, old_val, snes_frame_counter,
              g_last_recomp_func ? g_last_recomp_func : "?");
    }
  }
  if (getenv("AR_WATCHOBJ")) {
    static long wo = -2;
    if (wo == -2) { const char *e = getenv("AR_WATCHOBJ"); wo = e ? (long)strtoul(e, NULL, 16) : -1; }
    if (wo >= 0 && off >= (uint32_t)wo && off < (uint32_t)wo + 0x40 && old_val != val) {
      extern int snes_frame_counter; extern const char *g_last_recomp_func;
      fprintf(stderr, "[wobj-dma] $%04x=%02x (was %02x) f=%d cur=%s\n",
              off, val, old_val, snes_frame_counter,
              g_last_recomp_func ? g_last_recomp_func : "?");
    }
  }
}

void snes_write(Snes* snes, uint32_t adr, uint8_t val) {
  uint8_t bank = adr >> 16;
  adr &= 0xffff;
  if(bank == 0x7e || bank == 0x7f) {
    uint32_t addr = ((bank & 1) << 16) | adr;
#if SNESRECOMP_REVERSE_DEBUG
    { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
      uint8_t old = snes->ram[addr];
      snes->ram[addr] = val;
      debug_on_wram_write_byte(addr, old, val); }
#else
    { uint8_t old = snes->ram[addr];
      snes->ram[addr] = val; // ram
      snes_write_watch(addr, old, val); }
#endif
  }
  if(bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) {
    if(adr < 0x2000) {
#if SNESRECOMP_REVERSE_DEBUG
      { extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        uint8_t old = snes->ram[adr];
        snes->ram[adr] = val;
        debug_on_wram_write_byte((uint32_t)adr, old, val); }
#else
      { uint8_t old = snes->ram[adr];
        snes->ram[adr] = val; // ram mirror
        snes_write_watch(adr, old, val); }
#endif
    }
    if(adr >= 0x2100 && adr < 0x2200) {
      snes_writeBBus(snes, adr & 0xff, val); // B-bus
    }
    if(adr >= 0x4200 && adr < 0x4220) {
      snes_writeReg(snes, adr, val); // internal registers
    }
    if(adr >= 0x4300 && adr < 0x4380) {
      dma_write(snes->dma, adr, val); // dma registers
    }
    if(adr >= 0x2100 && adr < 0x4400) {
      debug_server_on_reg_write(adr, val);
    }
  }
  // write to cart
  cart_write(snes->cart, bank, adr, val);
}
