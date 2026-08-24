#include "actraiser/actraiser_action_room_loader.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "actraiser/actraiser_lzss.h"
#include "actraiser_action_room_hle_internal.h"
#include "actraiser_game.h"

enum {
  kDpBgWidth = 0x00,
  kDpBgHeight = 0x02,
  kDpLayerSelector = 0x04,
  kDpMetatileSourceFlags = 0x06,
  kDpSource = 0xA5,
  kDpSourceBank = 0xA7,
  kDpDestination = 0xA8,
  kDpDestinationBank = 0xAA,
  kDpOutputSize = 0xB3,
  kDpOutputAddress = 0xB5,
  kMetatileWorkspace = 0x6000,
  kMetatileBytes = 0x0800,
  kBg1Definitions = 0x2100,
  kBg2Definitions = 0x2900,
  kBg3Definitions = 0x3100,
  kBgDestinationTable = 0x0046,
};

typedef struct ActionRoomLoadDiagnostics {
  unsigned metatile_commands;
  unsigned map_commands;
  size_t staged_bytes;
  bool registered;
} ActionRoomLoadDiagnostics;

static ActionRoomLoadDiagnostics s_diagnostics;

static uint8_t PeekOperand(CpuState *cpu, unsigned operand) {
  return ActionRoomHle_ReadLongIndexed(
      cpu, 0xA2, (uint16_t)(cpu->Y + operand));
}

static uint8_t ReadOperand(CpuState *cpu) {
  const uint8_t value = PeekOperand(cpu, 0);
  cpu->Y = (uint16_t)(cpu->Y + 1u);
  return value;
}

static bool ActionRoomLoadEnabled(void) {
  const char *value = getenv("AR_ACTION_ROOM_LOAD_HLE");
  return !value || !*value || value[0] != '0';
}

static bool IsActionRoomLoad(CpuState *cpu) {
  if (!cpu || !ActionRoomLoadEnabled() || cpu->emulation ||
      !cpu->m_flag || cpu->x_flag)
    return false;
  return ActRaiser_IsActionMapGroup(cpu_read8(
      cpu, kSnesLowWramBank, kActRaiserWram_MapGroup));
}

bool ActRaiser_ActionMetatileLoadHleEnabled(CpuState *cpu) {
  /* Every command-5 invocation in the 49-room action domain loads the full
   * 2 KiB table at workspace offset zero into exactly BG1 or BG2. The guarded
   * native fallback remains authoritative for raw/non-action/BG3 variants. */
  return IsActionRoomLoad(cpu) &&
      PeekOperand(cpu, 0) == 0x00 && PeekOperand(cpu, 1) == 0x20 &&
      PeekOperand(cpu, 2) == 0x00 &&
      (PeekOperand(cpu, 3) == 1 || PeekOperand(cpu, 3) == 2);
}

bool ActRaiser_ActionMapLoadHleEnabled(CpuState *cpu) {
  /* Action rooms use compressed, single-layer selectors only. Raw map loads
   * and selector zero are non-action paths and deliberately stay native. */
  return IsActionRoomLoad(cpu) &&
      (PeekOperand(cpu, 0) == 1 || PeekOperand(cpu, 0) == 2);
}

static uint32_t ReadLinearPointer(CpuState *cpu) {
  const uint32_t value = (uint32_t)ReadOperand(cpu) |
      ((uint32_t)ReadOperand(cpu) << 8) |
      ((uint32_t)ReadOperand(cpu) << 16);
  const uint16_t address = (uint16_t)((value & 0x7FFFu) | 0x8000u);
  const uint8_t bank = (uint8_t)(value >> 15);
  ActionRoomHle_WriteDirectPage16(cpu, kDpSource, address);
  ActionRoomHle_WriteDirectPage8(cpu, kDpSourceBank, bank);
  return ((uint32_t)bank << 16) | address;
}

static uint16_t ReadSource16(CpuState *cpu) {
  return cpu_read16(cpu,
                    ActionRoomHle_ReadDirectPage8(cpu, kDpSourceBank),
                    ActionRoomHle_ReadDirectPage16(cpu, kDpSource));
}

static uint8_t ReadSource8(CpuState *cpu) {
  return cpu_read8(cpu,
                   ActionRoomHle_ReadDirectPage8(cpu, kDpSourceBank),
                   ActionRoomHle_ReadDirectPage16(cpu, kDpSource));
}

static void AdvanceSource(CpuState *cpu, uint16_t bytes) {
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpSource,
      (uint16_t)(ActionRoomHle_ReadDirectPage16(cpu, kDpSource) + bytes));
}

static void SetAccumulator16(CpuState *cpu) {
  cpu->P = (uint8_t)(cpu->P & ~CPU_P_M);
  cpu_p_to_mirrors(cpu);
}

static void RestoreStatus(CpuState *cpu, uint8_t status) {
  cpu->P = status;
  cpu_p_to_mirrors(cpu);
}

static RecompReturn CallLzss(CpuState *cpu, uint16_t return_pc) {
  /* Reproduce the JSL frame that the existing $02:C5C9 HLE consumes. The
   * return bytes also preserve the native stack residue below this handler. */
  cpu_write8(cpu, 0x00, cpu->S, cpu->PB);
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu_write8(cpu, 0x00, cpu->S, (uint8_t)(return_pc >> 8));
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu_write8(cpu, 0x00, cpu->S, (uint8_t)return_pc);
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu->host_return_valid = 1;
  const uint8_t saved_pb = cpu->PB;
  cpu->PB = 0x02;
  const RecompReturn result = ActRaiser_LzssDecompress(cpu);
  cpu->PB = saved_pb;
  return result;
}

static void CopyMetatileSlice(CpuState *cpu, uint16_t destination,
                              uint16_t source_begin, uint16_t source_end,
                              uint16_t destination_offset) {
  ActionRoomHle_WriteDirectPage16(cpu, kDpDestination, destination);
  const uint8_t destination_bank =
      ActionRoomHle_ReadDirectPage8(cpu, kDpDestinationBank);
  uint16_t source = source_begin;
  uint16_t output = destination_offset;
  while (source != source_end) {
    const uint16_t word = cpu_read16(
        cpu, kSnesLowWramBank,
        (uint16_t)(kMetatileWorkspace + source));
    const uint16_t swapped = (uint16_t)((word << CHAR_BIT) | (word >> CHAR_BIT));
    cpu_write16(cpu, destination_bank,
                (uint16_t)(destination + output), swapped);
    source = (uint16_t)(source + 2u);
    output = (uint16_t)(output + 2u);
  }
  cpu->X = source;
}

static void ReportDiagnostics(void) {
  if (!s_diagnostics.metatile_commands && !s_diagnostics.map_commands) return;
  fprintf(stderr,
          "[action-room-load-hle] summary command5=%u command4=%u bytes=%zu\n",
          s_diagnostics.metatile_commands, s_diagnostics.map_commands,
          s_diagnostics.staged_bytes);
}

static void RegisterDiagnostics(void) {
  if (s_diagnostics.registered) return;
  s_diagnostics.registered = true;
  atexit(ReportDiagnostics);
}

RecompReturn ActRaiser_LoadActionMetatiles(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$02:B363",
      kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes);
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p); /* PHP */
  cpu->S = (uint16_t)(cpu->S - 1u);
  SetAccumulator16(cpu);

  const uint8_t source_begin_operand = ReadOperand(cpu);
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpMetatileSourceFlags,
      (uint16_t)source_begin_operand << CHAR_BIT);
  const uint16_t source_begin = (uint16_t)(
      ((unsigned)source_begin_operand & 0x7Fu) << 6);
  const uint16_t source_end = (uint16_t)((unsigned)ReadOperand(cpu) << 6);
  const uint16_t destination_offset =
      (uint16_t)((unsigned)ReadOperand(cpu) << 6);
  ActionRoomHle_WriteDirectPage16(cpu, kDpBgWidth, source_begin);
  ActionRoomHle_WriteDirectPage16(cpu, kDpBgHeight, source_end);
  ActionRoomHle_WriteDirectPage16(cpu, kDpLayerSelector,
                                  destination_offset);

  uint16_t selector = ReadOperand(cpu);
  ActRaiserCpuHle_PushWord(cpu, selector);
  cpu->X = kDpSource;
  (void)ReadLinearPointer(cpu);

  const uint16_t output_size = ReadSource16(cpu);
  ActionRoomHle_WriteDirectPage16(cpu, kDpOutputSize, output_size);
  AdvanceSource(cpu, 2);
  cpu->X = kMetatileWorkspace;
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpOutputAddress, kMetatileWorkspace);
  const RecompReturn decompressed = CallLzss(cpu, 0xB3B8);
  if (decompressed != RECOMP_RETURN_NORMAL) return decompressed;

  selector = ActRaiserCpuHle_PopWord(cpu);
  static const uint16_t kDestinations[] = {
    kBg1Definitions, kBg2Definitions, kBg3Definitions,
  };
  for (unsigned layer = 0; layer < 3; layer++) {
    cpu->X = kDestinations[layer];
    const bool selected = (selector & 1u) != 0;
    selector >>= 1;
    if (!selected) continue;
    ActRaiserCpuHle_PushWord(cpu, selector);
    ActRaiserCpuHle_PushWord(cpu, cpu->Y);
    CopyMetatileSlice(cpu, kDestinations[layer], source_begin, source_end,
                      destination_offset);
    cpu->Y = ActRaiserCpuHle_PopWord(cpu);
    selector = ActRaiserCpuHle_PopWord(cpu);
  }
  cpu->A = selector;
  cpu->X = kBg3Definitions;

  RestoreStatus(cpu, saved_p);
  cpu->S = (uint16_t)(cpu->S + 1u + k65816RtsStackBytes);
  RegisterDiagnostics();
  s_diagnostics.metatile_commands++;
  s_diagnostics.staged_bytes += output_size;
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_LoadActionMap(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$02:B3EB",
      kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes);
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p); /* PHP */
  cpu->S = (uint16_t)(cpu->S - 1u);
  SetAccumulator16(cpu);

  uint16_t selector = ReadOperand(cpu);
  ActionRoomHle_WriteDirectPage16(cpu, kDpLayerSelector, selector);
  cpu->X = kDpSource;
  (void)ReadLinearPointer(cpu);
  const uint8_t pages_wide = ReadSource8(cpu);
  AdvanceSource(cpu, 1);
  const uint8_t pages_high = ReadSource8(cpu);
  AdvanceSource(cpu, 1);
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpBgWidth, (uint16_t)pages_wide << CHAR_BIT);
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpBgHeight, (uint16_t)pages_high << CHAR_BIT);
  ActionRoomHle_WriteDirectPage16(
      cpu, kDpOutputSize,
      (uint16_t)((unsigned)pages_wide * pages_high << CHAR_BIT));

  /* The guarded action domain is compressed, so its asset header supersedes
   * the dimensions-derived raw-copy size exactly as native $B420 does. */
  const uint16_t output_size = ReadSource16(cpu);
  ActionRoomHle_WriteDirectPage16(cpu, kDpOutputSize, output_size);
  AdvanceSource(cpu, 2);
  selector &= 0x7FFFu;

  unsigned destination_index = 0;
  while (destination_index < 2 && !(selector & 1u)) {
    selector >>= 1;
    destination_index++;
  }
  if (destination_index < 2) {
    selector >>= 1;
    cpu->X = (uint16_t)(destination_index * 4u);
    ActRaiserCpuHle_PushWord(cpu, selector);
    ActRaiserCpuHle_PushWord(cpu, cpu->Y);
    ActionRoomHle_WriteDirectPage16(
        cpu, (uint16_t)(kActRaiserWram_Bg1Width + cpu->X),
        ActionRoomHle_ReadDirectPage16(cpu, kDpBgWidth));
    ActionRoomHle_WriteDirectPage16(
        cpu, (uint16_t)(kActRaiserWram_Bg1Height + cpu->X),
        ActionRoomHle_ReadDirectPage16(cpu, kDpBgHeight));
    const uint16_t destination = cpu_read16(
        cpu, cpu->DB, (uint16_t)(kBgDestinationTable + cpu->X));
    ActionRoomHle_WriteDirectPage16(cpu, kDpOutputAddress, destination);
    const RecompReturn decompressed = CallLzss(cpu, 0xB470);
    if (decompressed != RECOMP_RETURN_NORMAL) return decompressed;
    cpu->Y = ActRaiserCpuHle_PopWord(cpu);
    selector = ActRaiserCpuHle_PopWord(cpu);
  }

  cpu->A = selector;
  RestoreStatus(cpu, saved_p);
  cpu->S = (uint16_t)(cpu->S + 1u + k65816RtsStackBytes);
  RegisterDiagnostics();
  s_diagnostics.map_commands++;
  s_diagnostics.staged_bytes += output_size;
  return RECOMP_RETURN_NORMAL;
}
