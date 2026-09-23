#include "actraiser/actraiser_statue_volley.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/regional_volley.h"
#include "quintet_lzss.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static bool enabled;
static uint32_t target, origin;
bool ActRaiserRegional_DoubleStatueVolley(void) { return enabled; }
int cpu_hle_tailcall_request(uint32_t pc, uint32_t from) { target=pc;origin=from;return 1; }
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) { (void)cpu;assert(!bank);return memory[at]; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) { return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) { (void)cpu;assert(!bank);memory[at]=value; }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) { cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8); }
static void Write(unsigned at,uint16_t value) { ByteOrder_WriteLe16(memory+at,value); }
static CpuState Setup(unsigned x,unsigned facing,unsigned state,unsigned count) {
  memset(memory,0,sizeof(memory));memory[0x18]=2;
  Write(x+0x16,0x4000);memory[x+0x18]=0x7e;
  Write(x+0x1a,state);Write(x+0x32,facing?0xbd84:0xbd76);Write(x+0x38,count);
  CpuState cpu={.A=0x9876,.X=x,.Y=0x4321,.S=0x1ef0,.P=CPU_P_N|CPU_P_Z|CPU_P_C|CPU_P_V};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckBoundaries(void) {
  for(unsigned on=0;on<2;++on)for(unsigned facing=0;facing<2;++facing)
  for(unsigned slot=0;slot<80;++slot)for(unsigned phase=0;phase<4;++phase)for(unsigned count=0;count<4;++count) {
    static const unsigned states[]={14,15,33,35};enabled=on;
    CpuState cpu=Setup(0x6a0+64*slot,facing,states[phase],count),expected=cpu;
    /* No new offscreen gate between shots. */
    Write(cpu.X+0x30,0x400);
    const bool begin=on && phase==0,repeat=on && phase==1 && count==1;
    assert(ActRaiser_StatueVolleyBeginEntry(&cpu)==begin);
    assert(ActRaiser_StatueVolleyRepeatEntry(&cpu)==repeat);
    uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    if(begin || repeat) {
      assert((begin?ActRaiser_StatueVolleyBegin(&cpu):ActRaiser_StatueVolleyRepeat(&cpu))==RECOMP_RETURN_TAILCALL);
      assert(target==0xbda2 && origin==(begin?0xbd9f:0xbda8));
      expected.A=15;ActRaiserCpuHle_SetNegativeZero16(&expected,15);
      ByteOrder_WriteLe16(before+cpu.X+0x38,begin?1:0);
    }
    assert(!memcmp(&expected,&cpu,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
  }
  for(unsigned which=0;which<16;++which) {
    enabled=true;CpuState cpu=Setup(0x8e0,0,14,1);
    switch(which) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;
      case 8:cpu.X=0x69f;break;case 9:cpu.X=0x1aa0;break;case 10:cpu.X++;break;
      case 11:memory[0x18]=1;break;case 12:Write(0x912,0xbba8);break;
      case 13:Write(0x8f6,0x5000);break;case 14:memory[0x8f8]=0x7f;break;
      case 15:Write(0x8fa,33);break;
    }
    assert(!ActRaiser_StatueVolleyBeginEntry(&cpu) && !ActRaiser_StatueVolleyRepeatEntry(&cpu));
  }
  assert(!ActRaiser_StatueVolleyBeginEntry(NULL) && !ActRaiser_StatueVolleyRepeatEntry(NULL));
}

typedef struct Program {
  uint8_t code[32768],animation[65536];
  uint16_t root,sequence,spawn;
} Program;
typedef struct Trace { unsigned count,shot[8],end,allocated; } Trace;
static uint8_t Code(const Program *p,uint16_t at) { assert(at>=0x8000);return p->code[at-0x8000]; }
static uint16_t Word(const Program *p,uint16_t at) { return Code(p,at)|(uint16_t)Code(p,at+1)<<8; }
static unsigned Duration(const Program *p,unsigned state) {
  unsigned at=ByteOrder_ReadLe16(p->animation+2+2*state),total=0;
  while(p->animation[at]!=255) { assert(at<65530);total+=p->animation[at+1]+1;at+=4; }
  return total;
}
/* Read the actual five-ROM root programs. The native sequence and allocator
 * CALLS are trace boundaries, not reimplementations of those helpers. Compare
 * call ordering/timing, skipped spawns and activation gates independently of
 * the C adapter. Full native scheduling/collision remains integration scope. */
static Trace Run(const Program *p,bool enhanced,unsigned free_slots,bool offscreen,unsigned die_after) {
  enabled=enhanced;CpuState cpu=Setup(0x8e0,0,14,0xbeef);
  Trace trace={0};unsigned tick=0,cycles=0;uint16_t pc=p->root;
  for(unsigned step=0;step<400;++step) {
    if(enhanced && pc==0xbd9f && ActRaiser_StatueVolleyBeginEntry(&cpu)) {
      assert(ActRaiser_StatueVolleyBegin(&cpu)==RECOMP_RETURN_TAILCALL);pc=target;
    } else if(enhanced && pc==0xbda8 && ActRaiser_StatueVolleyRepeatEntry(&cpu)) {
      assert(ActRaiser_StatueVolleyRepeat(&cpu)==RECOMP_RETURN_TAILCALL);pc=target;
    }
    const uint8_t opcode=Code(p,pc++);
    if(opcode==0x60) { trace.end=tick;return trace; }
    if(opcode==0xf0) { const int8_t delta=(int8_t)Code(p,pc++);if(cpu._flag_Z)pc+=delta;continue; }
    const uint16_t operand=Word(p,pc);pc+=2;
    switch(opcode) {
      case 0xbd:
        assert(operand==0x30);cpu.A=cpu_read16(&cpu,0,cpu.X+operand);ActRaiserCpuHle_SetNegativeZero16(&cpu,cpu.A);break;
      case 0x89:
        assert(operand==0x400);cpu._flag_Z=!(cpu.A&operand);break;
      case 0xa9:cpu.A=operand;ActRaiserCpuHle_SetNegativeZero16(&cpu,cpu.A);break;
      case 0x20:
        if(operand==p->sequence) {
          assert(cpu.A==14 || cpu.A==15);Write(cpu.X+0x1a,cpu.A);tick+=Duration(p,cpu.A);
        } else {
          assert(operand==p->spawn && trace.count<8);
          trace.shot[trace.count++]=tick;if(free_slots){--free_slots;++trace.allocated;}
          if(offscreen)Write(cpu.X+0x30,0x400);
          if(trace.count==die_after){trace.end=tick;return trace;}
        }
        break;
      case 0x82:
        ++tick;
        if(++cycles==3){trace.end=tick;return trace;}
        pc=p->root;break; /* Native 86D0 resets this controller on next update. */
      default:assert(!"unexpected native statue root opcode");
    }
  }
  assert(!"unterminated statue program");return trace;
}
static void CheckOriginal(int argc,char **argv) {
  static Program programs[5];
  static const unsigned roots[]={0xbd90,0xbe24,0xba48,0xba4a,0xba4d};
  static const unsigned sequence[]={0x8657,0x8646,0x856f,0x856f,0x856f};
  static const unsigned spawn[]={0xbdb1,0xbe4e,0xba72,0xba74,0xba77};
  static uint8_t rom[1048576];
  assert(argc==6);
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(argv[region+1],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    Program *p=&programs[region];memcpy(p->code,rom,32768);
    p->root=roots[region];p->sequence=sequence[region];p->spawn=spawn[region];
    const unsigned offset=region==1?0xc9fab:0x5782e,size=ByteOrder_ReadLe16(rom+offset);
    assert(size<=sizeof(p->animation) && QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,
        p->animation,size,NULL));
    assert(Duration(p,14)==60 && Duration(p,15)==16);
    /* Both native helpers used by this root leave its local repeat counter
     * unused. 8657's only parent writes are the resume word and reader state;
     * allocation initializes a CHILD, never the parent's +38. */
    assert(Code(p,p->sequence)==0x20 && Code(p,p->sequence+3)==0x68 &&
        Code(p,p->sequence+4)==0x9d && Word(p,p->sequence+5)==0x1e);
    for(unsigned free_slots=0;free_slots<3;++free_slots)for(unsigned offscreen=0;offscreen<2;++offscreen)
    for(unsigned death=0;death<3;++death) {
      const Trace native=Run(p,false,free_slots,offscreen,death);
      const Trace host=Run(&programs[0],region!=0,free_slots,offscreen,death);
      assert(!memcmp(&native,&host,sizeof(native)));
      if(!death && !offscreen) {
        assert(native.count==(region?6:3));
        assert(native.shot[region?2:1]-native.shot[0]==(region?153:137));
        if(region)assert(native.shot[1]-native.shot[0]==16);
      }
    }
  }
  puts("five original root programs: 90 call-order/timing/allocation-gate traces match US/hybrid");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);CheckBoundaries();
  if(argc==6)CheckOriginal(argc,argv);
  puts("statue volley adapter boundaries passed");return 0;
}
