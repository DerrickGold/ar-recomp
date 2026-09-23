#ifndef ACTRAISER_BRIDGE_EXTENSION_H
#define ACTRAISER_BRIDGE_EXTENSION_H
#include "snesrecomp/game/cpu.h"
/* Completed extension bridges not duplicated by a native record. Read-only,
 * independent of the current bridge-limit setting; owns extension validation
 * and deduplication. Caller supplies the native town-bank context. */
unsigned ActRaiserBridgeExtension_Count(CpuState *cpu,uint8_t town_bank,unsigned town);
#endif
