#ifndef ACTRAISER_STAGE_PLACEMENTS_H
#define ACTRAISER_STAGE_PLACEMENTS_H
#include "snesrecomp/game/cpu.h"
#include "regional/action/regional_placements.h"

/* Game-thread, non-yielding room preparation. Captures numerical programs and
 * randomization before actor creation; failure leaves the previous snapshot
 * unchanged. US/US resets to native placement loading. Never call on a live
 * actor pool in response to a settings edit. No donor ROM is required. */
bool ActRaiserStagePlacements_Prepare(uint16_t scene,
    const ArRegionalPlacementPolicy *policy, bool action_mode,
    ArRegionalDifficulty difficulty, uint8_t terrain_profile);
void ActRaiserStagePlacements_Reset(void);
/* Binds the captured numerical generation, including randomization, without
 * rescanning placements. US delegation preserves the prior digest verbatim. */
bool ActRaiserStagePlacements_Fingerprint(const uint8_t previous[32],uint8_t out[32],bool *native);
/* $941C initial batch: native cursor plus $0AE0 ordinary / $0B20 statue-light
 * entry slot, selected by the original $9354 predicate. Preflight the actual
 * capacity including the sentinel before any writes. No live-pool rewrite. */
bool ActRaiser_StagePlacementsEntry(CpuState *cpu);
RecompReturn ActRaiser_StagePlacements(CpuState *cpu);
bool ActRaiser_StageWaveEntry(CpuState *cpu);
RecompReturn ActRaiser_StageWave(CpuState *cpu);
#endif
