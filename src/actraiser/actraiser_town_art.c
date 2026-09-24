#include "actraiser_town_art.h"
#include "actraiser_game.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_regional_media.h"
#include "actraiser_regional_runtime.h"

static bool TownBank(CpuState *cpu,ActRaiserTownArtBank *bank) {
  const uint16_t source=cpu_read16(cpu,0,0xa5);
  const uint8_t source_bank=cpu_read8(cpu,0,0xa7);
  if(source_bank==0x0c && (source==0x8000 || source==0xc000)) {
    *bank=source==0x8000?kActRaiserTownArt_Early:kActRaiserTownArt_Late;return true;
  }
  if(source_bank==0x0d && source==0x8000) {*bank=kActRaiserTownArt_Objects;return true;}
  return false;
}
bool ActRaiser_TownArtEntry(CpuState *cpu) {
  if(!cpu || cpu->emulation || cpu->m_flag || cpu->x_flag || cpu->D || cpu->DB || cpu->PB!=2)return false;
  const uint16_t scene=cpu_read16(cpu,0,0x18);
  ActRaiserTownArtBank bank;
  return !(scene&255) && scene>=0x0100 && scene<=0x0600 &&
      cpu_read16(cpu,0,0)==0 && cpu_read16(cpu,0,2)==0x4000 && TownBank(cpu,&bank);
}
RecompReturn ActRaiser_LoadTownArt(CpuState *cpu) {
  if(!ActRaiser_TownArtEntry(cpu))ActRaiserHleFatal("Unsupported town character upload");
  ActRaiserTownArtBank bank=kActRaiserTownArt_Early;
  (void)TownBank(cpu,&bank);
  const uint16_t scene=cpu_read16(cpu,0,0x18);
  uint8_t mask=ActRaiserRegional_TownArtworkSnapshot(scene);
  if(bank!=kActRaiserTownArt_Objects && !ActRaiserRegional_BeginTownArtwork(scene,&mask))
    ActRaiserHleFatal("Cannot capture regional town artwork");
  ActRaiserTownArtSpan spans[kActRaiserTownArtMaximumSpans];
  const unsigned count=ActRaiserRegionalMedia_TownSpans(mask,bank,spans);
  const uint16_t source=cpu_read16(cpu,0,0xa5);
  const uint8_t source_bank=cpu_read8(cpu,0,0xa7);
  ActRaiserCpuHle_PushWord(cpu,cpu->Y); /* Native PHY residue. */
  unsigned span=0;
  for(cpu->Y=0;cpu->Y<0x4000;cpu->Y=(uint16_t)(cpu->Y+2)) {
    cpu->A=cpu_read16(cpu,source_bank,(uint16_t)(source+cpu->Y));
    while(span<count && cpu->Y>=spans[span].offset+spans[span].bytes.size)++span;
    uint16_t pixels=cpu->A;
    if(span<count && cpu->Y>=spans[span].offset) {
      const uint8_t *p=spans[span].bytes.data+cpu->Y-spans[span].offset;
      pixels=(uint16_t)(p[0]|(uint16_t)p[1]<<8);
    }
    cpu_write16(cpu,0,0x2118,pixels);
  }
  cpu->Y=ActRaiserCpuHle_PopWord(cpu);
  cpu->S=(uint16_t)(cpu->S+1);
  cpu->P=cpu_read8(cpu,0,cpu->S); /* Original command's PHP / PLP. */
  cpu_p_to_mirrors(cpu);
  cpu->S=(uint16_t)(cpu->S+k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
