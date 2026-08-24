#ifndef SPC_H
#define SPC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Spc Spc;

#include "apu.h"
#include "saveload.h"

struct Spc {
  Apu* apu;
  // registers
  uint8_t a;
  uint8_t x;
  uint8_t y;
  uint8_t sp;
  uint16_t pc;
  // flags
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool h;
  bool p;
  bool b;
  // stopping
  bool stopped;
  // internal use
  uint8_t cyclesUsed; // indicates how many cycles an opcode used
};

Spc* spc_init(Apu* apu);
void spc_free(Spc* spc);
void spc_reset(Spc* spc);
int spc_runOpcode(Spc* spc);
void spc_saveload(Spc *spc, SaveLoadInfo *sli);

/* Called immediately before an opcode executes, with its pre-fetch PC. */
extern void (*g_spc_opcode_trace_hook)(Spc *spc, uint16_t pc);
/* Optional game-owned correction seam. Runs after the observer above but
 * before opcode fetch, so it may adjust flags/PC while trace provenance keeps
 * the original instruction address. */
extern void (*g_spc_opcode_patch_hook)(Spc *spc, uint16_t pc);
/* Optional game-owned timing seam. Returning zero executes the next opcode in
 * the same APU master cycle; NULL preserves every native opcode cost. */
extern int (*g_spc_opcode_cycle_hook)(Spc *spc, uint16_t pc, int cycles);

#endif
