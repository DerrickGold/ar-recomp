#include "actraiser/actraiser_town_census.h"
bool ActRaiserRegional_CopySupport(ArRegionalSupportSnapshot *snapshot) {
  const ArRegionalSupportPolicy native={{0}};return ArRegionalSupport_Resolve(&native,snapshot);
}
#include "actraiser/actraiser_bridge_extension.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;
static uint8_t low[65536],town_ram[65536],sram[8192],rom[65536],us_rom[65536],jp_rom[65536];
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;
  switch(bank) {
    case 0:case 1:return low[address];
    case 3:return rom[address];
    case 0x7f:return town_ram[address];
    case 0x70:assert(address<sizeof(sram));return sram[address];
    default:assert(!"unexpected memory bank");return 0;
  }
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) { return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,address+1)<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;
  if(bank==0 || bank==1)low[address]=value;
  else { assert(bank==0x7f);town_ram[address]=value; }
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,value);cpu_write8(cpu,bank,address+1,value>>8);
}
void SaveSystem_ResyncShadowRange(size_t offset,size_t size) { (void)offset;(void)size;assert(!"census must not mutate bridge storage"); }
static void Word(uint8_t *p,unsigned at,unsigned value) { p[at]=value;p[at+1]=value>>8; }
static unsigned WordAt(const uint8_t *p,unsigned at) { return p[at]|p[at+1]<<8; }
static CpuState Setup(unsigned town,unsigned flags,bool japanese) {
  memset(low,0xa5,sizeof(low));memset(town_ram,0,sizeof(town_ram));memset(sram,0,sizeof(sram));
  Word(town_ram,0x7bfb,2*town);Word(town_ram,0x6b18+2*town,2);
  /* In the ROM-free tier, only the pointer table is synthetic. */
  if(!us_rom[0xc07e]) {
    memset(rom,0,sizeof(rom));
    for(unsigned i=0;i<6;++i)Word(rom,0xdc74+2*i,0x6be7+i*512);
  } else memcpy(rom,japanese?jp_rom:us_rom,sizeof(rom));
  CpuState cpu={.PB=3,.DB=0x7f,.S=0x1ff0,.A=0x1234,.X=0x5678,.Y=0xabcd,.P=flags};cpu_p_to_mirrors(&cpu);return cpu;
}
static unsigned Base(unsigned town,bool japanese) { return WordAt(rom,(japanese?0xd779:0xdc74)+2*town); }
static ArRegionalSupportSnapshot Profile(unsigned source) {
  ArRegionalSupportPolicy policy;ArRegionalSupportSnapshot snapshot;
  assert(ArRegionalSupport_Init(&policy,source) && ArRegionalSupport_Resolve(&policy,&snapshot));return snapshot;
}
static unsigned ExpectedSupport(unsigned flags,const ArRegionalSupportSnapshot *s) {
  if(!(flags&128) || !(flags&15))return 0;
  switch(flags&15) {
    case 2:return flags&64?0:s->amount[flags&16?1:0];
    case 3:return flags&64?0:s->amount[2];
    case 4:return s->amount[3];
    default:return s->amount[4];
  }
}
static unsigned ExpectedPeople(unsigned flags) { return !(flags&128) || (flags&15)?0:(flags&48)==32?8:(flags&48)==16?6:4; }
static void Policies(void) {
  for(unsigned n=0;n<243;++n) {
    ArRegionalSupportPolicy policy;unsigned digits=n,japanese=0;
    for(unsigned i=0;i<5;++i) { policy.source[i]=digits%3;japanese+=digits%3==1;digits/=3; }
    ArRegionalSupportSnapshot s;ArRegionalSource source;
    assert(ArRegionalSupport_Resolve(&policy,&s) && ArRegionalSupport_Valid(&s));
    assert(ArRegionalSupport_GroupSource(&policy,&source)==(!japanese || japanese==5));
    for(unsigned i=0;i<5;++i)assert(s.amount[i]==ArRegionalSupport_Descriptor(i)->amount[policy.source[i]]);
    for(unsigned flags=0;flags<256;++flags) {
      CpuState cpu=Setup(0,CPU_P_V|CPU_P_C,false);town_ram[Base(0,false)+2]=flags;
      assert(ActRaiserTownCensus_Run(&cpu,&s)==RECOMP_RETURN_NORMAL);
      assert(WordAt(low,0x21c)==ExpectedPeople(flags)+2 && WordAt(town_ram,0x6b26)==ExpectedSupport(flags,&s));
    }
  }
  ArRegionalSupportPolicy bad={{0,0,0,0,3}},before=bad;ArRegionalSupportSnapshot s={{1,2,3,4,5}},sentinel=s;
  assert(!ArRegionalSupport_Init(&bad,3) && !memcmp(&bad,&before,sizeof(bad)));
  assert(!ArRegionalSupport_Resolve(&bad,&s) && !memcmp(&s,&sentinel,sizeof(s)) && !ArRegionalSupport_Valid(&s));
}
/* Only the 201-byte native census vocabulary, with a strict instruction cap.
 * Inputs/outputs are checked separately from this optional local-ROM oracle. */
static void Native(CpuState *c,uint16_t pc) {
  for(unsigned step=0;step<10000;++step) {
    const uint8_t op=rom[pc++];uint16_t v;
    switch(op) {
      case 0x08:low[c->S--]=c->P;continue;
      case 0xda:low[c->S--]=c->X>>8;low[c->S--]=(uint8_t)c->X;continue;
      case 0xfa:c->X=WordAt(low,c->S+1);c->S+=2;ActRaiserCpuHle_SetNegativeZero16(c,c->X);continue;
      case 0x28:c->P=low[++c->S];cpu_p_to_mirrors(c);continue;
      case 0x60:c->S+=2;return;
      case 0xc2:c->P&=~rom[pc++];cpu_p_to_mirrors(c);continue;
      case 0xaa:c->X=c->A;ActRaiserCpuHle_SetNegativeZero16(c,c->X);continue;
      case 0x98:c->A=c->Y;ActRaiserCpuHle_SetNegativeZero16(c,c->A);continue;
      case 0xe8:++c->X;ActRaiserCpuHle_SetNegativeZero16(c,c->X);continue;
      case 0x18:c->_flag_C=0;c->P&=~CPU_P_C;continue;
      case 0x38:c->_flag_C=1;c->P|=CPU_P_C;continue;
      case 0xf0:case 0xd0:case 0x80: {
        const int8_t offset=rom[pc++];if(op==0x80 || (op==0xf0?c->_flag_Z:!c->_flag_Z))pc+=offset;continue;
      }
      default:break;
    }
    v=WordAt(rom,pc);pc+=2;
    switch(op) {
      case 0x82:pc+=(int16_t)v;break;
      case 0x9c:Word(town_ram,v,0);break;
      case 0x8d:Word(town_ram,v,c->A);break;
      case 0x9d:Word(town_ram,(uint16_t)(v+c->X),c->A);break;
      case 0x9f:assert(rom[pc++]==0);Word(low,(uint16_t)(v+c->X),c->A);break;
      case 0xae:c->X=WordAt(town_ram,v);ActRaiserCpuHle_SetNegativeZero16(c,c->X);break;
      case 0xce:v=WordAt(town_ram,v)-1;Word(town_ram,WordAt(rom,pc-2),v);ActRaiserCpuHle_SetNegativeZero16(c,v);break;
      case 0xa0:c->Y=v;ActRaiserCpuHle_SetNegativeZero16(c,v);break;
      case 0x89:c->_flag_Z=!(c->A&v);c->P=(c->P&~CPU_P_Z)|(c->_flag_Z?CPU_P_Z:0);break;
      case 0xc9:c->_flag_C=c->A>=v;ActRaiserCpuHle_SetNegativeZero16(c,c->A-v);break;
      case 0xa9:case 0xad:case 0xbd:case 0xbf:case 0x29:case 0x69:case 0x6d:case 0xfd:
        if(op==0xad || op==0x6d)v=WordAt(town_ram,v);
        if(op==0xbd || op==0xfd)v=WordAt(town_ram,(uint16_t)(v+c->X));
        if(op==0xbf) { assert(rom[pc++]==3);v=WordAt(rom,(uint16_t)(v+c->X)); }
        if(op==0x29)v&=c->A;
        if(op==0x69 || op==0x6d)v+=c->A+c->_flag_C;
        if(op==0xfd)v=c->A-v-!c->_flag_C;
        c->A=v;ActRaiserCpuHle_SetNegativeZero16(c,v);break;
      default:assert(!"unexpected census opcode");
    }
  }
  assert(!"census exceeded bounded instruction count");
}
static void NativeParity(void) {
  for(unsigned source=0;source<2;++source)for(unsigned town=0;town<6;++town)
    for(unsigned flags=0;flags<256;++flags)for(unsigned mode=0;mode<2;++mode) {
      const unsigned p=CPU_P_V|CPU_P_C|CPU_P_I|CPU_P_N|(mode?CPU_P_M:0);
      CpuState c=Setup(town,p,false);const unsigned base=Base(town,false);
      town_ram[base+2]=flags;town_ram[base+3]=0x91;Word(town_ram,0x9f57+town*2,9);
      ArRegionalSupportSnapshot s=Profile(source);assert(ActRaiserTownCensus_Run(&c,&s)==0);
      const unsigned pop=WordAt(low,0x21c+town*2),support=WordAt(town_ram,0x6b26+town*2),people=WordAt(town_ram,0x7c05);
      CpuState native=Setup(town,p,source==1);const unsigned native_base=Base(town,source==1);
      town_ram[native_base+2]=flags;town_ram[native_base+3]=0x91;Word(town_ram,(source?0x9f4b:0x9f57)+town*2,9);
      Native(&native,source?0xbd27:0xc07e);
      /* Independent struct initializations need not have equal padding before
       * ram. Compare every CPU member, not uninitialized object representation. */
#define SAME(field) assert(c.field == native.field)
      SAME(A);SAME(X);SAME(Y);SAME(S);SAME(D);SAME(DB);SAME(PB);SAME(host_return_valid);
      SAME(P);SAME(m_flag);SAME(x_flag);SAME(emulation);SAME(_flag_N);SAME(_flag_V);
      SAME(_flag_Z);SAME(_flag_C);SAME(_flag_I);SAME(_flag_D);SAME(ram);
#undef SAME
      assert(pop==WordAt(low,(source?0x21b:0x21c)+town*2) && support==WordAt(town_ram,0x6b26+town*2));
      assert(people==WordAt(town_ram,0x7c05) && support==WordAt(town_ram,0x7c07) && !WordAt(town_ram,0x7c1d));
    }
}
static void BridgesAndGates(void) {
  for(unsigned source=0;source<3;++source)for(unsigned town=0;town<6;++town)for(unsigned toggle=0;toggle<2;++toggle) {
    CpuState c=Setup(town,CPU_P_M|CPU_P_C,false);g_settings.fix_bridge_limit=toggle;
    const unsigned base=Base(town,false),ext=0x1d74+town*64;
    memcpy(sram+0x1d70,"AXB1",4);memcpy(sram+ext,(uint8_t[]){8,12,0x81,0},4);
    memcpy(sram+ext+4,sram+ext,4); /* duplicate extension record never doubles support */
    Word(town_ram,0x6800+town*128+3*16+2*2,0x80);
    ArRegionalSupportSnapshot s=Profile(source);
    assert(ActRaiserBridgeExtension_Count(&c,0x7f,town)==1);
    assert(ActRaiserTownCensus_Run(&c,&s)==0 && WordAt(town_ram,0x6b26+town*2)==s.amount[4] && c.Y==0xabcd);
    memcpy(town_ram+base,sram+ext,4); /* retained native copy wins */
    assert(ActRaiserBridgeExtension_Count(&c,0x7f,town)==0);
    assert(ActRaiserTownCensus_Run(&c,&s)==0 && WordAt(town_ram,0x6b26+town*2)==s.amount[4]);
    for(unsigned i=1;i<128;++i)town_ram[base+4*i+2]=0xa0;
    assert(ActRaiserTownCensus_Run(&c,&s)==0 && WordAt(low,0x21c+town*2)==1018);
    Word(town_ram,0x6b18+town*2,0);Word(low,0x21c+town*2,777);Word(town_ram,0x6b26+town*2,888);
    const CpuState before=c;
    assert(ActRaiserTownCensus_Run(&c,&s)==0 && WordAt(low,0x21c+town*2)==777 && WordAt(town_ram,0x6b26+town*2)==888);
    assert(c.A==0 && c.Y==before.Y && c.X==before.X && c.S==before.S+2 && c.P==before.P);
    assert(!WordAt(town_ram,0x7c05) && !WordAt(town_ram,0x7c07) && WordAt(town_ram,0x7c1d)==128);
  }
}
static void Load(const char *path,uint8_t *code) { FILE *f=fopen(path,"rb");assert(f && !fseek(f,3*0x8000,SEEK_SET));assert(fread(code+0x8000,1,0x8000,f)==0x8000 && !fclose(f)); }
static void SemanticRefresh(void) {
  for (unsigned town=0;town<6;++town) for (unsigned source=0;source<3;++source)
    for (unsigned flags=0;flags<256;++flags) {
      CpuState cpu=Setup(town,0xff,false),before=cpu;
      ArRegionalSupportSnapshot snapshot=Profile(source);
      town_ram[Base(town,false)+2]=flags;
      Word(town_ram,0x9f57+2*town,1);
      static uint8_t expected_low[sizeof(low)],expected_town[sizeof(town_ram)];
      memcpy(expected_low,low,sizeof(low));memcpy(expected_town,town_ram,sizeof(town_ram));
      Word(expected_low,0x21c+2*town,ExpectedPeople(flags)+1);
      Word(expected_town,0x6b26+2*town,ExpectedSupport(flags,&snapshot));
      assert(ActRaiserTownCensus_Refresh(&cpu,town,&snapshot));
      assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(low,expected_low,sizeof(low)) &&
             !memcmp(town_ram,expected_town,sizeof(town_ram)));
      assert(!ActRaiserTownCensus_Refresh(&cpu,6,&snapshot));
      assert(!ActRaiserTownCensus_Refresh(&cpu,town,NULL));
      Word(town_ram,0x6b18+2*town,0);memcpy(expected_town,town_ram,sizeof(town_ram));
      assert(ActRaiserTownCensus_Refresh(&cpu,town,&snapshot));
      assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(low,expected_low,sizeof(low)) &&
             !memcmp(town_ram,expected_town,sizeof(town_ram)));
    }
}

int main(int argc,char **argv) {
  if(argc>1) { assert(argc==3);Load(argv[1],us_rom);Load(argv[2],jp_rom); }
  Policies();BridgesAndGates();SemanticRefresh();if(argc>1)NativeParity();
  puts("town census: 243 mixed support profiles, structure classes, native occupancy/bias/gates, extension bridges and optional US/JP ROM parity passed");return 0;
}
