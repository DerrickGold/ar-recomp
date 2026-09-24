#include "actraiser/actraiser_fire_enemy.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/action/regional_fire_enemy.h"
#include "quintet_lzss.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536],snapshot;
static unsigned target,origin;
uint8_t ActRaiserRegional_FireSnapshot(void){return snapshot;}
int cpu_hle_tailcall_request(uint32_t pc,uint32_t from){target=pc;origin=from;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Write(unsigned at,unsigned value){ByteOrder_WriteLe16(memory+at,(uint16_t)value);}
static CpuState Setup(unsigned slot,unsigned state,unsigned flip,unsigned value,unsigned flags) {
  memset(memory,0xa5,sizeof(memory));const unsigned x=0x6a0+64*slot;
  memory[0x18]=3;Write(x+0x32,0xc3a5);Write(x+0x16,0x4000);memory[x+0x18]=0x7e;
  Write(x+0x1a,state);Write(x+0x28,flip);
  CpuState cpu={.X=x,.Y=0x1aa2,.A=value,.S=0x1ef0,.P=(uint8_t)flags};cpu_p_to_mirrors(&cpu);return cpu;
}
static void Compare(CpuState *cpu,unsigned value) {
  cpu->_flag_C=cpu->A>=value;cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
static void Prefixes(void) {
  unsigned cases=0;
  for(unsigned n=0;n<81;++n) {
    ArRegionalFirePolicy policy;unsigned digits=n;
    for(unsigned i=0;i<4;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalFire_Resolve(&policy,&snapshot));
    for(unsigned value=0;value<256;++value)for(unsigned carry=0;carry<2;++carry) {
      CpuState cpu=Setup(value&1?0:79,12,0,value,CPU_P_V|(carry?CPU_P_C:CPU_P_N)),expected=cpu;
      uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(before));
      assert(ActRaiser_FireChildEntry(&cpu)==(policy.source[2]==1));
      if(policy.source[2]==1)assert(ActRaiser_FireChild(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc408 && origin==0xc405);
      else Compare(&cpu,160);
      Compare(&expected,policy.source[2]==1?128:160);
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
      assert(ActRaiser_FireBounceEntry(&cpu)==(policy.source[3]==1));
      if(policy.source[3]==1)assert(ActRaiser_FireBounce(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc40d && origin==0xc40a);
      else Compare(&cpu,242);
      Compare(&expected,policy.source[3]==1?210:242);
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));++cases;
    }
    for(unsigned flip=0;flip<4;++flip) {
      CpuState cpu=Setup(11,12,flip<<14,64,CPU_P_V|CPU_P_C|CPU_P_N),expected=cpu;
      uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(before));
      const bool close=policy.source[1]==1 && (flip&1);
      assert(ActRaiser_FireCloseEntry(&cpu)==close);
      if(close){assert(ActRaiser_FireClose(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc3f0 && origin==0xc3dd);Compare(&expected,96);}
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
      Write(cpu.X+0x1a,13);memcpy(before,memory,sizeof(before));
      assert(ActRaiser_FireHoverEntry(&cpu)==(policy.source[1]==1));
      if(policy.source[1]==1)assert(ActRaiser_FireHover(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc3f6 && origin==0xc3ea);
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
    }
  }
  snapshot=15;
  for(unsigned value=0;value<256;++value)for(unsigned negative=0;negative<2;++negative) {
    CpuState cpu=Setup(11,12,0x4000,value,negative?CPU_P_N:0);
    assert(ActRaiser_FireCloseEntry(&cpu)==(negative && value<96));
  }
  for(unsigned hover=0;hover<2;++hover)for(unsigned bad=0;bad<17;++bad) {
    CpuState cpu=Setup(0,hover?13:12,0x4000,200,0);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x660;break;case 10:cpu.X=0x1aa0;break;case 11:memory[0x18]=4;break;
      case 12:Write(cpu.X+0x32,0xc66f);break;case 13:Write(cpu.X+0x16,0x5000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x1a,15);break;case 16:snapshot=16;break;
    }
    assert(!ActRaiser_FireCloseEntry(&cpu) && !ActRaiser_FireHoverEntry(&cpu) && !ActRaiser_FireChildEntry(&cpu) && !ActRaiser_FireBounceEntry(&cpu));
    snapshot=15;
  }
  CpuState cpu=Setup(0,12,0,256,0);assert(!ActRaiser_FireChildEntry(&cpu) && !ActRaiser_FireBounceEntry(&cpu));
  printf("fire enemy: %u random-byte/mixed-policy cases, 324 facing cases and 34 invalid-owner cases passed\n",cases);
}
static int16_t Signed(uint8_t value){return value<128?value:(int16_t)((int)value-256);}
static void Roms(char **paths) {
  const unsigned offsets[]={0xd0000,0xce8fe,0xd0000,0xcf22a,0xceb10};
  const unsigned child[]={0xc405,0xc494,0xc0cc,0xc0ce,0xc0d1};
  static uint8_t rom[1048576],blobs[5][65536];unsigned cases=0;
  for(unsigned r=0;r<5;++r) {
    FILE *file=fopen(paths[r],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    const unsigned size=ByteOrder_ReadLe16(rom+offsets[r]);
    assert(QuintetLzss_DecompressAsset(rom+offsets[r],sizeof(rom)-offsets[r],blobs[r],size,NULL));
    const uint8_t *cmp=rom+child[r]-0x8000;
    assert(cmp[0]==0xc9 && ByteOrder_ReadLe16(cmp+1)==(r==1?128:160));
    assert(cmp[5]==0xc9 && ByteOrder_ReadLe16(cmp+6)==(r==1?210:242));
    for(unsigned state=12;state<=14;++state)for(unsigned row=0;row<8;++row) {
      const unsigned us=ByteOrder_ReadLe16(blobs[0]+2+state*2)+row*4;
      const unsigned other=ByteOrder_ReadLe16(blobs[r]+2+state*2)+row*4;
      const uint8_t *a=blobs[0]+us,*b=blobs[r]+other;
      int16_t dx=Signed(a[2]),dy=Signed(a[3]);
      const bool projected=ArRegionalFire_CurveRow(r==1?1:0,state,row,a[0],a[1],&dx,&dy);
      assert(projected==(r==1 && state!=12));
      assert(a[0]==b[0] && a[1]==b[1] && dx==Signed(b[2]) && dy==Signed(b[3]));++cases;
    }
  }
  printf("fire enemy: %u five-ROM movement rows and all threshold signatures verified\n",cases);
}
int main(int argc,char **argv){assert(argc==1 || argc==6);Prefixes();if(argc==6)Roms(argv+1);return 0;}
