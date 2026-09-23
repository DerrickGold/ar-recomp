#include "actraiser_lives_display.h"
#include "actraiser_hle_fatal.h"

bool ActRaiserLivesDisplay_Entry(const CpuState *cpu) {
  return cpu && cpu->PB == 2 && !cpu->D && !cpu->emulation &&
      !cpu->m_flag && !cpu->x_flag && cpu->X == 0x50 && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}

void ActRaiserLivesDisplay_DrawZeroBased(CpuState *cpu) {
  if (!ActRaiserLivesDisplay_Entry(cpu)) ActRaiserHleFatal("Unsupported action lives HUD entry");
  const uint8_t lives = cpu_read8(cpu, 0, 0x1c);
  /* Nibble-to-glyph behavior also preserves the native handling of malformed
   * debug values. This is deliberately not a decimal conversion of stock. */
  cpu_write8(cpu, 0x7f, (uint16_t)(0xb000 + cpu->X), (uint8_t)(0x30 + (lives >> 4)));
  const uint8_t ones = (uint8_t)(0x30 + (lives & 15));
  cpu_write8(cpu, 0x7f, (uint16_t)(0xb002 + cpu->X), ones);
  cpu->A = (cpu->A & 0xff00) | ones;
  cpu_mirrors_to_p(cpu);
  cpu->P = (cpu->P & (uint8_t)~(CPU_P_N | CPU_P_Z | CPU_P_C | CPU_P_V)) | CPU_P_M;
  cpu_p_to_mirrors(cpu);
}
