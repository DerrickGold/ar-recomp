#include "actraiser_lair_reloads.h"
#include "byte_order.h"

bool ActRaiserLairReloads_Entry(const CpuState *cpu) {
  return cpu && cpu->PB==3 && cpu->DB==0x7f && !cpu->D && !cpu->x_flag && !cpu->emulation &&
      !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
static void Read(CpuState *cpu, uint16_t values[24]) {
  for (unsigned i=0;i<24;++i) values[i]=cpu_read16(cpu,0x7f,(uint16_t)(0x9628+2*i));
}
bool ActRaiserLairReloads_Check(ArRegionalLairReloads *h, CpuState *cpu, ArRegionalSource source) {
  if (!cpu) return false;
  uint16_t values[24]; Read(cpu,values);
  return ArRegionalLairReloads_Check(h,source,values);
}
bool ActRaiserLairReloads_Initialize(ArRegionalLairReloads *h, CpuState *cpu) {
  if (!cpu || !ArRegionalLairReloads_Valid(h) || h->initialized_towns) return false;
  ArRegionalLairReloads next={0}; ArRegionalLairReloads_Init(&next);
  if (!ActRaiserLairReloads_Check(&next,cpu,kArRegionalSource_US)) return false;
  *h=next; return true;
}
bool ActRaiserLairReloads_AdoptSaved(ArRegionalLairReloads *h, const uint8_t image[kActRaiserSramSize]) {
  if (!image || !Save_ChecksumValid(image)) return false;
  uint16_t values[24];
  for (unsigned i=0;i<24;++i) values[i]=ByteOrder_ReadLe16(image+0x1573+2*i);
  return ArRegionalLairReloads_Adopt(h,kArRegionalSource_US,values);
}
bool ActRaiserLairReloads_Project(ArRegionalLairReloads *h, CpuState *cpu,
                                 ArRegionalSource current, ArRegionalSource target) {
  /* Master entry has not installed DB=$7f yet; all accesses are banked. */
  if (!cpu || cpu->PB!=3 || cpu->D || cpu->x_flag || cpu->emulation || cpu->_flag_D ||
      (cpu->P & CPU_P_D) || (unsigned)target>=kArRegionalSource_Count ||
      !ActRaiserLairReloads_Check(h,cpu,current)) return false;
  if ((current==kArRegionalSource_Japan)==(target==kArRegionalSource_Japan)) return true;
  for (unsigned i=0;i<24;++i) {
    const uint16_t next=h->delay[target==kArRegionalSource_Japan][i];
    if (next!=h->delay[current==kArRegionalSource_Japan][i])
      cpu_write16(cpu,0x7f,(uint16_t)(0x9628+2*i),next);
  }
  return true;
}
bool ActRaiserLairReloads_BeginReduction(ArRegionalLairReloads *h, CpuState *cpu,
    ArRegionalSource source, ArRegionalLairReloads *candidate, unsigned *town) {
  if (!candidate || !town || !ActRaiserLairReloads_Entry(cpu)) return false;
  const unsigned index=cpu_read16(cpu,0x7f,0x7bfb);
  if (index>=12 || (index&1) || !ActRaiserLairReloads_Check(h,cpu,source)) return false;
  ArRegionalLairReloads next=*h;
  if (!ArRegionalLairReloads_ReduceTown(&next,index/2)) return false;
  *candidate=next; *town=index/2; return true;
}
bool ActRaiserLairReloads_EndReduction(ArRegionalLairReloads *h, CpuState *cpu,
    ArRegionalSource source, const ArRegionalLairReloads *candidate, unsigned town, RecompReturn result) {
  if (!h || !cpu || !candidate || town>=6 || !ArRegionalLairReloads_Valid(h)) return false;
  ArRegionalLairReloads next=*candidate;
  if (result!=RECOMP_RETURN_NORMAL || !ActRaiserLairReloads_Check(&next,cpu,source)) {
    h->diverged_towns |= (uint8_t)(next.diverged_towns | (1u<<town)); return false;
  }
  *h=next; return true;
}
