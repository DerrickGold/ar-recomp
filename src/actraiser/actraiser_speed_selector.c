#include "actraiser_speed_selector.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

bool ActRaiserSpeedSelector_Entry(const CpuState *cpu, bool wide) {
  return cpu && cpu->PB == 1 && cpu->DB == 1 && !cpu->D && !cpu->emulation &&
      !cpu->_flag_D && cpu->m_flag == !wide && !cpu->x_flag;
}
static void Require(CpuState *cpu, uint16_t maximum, bool wide) {
  if ((maximum != 7 && maximum != 9) || !ActRaiserSpeedSelector_Entry(cpu, wide))
    ActRaiserHleFatal("Invalid regional message-speed selector prefix");
}

void ActRaiserSpeedSelector_Position(CpuState *cpu, uint16_t maximum) {
  Require(cpu, maximum, true);
  uint16_t cursor = cpu_read16(cpu, 0, 0x0a);
  /* Clamp only the temporary selector: Y/cancel must leave $0200 intact. */
  if (cursor > maximum) cpu_write16(cpu, 0, 0x0a, cursor = maximum);
  /* $8B18 LDA/CLC/ADC/TAX/LDY. Valid positions cannot carry/overflow.
   * The pointer stream draws its mark one column after this base. */
  cpu->A = cpu->X = (uint16_t)(cursor + 0x0b11 + (maximum == 7));
  cpu->_flag_C = cpu->_flag_V = 0;
  cpu->P &= (uint8_t)~0x41u;
  cpu->Y = 0xfa94;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->Y);
}

uint32_t ActRaiserSpeedSelector_Right(CpuState *cpu, uint16_t maximum) {
  Require(cpu, maximum, false);
  const uint8_t cursor = cpu_read8(cpu, 0, 0x0a);
  cpu_write_a8(cpu, cursor);
  cpu->_flag_C = cursor >= maximum;
  cpu->P = (uint8_t)((cpu->P & ~1u) | cpu->_flag_C);
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)(cursor - maximum));
  if (cpu->_flag_C) return 0x018b2f;
  cpu_write8(cpu, 0, 0x0a, (uint8_t)(cursor + 1));
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)(cursor + 1));
  return 0x018b11;
}

void ActRaiserSpeedSelector_DrawNativeScale(CpuState *cpu, uint16_t maximum) {
  Require(cpu, maximum, false);
  if (maximum == 9) return;
  /* $0C12: row12, column18 in the $7F:B000 BG3 shadow. No VRAM/PPU access
   * and no ROM edits. The original composer owns the dirty/upload flag. */
  for (unsigned i = 0; i < 10; ++i)
    cpu_write8(cpu, 0x7f, (uint16_t)(0xb324 + i * 2),
               i == 0 || i == 9 ? ' ' : (uint8_t)('0' + i - 1));
}
