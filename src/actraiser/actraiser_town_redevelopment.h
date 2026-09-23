#ifndef ACTRAISER_TOWN_REDEVELOPMENT_H
#define ACTRAISER_TOWN_REDEVELOPMENT_H

#include "snesrecomp/game/cpu.h"

enum { kActRaiserRedevelopmentTowns = 6 };

typedef enum ActRaiserRedevelopmentStatus {
  kActRaiserRedevelopment_Ready,
  kActRaiserRedevelopment_Invalid,
  kActRaiserRedevelopment_StructureBusy,
  kActRaiserRedevelopment_UnknownStructure,
  kActRaiserRedevelopment_InvalidFootprint,
  kActRaiserRedevelopment_MarkMismatch,
  kActRaiserRedevelopment_PopulationBias,
  kActRaiserRedevelopment_GrowthRange,
  kActRaiserRedevelopment_Stale,
} ActRaiserRedevelopmentStatus;

typedef struct ActRaiserTownRedevelopmentPlan {
  uint64_t fingerprint;
  uint8_t requested_towns, affected_towns;
  bool japanese_construction;
  uint16_t removed[kActRaiserRedevelopmentTowns];
  uint16_t houses[kActRaiserRedevelopmentTowns];
  uint16_t growth_credit[kActRaiserRedevelopmentTowns];
} ActRaiserTownRedevelopmentPlan;

/* Bounded, read-only preview of the US-layout town records and cell marks.
 * Failure leaves plan unchanged. Bit N selects town N; undeveloped towns are
 * skipped. Capture the intended construction policy alongside the owner's
 * revision token. Allowance tops up to removed houses at that policy's price;
 * support buildings normally return their price, so they get no per-building
 * credit. A support-only reset may top up to one start to avoid a zero-growth
 * deadlock. Larger reserves are preserved. This is a bounded reconstruction
 * floor, not historical spending or an additive per-record refund.
 * Unsupported/stale inputs
 * fail closed; no bias reset, ordinary house-loss or lair reward is invented.
 * Only the Palace conversion transaction may activate this from settings. */
ActRaiserRedevelopmentStatus ActRaiserTownRedevelopment_Preview(
    CpuState *cpu, uint8_t towns, bool japanese_construction, ActRaiserTownRedevelopmentPlan *plan);

/* Mutation core ONLY, not an activation/persistence API. The owning transaction
 * must have confirmed this preview, captured a complete recovery copy, and
 * suspended native town/actor/visual/construction work at an audited boundary.
 * Revalidates the entire preview before any write. Clears only eligible active
 * record flags and their matching marks; supplies the previewed top-up once.
 * Leaves CPU registers, SRAM, policy, roads, landmarks, story, timers, population
 * and visual queues untouched. Before resuming native work the owner must
 * retire stale visual/construction ownership, redraw and run the existing
 * census/report refresh under the new policy, then persist one coherent save.
 * Reusing an applied nonempty preview fails Stale; a fresh empty preview cannot
 * issue more credit. Never call this directly from an overlay callback. */
ActRaiserRedevelopmentStatus ActRaiserTownRedevelopment_Apply(
    CpuState *cpu, const ActRaiserTownRedevelopmentPlan *plan);

#endif
