#include "actraiser_town_wait.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

bool ActRaiserTownWait_Entry(CpuState *cpu) {
  if (!cpu || cpu->PB != 3 || cpu->DB != 0x7f || cpu->D || cpu->emulation ||
      cpu->m_flag || cpu->x_flag) return false;
  const unsigned town = cpu_read16(cpu, 0x7f, 0x7bfb);
  return town < 12 && !(town & 1);
}

RecompReturn ActRaiserTownWait_Step(CpuState *cpu, uint16_t reload) {
  if (!ActRaiserTownWait_Entry(cpu)) ActRaiserHleFatal("Unsupported town wait entry");
  cpu_mirrors_to_p(cpu);
  cpu->X = cpu_read16(cpu, 0x7f, 0x7bfb);
  const uint16_t timer_address = (uint16_t)(0x7ce1 + cpu->X);
  const uint16_t remaining = (uint16_t)(cpu_read16(cpu, 0x7f, timer_address) - 1u);
  cpu_write16(cpu, 0x7f, timer_address, remaining);
  ActRaiserCpuHle_SetNegativeZero16(cpu, remaining);
  if (!remaining) {
    cpu->A = reload;
    cpu_write16(cpu, 0x7f, timer_address, reload);
    const uint16_t state_address = (uint16_t)(0x7cc9 + cpu->X);
    const uint16_t state = (uint16_t)(cpu_read16(cpu, 0x7f, state_address) + 1u);
    cpu_write16(cpu, 0x7f, state_address, state);
    ActRaiserCpuHle_SetNegativeZero16(cpu, state);
  }
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
