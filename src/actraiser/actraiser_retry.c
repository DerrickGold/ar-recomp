#include "actraiser_retry.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

bool ActRaiserRetry_Entry(const CpuState *cpu) {
  return cpu && cpu->PB == 0 && cpu->DB == 0 && cpu->D == 0 &&
      !cpu->x_flag && !cpu->emulation;
}

uint16_t ActRaiserRetry_Prefix(CpuState *cpu, bool clear_score) {
  if (!ActRaiserRetry_Entry(cpu)) ActRaiserHleFatal("Unsupported checkpoint-retry entry");
  cpu_mirrors_to_p(cpu);
  cpu->P &= (uint8_t)~CPU_P_M; /* native REP #$20 */
  cpu_p_to_mirrors(cpu);
  cpu->A = cpu_read16(cpu, 0, 0x032c);
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->A);
  if (!cpu->A) return 0x982f;
  cpu_write16(cpu, 0, 0x032c, 0);
  if (clear_score) cpu_write16(cpu, 0, 0x001f, 0);
  /* STZ leaves the marker's A/N/Z and all other registers/flags intact. */
  return 0x9826;
}
