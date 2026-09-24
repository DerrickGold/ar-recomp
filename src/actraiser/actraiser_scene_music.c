#include "actraiser_scene_music.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
#include "regional/regional_music.h"

bool ActRaiser_SceneMusicEntry(CpuState *cpu) {
  return cpu && !cpu->emulation && cpu->m_flag && !cpu->x_flag && !cpu->D &&
      cpu->PB==2 && cpu->DB==0 && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      cpu_read8(cpu,0,0x0334)==cpu_read8(cpu,0,0x008d);
}
RecompReturn ActRaiser_SceneMusic(CpuState *cpu) {
  uint8_t profile;
  if(!ActRaiser_SceneMusicEntry(cpu) || !ActRaiserRegional_BeginSceneMusic(&profile) || profile>1)
    ActRaiserHleFatal("Invalid regional scene music boundary");
  /* Only the accepted Fillmore cave declaration. The identical Japanese
   * Fillmore image is already present in the US cartridge at $18:947F.
   * Unrecognized/custom resources, boss music, track/variant selectors and
   * the resident source cache are not changed here. */
  if(ArRegionalMusic_UseFillmore(profile,cpu_read16(cpu,0,0x18)) &&
      cpu_read16(cpu,0,0xa5)==0xf69f && cpu_read8(cpu,0,0xa7)==0x0e &&
      cpu_read8(cpu,0,0x8c)==1 && cpu_read8(cpu,0,0x8d)==0) {
    cpu_write16(cpu,0,0xa5,0x947f);cpu_write8(cpu,0,0xa7,0x18);
  }
  /* LDX $A5 only. Native comparison suppresses redundant uploads; the native
   * continuation owns SPC handshakes, resident cache and playback command. */
  cpu->X=cpu_read16(cpu,0,0xa5);
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_N|CPU_P_Z)) | (cpu->X?0:CPU_P_Z) |
      (cpu->X&0x8000?CPU_P_N:0));
  cpu_p_to_mirrors(cpu);
  cpu_hle_tailcall_request(0x02b655,0x02b653);
  return RECOMP_RETURN_TAILCALL;
}
