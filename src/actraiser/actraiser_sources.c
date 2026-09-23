#include "actraiser_sources.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

static bool Shape(const CpuState *cpu) {
  return cpu && cpu->PB==1 && cpu->DB==1 && !cpu->D && !cpu->emulation &&
      cpu->m_flag && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
bool ActRaiserSources_CollectionEntry(const CpuState *cpu) {
  return Shape(cpu) && ((uint8_t)cpu->A==5 || (uint8_t)cpu->A==6);
}
uint32_t ActRaiserSources_CollectionRoute(CpuState *cpu, bool automatic) {
  if (!ActRaiserSources_CollectionEntry(cpu)) ActRaiserHleFatal("Unsupported Source collection entry");
  /* US's last compare equals item5/6; JP compares both against5 before taking
   * the held-insertion branch. CMP preserves A and overflow. */
  const uint8_t difference=automatic ? 0 : (uint8_t)cpu->A-5;
  cpu->_flag_C=1; cpu->P|=CPU_P_C;
  ActRaiserCpuHle_SetNegativeZero8(cpu,difference);
  return automatic ? 0x018922 : 0x01892d;
}
bool ActRaiserSources_KeepCarried(CpuState *cpu, uint8_t item) {
  if (!Shape(cpu) || (item!=5 && item!=6) || (uint8_t)cpu->A!=item) return false;
  /* The RTS dispatcher pushes $9C82 and P above the collection JSR's $8924.
   * Use Offering calls the same dispatcher from a different native return.
   * Preserve its behavior even with regional changes pending mid-dialogue. */
  if (cpu_read16(cpu,0,(uint16_t)(cpu->S+1))!=0x9c82 ||
      cpu_read16(cpu,0,(uint16_t)(cpu->S+4))!=0x8924) return false;
  for (unsigned i=0;i<8;++i)
    if (cpu_read8(cpu,0,(uint16_t)(0x02a2+i))==item) return true;
  return false;
}
