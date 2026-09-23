#ifndef ACTRAISER_RECOVERY_H
#define ACTRAISER_RECOVERY_H

#include "regional/regional_recovery.h"
#include "snesrecomp/game/cpu.h"

bool ActRaiserRecovery_CycleEntry(const CpuState *cpu);
bool ActRaiserRecovery_DrainEntry(const CpuState *cpu);
bool ActRaiserRecovery_MotionEntry(const CpuState *cpu);
/* Prefixes retain native stack frames and return raw callee escape tokens.
 * The runtime adapter owns the continuation or one-level escape propagation. */
RecompReturn ActRaiserRecovery_Cycle(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot);
RecompReturn ActRaiserRecovery_Drain(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot);
void ActRaiserRecovery_Motion(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot, bool stopped);
/* Retire only a changed leaf's old queue/phase; never adjust earned HP/SP. */
void ActRaiserRecovery_Reconcile(CpuState *cpu, unsigned changed);

#endif
