#include "actraiser_regional_mosaic.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
#include "action/action_room_mosaic.h"

bool ActRaiser_RegionalMosaicEntry(CpuState *cpu) {
  const uint8_t pattern=ActRaiserRegional_MosaicSnapshot();
  return pattern>0 && pattern<kActionRoomMosaicPatterns && cpu && !cpu->emulation &&
      cpu->PB==2 && cpu->DB==0x7e && !cpu->D && cpu->m_flag && !cpu->x_flag &&
      !cpu->_flag_D && !(cpu->P&CPU_P_D) && cpu_read16(cpu,0,0x18)==0x0504 &&
      !cpu->X && !cpu->Y && cpu_read16(cpu,0,0)==0x100+(cpu_read8(cpu,0,0x88)>>2) &&
      cpu_read8(cpu,0,0x0c)==112;
}
RecompReturn ActRaiser_RegionalMosaic(CpuState *cpu) {
  if(!ActRaiser_RegionalMosaicEntry(cpu))ActRaiserHleFatal("Invalid regional mosaic loop");
  const uint8_t pattern=ActRaiserRegional_MosaicSnapshot(),phase=cpu_read8(cpu,0,0);
  uint8_t bit=0;
  for(unsigned band=0;band<112;++band) {
    if(!ActionRoomMosaic_Bit(pattern,phase+band,&bit))ActRaiserHleFatal("Invalid regional mosaic pattern");
    cpu_write8(cpu,0x7e,(uint16_t)(0x6000+band*2),2);
    cpu_write8(cpu,0x7e,(uint16_t)(0x6001+band*2),(uint8_t)((bit<<4)|2));
  }
  cpu_write8(cpu,0,0,(uint8_t)(phase+112));cpu_write8(cpu,0,12,0);
  cpu->X=(uint16_t)(0x100+phase+111);cpu->Y=224;
  cpu_write_a8(cpu,(uint8_t)((bit<<4)|2));
  /* Final DEC sets Z/N. Preserve US ASL carry (source bit4), unrelated to
   * the displayed bit0. Native continuation retains terminator/setup/return. */
  const uint8_t carry=(cpu_read8(cpu,2,(uint16_t)(0x96d4+cpu->X))>>4)&1;
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_N|CPU_P_Z|CPU_P_C))|CPU_P_Z|carry);
  cpu_p_to_mirrors(cpu);
  if(!cpu_hle_tailcall_request(0x0293ba,0x02939c))ActRaiserHleFatal("Mosaic loop lost its native owner");
  return RECOMP_RETURN_TAILCALL;
}
