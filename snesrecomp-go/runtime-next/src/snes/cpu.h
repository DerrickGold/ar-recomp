#ifndef SNESRECOMP_NEXT_CPU_H
#define SNESRECOMP_NEXT_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cpu Cpu;
typedef struct SaveLoadInfo SaveLoadInfo;

/* Register-only shell used by statically recompiled 65816 code. */
struct Cpu {
    uint16_t a;
    uint16_t x;
    uint16_t y;
    uint16_t sp;
    uint16_t pc;
    uint16_t dp;
    uint8_t k;
    uint8_t db;
    bool c;
    bool z;
    bool v;
    bool n;
    bool i;
    bool d;
    bool xf;
    bool mf;
    bool e;
};

extern Cpu *g_snes_cpu;

Cpu *cpu_init(void);
void cpu_free(Cpu *cpu);
void cpu_reset(Cpu *cpu);
uint8_t cpu_getFlags(Cpu *cpu);
void cpu_setFlags(Cpu *cpu, uint8_t value);
void cpu_saveload(Cpu *cpu, SaveLoadInfo *info);

#ifdef __cplusplus
}
#endif

#endif
