#ifndef SNESRECOMP_NEXT_SPC_H
#define SNESRECOMP_NEXT_SPC_H

#include <stdbool.h>
#include <stdint.h>

#include "dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Apu Apu;
typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct Spc Spc;

struct Spc {
    Apu *apu;
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint16_t pc;
    bool c;
    bool z;
    bool v;
    bool n;
    bool i;
    bool h;
    bool p;
    bool b;
    bool stopped;
    uint8_t cyclesUsed;
};

Spc *spc_init(Apu *apu);
void spc_free(Spc *spc);
void spc_reset(Spc *spc);
int spc_runOpcode(Spc *spc);
void spc_saveload(Spc *spc, SaveLoadInfo *info);

extern void (*g_spc_opcode_patch_hook)(Spc *spc, uint16_t pc);
extern int (*g_spc_opcode_cycle_hook)(Spc *spc, uint16_t pc, int cycles);

#ifdef __cplusplus
}
#endif

#endif
