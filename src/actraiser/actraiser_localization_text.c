#include "actraiser/actraiser_localization_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

enum { kComposeObservationCapacity = 256 };

static ActRaiserLocalizationTextObservation s_text = {
  .struct_size = sizeof(ActRaiserLocalizationTextObservation),
  .abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION,
};
static bool s_text_valid;
static bool s_pending_page_advance;
static bool s_pending_page_retains_rows;
static ActRaiserLocalizationComposeObservation
    s_compose[kComposeObservationCapacity];
static uint64_t s_compose_serial;
static size_t s_compose_count;
static uint32_t s_pending_source_table_pc24;
static uint16_t s_pending_source_selector;
static bool s_pending_source_selector_valid;
static uint32_t s_wrapper_context_pc24;
static uint32_t s_wrapper_text_caller_pc24;
static bool s_wrapper_context_valid;
static bool s_trace_configured;
static bool s_trace_enabled;

static uint16_t ReadStackReturnAddress(CpuState *cpu) {
  return (uint16_t)(cpu_read8(cpu, 0, (uint16_t)(cpu->S + 1u)) |
                    ((uint16_t)cpu_read8(
                         cpu, 0, (uint16_t)(cpu->S + 2u)) << 8));
}

static uint32_t ReadLongCallSite(CpuState *cpu) {
  const uint16_t return_address = ReadStackReturnAddress(cpu);
  const uint8_t caller_bank =
      cpu_read8(cpu, 0, (uint16_t)(cpu->S + 3u));
  return ((uint32_t)caller_bank << 16) |
      (uint16_t)(return_address + 1u);
}

static uint32_t ReadShortCallSite(CpuState *cpu) {
  return ((uint32_t)cpu->PB << 16) |
      (uint16_t)(ReadStackReturnAddress(cpu) + 1u);
}

static bool ObserveDialogueWrapper(CpuState *cpu, bool long_call,
                                   uint32_t text_caller_pc24) {
  if (!cpu) return false;
  s_wrapper_context_pc24 = long_call
      ? ReadLongCallSite(cpu) : ReadShortCallSite(cpu);
  s_wrapper_text_caller_pc24 = text_caller_pc24;
  s_wrapper_context_valid = true;
  return false;
}

bool ActRaiser_LocalizationObserveDialogueWrapper0(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, true, UINT32_C(0x019330));
}

bool ActRaiser_LocalizationObserveDialogueWrapper1(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, true, UINT32_C(0x019358));
}

bool ActRaiser_LocalizationObserveDialogueWrapper2(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, true, UINT32_C(0x019371));
}

bool ActRaiser_LocalizationObserveDialogueWrapper3(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, true, UINT32_C(0x019390));
}

bool ActRaiser_LocalizationObserveDialogueWrapper4(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, true, UINT32_C(0x0193A3));
}

bool ActRaiser_LocalizationObserveDialogueWrapper5(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, false, UINT32_C(0x0193B2));
}

bool ActRaiser_LocalizationObserveDialogueWrapper6(CpuState *cpu) {
  return ObserveDialogueWrapper(cpu, false, UINT32_C(0x0193BA));
}

static bool TraceEnabled(void) {
  if (!s_trace_configured) {
    const char *value = getenv("AR_LOCALIZATION_TEXT_TRACE");
    s_trace_enabled = value && value[0] && value[0] != '0';
    s_trace_configured = true;
  }
  return s_trace_enabled;
}

bool ActRaiser_LocalizationObserveTextEntry(CpuState *cpu) {
  if (!cpu) return false;
  uint64_t serial = s_text.serial + 1u;
  if (!serial) serial = 1u;
  const uint32_t caller_pc24 = ((uint32_t)cpu->PB << 16) |
      (uint16_t)(ReadStackReturnAddress(cpu) + 1u);
  uint32_t context_pc24 = 0;
  if (s_wrapper_context_valid) {
    if (s_wrapper_text_caller_pc24 == caller_pc24)
      context_pc24 = s_wrapper_context_pc24;
    s_wrapper_context_valid = false;
  }
  s_text = (ActRaiserLocalizationTextObservation){
    .struct_size = sizeof(s_text),
    .abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION,
    .serial = serial,
    .source_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y,
    .cursor_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y,
    .caller_pc24 = caller_pc24,
    .context_pc24 = context_pc24,
    .game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame),
    .direct_page = cpu->D,
    .selector_x = cpu->X,
    .map_group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup),
    .map_number = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap),
    .entry_compose_serial = s_compose_serial,
  };
  s_text_valid = true;
  s_pending_page_advance = false;
  s_pending_page_retains_rows = false;
  if (TraceEnabled()) {
    fprintf(stderr,
            "[localization-text] serial=%llu gf=%u map=%02X/%02X "
            "source=$%02X:%04X caller=$%02X:%04X context=$%02X:%04X "
            "x=$%04X dp=$%04X\n",
            (unsigned long long)s_text.serial, s_text.game_frame,
            s_text.map_group, s_text.map_number,
            (unsigned)(s_text.source_pc24 >> 16),
            (unsigned)(s_text.source_pc24 & 0xffffu),
            (unsigned)(s_text.caller_pc24 >> 16),
            (unsigned)(s_text.caller_pc24 & 0xffffu),
            (unsigned)(s_text.context_pc24 >> 16),
            (unsigned)(s_text.context_pc24 & 0xffffu), s_text.selector_x,
            s_text.direct_page);
  }
  return false;
}

bool ActRaiser_LocalizationObserveTextByte(CpuState *cpu) {
  if (!cpu || !s_text_valid) return false;
  if (s_pending_page_advance) {
    ++s_text.page_index;
    if (!s_pending_page_retains_rows)
      s_text.window_start_page = s_text.page_index;
    s_text.page_unit_index = 0;
    s_pending_page_advance = false;
    s_text.awaiting_page_advance = false;
    s_text.continuation_cell_valid = false;
  }
  s_text.cursor_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y;
  s_text.game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame);
  const uint8_t code = cpu_read8(cpu, cpu->DB, cpu->Y);
  s_text.yielded_to_menu = false;
  if (s_text.page_unit_index != UINT16_MAX) ++s_text.page_unit_index;
  /* The native $02 page operation waits and scrolls before the next reader
   * entry. Publish the new page only when that following token is reached. */
  if (code == 0x02u) {
    s_pending_page_advance = true;
    /* $01:8F97 chooses clear ($9032) when this byte is zero; otherwise
     * it advances a row and $905B copies rows up at the bottom of the box.
     * Snapshot the choice before waiting, without modifying native state. */
    s_pending_page_retains_rows = cpu_read8(cpu, 0, 0x0200) != 0;
    s_text.awaiting_page_advance = true;
  } else if (code == 0x05u) {
    s_text.window_start_page = s_text.page_index;
  } else if (code == 0x00u) {
    s_text.terminal_compose_serial = s_compose_serial;
    s_text.terminal = true;
  } else if (code == 0x01u) {
    s_text.yielded_to_menu = true;
  }
  if (TraceEnabled() && (code == 0x02u || code == 0x00u || code == 0x01u))
    fprintf(stderr, "[localization-page] gf=%u serial=%llu page=%u "
                    "first=%u units=%u control=%02X retain=%u\n",
            s_text.game_frame, (unsigned long long)s_text.serial,
            s_text.page_index, s_text.window_start_page,
            s_text.page_unit_index, code, (unsigned)s_pending_page_retains_rows);
  return false;
}

bool ActRaiser_LocalizationObserveIndexedComposeSource(CpuState *cpu) {
  if (!cpu) return false;
  const uint16_t native_selector = cpu->A & UINT16_C(0x007F);
  const uint16_t native_base =
      cpu_read16(cpu, 0, (uint16_t)(cpu->D + 8u));
  /* The helper receives a one-based selection and adds its doubled value to
   * an address two bytes before the first pointer. Normalize both values to
   * the extraction catalog's first-entry table and zero-based slot. */
  s_pending_source_table_pc24 = ((uint32_t)cpu->DB << 16) |
      (uint16_t)(native_base + 2u);
  s_pending_source_selector = native_selector
      ? (uint16_t)(native_selector - 1u) : 0;
  s_pending_source_selector_valid = native_selector != 0;
  return false;
}

bool ActRaiser_LocalizationObserveContinuation(CpuState *cpu) {
  if (cpu && s_text_valid && s_text.awaiting_page_advance &&
      cpu->X < 0x0800 && !(cpu->X & 1)) {
    s_text.continuation_cell = cpu->X / 2;
    s_text.continuation_cell_valid = true;
  }
  return false;
}

static bool ObserveSurfaceClear(CpuState *cpu, uint8_t first_column,
                                uint8_t first_row, uint8_t column_count,
                                uint8_t row_count) {
  if (!cpu) return false;
  /* Native US dialogue owns columns 5..28, rows 19..24. A small choice
   * window above it may close while that dialogue remains on screen. */
  const bool clears_dialogue = first_column < 29 && first_row < 25 &&
      first_column + column_count > 5 && first_row + row_count > 19;
  if (!++s_compose_serial) ++s_compose_serial;
  const size_t slot = (size_t)((s_compose_serial - 1u) %
                               kComposeObservationCapacity);
  s_compose[slot] = (ActRaiserLocalizationComposeObservation){
      .struct_size = sizeof(s_compose[slot]),
      .abi_version = ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION,
      .serial = s_compose_serial,
      .game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame),
      .map_group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup),
      .map_number = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap),
      .clear_first_column = first_column,
      .clear_column_count = column_count,
      .clear_first_row = first_row,
      .clear_row_count = row_count,
      .clears_dialogue = clears_dialogue,
  };
  if (s_compose_count < kComposeObservationCapacity) ++s_compose_count;
  if (clears_dialogue) {
    s_text_valid = false;
    s_pending_page_advance = false;
  }
  if (TraceEnabled())
    fprintf(stderr, "[localization-clear] serial=%llu gf=%u "
            "columns=%u+%u rows=%u+%u dialogue=%u\n",
            (unsigned long long)s_compose_serial,
            s_compose[slot].game_frame, first_column, column_count,
            first_row, row_count, clears_dialogue);
  return false;
}

static void ObserveEraseSpan(CpuState *cpu, uint32_t cell, uint32_t count) {
  /* $C1E9/$C1ED erase each cell and the cell one row above it. Split at
   * physical row boundaries instead of using a bounding rectangle that could
   * invalidate untouched neighbors. Writes outside BG3's 32x32 map do not
   * belong to any localization surface. */
  while (count && cell < 32u * 33u) {
    const uint32_t row = cell / 32u;
    const uint32_t column = cell % 32u;
    const uint32_t width = count < 32u - column ? count : 32u - column;
    const uint32_t first_row = row ? row - 1u : 0;
    const uint32_t rows = row && row < 32 ? 2 : 1;
    (void)ObserveSurfaceClear(cpu, (uint8_t)column, (uint8_t)first_row,
                             (uint8_t)width, (uint8_t)rows);
    cell += width;
    count -= width;
  }
}

bool ActRaiser_LocalizationObserveTextErase(CpuState *cpu) {
  /* All five decoded native callers use 16-bit indices. An unsupported
   * entry width must not invent a 16-bit footprint for an 8-bit cursor. */
  if (!cpu || cpu->x_flag) return false;
  /* Mirror only $02:C1B7's write footprint: A = row/column, DB:Y = record.
   * Unlike the text composer, this routine interprets just $00 and $0D;
   * every other byte erases one cell. Do not decode words/placeholders here.
   * This bounded, read-only scan runs only on native erasure, never per frame. */
  uint32_t line_cell = (cpu->A >> 8) * 32u + (cpu->A & 0xffu);
  uint32_t count = 0;
  enum { kEraseRecordByteLimit = 2048 };
  for (uint32_t index = 0;
       index < kEraseRecordByteLimit && line_cell < 32u * 33u; ++index) {
    const uint16_t address = (uint16_t)(cpu->Y + index);
    const bool wram = cpu->DB == 0x7e || cpu->DB == 0x7f ||
        ((cpu->DB & 0x7f) < 0x40 && address < 0x2000);
    /* Observation must not duplicate MMIO side effects if a bad/changed
     * record pointer leaves ROM/WRAM. All audited callers use these regions. */
    if (!wram && address < 0x8000) break;
    const uint8_t byte = cpu_read8(cpu, cpu->DB, address);
    if (byte == 0 || byte == 0x0d) {
      ObserveEraseSpan(cpu, line_cell, count);
      if (!byte) return false;
      line_cell += 32u;
      count = 0;
    } else {
      ++count;
    }
  }
  ObserveEraseSpan(cpu, line_cell, count);
  return false;
}

bool ActRaiser_LocalizationObserveMenuClear(CpuState *cpu) {
  /* $01:8CCE clears $7F:B100..B7FF, preserving the status strip. */
  return ObserveSurfaceClear(cpu, 0, 4, 32, 28);
}

bool ActRaiser_LocalizationObserveGeneralClear(CpuState *cpu) {
  /* $02:ABC4/$BA41 clear $7F:B000..B6FF (28 complete rows). */
  return ObserveSurfaceClear(cpu, 0, 0, 32, 28);
}

bool ActRaiser_LocalizationObserveTextCompose(CpuState *cpu) {
  if (!cpu) return false;
  uint64_t serial = s_compose_serial + 1u;
  if (!serial) serial = 1u;
  s_compose_serial = serial;
  const size_t slot =
      (size_t)((serial - 1u) % kComposeObservationCapacity);
  const uint32_t caller_pc24 = ReadLongCallSite(cpu);
  const bool indexed_source = s_pending_source_selector_valid &&
      caller_pc24 == UINT32_C(0x018C93);
  s_compose[slot] = (ActRaiserLocalizationComposeObservation){
    .struct_size = sizeof(s_compose[slot]),
    .abi_version = ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION,
    .serial = serial,
    .source_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y,
    .caller_pc24 = caller_pc24,
    .destination = cpu->A,
    .source_table_pc24 = indexed_source
        ? s_pending_source_table_pc24 : 0,
    .source_selector = indexed_source
        ? s_pending_source_selector : 0,
    .game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame),
    .map_group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup),
    .map_number = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap),
  };
  s_pending_source_table_pc24 = 0;
  s_pending_source_selector = 0;
  s_pending_source_selector_valid = false;
  if (s_compose_count < kComposeObservationCapacity) ++s_compose_count;
  if (TraceEnabled()) {
    const ActRaiserLocalizationComposeObservation *event = &s_compose[slot];
    fprintf(stderr,
            "[localization-compose] serial=%llu gf=%u map=%02X/%02X "
            "source=$%02X:%04X destination=%02X/%02X caller=$%02X:%04X "
            "table=$%02X:%04X selector=%u\n",
            (unsigned long long)event->serial, event->game_frame,
            event->map_group, event->map_number,
            (unsigned)(event->source_pc24 >> 16),
            (unsigned)(event->source_pc24 & 0xffffu),
            (unsigned)(event->destination >> 8),
            (unsigned)(event->destination & 0xffu),
            (unsigned)(event->caller_pc24 >> 16),
            (unsigned)(event->caller_pc24 & 0xffffu),
            (unsigned)(event->source_table_pc24 >> 16),
            (unsigned)(event->source_table_pc24 & 0xffffu),
            event->source_selector);
  }
  return false;
}

RecompReturn ActRaiser_LocalizationTextObserverTrap(CpuState *cpu) {
  (void)cpu;
  return RECOMP_RETURN_NORMAL;
}

bool ActRaiserLocalizationText_CopyObservation(
    ActRaiserLocalizationTextObservation *observation) {
  if (!observation || observation->struct_size < sizeof(*observation))
    return false;
  *observation = s_text;
  return s_text_valid;
}

bool ActRaiserLocalizationText_CopyComposeObservations(
    uint64_t after_serial,
    ActRaiserLocalizationComposeObservation *observations,
    size_t observation_capacity, size_t *observation_count,
    bool *dropped) {
  if (observation_count) *observation_count = 0;
  if (dropped) *dropped = false;
  if (!observation_count || (observation_capacity && !observations))
    return false;
  const uint64_t oldest = s_compose_count
      ? s_compose_serial - s_compose_count + 1u : s_compose_serial + 1u;
  uint64_t first = after_serial == UINT64_MAX
      ? UINT64_MAX : after_serial + 1u;
  if (first < oldest) {
    first = oldest;
    if (dropped) *dropped = true;
  }
  if (first > s_compose_serial || !observation_capacity) return true;
  const uint64_t available = s_compose_serial - first + 1u;
  const size_t count = available < observation_capacity
      ? (size_t)available : observation_capacity;
  for (size_t index = 0; index < count; ++index) {
    const uint64_t event_serial = first + index;
    observations[index] = s_compose[
        (size_t)((event_serial - 1u) % kComposeObservationCapacity)];
  }
  *observation_count = count;
  return true;
}

void ActRaiserLocalizationText_ResetObservation(void) {
  const uint64_t text_serial = s_text.serial;
  memset(&s_text, 0, sizeof(s_text));
  s_text.struct_size = sizeof(s_text);
  s_text.abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION;
  s_text.serial = text_serial;
  s_text_valid = false;
  s_pending_page_advance = false;
  s_pending_page_retains_rows = false;
  s_wrapper_context_pc24 = 0;
  s_wrapper_text_caller_pc24 = 0;
  s_wrapper_context_valid = false;
  s_pending_source_table_pc24 = 0;
  s_pending_source_selector = 0;
  s_pending_source_selector_valid = false;
  memset(s_compose, 0, sizeof(s_compose));
  s_compose_count = 0;
}
