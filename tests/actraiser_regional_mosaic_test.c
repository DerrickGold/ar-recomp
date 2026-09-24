#include "actraiser/actraiser_regional_mosaic.h"
#include "action/action_room_mosaic.h"
#include "regional/regional_mosaic.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t ram[65536],rom[65536],pattern;
static unsigned target,origin;
uint8_t ActRaiserRegional_MosaicSnapshot(void) { return pattern; }
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) { (void)cpu;assert(!bank || bank==2 || bank==0x7e);return bank==2?rom[at]:ram[at]; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) { (void)cpu;assert(!bank || bank==0x7e);ram[at]=value; }
int cpu_hle_tailcall_request(uint32_t pc,uint32_t site) {target=pc;origin=site;return 1;}
static CpuState Context(unsigned phase,unsigned flags) {
  memset(ram,0x5a,sizeof(ram));ByteOrder_WriteLe16(ram+0x18,0x0504);
  ram[0x88]=(uint8_t)(phase*4+3);ByteOrder_WriteLe16(ram,0x100+phase);ram[12]=112;
  CpuState cpu={.PB=2,.DB=0x7e,.A=0xbe70,.P=flags,.S=0x1efb};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void Adapter(void) {
  for(unsigned i=0;i<sizeof(rom);++i)rom[i]=(uint8_t)(i*113+37);
  for(pattern=0;pattern<3;++pattern)for(unsigned phase=0;phase<64;++phase)
    for(unsigned flags=0;flags<256;++flags) {
      if(!(flags&CPU_P_M) || flags&(CPU_P_D|CPU_P_X))continue;
      CpuState cpu=Context(phase,flags),expected=cpu;
      uint8_t before[65536];memcpy(before,ram,sizeof(ram));
      assert(ActRaiser_RegionalMosaicEntry(&cpu)==(pattern!=0));
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,ram,sizeof(ram)));
      if(!pattern)continue;
      uint8_t bit=0;
      for(unsigned band=0;band<112;++band) {
        assert(ActionRoomMosaic_Bit(pattern,phase+band,&bit));
        before[0x6000+band*2]=2;before[0x6001+band*2]=(bit<<4)|2;
      }
      before[0]=phase+112;before[12]=0;
      expected.X=0x100+phase+111;expected.Y=224;expected.A=0xbe02|(bit<<4);
      expected.P=(expected.P&~(CPU_P_N|CPU_P_Z|CPU_P_C))|CPU_P_Z|((rom[0x96d4+expected.X]>>4)&1);
      cpu_p_to_mirrors(&expected);
      assert(ActRaiser_RegionalMosaic(&cpu)==RECOMP_RETURN_TAILCALL && target==0x0293ba && origin==0x02939c);
      assert(!memcmp(&expected,&cpu,sizeof(cpu)) && !memcmp(before,ram,sizeof(ram)));
    }
  pattern=1;
  for(unsigned bad=0;bad<14;++bad) {
    CpuState cpu=Context(0,CPU_P_M);
    switch(bad) {
      case 0:cpu.PB=0;break;case 1:cpu.DB=0;break;case 2:cpu.D=1;break;
      case 3:cpu.emulation=1;break;case 4:cpu.m_flag=0;break;case 5:cpu.x_flag=1;break;
      case 6:cpu.P|=CPU_P_D;break;case 7:cpu._flag_D=1;break;case 8:cpu.X=1;break;
      case 9:cpu.Y=1;break;case 10:ram[0x18]=3;break;case 11:ram[12]=0;break;
      case 12:ram[1]=0;break;case 13:ram[0x88]=4;break;
    }
    assert(!ActRaiser_RegionalMosaicEntry(&cpu));
  }
  assert(!ActRaiser_RegionalMosaicEntry(NULL));
}
static void Portable(void) {
  static ActionRoomScene scene,expected;
  uint8_t value=255;
  assert(!ActionRoomMosaic_Bit(3,0,&value) && value==255);
  assert(!ActionRoomMosaic_Bit(0,175,&value) && !ActionRoomMosaic_Bit(0,0,NULL));
  assert(!ActionRoomMosaic_Project(NULL,0));
  assert(!ArRegionalMosaic_Resolve(3,&value) && !ArRegionalMosaic_Resolve(-1,&value));
  assert(!ArRegionalMosaic_Resolve(0,NULL));
  for(unsigned p=0;p<3;++p) {
    assert(ArRegionalMosaic_Resolve(p,&value) && value==p);
    memset(&scene,0,sizeof(scene));scene.group=4;scene.map=5;
    scene.have_video_profile=scene.have_raster_waveform=scene.have_raster_mosaic_wave_window=true;
    scene.raster_effect=kActionRoomRaster_Bg2MosaicWave;
    memset(scene.raster_mosaic_wave_window,0x5a,sizeof(scene.raster_mosaic_wave_window));expected=scene;
    for(unsigned i=0;i<175;++i)assert(ActionRoomMosaic_Bit(p,i,&expected.raster_mosaic_wave_window[i]));
    assert(ActionRoomMosaic_Project(&scene,p) && !memcmp(&scene,&expected,sizeof(scene)));
    for(unsigned frame=0;frame<1024;++frame)for(unsigned entry=0;entry<2;++entry) {
      ActionRoomSceneFrameRequest request={.game_frame=frame,.raster_entry_frame=entry};
      ActionRoomSceneFrameState state;assert(ActionRoomScene_BuildFrameState(&scene,&request,&state));
      for(unsigned line=0;line<224;++line) {
        /* Visible HDMA belongs to the preceding native update. */
        assert(ActionRoomMosaic_Bit(p,(((frame-1)&255)>>2)+line/2,&value));
        assert(state.mosaic[line]==(entry?2:(value<<4)|2));
      }
    }
    scene.raster_effect=kActionRoomRaster_None;expected=scene;
    assert(ActionRoomMosaic_Project(&scene,p) && !memcmp(&scene,&expected,sizeof(scene)));
    scene.raster_effect=kActionRoomRaster_Bg2MosaicWave;scene.have_raster_mosaic_wave_window=false;expected=scene;
    assert(!ActionRoomMosaic_Project(&scene,p) && !memcmp(&scene,&expected,sizeof(scene)));
  }
}
int main(int argc,char **argv) {
  Adapter();Portable();
  if(argc==6)for(unsigned r=0;r<5;++r) {
    const unsigned offsets[]={0x117d4,0x11567,0x117d4,0x117d4,0x117be},patterns[]={0,1,2,0,2};
    FILE *f=fopen(argv[r+1],"rb");assert(f);assert(!fseek(f,offsets[r],SEEK_SET));
    uint8_t source[175];assert(fread(source,1,sizeof(source),f)==sizeof(source));fclose(f);
    for(unsigned phase=0;phase<64;++phase)for(unsigned band=0;band<112;++band) {
      uint8_t bit;assert(ActionRoomMosaic_Bit(patterns[r],phase+band,&bit));
      assert(bit==(source[phase+band]&1));
    }
  } else assert(argc==1);
  puts("Regional mosaic: native lookup contract, all phases, host projection, optional five-ROM patterns passed");
  return 0;
}
