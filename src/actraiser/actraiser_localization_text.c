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
static ActRaiserLocalizationComposeObservation
    s_compose[kComposeObservationCapacity];
static uint64_t s_compose_serial;
static size_t s_compose_count;
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
  };
  s_text_valid = true;
  s_pending_page_advance = false;
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
    s_text.page_unit_index = 0;
    s_pending_page_advance = false;
  }
  s_text.cursor_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y;
  s_text.game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame);
  const uint8_t code = cpu_read8(cpu, cpu->DB, cpu->Y);
  if (s_text.page_unit_index != UINT16_MAX) ++s_text.page_unit_index;
  /* The native $02 page operation waits and scrolls before the next reader
   * entry. Publish the new page only when that following token is reached. */
  if (code == 0x02u)
    s_pending_page_advance = true;
  else if (code == 0x00u) {
    s_text.terminal_compose_serial = s_compose_serial;
    s_text.terminal = true;
  }
  return false;
}

bool ActRaiser_LocalizationObserveTextCompose(CpuState *cpu) {
  if (!cpu) return false;
  uint64_t serial = s_compose_serial + 1u;
  if (!serial) serial = 1u;
  s_compose_serial = serial;
  const size_t slot =
      (size_t)((serial - 1u) % kComposeObservationCapacity);
  s_compose[slot] = (ActRaiserLocalizationComposeObservation){
    .struct_size = sizeof(s_compose[slot]),
    .abi_version = ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION,
    .serial = serial,
    .source_pc24 = ((uint32_t)cpu->DB << 16) | cpu->Y,
    .caller_pc24 = ReadLongCallSite(cpu),
    .destination = cpu->A,
    .game_frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame),
    .map_group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup),
    .map_number = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap),
  };
  if (s_compose_count < kComposeObservationCapacity) ++s_compose_count;
  if (TraceEnabled()) {
    const ActRaiserLocalizationComposeObservation *event = &s_compose[slot];
    fprintf(stderr,
            "[localization-compose] serial=%llu gf=%u map=%02X/%02X "
            "source=$%02X:%04X destination=%02X/%02X caller=$%02X:%04X\n",
            (unsigned long long)event->serial, event->game_frame,
            event->map_group, event->map_number,
            (unsigned)(event->source_pc24 >> 16),
            (unsigned)(event->source_pc24 & 0xffffu),
            (unsigned)(event->destination >> 8),
            (unsigned)(event->destination & 0xffu),
            (unsigned)(event->caller_pc24 >> 16),
            (unsigned)(event->caller_pc24 & 0xffffu));
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

bool ActRaiserLocalizationText_TerminalWasReplaced(
    const ActRaiserLocalizationTextObservation *observation) {
  return observation &&
      observation->abi_version ==
          ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION &&
      observation->terminal &&
      s_compose_serial != observation->terminal_compose_serial;
}

void ActRaiserLocalizationText_ResetObservation(void) {
  const uint64_t text_serial = s_text.serial;
  memset(&s_text, 0, sizeof(s_text));
  s_text.struct_size = sizeof(s_text);
  s_text.abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION;
  s_text.serial = text_serial;
  s_text_valid = false;
  s_pending_page_advance = false;
  s_wrapper_context_pc24 = 0;
  s_wrapper_text_caller_pc24 = 0;
  s_wrapper_context_valid = false;
  memset(s_compose, 0, sizeof(s_compose));
  s_compose_count = 0;
}
