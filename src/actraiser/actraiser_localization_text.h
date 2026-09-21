#ifndef ACTRAISER_LOCALIZATION_TEXT_H
#define ACTRAISER_LOCALIZATION_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp/game/cpu.h"

#define ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION UINT32_C(11)
#define ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION UINT32_C(4)

/* Called only after the native interpreter actually returns. In particular,
 * exposing $01 does not by itself acknowledge its menu yield. */
void ActRaiserLocalizationText_ObserveReturn(void);
/* $8FC5 loops internally across consecutive dictionary words, bypassing its
 * function-entry seam. Observe the literal/control it actually returns, before
 * the caller dispatches it; first_cursor is the original reader input. */
void ActRaiserLocalizationText_ObserveDecodedByte(CpuState *cpu,
                                                 uint16_t first_cursor);

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
  /* Earliest continuation still owned by the native text window. A $02 wait
   * can retain rows or clear them, depending on native text-state $0200. */
  uint16_t window_start_page;
  /* One-based locked-anchor identity of the most recent $05 clear within
   * this retained window. Zero means a page clear/entry, not an anchor. */
  uint16_t window_start_control_count;
  /* $01/$03/$04/$05 are the contract's locked controls. A reader entry
   * observes a pending operation; only the following entry proves it ran.
   * $02 is presentation pagination and is not a locked control. */
  uint16_t completed_control_count;
  bool control_pending;
  /* Native source bytes consumed on the current page, including consecutive
   * dictionary tokens traversed by the reader's internal loop. A dictionary
   * token is one unit, not its expanded glyph count. */
  uint16_t page_unit_index;
  /* Native X at interpreter entry. Pointer-matrix and offering dispatchers
   * retain their selected slot here, disambiguating aliased source records. */
  uint16_t selector_x;
  uint8_t map_group;
  uint8_t map_number;
  /* Fixed-composer generation visible when this dialogue invocation began.
   * This orders inverse UI transitions without relying on English text,
   * transient WRAM flags, or a one-frame native tilemap probe. */
  uint64_t entry_compose_serial;
  /* A terminal byte begins the native acknowledgement wait; it does not make
   * the composed page disappear. This snapshots the fixed-composer generation
   * at that byte so the frame adapter can retain the page until later UI work
   * replaces it. */
  uint64_t terminal_compose_serial;
  /* True between the native page-break token and the first token of the next
   * page. This is semantic continuation state, not a glyph/tile observation. */
  bool awaiting_page_advance;
  /* $01 returns to a native menu with the segment fully visible. It is
   * neither a continuation wait nor the terminal acknowledgement loop. */
  bool yielded_to_menu;
  bool continuation_cell_valid;
  uint16_t continuation_cell;
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
  /* `$01:8C79` resolves the selected magic/possession table immediately
   * before composing it. Some slots deliberately alias one source record, so
   * retain both the originating table and its logical index. A zero table
   * means this compose did not pass through that indexed source resolver. */
  uint32_t source_table_pc24;
  uint16_t source_selector;
  uint16_t game_frame;
  uint8_t map_group;
  uint8_t map_number;
  /* Nonzero extents describe an ordered native tilemap erase, not a compose.
   * Partial erases keep unrelated menus/dialogue alive. These are native
   * write regions, never inferred from framebuffer/tile contents. */
  uint8_t clear_first_column;
  uint8_t clear_column_count;
  uint8_t clear_first_row;
  uint8_t clear_row_count;
  bool clears_dialogue;
} ActRaiserLocalizationComposeObservation;

/* Read-only hle_func_if predicates. They always return false, so the native
 * USA routines remain authoritative while the host records their identity. */
bool ActRaiser_LocalizationObserveTextEntry(CpuState *cpu);
bool ActRaiser_LocalizationObserveTextByte(CpuState *cpu);
bool ActRaiser_LocalizationObserveIndexedComposeSource(CpuState *cpu);
bool ActRaiser_LocalizationObserveTextCompose(CpuState *cpu);
bool ActRaiser_LocalizationObserveTextErase(CpuState *cpu);
bool ActRaiser_LocalizationObserveMenuClear(CpuState *cpu);
bool ActRaiser_LocalizationObserveGeneralClear(CpuState *cpu);
bool ActRaiser_LocalizationObserveHudTemplate(CpuState *cpu);
bool ActRaiser_LocalizationObserveContinuation(CpuState *cpu);
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
void ActRaiserLocalizationText_ResetObservation(void);

#endif /* ACTRAISER_LOCALIZATION_TEXT_H */
