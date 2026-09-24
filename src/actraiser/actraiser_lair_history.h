#ifndef ACTRAISER_LAIR_HISTORY_H
#define ACTRAISER_LAIR_HISTORY_H

#include "regional/towns/regional_lair_history.h"
#include "snesrecomp/game/cpu.h"
#include "save_system.h"

typedef enum ActRaiserLairEvent {
  kActRaiserLairEvent_Kill,
  kActRaiserLairEvent_Miracle,
  kActRaiserLairEvent_House,
  kActRaiserLairEvent_Score,
  kActRaiserLairEvent_Count,
} ActRaiserLairEvent;
typedef struct ActRaiserLairCapture {
  ActRaiserLairEvent event;
  ArRegionalLairAccounting policy;
  unsigned town, candidates, sealed_mask;
  uint16_t bcd_score, completed_acts;
  uint8_t subtype;
} ActRaiserLairCapture;

bool ActRaiserLairHistory_Entry(const CpuState *cpu);
/* Safe accounting publication may also be requested by bank00's accepted
 * act-clear owner. This does not authorize observation of arbitrary routines. */
bool ActRaiserLairHistory_ProjectionEntry(const CpuState *cpu);
/* US03:B4B8 AND #$00ff / TAY prefix. For JP, substitute the fixed four
 * units before the shared native stock/growth distributor. No WRAM writes. */
bool ActRaiserLairHouse_Units(CpuState *cpu, bool tiered);
/* Validated durable save, not live WRAM or the session-only SRAM shadow. */
bool ActRaiserLairHistory_ReadSavedStocks(const uint8_t image[kActRaiserSramSize],
                                         uint16_t out[kArRegionalLairCount]);
/* Game-owned safe tick only, outside any captured stock/effect transaction.
 * No CPU flags/registers, actors, clocks, seals, growth or rewards change.
 * Validate the complete old projection before writing any target counter.
 * An unaccounted native write quarantines its town and rejects the switch. */
ArRegionalLairProjectionResult ActRaiserLairHistory_Project(ArRegionalLairHistory *history,
    CpuState *cpu, const ArRegionalLairAccounting *current,
    const ArRegionalLairAccounting *target);
/* Called only for a confirmed new campaign, after native seed installation.
 * Verify the US native seed projection; never overwrite existing history. */
bool ActRaiserLairHistory_Initialize(ArRegionalLairHistory *history, CpuState *cpu);
/* Captures before native zero-stock early-outs. CPU and WRAM remain unchanged.
 * Unknown/diverged towns are not silently initialized. A mismatch quarantines
 * the town's retained history, rather than repairing a cheat/native write. */
bool ActRaiserLairHistory_Begin(ArRegionalLairHistory *history, CpuState *cpu,
    ActRaiserLairEvent event, const ArRegionalLairAccounting *policy,
    ActRaiserLairCapture *capture);
/* Native body has already run with its original call frame. Apply the same
 * semantic event to a candidate, compare its selected stock with native RAM,
 * then commit. Escape/mismatch retains prior history marked diverged. Does
 * not generate a second seal, soul, growth/SP award, dialogue or native write. */
bool ActRaiserLairHistory_End(ArRegionalLairHistory *history, CpuState *cpu,
                            const ActRaiserLairCapture *capture, RecompReturn result);
/* Event/save/request boundary audit, not a per-frame reconciliation pass. */
bool ActRaiserLairHistory_Check(ArRegionalLairHistory *history, CpuState *cpu,
                              const ArRegionalLairAccounting *policy);

#endif
