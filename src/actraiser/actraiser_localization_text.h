#ifndef ACTRAISER_LOCALIZATION_TEXT_H
#define ACTRAISER_LOCALIZATION_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp/game/cpu.h"

#define ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION UINT32_C(4)
#define ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION UINT32_C(1)

/* Immutable host-side identity observed at the untouched bank-$01 dialogue
 * interpreter. No pointer in this value borrows CPU or ROM storage. */
typedef struct ActRaiserLocalizationTextObservation {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t serial;
  uint32_t source_pc24;
  uint32_t cursor_pc24;
  uint32_t caller_pc24;
  /* Outer dialogue-wrapper continuation when the interpreter was reached
   * through one of the seven shared wrappers; zero for direct calls. */
  uint32_t context_pc24;
  uint16_t game_frame;
  uint16_t direct_page;
  uint16_t page_index;
  /* Top-level native decoder invocations completed on the current page. A
   * dictionary token is one unit even when it expands to several glyphs; the
   * runtime maps this native ratio onto complete enhanced grapheme clusters. */
  uint16_t page_unit_index;
  /* Native X at interpreter entry. Pointer-matrix and offering dispatchers
   * retain their selected slot here, disambiguating aliased source records. */
  uint16_t selector_x;
  uint8_t map_group;
  uint8_t map_number;
  /* A terminal byte begins the native acknowledgement wait; it does not make
   * the composed page disappear. This snapshots the fixed-composer generation
   * at that byte so the frame adapter can retain the page until later UI work
   * replaces it. */
  uint64_t terminal_compose_serial;
  bool terminal;
} ActRaiserLocalizationTextObservation;

/* One invocation of the native fixed-text composer at $02:BF60. A contains
 * its row/column destination (`row << 8 | column`) and DB:Y the source. */
typedef struct ActRaiserLocalizationComposeObservation {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t serial;
  uint32_t source_pc24;
  uint32_t caller_pc24;
  uint16_t destination;
  uint16_t game_frame;
  uint8_t map_group;
  uint8_t map_number;
} ActRaiserLocalizationComposeObservation;

/* Read-only hle_func_if predicates. They always return false, so the native
 * USA routines remain authoritative while the host records their identity. */
bool ActRaiser_LocalizationObserveTextEntry(CpuState *cpu);
bool ActRaiser_LocalizationObserveTextByte(CpuState *cpu);
bool ActRaiser_LocalizationObserveTextCompose(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper0(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper1(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper2(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper3(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper4(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper5(CpuState *cpu);
bool ActRaiser_LocalizationObserveDialogueWrapper6(CpuState *cpu);
RecompReturn ActRaiser_LocalizationTextObserverTrap(CpuState *cpu);

bool ActRaiserLocalizationText_CopyObservation(
    ActRaiserLocalizationTextObservation *observation);
bool ActRaiserLocalizationText_CopyComposeObservations(
    uint64_t after_serial,
    ActRaiserLocalizationComposeObservation *observations,
    size_t observation_capacity, size_t *observation_count,
    bool *dropped);
/* P6 dialogue waits block the game thread, so a later fixed composer is the
 * first unambiguous replacement generation. P7 refines this into the complete
 * destination-aware ownership model for simultaneous menu surfaces. */
bool ActRaiserLocalizationText_TerminalWasReplaced(
    const ActRaiserLocalizationTextObservation *observation);
void ActRaiserLocalizationText_ResetObservation(void);

#endif /* ACTRAISER_LOCALIZATION_TEXT_H */
