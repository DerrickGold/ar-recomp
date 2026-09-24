#include "actraiser/actraiser_stage_hazards.h"
#include "regional/regional_hazards.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[65536], rom_bank[65536], profile;
static unsigned target, origin;
uint8_t ActRaiserRegional_HazardSnapshot(void) { return profile; }
int cpu_hle_tailcall_request(uint32_t pc,uint32_t site) { target=pc;origin=site;return 1; }
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {
  (void)cpu;assert(!bank || bank==0x0a);
  return bank && at>=0x8000?rom_bank[at]:ram[at];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {
  return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {
  (void)cpu;assert(!bank);ram[at]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {
  cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);
}
static void WriteBoxes(uint8_t *bytes,const ArRegionalHazards *hazards) {
  for(unsigned i=0;i<hazards->count;++i) {
    const ArRegionalHazardBox *b=&hazards->boxes[i];
    const uint16_t words[]={b->left,b->width,b->top,b->height,b->damage_or_flag};
    for(unsigned j=0;j<5;++j)ByteOrder_WriteLe16(bytes+i*10+j*2,words[j]);
  }
}
static CpuState Setup(uint16_t scene,unsigned status,ArRegionalHazards *native) {
  assert(ArRegionalHazards_Copy(0,scene,native));
  memset(ram,0x5a,sizeof(ram));memset(rom_bank,0xff,sizeof(rom_bank));
  ByteOrder_WriteLe16(ram+0x18,scene);
  ByteOrder_WriteLe16(ram,native->count);WriteBoxes(ram+0x1ae4,native);
  CpuState cpu={.DB=0x0a,.Y=0xb200,.X=native->count*10,.A=0xff,.S=0x1efe,.P=status};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void Adapters(void) {
  unsigned scenes=0,changed[3]={0};
  for(unsigned room=1;room<=8;++room)for(unsigned area=1;area<=7;++area) {
    const uint16_t scene=(uint16_t)(room*256+area);ArRegionalHazards native,selected;
    if(!ArRegionalHazards_Copy(0,scene,&native))continue;
    ++scenes;
    for(profile=0;profile<3;++profile) {
      assert(ArRegionalHazards_Copy(profile,scene,&selected));
      const bool differs=memcmp(&native,&selected,sizeof(native))!=0;
      changed[profile]+=differs;
      for(unsigned flags=0;flags<256;++flags) {
        if(flags&(CPU_P_M|CPU_P_X|CPU_P_D))continue;
        CpuState cpu=Setup(scene,flags,&native),before=cpu;
        uint8_t expected[65536];memcpy(expected,ram,sizeof(ram));
        assert(ActRaiser_StageHazardsEntry(&cpu)==differs);
        assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(expected,ram,sizeof(ram)));
        if(!differs)continue;
        WriteBoxes(expected+0x1ae4,&selected);ByteOrder_WriteLe16(expected,selected.count);
        before.A=selected.count;before.P=(before.P&~(CPU_P_N|CPU_P_Z))|(before.A?0:CPU_P_Z);
        cpu_p_to_mirrors(&before);
        assert(ActRaiser_StageHazards(&cpu)==RECOMP_RETURN_TAILCALL);
        assert(target==0x940e && origin==0x940c);
        assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,expected,sizeof(ram)));
        /* Native LDA result publishes the count; the source cursor remains
         * the US terminator regardless of restored/deleted hazard records. */
        assert(cpu.Y==0xb200 && cpu.S==0x1efe && cpu.X==native.count*10);
      }
    }
  }
  assert(scenes==49 && changed[0]==0 && changed[1]==20 && changed[2]==20);
  for(unsigned bad=0;bad<16;++bad) {
    profile=1;ArRegionalHazards native;CpuState cpu=Setup(0x0101,0,&native);
    switch(bad) {
      case 0:cpu.DB=0;break; case 1:cpu.PB=2;break;case 2:cpu.D=2;break;
      case 3:cpu.emulation=1;break;case 4:cpu.m_flag=1;break;case 5:cpu.x_flag=1;break;
      case 6:cpu.P|=CPU_P_D;break;case 7:cpu._flag_D=1;break;case 8:++cpu.X;break;
      case 9:++ram[0];break;case 10:++ram[0x1ae4];break;case 11:++ram[0x1aec];break;
      case 12:rom_bank[cpu.Y]=0;break;case 13:ram[0x18]=0;break;
      case 14:profile=3;break;case 15:profile=255;break;
    }
    assert(!ActRaiser_StageHazardsEntry(&cpu));
  }
  assert(!ActRaiser_StageHazardsEntry(NULL));
  ArRegionalHazards untouched;memset(&untouched,0x5a,sizeof(untouched));ArRegionalHazards original=untouched;
  assert(!ArRegionalHazards_Copy(3,0x0101,&untouched) && !memcmp(&original,&untouched,sizeof(original)));
  assert(!ArRegionalHazards_Copy(0,0,&untouched) && !memcmp(&original,&untouched,sizeof(original)));
  assert(!ArRegionalHazards_Copy(0,0x0101,NULL));uint8_t value=0xff;
  assert(!ArRegionalHazards_Resolve(-1,&value) && value==0xff);
  assert(!ArRegionalHazards_Resolve(3,&value) && value==0xff);
  assert(!ArRegionalHazards_Resolve(0,NULL));
}

/* Optional independent ROM check: native source words, not the generator or
 * implementation's expansion. European Story/Action, English/German/French. */
static void RomCheck(const char *path,unsigned source) {
  FILE *f=fopen(path,"rb");assert(f);uint8_t *rom=malloc(0x100000);assert(rom);
  assert(fread(rom,1,0x100000,f)==0x100000 && fgetc(f)==EOF);fclose(f);
  const uint8_t *bank=rom+(source==2?31:10)*0x8000;
  for(unsigned mode=0;mode<(source==2?2u:1u);++mode) {
    unsigned at=source==2?ByteOrder_ReadLe16(bank+mode*2):0x3100,rooms=0;
    for(unsigned guard=0;guard<64;++guard,at+=4) {
      assert(at+4<0x8000);const uint16_t scene=ByteOrder_ReadLe16(bank+at);
      if(scene==0xffff)break;
      if(!(scene&255))continue;
      const unsigned start=ByteOrder_ReadLe16(bank+at+2)+(source==2?0:0x3100)+3;
      ArRegionalHazards expected;assert(ArRegionalHazards_Copy(source,scene,&expected));++rooms;
      unsigned i=0,p=start;
      while(p<0x8000 && bank[p]!=255) {
        assert(i<expected.count && p+5<0x8000);const ArRegionalHazardBox *b=&expected.boxes[i++];
        assert(b->left==(uint16_t)(bank[p]*16-4));
        assert(b->width==((bank[p+1]-bank[p])&255)*16+24);
        assert(b->top==(uint16_t)(bank[p+2]*16-16));
        assert(b->height==((bank[p+3]-bank[p+2])&255)*16+48);
        assert(b->damage_or_flag==bank[p+4]);p+=5;
      }
      assert(p<0x8000 && i==expected.count);
    }
    assert(rooms==49);
  }
  free(rom);
}
int main(int argc,char **argv) {
  Adapters();
  assert(argc==1 || argc==6);
  for(int i=1;i<argc;++i)RomCheck(argv[i],i==1?0:i==2?1:2);
  puts("Regional hazards: 49 rooms, three profiles, native continuation and ROM checks passed");
  return 0;
}
