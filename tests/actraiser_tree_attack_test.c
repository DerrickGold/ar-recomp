#include "actraiser/actraiser_tree_attack.h"
#include "regional/action/regional_action_motion.h"
#include "byte_order.h"
#include "quintet_lzss.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[65536];
static uint16_t snapshot;
static unsigned target,helper_calls,births,turns;
CpuReturnScope *g_cpu_return_scope,*g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
uint16_t ActRaiserRegional_ActionMotionSnapshot(void){return snapshot;}
int cpu_hle_tailcall_request(uint32 pc,uint32 from){assert(from==0xa975 || from==0xa9bf);target=pc;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(!bank || bank==0x7e);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(!bank || bank==0x7e);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static uint16_t Read(unsigned at){return ByteOrder_ReadLe16(memory+at);}
static void Write(unsigned at,uint16_t value){ByteOrder_WriteLe16(memory+at,value);}
static int16_t Signed(uint8_t value){return value<128?value:(int)value-256;}
static RecompReturn Returned(CpuState *cpu) {
  assert(cpu->host_return_valid && g_cpu_return_scope &&
      g_cpu_return_scope->entry_stack==cpu->S && Read(cpu->S+1)==0xa9be);
  cpu->S+=2;++helper_calls;return RECOMP_RETURN_NORMAL;
}
/* Model bounded helpers, not the controller under test. Live production
 * fixtures additionally exercise their actual generated implementations. */
RecompReturn bank_00_8E2F_M0X0(CpuState *cpu) {
  const unsigned x=cpu->X,at=0x4000+Read(0x4002+2*Read(x+0x1a))+4*Read(x+0x1c);
  if(memory[at]==255){cpu->_flag_C=1;Write(x+0x1c,0);}
  else {
    cpu->_flag_C=0;Write(x+0x22,memory[at]);Write(x+0x24,memory[at+1]);
    int16_t dx=Signed(memory[at+2]),dy=Signed(memory[at+3]);
    if(Read(x+0x28)&0x4000)dx=-dx;if(Read(x+0x28)&0x8000)dy=-dy;
    Write(x+6,dx);Write(x+8,dy);
  }
  return Returned(cpu);
}
static RecompReturn Allocate(CpuState *cpu,unsigned start) {
  unsigned y;for(y=start;y<0x1aa0;y+=64)if(Read(y)&0x4000)break;
  cpu->_flag_C=y==0x1aa0;
  if(!cpu->_flag_C) {
    memcpy(memory+y,memory+cpu->X,64);Write(y,0x2000);Write(y+0x14,0);
    Write(y+6,0);Write(y+8,0);Write(y+0x2e,0);Write(y+0x2c,0);Write(y+0x3a,cpu->X);++births;
  } else y=0x1aa2;
  cpu->Y=y;return Returned(cpu);
}
RecompReturn bank_00_8538_M0X0(CpuState *cpu){return Allocate(cpu,0x8a0);}
RecompReturn bank_00_853D_M0X0(CpuState *cpu){return Allocate(cpu,cpu->X);}
RecompReturn bank_00_85E9_M0X0(CpuState *cpu){Write(cpu->X+0x28,0x4000);return Returned(cpu);}
RecompReturn bank_00_871E_M0X0(CpuState *cpu){Write(cpu->X+0x28,Read(cpu->X+0x28)^0x4000);++turns;return Returned(cpu);}
RecompReturn bank_00_8FE7_M0X0(CpuState *cpu){cpu->_flag_C=Read(cpu->X+4)>=432;return Returned(cpu);}
RecompReturn bank_00_9108_M0X0(CpuState *cpu){cpu->_flag_C=1;return Returned(cpu);}

static void Program(unsigned state,const uint8_t *rows,unsigned length) {
  const unsigned at=0x4100+state*40;
  Write(0x4002+2*state,at-0x4000);memcpy(memory+at,rows,length);memory[at+length]=255;
}
#define PROGRAM(state,...) do{static const uint8_t rows[]={__VA_ARGS__};Program(state,rows,sizeof(rows));}while(0)
static CpuState Setup(void) {
  memset(memory,0,sizeof(memory));helper_calls=births=turns=0;Write(0x18,0x0101);Write(0x4000,0x600);
  PROGRAM(7,20,2,0,0,21,2,0,0,20,2,0,0,21,2,0,0,20,7,0,0,21,7,0,0);
  PROGRAM(10,6,63,0,0);PROGRAM(16,13,3,0,2,13,3,0,2);PROGRAM(17,13,5,0,0);
  PROGRAM(18,22,3,255,0,23,3,255,0,24,3,255,0,25,3,255,0);
  PROGRAM(20,25,9,0,0,24,9,0,0,26,9,0,0);
  PROGRAM(22,14,3,1,253,14,3,1,254,16,3,1,255,16,3,1,1,17,3,1,2,17,3,1,3);
  PROGRAM(23,25,5,0,254,25,5,0,255,25,5,0,0,25,5,0,1,25,5,0,2,22,3,0,255,22,5,0,0,22,3,0,1);
  PROGRAM(24,15,3,255,253,15,3,255,254,17,3,255,255,17,3,255,1,16,3,255,2,16,3,255,3);
  for(unsigned x=0x6a0;x<0x1aa0;x+=64)Write(x,0x4000);
  for(unsigned x=0x12a0;x<=0x12e0;x+=64) {
    Write(x,0);Write(x+2,1792);Write(x+4,x==0x12a0?432:392);
    Write(x+0x32,x==0x12a0?0xa934:0xa9b3);Write(x+0x16,0x4000);memory[x+0x18]=0x7e;
    Write(x+0x2a,1);Write(x+0x30,x==0x12a0?0:0x30);Write(x+0x3e,0);
  }
  snapshot=1u<<kArRegionalActionMotion_TreeSeeds;
  return (CpuState){.X=0x12a0,.S=0x1ef0,.A=0x1234,.Y=0x4567};
}
static void Run(CpuState *cpu) {
  const uint16_t stack=cpu->S;target=0;
  assert(ActRaiser_TreeControllerEntry(cpu));
  assert(ActRaiser_TreeController(cpu)==RECOMP_RETURN_TAILCALL && cpu->S==stack && !g_cpu_return_scope);
  assert(target==0xa948 || target==0x85b7);
  if(target==0x85b7)Write(cpu->X,0x4000);
}
static void Tick(CpuState *cpu) {
  for(unsigned x=0x6a0;x<0x1aa0;x+=64) {
    if((Read(x)&0xc400) || Read(x+0x32)!=0xa9b3)continue;
    Write(x+2,Read(x+2)+Read(x+6));Write(x+4,Read(x+4)+Read(x+8));
    if(Read(x+0x24)){Write(x+0x24,Read(x+0x24)-1);continue;}
    cpu->X=x;if(ActRaiser_TreeControllerEntry(cpu))Run(cpu);
  }
}
static void Lifecycle(void) {
  for(unsigned free_slots=0;free_slots<3;++free_slots) {
    CpuState cpu=Setup();assert(ActRaiser_TreePrepareEntry(&cpu));
    assert(ActRaiser_TreePrepare(&cpu)==RECOMP_RETURN_TAILCALL && target==0xa948);
    assert(Read(0x12a0+0x24)==128 && Read(0x12a0+0x12)==0xa97b && Read(0x12e0+0x38)==1);
    for(unsigned x=0x1320+free_slots*64;x<0x1aa0;x+=64)Write(x,0);
    for(unsigned t=0;t<28;++t)Tick(&cpu);
    assert(births==0);
    Tick(&cpu);assert(births==free_slots && !Read(0x12e0+0x38) && !Read(0x12e0+0x3e));
    if(free_slots)assert(Read(0x1322)==1760 && Read(0x1324)==416 && Read(0x133a)==16 && Read(0x135a)==0x12e0);
    if(free_slots==2)assert(Read(0x1362)==1824 && Read(0x1364)==416);
    snapshot=0; /* Debug-cache loss must not abandon existing children. */
    for(unsigned t=0;t<700;++t)Tick(&cpu);
    assert(births==free_slots*3 && turns==free_slots*16);
    for(unsigned x=0x6a0;x<0x1aa0;x+=64)
      if(x!=0x12e0 && Read(x+0x32)==0xa9b3)assert(Read(x)&0x4000);
    assert(!g_cpu_return_scope && cpu.S==0x1ef0);
  }
}
static void Guards(void) {
  for(unsigned bad=0;bad<21;++bad) {
    CpuState cpu=Setup();
    switch(bad) {
      case 0:snapshot=0;break;case 1:cpu.PB=1;break;case 2:cpu.DB=1;break;case 3:cpu.D=1;break;
      case 4:cpu.m_flag=1;break;case 5:cpu.x_flag=1;break;case 6:cpu.emulation=1;break;
      case 7:cpu._flag_D=1;break;case 8:cpu.P=CPU_P_D;break;case 9:cpu.X++;break;
      case 10:cpu.X=0x1aa0;break;case 11:Write(0x18,0x0201);break;
      case 12:Write(cpu.X+0x32,0xa9b3);break;case 13:Write(cpu.X+0x3a,0x8e0);break;
      case 14:Write(cpu.X+0x16,0x5000);break;case 15:memory[cpu.X+0x18]=0x7f;break;
      case 16:Write(0x1312,0xa934);break;case 17:Write(0x12e0,0x4000);break;
      case 18:Write(0x4000,0xffff);break;case 19:memory[0x4100+7*40]++;break;
      case 20:Write(cpu.X+0x3c,1);break;
    }
    uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    assert(!ActRaiser_TreePrepareEntry(&cpu) && !memcmp(before,memory,sizeof(memory)) && !helper_calls);
  }
}
static void Rom(const char *path,unsigned region) {
  static uint8_t rom[0x100000],blob[4096],baseline_blob[4096];const unsigned offsets[]={0xcd695,0xcbe43,0xcd696,0xcce78,0xcce78};
  FILE *f=fopen(path,"rb");assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom));fclose(f);
  const size_t written=ByteOrder_ReadLe16(rom+offsets[region]);assert(written==3091);
  assert(QuintetLzss_DecompressAsset(rom+offsets[region],sizeof(rom)-offsets[region],blob,written,NULL));
  CpuState cpu=Setup();memcpy(memory+0x4000,blob,written);
  assert(ActRaiser_TreePrepareEntry(&cpu));
  if(!region)memcpy(baseline_blob,blob,written);
  const unsigned visuals[]={6,13,14,15,16,17,20,21,22,23,24,25,26};
  for(unsigned i=0;i<sizeof(visuals)/sizeof(visuals[0]);++i) {
    const unsigned a=ByteOrder_ReadLe16(blob+ByteOrder_ReadLe16(blob)+visuals[i]*2);
    const unsigned b=ByteOrder_ReadLe16(baseline_blob+ByteOrder_ReadLe16(baseline_blob)+visuals[i]*2);
    const unsigned length=5+7*blob[a+4];
    assert(a+length<=written && b+length<=written && !memcmp(blob+a,baseline_blob+b,length));
  }
  const unsigned peer[]={0xa9bf,0xa98a,0xa5a0,0xa5a2,0xa5a5};
  if(!region)assert(rom[peer[region]-0x8000]==0x60);
  else {
    static const uint8_t guard[]={0xbd,0x38,0,0xd0,1,0x60};
    assert(!memcmp(rom+peer[region]-0x8000,guard,sizeof(guard)));
    const unsigned request[]={0,0xa93d,0xa553,0xa555,0xa558};
    static const uint8_t wait[]={0xfe,0x78,0,0xa9,0x80,0,0x20};
    assert(!memcmp(rom+request[region]-0x8000,wait,sizeof(wait)));
    assert(ByteOrder_ReadLe16(rom+request[region]-0x8000+7)==(region==1?0x86e9:0x8612));
  }
  printf("tree %u: native sequences, 13 compositions and controller guards verified\n",region);
}
int main(int argc,char **argv) {
  Lifecycle();Guards();
  if(argc==6)for(unsigned i=0;i<5;++i)Rom(argv[i+1],i);
  puts("tree allocation, phases, retirement and guards: passed");return 0;
}
