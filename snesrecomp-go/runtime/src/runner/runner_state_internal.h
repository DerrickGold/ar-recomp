#pragma once

#include <stdbool.h>

typedef struct Cpu Cpu;
typedef struct Dma Dma;
typedef struct Ppu Ppu;
typedef struct Snes Snes;

/* Process-owned singleton state used only by the linked runtime
 * implementation. Public game code receives an opaque SrRunnerHandle. */
extern Snes *g_snes;
extern Cpu *g_snes_cpu;
extern Ppu *g_ppu;
extern Dma *g_dma;
extern bool g_fail;
