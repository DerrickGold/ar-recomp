#include "actraiser/actraiser_scene_music.h"
#include "regional/regional_music.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t ram[65536],profile;
static unsigned target,origin,captures;
bool ActRaiserRegional_BeginSceneMusic(uint8_t *out) { *out=profile;++captures;return true; }
int cpu_hle_tailcall_request(uint32_t pc,uint32_t site) { target=pc;origin=site;return 1; }
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) { (void)cpu;assert(!bank);return ram[at]; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {
  return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) { (void)cpu;assert(!bank);ram[at]=value; }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {
  cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);
}
static void Adapters(void) {
  for(unsigned source=0;source<3;++source) {
    assert(ArRegionalMusic_Resolve(source,&profile));assert(profile==(source==1));
    for(unsigned flags=0;flags<256;++flags) {
      if(!(flags&CPU_P_M) || (flags&(CPU_P_X|CPU_P_D)))continue;
      for(unsigned scene=0;scene<0x0909;scene+=1) {
        if((scene&255)>8)continue;
        for(unsigned resource=0;resource<5;++resource) {
          memset(ram,0x5a,sizeof(ram));ByteOrder_WriteLe16(ram+0x18,scene);
          const uint16_t pointers[]={0xf69f,0x947f,0xabcd,0,0xf69f};
          ByteOrder_WriteLe16(ram+0xa5,pointers[resource]);ram[0xa7]=resource==1?0x18:0x0e;
          ram[0x334]=ram[0x8d]=0;ram[0x8c]=resource==4?2:1;
          CpuState cpu={.PB=2,.DB=0,.A=0xbe00,.X=0xa5,.Y=0x1234,.S=0x1efc,.P=flags};
          cpu_p_to_mirrors(&cpu);CpuState before=cpu;
          uint8_t expected[65536];memcpy(expected,ram,sizeof(ram));
          const unsigned count=captures;
          assert(ActRaiser_SceneMusicEntry(&cpu) && captures==count);
          assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(expected,ram,sizeof(ram)));
          const bool change=profile==1 && (scene==0x0201 || scene==0x0301) && resource==0;
          if(change) {ByteOrder_WriteLe16(expected+0xa5,0x947f);expected[0xa7]=0x18;}
          before.X=ByteOrder_ReadLe16(expected+0xa5);
          before.P=(before.P&~(CPU_P_N|CPU_P_Z))|(before.X?0:CPU_P_Z)|(before.X&0x8000?CPU_P_N:0);
          cpu_p_to_mirrors(&before);
          assert(ActRaiser_SceneMusic(&cpu)==RECOMP_RETURN_TAILCALL);
          assert(captures==count+1 && target==0x02b655 && origin==0x02b653);
          assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(expected,ram,sizeof(ram)));
          /* No cache, selector, APU-port or handshake writes. A repeated
           * accepted source goes through the same native cache comparison. */
          assert(ActRaiser_SceneMusic(&cpu)==RECOMP_RETURN_TAILCALL);
          assert(!memcmp(expected,ram,sizeof(ram)));
        }
      }
    }
  }
  for(unsigned bad=0;bad<9;++bad) {
    CpuState cpu={.PB=2,.DB=0,.P=CPU_P_M};cpu_p_to_mirrors(&cpu);
    ram[0x334]=ram[0x8d]=0;
    switch(bad) {
      case 0:cpu.emulation=1;break;case 1:cpu.m_flag=0;break;
      case 2:cpu.x_flag=1;break;case 3:cpu.D=2;break;case 4:cpu.PB=0;break;
      case 5:cpu.DB=2;break;case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;
      case 8:ram[0x8d]=1;break;
    }
    assert(!ActRaiser_SceneMusicEntry(&cpu));
  }
  assert(!ActRaiser_SceneMusicEntry(NULL));
  uint8_t unchanged=0xff;assert(!ArRegionalMusic_Resolve(-1,&unchanged) && unchanged==0xff);
  assert(!ArRegionalMusic_Resolve(3,&unchanged) && !ArRegionalMusic_Resolve(0,NULL));
  assert(!ArRegionalMusic_UseFillmore(2,0x0201));
}
static size_t SongSize(const uint8_t *r,size_t offset) {
  const size_t start=offset;
  for(unsigned block=0;block<64;++block) {
    assert(offset<=0xffffc);
    const unsigned size=ByteOrder_ReadLe16(r+offset),dest=ByteOrder_ReadLe16(r+offset+2);
    offset+=4;
    if(!size) {assert(dest&255);assert(offset-1+(dest&255)<=0x100000);return offset-1+(dest&255)-start;}
    assert(size<=32768 && size<=0x100000-offset);offset+=size;
  }
  assert(false);return 0;
}
static void RomCheck(char **paths) {
  static const unsigned lengths[]={6,5,3,1,4,7,6,6};
  uint8_t *r[5];
  for(unsigned i=0;i<5;++i) {
    FILE *f=fopen(paths[i],"rb");assert(f);r[i]=malloc(0x100000);assert(r[i]);
    assert(fread(r[i],1,0x100000,f)==0x100000 && fgetc(f)==EOF);fclose(f);
    size_t cursor=0x28003;unsigned caves=0;
    for(unsigned guard=0;guard<64;++guard) {
      const unsigned group=r[i][cursor++],room=r[i][cursor++];
      if(group>1)break;
      while(r[i][cursor]) {
        unsigned byte=r[i][cursor++],bit=0;while(byte>>1){byte>>=1;++bit;}
        if(bit==1 && group==1 && (room==2 || room==3)) {
          assert(r[i][cursor]==1 && !r[i][cursor+1]);
          const size_t file=r[i][cursor+2]|r[i][cursor+3]<<8|r[i][cursor+4]<<16;
          assert(file==(i==1?0xbf04c:0x7769f));++caves;
        }
        cursor+=lengths[bit];assert(cursor<0x30000);
      }
      ++cursor;
    }
    assert(caves==2);
  }
  const size_t size=SongSize(r[0],0xc147f);
  assert(size==SongSize(r[1],0xbf04c) && !memcmp(r[0]+0xc147f,r[1]+0xbf04c,size));
  for(unsigned i=0;i<5;++i)free(r[i]);
}
int main(int argc,char **argv) {
  Adapters();if(argc==6)RomCheck(argv+1);else assert(argc==1);
  puts("Scene music: guarded routing, untouched native cache/handshake, five-ROM sources passed");
  return 0;
}
