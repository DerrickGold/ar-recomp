#ifndef AR_ACTRAISER_DEATH_HEIM_RUNTIME_H
#define AR_ACTRAISER_DEATH_HEIM_RUNTIME_H

#include "actraiser/actraiser_death_heim_rush.h"
#include "snesrecomp/game/cpu.h"

/* Game-thread owner. Reset invalidates every admitted actor token, but retains
 * the configured recipe. Configure is allowed only outside an active rush. */
void ActRaiserDeathHeim_Reset(void);
bool ActRaiserDeathHeim_Configure(const ActRaiserDeathHeimRushConfig *config);
bool ActRaiserDeathHeim_Active(void);
bool ActRaiserDeathHeim_RosterAvailable(unsigned roster);
bool ActRaiserDeathHeim_ExtendedEnabled(void);
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_CurrentEncounter(void);
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_LastCompletedEncounter(void);
ActRaiserDeathHeimRushPhase ActRaiserDeathHeim_Phase(void);
bool ActRaiserDeathHeim_RetryEncounter(CpuState *cpu);
/* Called after the native boss initializer has installed its original source.
 * A repeated admission after a room retry renews the attempt token. */
bool ActRaiserDeathHeim_AdmitEncounter(CpuState *cpu, uint16_t owner);
/* Stage the native beam on a freshly initialized Act 1 root. Its original
 * record is restored before its first AI update, after native state 14. */
bool ActRaiserDeathHeim_BeginArrival(CpuState *cpu, uint16_t owner);
/* The native scene owner calls this after a scene transition, not during its
 * intermediate destination stores. Natural Act 1 rooms are part of the rush. */
void ActRaiserDeathHeim_ObserveScene(CpuState *cpu);
/* Commit exactly the admitted root, then stage the ordinary native hub load. */
bool ActRaiserDeathHeim_RequestHubReturn(CpuState *cpu);
uint8_t ActRaiserDeathHeim_EyeMask(unsigned city);
/* Identify the original glow/effect actor pair by source and spawn position.
 * Returns city 1..6, or zero for anything outside a live extended rush. */
unsigned ActRaiserDeathHeim_EyeActorCity(CpuState *cpu, uint16_t owner);
bool ActRaiserDeathHeim_Fingerprint(const uint8_t previous[32], uint8_t out[32],
                                    bool *baseline);

bool ActRaiser_DeathHeimHubRouteEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimHubRoute(CpuState *cpu);
bool ActRaiser_DeathHeimEyesEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimEyes(CpuState *cpu);
bool ActRaiser_DeathHeimAdmitEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimAdmit(CpuState *cpu);
bool ActRaiser_DeathHeimVictoryEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimVictory(CpuState *cpu);
bool ActRaiser_DeathHeimAct1VictoryEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimAct1Victory(CpuState *cpu);
bool ActRaiser_DeathHeimArrivalStartEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArrivalStart(CpuState *cpu);
bool ActRaiser_DeathHeimArrivalCompleteEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArrivalComplete(CpuState *cpu);
bool ActRaiser_DeathHeimDepartureStartEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimDepartureStart(CpuState *cpu);

#endif
