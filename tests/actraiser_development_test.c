#include "actraiser/actraiser_development.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope,CpuState *cpu) { (void)scope;(void)cpu;return 0; }
static uint8_t ram[0x20000], initial[0x20000], expected[0x20000], code[65536];
static unsigned calls,stop,actor_calls,visual_calls,event_calls,cycle_calls;
static bool long_calls[32];
static bool posted;
static RecompReturn escape;
typedef struct Trace {uint32_t leaf;uint16_t a,x,y,s,frame;uint8_t p,db;} Trace;
static Trace trace[32],reference_trace[32];
uint8 cpu_read8(CpuState *c,uint8 bank,uint16 address) {
  (void)c;if(bank!=0 && bank!=0x7f)fprintf(stderr,"unexpected read %02x:%04x calls=%u\n",bank,address,calls);
  assert(bank==0 || bank==0x7f);return ram[(bank==0x7f?0x10000:0)+address];
}
uint16 cpu_read16(CpuState *c,uint8 bank,uint16 address) {return cpu_read8(c,bank,address)|cpu_read8(c,bank,address+1)<<8;}
void cpu_write8(CpuState *c,uint8 bank,uint16 address,uint8 value) {
  (void)c;assert(bank==0 || bank==0x7f);ram[(bank==0x7f?0x10000:0)+address]=value;
}
void cpu_write16(CpuState *c,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(c,bank,address,value);cpu_write8(c,bank,address+1,value>>8);
}
static void A(CpuState *c,uint16_t value) {
  cpu_write_a_m(c,value);
  if(c->m_flag) ActRaiserCpuHle_SetNegativeZero8(c,value); else ActRaiserCpuHle_SetNegativeZero16(c,value);
}
static RecompReturn Leaf(CpuState *c,uint32_t target,unsigned bytes) {
  assert(calls<32);
  long_calls[calls]=bytes==3;
  trace[calls++]=(Trace){target,c->A,c->X,c->Y,c->S,cpu_read16(c,0,c->S+1),c->P,c->DB};
  if(calls==stop){c->S=0x1789;c->PB=0x42;return escape;}
  if(target==0x008519)c->DB=0x7f;
  if(target==0x01b898){++actor_calls;if(posted)cpu_write16(c,0x7f,0x96e8,4);}
  if(target==0x039e40)++visual_calls;
  if(target==0x03826d)++event_calls;
  if(target==0x038271)++cycle_calls;
  A(c,0x6f23);c->X=0x4444;c->Y=0x8888;c->S+=bytes;
  return RECOMP_RETURN_NORMAL;
}
#define STUB(bank,pc,m,bytes) RecompReturn bank_##bank##_##pc##_M##m##X0(CpuState *c){return Leaf(c,0x##bank##pc,bytes);}
STUB(00,8519,0,3) STUB(03,8238,0,2) STUB(03,C147,0,2) STUB(03,826D,0,2)
STUB(03,F5BE,0,2) STUB(03,8271,0,2) STUB(03,8E0C,0,2) STUB(03,89F7,0,2)
STUB(03,86F1,0,2) STUB(03,9DE4,0,2) STUB(03,B97F,0,2) STUB(01,B898,0,3)
STUB(01,B1B7,0,3) STUB(03,AF54,0,2) STUB(01,9840,0,3) STUB(03,AF47,0,2)
STUB(01,929E,1,2) STUB(02,AFF8,1,3) STUB(01,B21B,1,3) STUB(01,93BE,1,2)
STUB(02,C206,1,3) STUB(01,ACD9,0,3) STUB(03,9E40,0,3) STUB(01,93CB,1,2)
#undef STUB

static uint16_t Address(uint8_t bank,uint16_t address,bool jp) {
  if(bank==0 && address==0x0349)return 0x0347; /* PAL game-mode relocation */
  if(!jp)return address;
  if(bank==0 && address==0x0335)return 0x0347;
  if(bank==0x7f) {
    if(address==0x9744)return 0x9750;
    if(address>=0x96dc && address<=0x96e0)return address+12;
  }
  return address;
}
static void Target(uint32_t *target,uint16_t *frame,bool jp) {
  if(*target==0x008431){*target=0x008519;return;} /* PAL DB setter */
  if(!jp)return;
  static const struct {uint32_t jp,us;uint16_t frame;} map[]={
    {0x008513,0x008519,0x819d},{0x038243,0x038238,0x81a0},{0x03bdf0,0x03c147,0x81b4},
    {0x038285,0x03826d,0x81d4},{0x03f09a,0x03f5be,0x81d9},{0x03828c,0x038271,0x81ed},
    {0x038cf9,0x038e0c,0x81f0},{0x0388e4,0x0389f7,0x81f8},{0x0386a1,0x0386f1,0x81fd},
    {0x039bcf,0x039de4,0x8200},{0x03b708,0x03b97f,0x8203},{0x01b83a,0x01b898,0x820a},
    {0x01b187,0x01b1b7,0x820e},{0x03ad1c,0x03af54,0x822d},{0x0197df,0x019840,0x8231},
    {0x03ad0f,0x03af47,0x8234}};
  for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);++i)if(*target==map[i].jp){*target=map[i].us;*frame=map[i].frame;return;}
  assert(!"unmapped JP development callee");
}

/* Interpret bounded native coordinators; callees are observed common contracts,
 * not emulations of their differing regional game logic. JP owners/fields and
 * pushed caller words are explicitly normalized. No ROM bytes are changed. */
static void Native(CpuState *c,uint16_t pc,bool jp) {
  for(unsigned n=0;n<160;++n) {
    const uint8_t op=code[pc++];
    if(op==0x08){cpu_mirrors_to_p(c);cpu_write8(c,0,c->S--,c->P);continue;}
    if(op==0x8b){cpu_write8(c,0,c->S--,c->DB);continue;}
    if(op==0xab){c->DB=cpu_read8(c,0,++c->S);ActRaiserCpuHle_SetNegativeZero8(c,c->DB);continue;}
    if(op==0x28){c->P=cpu_read8(c,0,++c->S);cpu_p_to_mirrors(c);continue;}
    if(op==0x60 || op==0x6b){c->S+=op==0x60?2:3;return;}
    if(op==0x4a){c->_flag_C=c->A&1;c->P=(c->P&~1u)|c->_flag_C;A(c,c->A>>1);continue;}
    uint16_t v=code[pc++];
    if(op==0xe2 || op==0xc2){c->P=op==0xe2?c->P|v:c->P&~v;cpu_p_to_mirrors(c);continue;}
    if(op==0xf0 || op==0xd0 || op==0x90 || op==0x80) {
      if(op==0x80 || (op==0xf0 && c->_flag_Z) || (op==0xd0 && !c->_flag_Z) || (op==0x90 && !c->_flag_C))pc+=(int8_t)v;
      continue;
    }
    if(op==0xa9 || op==0xc9) {
      if(!c->m_flag)v|=code[pc++]<<8;
      if(op==0xa9)A(c,v);
      else {c->_flag_C=cpu_read_a_m(c)>=v;c->P=(c->P&~1u)|c->_flag_C;ActRaiserCpuHle_SetNegativeZero16(c,c->A-v);}
      continue;
    }
    v|=code[pc++]<<8;
    if(op==0x82){pc+=(int16_t)v;continue;}
    if(op==0x20 || op==0x22) {
      uint8_t bank=op==0x22?code[pc++]:c->PB;uint32_t target=((uint32_t)bank<<16)|v;
      uint16_t frame=pc-1;Target(&target,&frame,jp);bank=target>>16;
      const uint8_t old_pb=c->PB;
      if(op==0x22)cpu_write8(c,0,c->S--,old_pb);
      ActRaiserCpuHle_PushWord(c,frame);c->PB=bank;c->host_return_valid=1;
      assert(Leaf(c,target,op==0x22?3:2)==RECOMP_RETURN_NORMAL);c->PB=old_pb;
      continue;
    }
    uint8_t bank=c->DB;if(op==0xaf)bank=code[pc++];v=Address(bank,v,jp);
    switch(op) {
      case 0xad:case 0xaf:A(c,cpu_read16(c,bank,v));break;
      case 0x8d:cpu_write16(c,bank,v,c->A);break;
      case 0x9c:cpu_write16(c,bank,v,0);break;
      case 0xee:{const uint16_t value=cpu_read16(c,bank,v)+1;cpu_write16(c,bank,v,value);ActRaiserCpuHle_SetNegativeZero16(c,value);break;}
      default:fprintf(stderr,"unexpected %02x at %04x\n",op,pc-1);assert(0);
    }
  }
  assert(!"coordinator did not return");
}

static CpuState Setup(unsigned flags,bool narrow) {
  memset(ram,0,sizeof(ram));calls=stop=actor_calls=visual_calls=event_calls=cycle_calls=0;posted=false;
  memset(trace,0,sizeof(trace));
  CpuState c={.PB=3,.DB=1,.A=0xcafe,.X=0xbeef,.Y=0x3210,.S=0x1efa,.m_flag=narrow,
      ._flag_C=flags&1,._flag_D=(flags>>1)&1,._flag_I=(flags>>2)&1,._flag_V=(flags>>3)&1};
  cpu_mirrors_to_p(&c);cpu_write16(&c,0,0x0347,1);
  cpu_write16(&c,0x7f,0x96ea,0xfff3);cpu_write16(&c,0x7f,0x96ec,0x803f);
  return c;
}
static unsigned Cases(bool native,bool jp) {
  static const uint16_t phases[]={0,1,3,7,8,65535},clocks[]={0,479,719,65535},dividers[]={0,1,4,5,65535};
  const ArRegionalDevelopmentSnapshot snapshot={jp?5:1,jp?480:720,jp?5:1};
  unsigned count=0;
  for(unsigned phase=0;phase<6;++phase)for(unsigned clock=0;clock<4;++clock)
    for(unsigned divider=0;divider<5;++divider)for(unsigned shape=0;shape<32;++shape) {
      CpuState c=Setup(shape%16,shape&1);posted=shape&2;
      cpu_write16(&c,0x7f,0x9200,phases[phase]);cpu_write16(&c,0x7f,0x91fe,clocks[clock]);
      cpu_write16(&c,0x7f,0x7ced,dividers[divider]);cpu_write16(&c,0x7f,0x9750,(shape>>2)&1);
      cpu_write16(&c,0x7f,0x7cfb,(shape>>3)&1);cpu_write16(&c,0,0x0347,shape&16?7:1);
      CpuState ref=c;memcpy(initial,ram,sizeof(ram));
      assert(ActRaiserDevelopment_Master(&c,&snapshot)==RECOMP_RETURN_NORMAL);
      assert(c.S==ref.S+3 && c.P==ref.P && c.DB==ref.DB && c.PB==ref.PB);
      assert(actor_calls==(shape&16?0u:1u));
      if(native) {
        const unsigned expected_calls=calls;memcpy(reference_trace,trace,sizeof(trace));memcpy(expected,ram,sizeof(ram));
        memcpy(ram,initial,sizeof(ram));calls=actor_calls=visual_calls=event_calls=cycle_calls=0;
        Native(&ref,jp?0x8190:0x8193,jp);
        assert(calls==expected_calls);
        if(memcmp(reference_trace,trace,calls*sizeof(Trace))) {
          fprintf(stderr,"trace diff jp=%d phase=%u clock=%u divider=%u shape=%u\n",jp,phase,clock,divider,shape);assert(0);
        }
        assert(!memcmp(&ref,&c,sizeof(c)) && !memcmp(expected,ram,sizeof(ram)));
      }
      ++count;
    }
  return count;
}

static unsigned Effects(bool native) {
  unsigned count=0;
  for(unsigned world=0;world<2;++world)for(unsigned flags=0;flags<16;++flags)
    for(unsigned narrow=0;narrow<2;++narrow)for(unsigned phase=0;phase<7;++phase) {
      const ArRegionalDevelopmentSnapshot us={1,720,1};
      CpuState c=Setup(flags,narrow);c.PB=1;
      cpu_write16(&c,0x7f,0x7ced,phase);cpu_write16(&c,0x7f,0x91fe,0x1234);
      cpu_write16(&c,0x7f,0x9200,3);CpuState ref=c;memcpy(initial,ram,sizeof(ram));
      assert(ActRaiserDevelopment_Effect(&c,&us,world)==RECOMP_RETURN_NORMAL);
      assert(c.S==ref.S+2 && c.P==ref.P && c.DB==ref.DB && c.PB==ref.PB);
      assert(actor_calls==world && visual_calls==1);
      assert(cpu_read16(&c,0x7f,0x7ced)==phase && cpu_read16(&c,0x7f,0x91fe)==0x1234 && cpu_read16(&c,0x7f,0x9200)==3);
      if(native) {
        const unsigned expected_calls=calls;memcpy(reference_trace,trace,sizeof(trace));memcpy(expected,ram,sizeof(ram));
        memcpy(ram,initial,sizeof(ram));calls=actor_calls=visual_calls=event_calls=cycle_calls=0;
        Native(&ref,world?0x9460:0x948e,false);
        assert(calls==expected_calls && !memcmp(reference_trace,trace,calls*sizeof(Trace)));
        assert(!memcmp(&ref,&c,sizeof(c)) && !memcmp(expected,ram,sizeof(ram)));
      }
      ++count;
    }
  return count;
}
static void MixedClocks(void) {
  for(unsigned d=0;d<3;++d)for(unsigned l=0;l<3;++l)for(unsigned e=0;e<3;++e) {
    const ArRegionalDevelopmentPolicy policy={{d,l,e}};
    ArRegionalDevelopmentSnapshot snapshot;
    assert(ArRegionalDevelopment_Resolve(&policy,&snapshot) && ArRegionalDevelopment_SnapshotValid(&snapshot));
    CpuState c=Setup(0,false);
    const unsigned ticks=snapshot.service_divider*snapshot.long_cycle;
    unsigned cycles=0,actors=0,events=0;
    for(unsigned i=0;i<ticks;++i) {
      calls=actor_calls=event_calls=cycle_calls=0;c.S=0x1efa;
      assert(ActRaiserDevelopment_Master(&c,&snapshot)==RECOMP_RETURN_NORMAL);
      cycles+=cycle_calls;actors+=actor_calls;events+=event_calls;
      assert(cycles==(i+1==ticks));
    }
    assert(cycles==1 && actors==ticks && events==snapshot.long_cycle/8);
    assert(!cpu_read16(&c,0x7f,0x91fe) && !cpu_read16(&c,0x7f,0x9200));
    /* Effect services advance the same divider but not the development clocks. */
    c=Setup(0,false);c.PB=1;cpu_write16(&c,0x7f,0x91fe,37);cpu_write16(&c,0x7f,0x9200,2);
    for(unsigned phase=0;phase<8;++phase)for(unsigned world=0;world<2;++world) {
      c.S=0x1efa;calls=actor_calls=visual_calls=0;cpu_write16(&c,0x7f,0x7ced,phase);
      assert(ActRaiserDevelopment_Effect(&c,&snapshot,world)==RECOMP_RETURN_NORMAL);
      const bool visuals=snapshot.effect_divider==1 || phase+1>=snapshot.effect_divider;
      assert(visual_calls==visuals && actor_calls==world);
      const unsigned next=snapshot.effect_divider==1?phase:visuals?0:phase+1;
      assert(cpu_read16(&c,0x7f,0x7ced)==next);
      assert(cpu_read16(&c,0x7f,0x91fe)==37 && cpu_read16(&c,0x7f,0x9200)==2);
    }
  }
}
static void SharedPhase(void) {
  const ArRegionalDevelopmentSnapshot jp = {5, 480, 5};
  CpuState c = Setup(0, false);
  cpu_write16(&c, 0x7f, 0x91fe, 37);
  cpu_write16(&c, 0x7f, 0x9200, 2);
  cpu_write16(&c, 0x7f, 0x7ced, 2);
  /* Two effect calls leave phase four for the next master call. Neither
   * scene transition may restore the old phase or advance the held clocks. */
  for (unsigned world = 0; world < 2; ++world) {
    c.PB = 1; c.S = 0x1efa; calls = actor_calls = visual_calls = 0;
    assert(ActRaiserDevelopment_Effect(&c, &jp, world) == RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&c, 0x7f, 0x7ced) == 3 + world);
    assert(cpu_read16(&c, 0x7f, 0x91fe) == 37);
    assert(cpu_read16(&c, 0x7f, 0x9200) == 2);
    assert(visual_calls == 0 && actor_calls == world);
  }
  c.PB = 3; c.S = 0x1efa; calls = actor_calls = 0;
  assert(ActRaiserDevelopment_Master(&c, &jp) == RECOMP_RETURN_NORMAL);
  assert(cpu_read16(&c, 0x7f, 0x7ced) == 0);
  assert(cpu_read16(&c, 0x7f, 0x91fe) == 38);
  assert(cpu_read16(&c, 0x7f, 0x9200) == 3 && actor_calls == 1);
}
static void EntryContracts(void) {
  assert(!ActRaiserDevelopment_MasterEntry(NULL));
  assert(!ActRaiserDevelopment_EffectEntry(NULL));
  for (unsigned shape = 0; shape < 32; ++shape) {
    CpuState c = Setup(0, shape & 1);
    c.PB = shape & 2 ? 1 : 3;
    c.D = shape & 4 ? 1 : 0;
    c.x_flag = !!(shape & 8); c.emulation = !!(shape & 16);
    const bool valid = !(shape & 28);
    assert(ActRaiserDevelopment_MasterEntry(&c) == (valid && c.PB == 3));
    assert(ActRaiserDevelopment_EffectEntry(&c) == (valid && c.PB == 1));
    c.DB = 0x7f;
    assert(!ActRaiserDevelopment_EffectEntry(&c));
    assert(ActRaiserDevelopment_MasterEntry(&c) == (valid && c.PB == 3));
  }
}
static CpuState EscapeSetup(unsigned body) {
  CpuState c=Setup(13,false);posted=true;
  cpu_write16(&c,0x7f,0x9750,1);cpu_write16(&c,0x7f,0x91fe,719);
  if(body)c.PB=1;
  return c;
}
static void Escapes(void) {
  const ArRegionalDevelopmentSnapshot snapshot={1,720,1};
  static const RecompReturn tokens[]={RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,RECOMP_RETURN_SKIP_3,
      RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned body=0;body<3;++body) {
    CpuState c=EscapeSetup(body);
    assert((body?ActRaiserDevelopment_Effect(&c,&snapshot,body==1):ActRaiserDevelopment_Master(&c,&snapshot))==RECOMP_RETURN_NORMAL);
    const unsigned total=calls;
    for(unsigned at=1;at<=total;++at)for(unsigned t=0;t<6;++t) {
      c=EscapeSetup(body);const uint8_t pb=c.PB;stop=at;escape=tokens[t];
      const RecompReturn expected_return=escape>=RECOMP_RETURN_TAILCALL?escape:(RecompReturn)(escape-1);
      assert((body?ActRaiserDevelopment_Effect(&c,&snapshot,body==1):ActRaiserDevelopment_Master(&c,&snapshot))==expected_return);
      assert(calls==at && c.S==0x1789 && !g_cpu_return_scope);
      assert(c.PB==((long_calls[at-1] && escape!=RECOMP_RETURN_PARKED_WAIT && escape!=RECOMP_RETURN_OWNED_UNWIND)?pb:0x42));
    }
  }
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);unsigned count=Cases(false,false)+Cases(false,true),effects=Effects(false);
  if(argc==6)for(unsigned i=1;i<6;++i) {
    FILE *f=fopen(argv[i],"rb");assert(f);assert(!fseek(f,0x18000,SEEK_SET));
    assert(fread(code+0x8000,1,0x8000,f)==0x8000);fclose(f);count+=Cases(true,i==2);
    if(i==1) {
      f=fopen(argv[i],"rb");assert(f);assert(!fseek(f,0x8000,SEEK_SET));
      assert(fread(code+0x8000,1,0x8000,f)==0x8000);fclose(f);effects+=Effects(true);
    }
  }
  MixedClocks();SharedPhase();EntryContracts();Escapes();
  printf("development master: %u cases; effects: %u; 27 mixed policies\n",count,effects);return 0;
}
