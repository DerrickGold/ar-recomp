#include "actraiser/actraiser_action_room_graphics.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "actraiser/actraiser_lzss.h"
#include "actraiser_game.h"

enum {
  kDpUploadBegin = 0x00,
  kDpUploadEnd = 0x02,
  kDpScript = 0xA2,
  kDpSource = 0xA5,
  kDpSourceBank = 0xA7,
  kDpOutputSize = 0xB3,
  kDpOutputAddress = 0xB5,
  kCharacterWorkspace = 0x6000,
  kPpuVramAddress = 0x2116,
  kPpuVramDataLow = 0x2118,
  kPpuCgramAddress = 0x2121,
  kPpuCgramData = 0x2122,
};

typedef struct ActionRoomGraphicsDiagnostics {
  unsigned character_commands;
  unsigned palette_commands;
  size_t staged_bytes;
  bool registered;
} ActionRoomGraphicsDiagnostics;

static ActionRoomGraphicsDiagnostics s_diagnostics;

static uint8_t ReadDp8(CpuState *cpu, uint16_t offset) {
  return cpu_read8(cpu, kSnesLowWramBank,
                   (uint16_t)(cpu->D + offset));
}

static uint16_t ReadDp16(CpuState *cpu, uint16_t offset) {
  return cpu_read16(cpu, kSnesLowWramBank,
                    (uint16_t)(cpu->D + offset));
}

static void WriteDp8(CpuState *cpu, uint16_t offset, uint8_t value) {
  cpu_write8(cpu, kSnesLowWramBank,
             (uint16_t)(cpu->D + offset), value);
}

static void WriteDp16(CpuState *cpu, uint16_t offset, uint16_t value) {
  cpu_write16(cpu, kSnesLowWramBank,
              (uint16_t)(cpu->D + offset), value);
}

static uint8_t ReadLongIndexed(CpuState *cpu, uint16_t pointer,
                               uint16_t index) {
  const uint32_t base = (uint32_t)ReadDp16(cpu, pointer) |
      ((uint32_t)ReadDp8(cpu, (uint16_t)(pointer + 2u)) << 16);
  const uint32_t address = (base + index) & 0xFFFFFFu;
  return cpu_read8(cpu, (uint8_t)(address >> 16), (uint16_t)address);
}

static uint8_t PeekOperand(CpuState *cpu, unsigned operand) {
  return ReadLongIndexed(cpu, kDpScript, (uint16_t)(cpu->Y + operand));
}

static uint8_t ReadOperand(CpuState *cpu) {
  const uint8_t value = PeekOperand(cpu, 0);
  cpu->Y = (uint16_t)(cpu->Y + 1u);
  return value;
}

static bool ActionRoomGraphicsEnabled(void) {
  const char *value = getenv("AR_ACTION_ROOM_GFX_HLE");
  return !value || !*value || value[0] != '0';
}

static bool IsActionRoomGraphics(CpuState *cpu) {
  if (!cpu || !ActionRoomGraphicsEnabled() || cpu->emulation ||
      !cpu->m_flag || cpu->x_flag || cpu->DB != 0)
    return false;
  return ActRaiser_IsActionMapGroup(cpu_read8(
      cpu, kSnesLowWramBank, kActRaiserWram_MapGroup));
}

static uint32_t PeekLinearPointer(CpuState *cpu, unsigned operand) {
  return (uint32_t)PeekOperand(cpu, operand) |
      ((uint32_t)PeekOperand(cpu, operand + 1u) << CHAR_BIT) |
      ((uint32_t)PeekOperand(cpu, operand + 2u) << (CHAR_BIT * 2));
}

static uint32_t LinearToSnes(uint32_t linear) {
  const uint16_t address = (uint16_t)((linear & 0x7FFFu) | 0x8000u);
  const uint8_t bank = (uint8_t)(linear >> 15);
  return ((uint32_t)bank << 16) | address;
}

static bool IsKnownCharacterDestination(uint8_t destination) {
  switch (destination) {
    case 0x00:
    case 0x10:
    case 0x30:
    case 0x40:
    case 0x50:
      return true;
    default:
      return false;
  }
}

bool ActRaiser_ActionCharacterLoadHleEnabled(CpuState *cpu) {
  if (!IsActionRoomGraphics(cpu) || PeekOperand(cpu, 0) != 0 ||
      !IsKnownCharacterDestination(PeekOperand(cpu, 2)))
    return false;

  const uint8_t end = PeekOperand(cpu, 1);
  if (end != 0x08 && end != 0x10) return false;
  const uint16_t expected_bytes = (uint16_t)((unsigned)end << 9);
  const uint32_t source = LinearToSnes(PeekLinearPointer(cpu, 3));
  return cpu_read16(cpu, (uint8_t)(source >> 16), (uint16_t)source) ==
      expected_bytes;
}

bool ActRaiser_ActionPaletteLoadHleEnabled(CpuState *cpu) {
  if (!IsActionRoomGraphics(cpu) || PeekOperand(cpu, 0) != 0 ||
      PeekOperand(cpu, 1) != 0x40)
    return false;
  const uint8_t destination = PeekOperand(cpu, 2);
  return destination == 0x00 || destination == 0x40 ||
      destination == 0x80;
}

static void RequireEntryMode(CpuState *cpu, const char *routine) {
  if (cpu && !cpu->emulation && cpu->m_flag && !cpu->x_flag && cpu->DB == 0)
    return;
  fprintf(stderr,
          "FATAL: %s HLE requires DB=0 native mode with 8-bit A and "
          "16-bit X/Y\n", routine);
  abort();
}

static void PushWord(CpuState *cpu, uint16_t value) {
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu_write16(cpu, 0x00, cpu->S, value);
  cpu->S = (uint16_t)(cpu->S - 1u);
}

static uint16_t PopWord(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1u);
  const uint16_t value = cpu_read16(cpu, 0x00, cpu->S);
  cpu->S = (uint16_t)(cpu->S + 1u);
  return value;
}

static uint32_t ReadLinearPointer(CpuState *cpu) {
  const uint32_t linear = (uint32_t)ReadOperand(cpu) |
      ((uint32_t)ReadOperand(cpu) << CHAR_BIT) |
      ((uint32_t)ReadOperand(cpu) << (CHAR_BIT * 2));
  const uint32_t source = LinearToSnes(linear);
  WriteDp16(cpu, kDpSource, (uint16_t)source);
  WriteDp8(cpu, kDpSourceBank, (uint8_t)(source >> 16));
  /* $02:B4C0 returns the masked middle linear-address byte in A. This only
   * survives command 6's 8-bit copy as its (zero) accumulator high byte. */
  cpu->A = (uint16_t)((linear >> CHAR_BIT) & 0x7Fu);
  cpu->host_return_valid = 1;
  return source;
}

static void AdvanceSource(CpuState *cpu, uint16_t bytes) {
  WriteDp16(cpu, kDpSource,
            (uint16_t)(ReadDp16(cpu, kDpSource) + bytes));
}

static void SetAccumulator16(CpuState *cpu) {
  cpu->P = (uint8_t)(cpu->P & ~CPU_P_M);
  cpu_p_to_mirrors(cpu);
}

static void SetAccumulator8(CpuState *cpu) {
  cpu->P = (uint8_t)(cpu->P | CPU_P_M);
  cpu_p_to_mirrors(cpu);
}

static void RestoreStatus(CpuState *cpu, uint8_t status) {
  cpu->P = status;
  cpu_p_to_mirrors(cpu);
}

static RecompReturn CallLzss(CpuState *cpu, uint16_t return_pc) {
  cpu_write8(cpu, 0x00, cpu->S, cpu->PB);
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu_write8(cpu, 0x00, cpu->S, (uint8_t)(return_pc >> CHAR_BIT));
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

static void ReportDiagnostics(void) {
  if (!s_diagnostics.character_commands && !s_diagnostics.palette_commands)
    return;
  fprintf(stderr,
          "[action-room-gfx-hle] summary command7=%u command6=%u bytes=%zu\n",
          s_diagnostics.character_commands, s_diagnostics.palette_commands,
          s_diagnostics.staged_bytes);
}

static void RegisterDiagnostics(void) {
  if (s_diagnostics.registered) return;
  s_diagnostics.registered = true;
  atexit(ReportDiagnostics);
}

RecompReturn ActRaiser_LoadActionCharacters(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  RequireEntryMode(cpu, "$02:B28E");
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p); /* PHP */
  cpu->S = (uint16_t)(cpu->S - 1u);
  SetAccumulator16(cpu);

  const uint16_t source_begin = (uint16_t)((unsigned)ReadOperand(cpu) << 8);
  const uint16_t copy_bytes = (uint16_t)((unsigned)ReadOperand(cpu) << 9);
  WriteDp16(cpu, kDpUploadBegin, source_begin);
  WriteDp16(cpu, kDpUploadEnd, copy_bytes);

  const uint16_t destination = ReadOperand(cpu);
  PushWord(cpu, destination);
  cpu_write16(cpu, cpu->DB, kPpuVramAddress,
              (uint16_t)(destination << 8));
  cpu->X = kDpSource;
  (void)ReadLinearPointer(cpu);

  /* The guard admits only compressed action shapes (source_begin == 0).
   * Native reads the decompressed byte count from the asset header, advances
   * the mapped source by two, and expands through the shared WRAM workspace. */
  const uint16_t output_size = cpu_read16(
      cpu, ReadDp8(cpu, kDpSourceBank), ReadDp16(cpu, kDpSource));
  WriteDp16(cpu, kDpOutputSize, output_size);
  AdvanceSource(cpu, 2);
  cpu->X = kCharacterWorkspace;
  WriteDp16(cpu, kDpOutputAddress, kCharacterWorkspace);
  const RecompReturn decompressed = CallLzss(cpu, 0xB2FE);
  if (decompressed != RECOMP_RETURN_NORMAL) return decompressed;

  (void)PopWord(cpu);
  cpu->X = source_begin;
  while (cpu->X != copy_bytes) {
    cpu->A = cpu_read16(
        cpu, kSnesLowWramBank,
        (uint16_t)(kCharacterWorkspace + cpu->X));
    cpu_write16(cpu, cpu->DB, kPpuVramDataLow, cpu->A);
    cpu->X = (uint16_t)(cpu->X + 2u);
  }

  RestoreStatus(cpu, saved_p);
  cpu->S = (uint16_t)(cpu->S + 1u + k65816RtsStackBytes);
  RegisterDiagnostics();
  s_diagnostics.character_commands++;
  s_diagnostics.staged_bytes += copy_bytes;
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_LoadActionPalette(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  RequireEntryMode(cpu, "$02:B330");
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0x00, cpu->S, saved_p); /* PHP */
  cpu->S = (uint16_t)(cpu->S - 1u);
  SetAccumulator16(cpu);

  const uint16_t source_begin = (uint16_t)((unsigned)ReadOperand(cpu) * 2u);
  const uint16_t source_end = (uint16_t)((unsigned)ReadOperand(cpu) * 2u);
  WriteDp16(cpu, kDpUploadBegin, source_begin);
  WriteDp16(cpu, kDpUploadEnd, source_end);
  const uint8_t destination = ReadOperand(cpu);
  SetAccumulator8(cpu);
  cpu_write8(cpu, cpu->DB, kPpuCgramAddress, destination);

  cpu->X = kDpSource;
  (void)ReadLinearPointer(cpu);
  const uint16_t script_y = cpu->Y;
  PushWord(cpu, script_y); /* PHY */
  cpu->Y = source_begin;
  while (cpu->Y != source_end) {
    const uint8_t low = ReadLongIndexed(cpu, kDpSource, cpu->Y++);
    cpu->A = (uint16_t)((cpu->A & 0xFF00u) | low);
    cpu_write8(cpu, cpu->DB, kPpuCgramData, low);
    const uint8_t high = ReadLongIndexed(cpu, kDpSource, cpu->Y++);
    cpu->A = (uint16_t)((cpu->A & 0xFF00u) | high);
    cpu_write8(cpu, cpu->DB, kPpuCgramData, high);
  }
  cpu->Y = PopWord(cpu); /* PLY */

  RestoreStatus(cpu, saved_p);
  cpu->S = (uint16_t)(cpu->S + 1u + k65816RtsStackBytes);
  RegisterDiagnostics();
  s_diagnostics.palette_commands++;
  s_diagnostics.staged_bytes += (size_t)(source_end - source_begin);
  return RECOMP_RETURN_NORMAL;
}
