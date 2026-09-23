#include "actraiser_story_prerequisites.h"
#include "actraiser_cpu_hle_internal.h"

static bool Shape(const CpuState *cpu, bool narrow) {
  return cpu && cpu->PB==3 && cpu->DB==0x7f && !cpu->D && !cpu->emulation &&
      cpu->m_flag==narrow && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
bool ActRaiserStory_ThresholdEntry(CpuState *cpu, ArRegionalStoryRule *rule) {
  if (!rule || !Shape(cpu,false)) return false;
  ArRegionalStoryRule next;
  unsigned town,event;
  if (cpu->X==0xf543) { next=kArRegionalStory_FillmoreHint;town=0;event=5; }
  else if (cpu->X==0xf56c) { next=kArRegionalStory_KasandoraTablet;town=4;event=9; }
  else return false;
  if (cpu_read16(cpu,0x7f,0x7bfb)!=town ||
      cpu_read8(cpu,3,(uint16_t)(cpu->X+2))!=event ||
      cpu_read16(cpu,3,cpu->X)!=ArRegionalStory_Descriptor(next)->value[kArRegionalSource_US]) return false;
  *rule=next; return true;
}
bool ActRaiserStory_LoadThreshold(CpuState *cpu, uint16_t threshold) {
  ArRegionalStoryRule rule;
  if (!ActRaiserStory_ThresholdEntry(cpu,&rule)) return false;
  cpu->A=threshold;
  ActRaiserCpuHle_SetNegativeZero16(cpu,threshold);
  return true;
}
bool ActRaiserStory_CompassEntry(const CpuState *cpu) { return Shape(cpu,true); }
