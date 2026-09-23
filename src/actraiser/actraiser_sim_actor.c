#include "actraiser_sim_actor.h"
bool ActRaiserSimActor_Town(CpuState *cpu,unsigned *town) {
  if (!cpu || !town) return false;
  const unsigned index=cpu_read16(cpu,0x7f,0x7bfb);
  if (index>=12 || (index&1)) return false;
  *town=index/2;return true;
}
bool ActRaiserSimActor_Locate(CpuState *cpu,unsigned actor,unsigned *town,unsigned *slot) {
  if (!cpu || !town || !slot || actor<0x0b30 || actor>0x0ba2 || (actor-0x0b30)%0x26 || !ActRaiserSimActor_Town(cpu,town)) return false;
  *slot=(actor-0x0b30)/0x26;
  const unsigned index=(*town*4+*slot)*2;
  /* +0F belongs to actor behavior (e.g. Bat carrying state), not species. */
  const unsigned species=cpu_read8(cpu,1,(uint16_t)(actor+0x0e));
  return species>=0x12 && species<=0x15 && cpu_read16(cpu,0x7f,(uint16_t)(0x9688+index))==actor &&
      cpu_read16(cpu,0x7f,(uint16_t)(0x95f8+index))==species;
}
