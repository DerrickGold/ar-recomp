#include "actraiser_magic_gesture.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

bool ActRaiserMagicGesture_Entry(const CpuState *cpu) {
  return cpu && !cpu->PB && !cpu->DB && !cpu->D && !cpu->emulation &&
      !cpu->m_flag && !cpu->x_flag;
}

bool ActRaiserMagicGesture_ControlsReleased(uint16_t buttons) {
  /* A/X, Y and Up. Other movement/jump buttons do not arm either gesture. */
  return !(buttons & 0x48c0);
}

uint32_t ActRaiserMagicGesture_Attack(CpuState *cpu) {
  if (!ActRaiserMagicGesture_Entry(cpu))
    ActRaiserHleFatal("Unsupported regional ground-attack prefix");
  /* The shared LDA #$4000 / TRB removes held attack from the native mask.
   * JP then tests Up; its $F9 mask corresponds to the US $F6 word. The next
   * LDA/BIT replaces TRB's Z while preserving C/V and every other flag. */
  cpu_write16(cpu,0,0x00f6,(uint16_t)(cpu_read16(cpu,0,0x00f6) & ~0x4000u));
  cpu->A=cpu_read16(cpu,0,0x00a1);
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  cpu->_flag_Z=!(cpu->A & 8);
  cpu->P=(uint8_t)((cpu->P & ~CPU_P_Z) | (cpu->_flag_Z ? CPU_P_Z : 0));
  return cpu->_flag_Z ? 0x009a73 : 0x009de1;
}
