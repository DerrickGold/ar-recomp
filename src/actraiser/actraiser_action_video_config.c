#include "actraiser/actraiser_action_video_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "actraiser_action_room_hle_internal.h"
#include "actraiser_game.h"

enum {
  kDpScript = 0xA2,
  kVideoProfileBank = 0x02,
  kVideoProfileAddress = 0x893E,
  kVideoProfileBytes = 28,

  kPpuBgMode = 0x2105,
  kPpuBg1Screen = 0x2107,
  kPpuBg2Screen = 0x2108,
  kPpuMainScreen = 0x212C,
  kPpuSubScreen = 0x212D,
  kPpuMainWindow = 0x212E,
  kPpuSubWindow = 0x212F,
  kPpuColorMathControl = 0x2130,
  kPpuColorMathLayers = 0x2131,
  kPpuFixedColor = 0x2132,
};

typedef struct ActionVideoConfigDiagnostics {
  unsigned commands;
  size_t applied_bytes;
  bool registered;
} ActionVideoConfigDiagnostics;

static ActionVideoConfigDiagnostics s_diagnostics;

static uint8_t PeekOperand(CpuState *cpu) {
  return ActionRoomHle_ReadLongIndexed(cpu, kDpScript, cpu->Y);
}

static bool ActionVideoConfigEnabled(void) {
  const char *value = getenv("AR_ACTION_ROOM_VIDEO_HLE");
  return !value || !*value || value[0] != '0';
}

static bool IsAuditedActionProfile(uint8_t profile) {
  /* Stock action scripts use every profile in $03-$2E except $08. The shared
   * command handler has additional title/SIM call shapes, which stay native. */
  return profile >= 0x03 && profile <= 0x2E && profile != 0x08;
}

bool ActRaiser_ActionVideoConfigHleEnabled(CpuState *cpu) {
  if (!cpu || !ActionVideoConfigEnabled() || cpu->emulation ||
      !cpu->m_flag || cpu->x_flag || cpu->D != 0 || cpu->DB != 0)
    return false;
  if (!ActRaiser_IsActionMapGroup(cpu_read8(
          cpu, kSnesLowWramBank, kActRaiserWram_MapGroup)))
    return false;
  return IsAuditedActionProfile(PeekOperand(cpu));
}

static void PushWordResidue(CpuState *cpu, uint16_t stack,
                            uint16_t value) {
  cpu_write8(cpu, 0x00, stack, (uint8_t)value);
  cpu_write8(cpu, 0x00, (uint16_t)(stack + 1u),
             (uint8_t)(value >> 8));
}

static void WritePpu(CpuState *cpu, uint16_t address, uint8_t value) {
  cpu_write8(cpu, kVideoProfileBank, address, value);
}

static uint8_t ReadProfile(CpuState *cpu, uint16_t offset, unsigned byte) {
  return cpu_read8(cpu, kVideoProfileBank,
                   (uint16_t)(kVideoProfileAddress + offset + byte));
}

static void ReportDiagnostics(void) {
  if (!s_diagnostics.commands) return;
  fprintf(stderr,
          "[action-room-video-hle] summary command3=%u bytes=%zu\n",
          s_diagnostics.commands, s_diagnostics.applied_bytes);
}

static void RegisterDiagnostics(void) {
  if (s_diagnostics.registered) return;
  s_diagnostics.registered = true;
  atexit(ReportDiagnostics);
}

RecompReturn ActRaiser_ApplyActionVideoConfig(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$02:B4E8",
      kActRaiserCpuHleEntryMode_DirectPageAndDbZeroNative8BitAccumulator16BitIndexes);

  const uint8_t saved_p = cpu->P;
  const uint8_t saved_db = cpu->DB;
  const uint16_t entry_s = cpu->S;

  /* PHB; LDA #$02; PHA; PLB. Keep the observable stack bytes and DB lifetime
   * even though the HLE reads the fixed profile bank explicitly. */
  cpu_write8(cpu, 0x00, cpu->S, saved_db);
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu->A = (uint16_t)((cpu->A & 0xFF00u) | kVideoProfileBank);
  cpu_write8(cpu, 0x00, cpu->S, kVideoProfileBank);
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu->S = (uint16_t)(cpu->S + 1u);
  cpu->DB = cpu_read8(cpu, 0x00, cpu->S);

  const uint8_t profile_index = PeekOperand(cpu);
  cpu->Y = (uint16_t)(cpu->Y + 1u);
  const uint16_t script_y = cpu->Y;
  const uint16_t profile_offset =
      (uint16_t)((unsigned)profile_index * kVideoProfileBytes);
  cpu->A = profile_offset;
  cpu->X = profile_offset;
  cpu->host_return_valid = 1;

  uint8_t profile[kVideoProfileBytes];
  for (unsigned i = 0; i < kVideoProfileBytes; i++)
    profile[i] = ReadProfile(cpu, profile_offset, i);

  WritePpu(cpu, kPpuMainScreen, profile[0]);
  WritePpu(cpu, kPpuMainWindow, profile[0]);
  WritePpu(cpu, kPpuSubScreen, profile[1]);
  WritePpu(cpu, kPpuSubWindow, profile[1]);
  WritePpu(cpu, kPpuColorMathControl, profile[2]);
  WritePpu(cpu, kPpuColorMathLayers, profile[3]);
  WritePpu(cpu, kPpuFixedColor, 0xE0);

  /* Clear only the attribute/high bytes, exactly like STZ $6B/$6F/$73/$90.
   * Low bytes are intentionally retained when the corresponding priority bit
   * is clear. A set bit writes the complete common attribute word. */
  ActionRoomHle_WriteDirectPage8(cpu, 0x6B, 0);
  ActionRoomHle_WriteDirectPage8(cpu, 0x6F, 0);
  ActionRoomHle_WriteDirectPage8(cpu, 0x73, 0);
  ActionRoomHle_WriteDirectPage8(cpu, 0x90, 0);
  if (profile[4] & 0x01)
    ActionRoomHle_WriteDirectPage16(cpu, 0x6A, 0x2000);
  if (profile[4] & 0x02)
    ActionRoomHle_WriteDirectPage16(cpu, 0x6E, 0x2000);
  if (profile[4] & 0x04)
    ActionRoomHle_WriteDirectPage16(cpu, 0x72, 0x2000);
  if (profile[4] & 0x08)
    ActionRoomHle_WriteDirectPage16(cpu, 0x8F, 0x1000);
  ActionRoomHle_WriteDirectPage8(
      cpu, 0x6B,
      (uint8_t)(ActionRoomHle_ReadDirectPage8(cpu, 0x6B) | 0x10));
  ActionRoomHle_WriteDirectPage8(
      cpu, 0x6F,
      (uint8_t)(ActionRoomHle_ReadDirectPage8(cpu, 0x6F) | 0x01));
  /* $90's OAM attribute-bias arm is unconditional in the native handler. */
  ActionRoomHle_WriteDirectPage8(
      cpu, 0x90,
      (uint8_t)(ActionRoomHle_ReadDirectPage8(cpu, 0x90) | 0x20));

  WritePpu(cpu, kPpuBg1Screen, (uint8_t)(0x60 | (profile[5] & 0x03)));
  WritePpu(cpu, kPpuBg2Screen,
           (uint8_t)(0x70 | ((profile[5] >> 2) & 0x03)));
  WritePpu(cpu, kPpuBgMode, profile[6]);

  for (unsigned plane = 0; plane < 6; plane++) {
    const uint8_t ratio = profile[7 + plane];
    ActionRoomHle_WriteDirectPage8(
        cpu, (uint16_t)(0x3A + plane * 2u), (uint8_t)(ratio >> 4));
    ActionRoomHle_WriteDirectPage8(
        cpu, (uint16_t)(0x3B + plane * 2u),
        (uint8_t)(ratio & 0x0F));
  }

  ActionRoomHle_WriteDirectPage8(cpu, 0xBB, profile[13]);
  ActionRoomHle_WriteDirectPage8(cpu, 0xBA, profile[14]);
  ActionRoomHle_WriteDirectPage8(cpu, 0xB9, profile[15]);
  ActionRoomHle_WriteDirectPage8(cpu, 0xBF, profile[16]);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC1, profile[17]);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC4, profile[18]);
  if (!(profile[18] & 0x40) && (profile[18] & 0x20))
    ActionRoomHle_WriteDirectPage8(
        cpu, 0xC1, (profile[18] & 0x80) ? 0xFF : 0x00);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC2, 1);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC3, 0);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC0, 0);

  ActionRoomHle_WriteDirectPage8(cpu, 0xC5, profile[19]);
  ActionRoomHle_WriteDirectPage8(
      cpu, 0xC9, (uint8_t)(profile[20] << 4));
  ActionRoomHle_WriteDirectPage8(cpu, 0xC6, profile[21]);
  ActionRoomHle_WriteDirectPage8(
      cpu, 0xCA, (uint8_t)(profile[22] << 4));
  ActionRoomHle_WriteDirectPage8(cpu, 0xC7, 0);
  ActionRoomHle_WriteDirectPage8(cpu, 0xC8, 0);

  ActionRoomHle_WriteDirectPage16(
      cpu, 0xDA, (profile[23] & 0x80) ? 0x1000 : 0x0000);
  ActionRoomHle_WriteDirectPage16(
      cpu, 0xE1, (uint16_t)((profile[23] & 0x70) << 3));
  ActionRoomHle_WriteDirectPage8(
      cpu, 0xDF, (uint8_t)((profile[23] & 0x0F) - 1u));
  ActionRoomHle_WriteDirectPage8(
      cpu, 0xDE, (uint8_t)(profile[24] - 1u));
  ActionRoomHle_WriteDirectPage16(
      cpu, 0xE6,
      (uint16_t)(profile[25] | ((uint16_t)profile[26] << 8)));
  ActionRoomHle_WriteDirectPage8(cpu, 0xE5, 0x3B);
  ActionRoomHle_WriteDirectPage8(cpu, 0xE8, 0);
  ActionRoomHle_WriteDirectPage16(cpu, 0xF2, profile[27]);

  /* The two native PHY lifetimes leave the advanced script cursor at these
   * four stack bytes; PLB leaves the saved DB at entry_s. Preserve this
   * contract before applying the final PLY/PLB/RTS register effects. */
  PushWordResidue(cpu, (uint16_t)(entry_s - 4u), script_y);
  PushWordResidue(cpu, (uint16_t)(entry_s - 2u), script_y);
  cpu_write8(cpu, 0x00, entry_s, saved_db);
  cpu->Y = script_y;
  cpu->X = profile_offset;
  cpu->A = profile[27];
  cpu->DB = saved_db;
  cpu->S = (uint16_t)(entry_s + k65816RtsStackBytes);

  /* The final 16-bit shifts leave C clear; the last BGSC ADC leaves V clear;
   * SEP restores M; PLY is superseded by PLB, whose restored DB=0 sets Z. */
  cpu->P = (uint8_t)((saved_p & (CPU_P_I | CPU_P_D)) |
                     CPU_P_M | CPU_P_Z);
  cpu_p_to_mirrors(cpu);

  RegisterDiagnostics();
  s_diagnostics.commands++;
  s_diagnostics.applied_bytes += kVideoProfileBytes;
  return RECOMP_RETURN_NORMAL;
}
