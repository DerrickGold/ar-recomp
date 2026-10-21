#ifndef AR_ACTRAISER_ANGEL_H
#define AR_ACTRAISER_ANGEL_H
/* Angel: game-thread movement HLE, fixed map axes and cardinal facing.
 * The frontend publishes one immutable stick sample per simulation tick.
 * Tests: tests/actraiser_angel_test.c. */

#include "snesrecomp/game/cpu.h"
#include "actraiser_angel_input.h"

void ActRaiserAngel_Reset(void);
/* `shot` is the twin-stick direction: 0 none, else native facing plus one. */
void ActRaiserAngel_SetInput(uint32_t stick, uint8_t shot, uint8_t map_group,
                             uint8_t map_number);
/* Whether the previous tick serviced player-controlled flight. Read by the
 * host to scope twin-stick bindings; menus, events and transitions are not. */
bool ActRaiserAngel_PlayerControlled(void);
bool ActRaiserAngel_DecodeEntry(CpuState *cpu);
bool ActRaiserAngel_MoveEntry(CpuState *cpu);

RecompReturn ActRaiser_AngelDecodeInput(CpuState *cpu);
RecompReturn ActRaiser_AngelDecodeDirection(CpuState *cpu);
RecompReturn ActRaiser_AngelNormalTail(CpuState *cpu);
RecompReturn ActRaiser_AngelMove(CpuState *cpu);
RecompReturn ActRaiser_AngelKnockback(CpuState *cpu);
RecompReturn ActRaiser_AngelService(CpuState *cpu);

#endif
