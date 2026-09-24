#include "actraiser/regional/actraiser_title_art.h"
#include "actraiser_game.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "actraiser/regional/actraiser_regional_runtime.h"

static bool TitleSource(CpuState *cpu,bool narrow,uint8_t bank,uint16_t source) {
  return cpu && !cpu->emulation && cpu->m_flag==narrow && !cpu->x_flag &&
      !cpu->D && !cpu->DB && cpu->PB==2 && cpu_read16(cpu,0,0x18)==0 &&
      cpu_read8(cpu,0,0xa7)==bank && cpu_read16(cpu,0,0xa5)==source;
}
bool ActRaiser_TitlePaletteEntry(CpuState *cpu) {
  return TitleSource(cpu,true,0x1c,0xba93) &&
      cpu_read16(cpu,0,0)==0 && cpu_read16(cpu,0,2)==256;
}
bool ActRaiser_TitleCharactersEntry(CpuState *cpu) {
  return TitleSource(cpu,false,0x0b,0x8300) &&
      cpu_read16(cpu,0,0)==0 && cpu_read16(cpu,0,2)==0x4000;
}
bool ActRaiser_TitleMapEntry(CpuState *cpu) {
  return TitleSource(cpu,true,5,0x8e7f) && cpu->X==0;
}
/* Each seam is a native tail: its owning function already pushed P. Map
 * also already pushed Y. Keep those exact stack writes, CPU residue and
 * port sequences; only the bytes sent to the PPU come from the donor. */
static RecompReturn Finish(CpuState *cpu) {
  cpu->Y=ActRaiserCpuHle_PopWord(cpu);
  cpu->P=cpu_read8(cpu,0,++cpu->S);cpu_p_to_mirrors(cpu);
  cpu->S=(uint16_t)(cpu->S+k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
static void ReadAccumulatorByte(CpuState *cpu,uint8_t bank,uint16_t address) {
  cpu->A=(uint16_t)((cpu->A&0xff00)|cpu_read8(cpu,bank,address));
}
RecompReturn ActRaiser_LoadTitlePalette(CpuState *cpu) {
  if(!ActRaiser_TitlePaletteEntry(cpu))ActRaiserHleFatal("Unsupported title palette upload");
  uint8_t mask=0;
  if(!ActRaiserRegional_BeginTitleArtwork(&mask))ActRaiserHleFatal("Cannot capture title artwork");
  const ActRaiserTitleArt art=ActRaiserRegionalMedia_Title(mask&kArRegionalArtwork_TitleMask);
  ActRaiserCpuHle_PushWord(cpu,cpu->Y);
  for(cpu->Y=0;cpu->Y<256;++cpu->Y) {
    ReadAccumulatorByte(cpu,0x1c,(uint16_t)(0xba93+cpu->Y));
    cpu_write8(cpu,0,0x2122,art.palette.data?art.palette.data[cpu->Y]:(uint8_t)cpu->A);
  }
  return Finish(cpu);
}
RecompReturn ActRaiser_LoadTitleCharacters(CpuState *cpu) {
  if(!ActRaiser_TitleCharactersEntry(cpu))ActRaiserHleFatal("Unsupported title characters upload");
  const ActRaiserTitleArt art=ActRaiserRegionalMedia_Title(ActRaiserRegional_TitleArtworkSnapshot()&kArRegionalArtwork_TitleMask);
  cpu->X=0;cpu_write16(cpu,0,0x2116,0);
  ActRaiserCpuHle_PushWord(cpu,cpu->Y);
  for(cpu->Y=0;cpu->Y<0x4000;++cpu->Y) {
    ReadAccumulatorByte(cpu,0x0b,(uint16_t)(0x8300+cpu->Y));
    cpu_write8(cpu,0,0x2119,art.characters.data?art.characters.data[cpu->Y]:(uint8_t)cpu->A);
  }
  return Finish(cpu);
}
RecompReturn ActRaiser_LoadTitleMap(CpuState *cpu) {
  if(!ActRaiser_TitleMapEntry(cpu))ActRaiserHleFatal("Unsupported title map upload");
  const ActRaiserTitleArt art=ActRaiserRegionalMedia_Title(ActRaiserRegional_TitleArtworkSnapshot()&kArRegionalArtwork_TitleMask);
  for(cpu->X=0;cpu->X<0x4000;++cpu->X) {
    ReadAccumulatorByte(cpu,0x7e,(uint16_t)(0xc000+cpu->X));
    cpu_write8(cpu,0,0x2118,art.map.data?art.map.data[cpu->X]:(uint8_t)cpu->A);
  }
  cpu->A=(uint16_t)((cpu->A&0xff00)|0x80);cpu_write8(cpu,0,0x2115,0x80);
  return Finish(cpu);
}
