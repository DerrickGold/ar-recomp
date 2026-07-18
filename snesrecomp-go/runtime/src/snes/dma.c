
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#include "dma.h"
#include "snes.h"
#include "../ar_trace.h"

static const int bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};

static const int transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};

static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB);

Dma* dma_init(Snes* snes) {
  Dma* dma = malloc(sizeof(Dma));
  dma->snes = snes;
  return dma;
}

void dma_free(Dma* dma) {
  free(dma);
}

void dma_reset(Dma* dma) {
  for(int i = 0; i < 8; i++) {
    dma->channel[i].bAdr = 0xff;
    dma->channel[i].aAdr = 0xffff;
    dma->channel[i].aBank = 0xff;
    dma->channel[i].size = 0xffff;
    dma->channel[i].indBank = 0xff;
    dma->channel[i].tableAdr = 0xffff;
    dma->channel[i].repCount = 0xff;
    dma->channel[i].unusedByte = 0xff;
    dma->channel[i].dmaActive = false;
    dma->channel[i].hdmaActive = false;
    dma->channel[i].mode = 7;
    dma->channel[i].fixed = true;
    dma->channel[i].decrement = true;
    dma->channel[i].indirect = true;
    dma->channel[i].fromB = true;
    dma->channel[i].unusedBit = true;
    dma->channel[i].doTransfer = false;
    dma->channel[i].terminated = false;
    dma->channel[i].offIndex = 0;
  }
  dma->dmaTimer = 0;
  dma->dmaBusy = false;
}

void dma_saveload(Dma *dma, SaveLoadInfo *sli) {
  sli->func(sli, &dma->channel, sizeof(*dma) - offsetof(Dma, channel));
}

uint8_t dma_read(Dma* dma, uint16_t adr) {
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      uint8_t val = dma->channel[c].mode;
      val |= dma->channel[c].fixed << 3;
      val |= dma->channel[c].decrement << 4;
      val |= dma->channel[c].unusedBit << 5;
      val |= dma->channel[c].indirect << 6;
      val |= dma->channel[c].fromB << 7;
      return val;
    }
    case 0x1: {
      return dma->channel[c].bAdr;
    }
    case 0x2: {
      return dma->channel[c].aAdr & 0xff;
    }
    case 0x3: {
      return dma->channel[c].aAdr >> 8;
    }
    case 0x4: {
      return dma->channel[c].aBank;
    }
    case 0x5: {
      return dma->channel[c].size & 0xff;
    }
    case 0x6: {
      return dma->channel[c].size >> 8;
    }
    case 0x7: {
      return dma->channel[c].indBank;
    }
    case 0x8: {
      return dma->channel[c].tableAdr & 0xff;
    }
    case 0x9: {
      return dma->channel[c].tableAdr >> 8;
    }
    case 0xa: {
      return dma->channel[c].repCount;
    }
    case 0xb:
    case 0xf: {
      return dma->channel[c].unusedByte;
    }
    default: {
      /* Soft for v2 boot: data-as-code reads occasionally hit invalid
       * DMA register offsets (e.g. \$430C/\$430E that don't exist).
       * Real fix is upstream — for now return 0 so boot continues. */
      return 0;
    }
  }
}

void dma_write(Dma* dma, uint16_t adr, uint8_t val) {
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      dma->channel[c].mode = val & 0x7;
      dma->channel[c].fixed = val & 0x8;
      dma->channel[c].decrement = val & 0x10;
      dma->channel[c].unusedBit = val & 0x20;
      dma->channel[c].indirect = val & 0x40;
      dma->channel[c].fromB = val & 0x80;
      break;
    }
    case 0x1: {
      dma->channel[c].bAdr = val;
      break;
    }
    case 0x2: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff00) | val;
      break;
    }
    case 0x3: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff) | (val << 8);
      break;
    }
    case 0x4: {
      dma->channel[c].aBank = val;
      break;
    }
    case 0x5: {
      dma->channel[c].size = (dma->channel[c].size & 0xff00) | val;
      break;
    }
    case 0x6: {
      dma->channel[c].size = (dma->channel[c].size & 0xff) | (val << 8);
      break;
    }
    case 0x7: {
      dma->channel[c].indBank = val;
      break;
    }
    case 0x8: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff00) | val;
      break;
    }
    case 0x9: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff) | (val << 8);
      break;
    }
    case 0xa: {
      dma->channel[c].repCount = val;
      break;
    }
    case 0xb:
    case 0xf: {
      dma->channel[c].unusedByte = val;
      break;
    }
    default: {
      break;
    }
  }
}

extern bool g_fail;

void dma_doDma(Dma* dma) {
  if(dma->dmaTimer > 0) {
    dma->dmaTimer -= 2;
    return;
  }
  // figure out first channel that is active
  int i = 0;
  for(i = 0; i < 8; i++) {
    if(dma->channel[i].dmaActive) {
      break;
    }
  }
  if(i == 8) {
    // no active channels
    dma->dmaBusy = false;
    return;
  }

  if (!dma->channel[i].fromB && (dma->channel[i].aBank & 0x80) && !(dma->channel[i].aAdr & 0x8000) && !g_fail) {
    printf("Warning! DMA from addr 0x%x\n", dma->channel[i].aBank << 16 | dma->channel[i].aAdr);
    g_fail = true;
  }

  {
    static int dbg_dma_logged;
    if (!dbg_dma_logged && !dma->channel[i].fromB
        && dma->channel[i].aBank == 0x00
        && dma->channel[i].aAdr >= 0x2100 && dma->channel[i].aAdr < 0x2200) {
      dbg_dma_logged = 1;
      extern const char *g_last_recomp_func;
      fprintf(stderr, "[dma] BAD SRC chan=%d aBank=%02X aAdr=%04X bAdr=%02X "
              "mode=%d size=%u fixed=%d dec=%d fromB=%d (last recomp: %s)\n",
              i, dma->channel[i].aBank, dma->channel[i].aAdr,
              dma->channel[i].bAdr, dma->channel[i].mode,
              (unsigned)dma->channel[i].size, dma->channel[i].fixed,
              dma->channel[i].decrement, dma->channel[i].fromB,
              g_last_recomp_func ? g_last_recomp_func : "?");
    }
  }
  /* AR_LAIRDMA=1: log the START of each VRAM DMA (bAdr $18/$19 = VMDATA) with
   * its source (aBank:aAdr) + dest + size — to find the garbage tilemap-source
   * buffer behind the lair-seal top-strip corruption. Fires once per burst
   * (offIndex==0 && full size). Gated on game-frame window AR_VW_LO/HI. */
  {
    static int ld = -1; static unsigned lo, hi;
    if (ld < 0) { ld = getenv("AR_LAIRDMA") ? atoi(getenv("AR_LAIRDMA")) : 0;
      const char *l = getenv("AR_VW_LO"), *h = getenv("AR_VW_HI");
      lo = l ? (unsigned)strtoul(l, NULL, 0) : 0;
      hi = h ? (unsigned)strtoul(h, NULL, 0) : 0xffffffffu; }
    extern Ppu *g_ppu;
    if (ld && !dma->channel[i].fromB && dma->channel[i].offIndex == 0
        && (dma->channel[i].bAdr == 0x18 || dma->channel[i].bAdr == 0x19)
        && (ld >= 2 || g_ppu->vramPointer < 0x1000) /* ld>=2: all VRAM dests */
        && dma->channel[i].size >= dma->channel[i].size /* burst-start proxy */) {
      extern uint8 g_ram[0x20000];
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      /* one line per burst: only when vramPointer is at the burst start (a
       * fresh $2116 set) — approximate by logging the first offIndex==0 whose
       * dest we haven't seen this frame */
      static unsigned last_gf2 = 0xffffffff, seen_dst;
      int fresh = 1;
      if (gf != last_gf2) { last_gf2 = gf; seen_dst = 0xffffffff; }
      if (g_ppu->vramPointer == seen_dst) fresh = 0;
      seen_dst = g_ppu->vramPointer;
      if (fresh && gf >= lo && gf <= hi) {
        static int nl; if (nl < 4000) { nl++;
          extern Ppu *g_ppu; extern const char *g_last_recomp_func;
          unsigned ab = dma->channel[i].aBank, aa = dma->channel[i].aAdr;
          char sb[40] = "(non-WRAM)";
          if (ab == 0x7E || ab == 0x7F) { unsigned b = ((ab & 1) << 16) | aa;
            snprintf(sb, sizeof sb, "%02x %02x %02x %02x %02x %02x",
              g_ram[b], g_ram[(b+1)&0x1ffff], g_ram[(b+2)&0x1ffff],
              g_ram[(b+3)&0x1ffff], g_ram[(b+4)&0x1ffff], g_ram[(b+5)&0x1ffff]); }
          fprintf(stderr, "[lairdma] gf=%u src=$%02x:%04x vram=$%04x size=%u "
            "src[0..5]=%s func=%s\n", gf, ab, aa, g_ppu->vramPointer,
            (unsigned)dma->channel[i].size, sb,
            g_last_recomp_func ? g_last_recomp_func : "?");
        }
      }
    }
  }
  // do channel i
  dma_transferByte(
    dma, dma->channel[i].aAdr, dma->channel[i].aBank,
    dma->channel[i].bAdr + bAdrOffsets[dma->channel[i].mode][dma->channel[i].offIndex++], dma->channel[i].fromB
  );
  dma->channel[i].offIndex &= 3;
  dma->dmaTimer += 6; // 8 cycles for each byte taken, -2 for this cycle
  if(!dma->channel[i].fixed) {
    dma->channel[i].aAdr += dma->channel[i].decrement ? -1 : 1;
  }
  dma->channel[i].size--;
  if(dma->channel[i].size == 0) {
    dma->channel[i].offIndex = 0; // reset offset index
    dma->channel[i].dmaActive = false;
    dma->dmaTimer += 8; // 8 cycle overhead per channel
  }
}

static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB) {
  // TODO: invalid writes:
  //   accesing b-bus via a-bus gives open bus,
  //   $2180-$2183 while accessing ram via a-bus open busses $2180-$2183
  //   cannot access $4300-$437f (dma regs), or $420b / $420c
  if(fromB) {
    snes_write(dma->snes, (aBank << 16) | aAdr, snes_readBBus(dma->snes, bAdr));
  } else {
    snes_writeBBus(dma->snes, bAdr, snes_read(dma->snes, (aBank << 16) | aAdr));
  }
}

bool dma_cycle(Dma* dma) {
  if(dma->dmaBusy) {
    dma_doDma(dma);
    return true;
  }
  return false;
}

void dma_startDma(Dma* dma, uint8_t val, bool hdma) {
  for(int i = 0; i < 8; i++) {
    if(hdma) {
      dma->channel[i].hdmaActive = val & (1 << i);
    } else {
      dma->channel[i].dmaActive = val & (1 << i);
      if ((val & (1 << i)) && ar_trace_active())
        ar_trace_dma(i, dma->channel[i].bAdr, dma->channel[i].aBank,
                     dma->channel[i].aAdr, dma->channel[i].size,
                     dma->channel[i].fromB);
    }
  }
  if(!hdma) {
    dma->dmaBusy = val;
    dma->dmaTimer += dma->dmaBusy ? 16 : 0; // 12-24 cycle overhead for entire dma transfer
  }
}
