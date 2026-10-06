#ifndef AR_ACTRAISER_ANGEL_H
#define AR_ACTRAISER_ANGEL_H
/* Angel: game-thread movement HLE, fixed map axes and cardinal facing.
 * The frontend publishes one immutable stick sample per simulation tick.
 * Tests: tests/actraiser_angel_test.c. */

#include "snesrecomp/game/cpu.h"
#include "actraiser_angel_input.h"

void ActRaiserAngel_Reset(void);
void ActRaiserAngel_SetInput(uint32_t stick, uint8_t map_group, uint8_t map_number);
bool ActRaiserAngel_DecodeEntry(CpuState *cpu);
bool ActRaiserAngel_MoveEntry(CpuState *cpu);

RecompReturn ActRaiser_AngelDecodeInput(CpuState *cpu);
RecompReturn ActRaiser_AngelDecodeDirection(CpuState *cpu);
RecompReturn ActRaiser_AngelNormalTail(CpuState *cpu);
RecompReturn ActRaiser_AngelMove(CpuState *cpu);
RecompReturn ActRaiser_AngelKnockback(CpuState *cpu);
RecompReturn ActRaiser_AngelService(CpuState *cpu);

#endif
