#ifndef ACTRAISER_POPULATION_CONVERSION_H
#define ACTRAISER_POPULATION_CONVERSION_H

#include "actraiser/actraiser_town_redevelopment.h"
#include "regional/session/regional_campaign.h"
#include "regional/regional_profiles.h"

typedef struct ActRaiserPopulationPreview {
  uint8_t campaign[16];
  uint32_t revision;
  ArRegionalSource source;
  bool redevelop;
  bool profile;
  ArRegionalProfileGroup group;
  ActRaiserTownRedevelopmentPlan town;
} ActRaiserPopulationPreview;

typedef enum ActRaiserPopulationResult {
  kActRaiserPopulation_Ready,
  kActRaiserPopulation_Unsafe,
  kActRaiserPopulation_InvalidTown,
  kActRaiserPopulation_Stale,
  kActRaiserPopulation_Unchanged,
  kActRaiserPopulation_CheckpointFailed,
  kActRaiserPopulation_RecoveryFailed,
  kActRaiserPopulation_RolledBack,
  kActRaiserPopulation_Committed,
  kActRaiserPopulation_NamePending,
} ActRaiserPopulationResult;

/* Called ONLY by the audited $01:85A2 Palace selector boundary after native
 * intro/arrival/award work, with town caches already saved. These checks are
 * additional guards, not permission to call from an arbitrary paused frame.
 * Preview is read-only and leaves out untouched on failure. */
ActRaiserPopulationResult ActRaiserPopulation_Preview(CpuState *cpu,
    const ArRegionalCampaign *campaign, ArRegionalSource source,
    ActRaiserPopulationPreview *out);

/* Whole Population/Gameplay selections share the same confirmation, recovery
 * copy and rollback. No member of the selection is published before commit. */
ActRaiserPopulationResult ActRaiserPopulation_PreviewProfile(CpuState *cpu,
    const ArRegionalCampaign *campaign, ArRegionalProfileGroup group,
    ArRegionalSource source, ActRaiserPopulationPreview *out);

/* Requires explicit user confirmation of this exact preview. No frame service
 * or UI work may run inside the commit. First save current progress under the
 * OLD policy and create a permanent recovery copy in an exclusive directory.
 * Then mutate all affected towns, retire stale visuals, refresh census/display,
 * and atomically persist native image + new regional policy. Failed candidate
 * persistence restores all mutated WRAM and the old policy. A committed name
 * failure never rolls back or repeats demolition. Roads, plot lists, native
 * pacing timers, progress and earned awards are not reset. Native town entry
 * redraws the map from the surviving records; no hidden SIM scene is run here.
 * A failed pre-conversion checkpoint may still have saved current progress,
 * but has not removed any buildings or changed rules. */
ActRaiserPopulationResult ActRaiserPopulation_Commit(CpuState *cpu,
    ArRegionalCampaign *campaign, const ActRaiserPopulationPreview *confirmed,
    const char *recovery_directory, SaveError *error);

#endif
