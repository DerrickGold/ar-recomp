#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_cpu_hle_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) {
  (void)scope; (void)cpu; return 0;
}
static uint8_t ram[0x20000], native_code[65536];
static bool have_rom, confirmed, target_cancel, affected;
static unsigned debits, effects, stories, calls, stop_call;
static RecompReturn interruption;
typedef struct Trace { uint32_t pc; uint16_t a, x, y, s, frame; uint8_t p, pb, db; } Trace;
static Trace trace[32];

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; return ram[(bank == 0x7f ? 0x10000 : 0) + address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; ram[(bank == 0x7f ? 0x10000 : 0) + address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, value); cpu_write8(cpu, bank, address + 1, value >> 8);
}
static void Carry(CpuState *c, bool b) { c->_flag_C=b; c->P=(c->P & ~1u) | b; }
static void A8(CpuState *c, uint8_t n) {
  c->A=(c->A & 0xff00u) | n; ActRaiserCpuHle_SetNegativeZero8(c,n);
}
static RecompReturn Leaf(CpuState *c, uint32_t pc, unsigned bytes) {
  assert(calls < sizeof(trace)/sizeof(trace[0]));
  trace[calls++] = (Trace){pc,c->A,c->X,c->Y,c->S,cpu_read16(c,0,c->S+1),c->P,c->PB,c->DB};
  if (calls == stop_call) { c->S=0x1abc; c->PB=0x42; return interruption; }
  assert(c->m_flag && !c->x_flag && !c->emulation && c->DB==1 && !c->D);
  switch (pc) {
    case 0x018d92: Carry(c,confirmed); break;
    case 0x019754: Carry(c,target_cancel); break;
    case 0x03ca5e: {
      unsigned current=cpu_read16(c,0,0x282), cost=c->A & 255;
      assert(current>=cost); cpu_write16(c,0,0x282,current-cost); ++debits; break;
    }
    case 0x0197e5: assert((c->A & 255)>=1 && (c->A & 255)<=5);
      ++effects; Carry(c,affected); break;
    case 0x01b1fe: ++stories; break;
    default: break;
  }
  /* Deterministic clobbers catch incorrect host register restoration. */
  c->X=0x1234; A8(c,0xb7);
  /* Generated RTL leaves PB to its JSL caller, unlike hardware RTL. */
  c->S+=bytes;
  return RECOMP_RETURN_NORMAL;
}
#define STUB(bank, pc, bytes) RecompReturn bank_##bank##_##pc##_M1X0(CpuState *c) { return Leaf(c,0x##bank##pc,bytes); }
STUB(01,8E29,2) STUB(01,8D92,2) STUB(01,8CB6,2) STUB(01,9754,2)
STUB(03,CA5E,3) STUB(01,97E5,2) STUB(01,93B4,2) STUB(01,B1FE,3)
STUB(01,9270,2) STUB(01,8CCE,2)
#undef STUB

/* Optional ROM-backed differential check. Interpret just these five bounded
 * command bodies, delegating their calls to the same observable leaves. No ROM
 * is bundled, modified, executed outside the selected range, or required by CI.
 * This is controller parity, not a simulation of the effects themselves. */
static void Native(CpuState *c, unsigned action) {
  static const uint16_t entries[]={0x8290,0x82fb,0x8366,0x8431,0x83d1};
  uint16_t pc=entries[action-5];
  A8(c,0);
  for (unsigned steps=0; steps<100; ++steps) {
    const uint8_t op=native_code[pc++];
    unsigned value=native_code[pc], width=1;
    switch (op) {
      case 0xa0: c->Y=value | native_code[pc+1]<<8; pc+=2;
        ActRaiserCpuHle_SetNegativeZero16(c,c->Y); break;
      case 0xaf: {
        const uint8_t bank=native_code[pc+2]; value|=native_code[pc+1]<<8; pc+=3;
        if (c->m_flag) A8(c,cpu_read8(c,bank,value));
        else { c->A=cpu_read16(c,bank,value); ActRaiserCpuHle_SetNegativeZero16(c,c->A); }
        break;
      }
      case 0xc2: c->P &= ~value; ++pc; c->m_flag=(c->P>>5)&1; break;
      case 0xe2: c->P |= value; ++pc; c->m_flag=(c->P>>5)&1; break;
      case 0xa9: A8(c,value); ++pc; break;
      case 0xc9: assert(!c->m_flag); value|=native_code[pc+1]<<8; pc+=2;
        Carry(c,c->A>=value); ActRaiserCpuHle_SetNegativeZero16(c,(uint16_t)(c->A-value)); break;
      case 0x20: case 0x22: {
        unsigned target=value | native_code[pc+1]<<8; width=op==0x22?3:2;
        uint8_t bank=width==3?native_code[pc+2]:c->PB;
        pc+=width;
        if (width==3) cpu_write8(c,0,c->S--,c->PB);
        ActRaiserCpuHle_PushWord(c,pc-1); c->PB=bank; c->host_return_valid=1;
        const uint8_t return_bank=c->PB;
        assert(Leaf(c,((uint32_t)bank<<16)|target,width)==RECOMP_RETURN_NORMAL);
        if (width==3) c->PB=cpu_read8(c,0,c->S);
        else c->PB=return_bank;
        break;
      }
      case 0xf0: case 0xb0: case 0x90: case 0x80:
        ++pc; if (op==0x80 || (op==0xf0 && c->_flag_Z) ||
            (op==0xb0 && c->_flag_C) || (op==0x90 && !c->_flag_C)) pc+=(int8_t)value;
        break;
      case 0x18: Carry(c,false); break;
      case 0x38: Carry(c,true); break;
      case 0x60: c->S+=2; return;
      default: fprintf(stderr,"unexpected opcode %02x at %04x\n",op,pc-1); abort();
    }
  }
  assert(!"native command did not return");
}

static CpuState Setup(unsigned action, unsigned sp, bool enabled) {
  memset(ram,0,sizeof(ram)); memset(trace,0,sizeof(trace));
  calls=debits=effects=stories=0;
  CpuState c={0}; c.A=0x5a00|action; c.X=0xf337; c.Y=0xaaaa;
  c.S=0x1ef0; c.PB=c.DB=1; c.P=0x65; c.m_flag=1; c._flag_C=1; c._flag_I=1; c._flag_V=1;
  cpu_write16(&c,0,0x282,sp); cpu_write8(&c,0x7f,0x9217,enabled);
  cpu_write16(&c,0,c.S+1,0x81c3);
  return c;
}

int main(int argc, char **argv) {
  if (argc==2) {
    FILE *rom=fopen(argv[1],"rb"); assert(rom);
    assert(!fseek(rom,0x8000,SEEK_SET));
    assert(fread(native_code+0x8000,1,0x8000,rom)==0x8000); fclose(rom);
    assert(native_code[0x8290]==0xa0 && native_code[0x82a3]==10 && native_code[0x83e4]==160);
    have_rom=true;
  }
  ArRegionalCostRule rule=kArRegionalCost_Fire;
  assert(!ActRaiserMiracle_Rule(4,&rule) && rule==kArRegionalCost_Fire);
  assert(!ActRaiserMiracle_Rule(10,&rule) && !ActRaiserMiracle_Entry(NULL,5));
  unsigned cases=0;
  for (unsigned region=0; region<3; ++region) {
    ArRegionalCostPolicy policy; ArRegionalCostSnapshot quote;
    assert(ArRegionalCosts_Init(&policy,region) && ArRegionalCosts_Resolve(&policy,&quote));
    for (unsigned action=5; action<=9; ++action) {
      assert(ActRaiserMiracle_Rule(action,&rule));
      for (unsigned scenario=0; scenario<32; ++scenario) {
        const unsigned price=quote.price[rule];
        unsigned sp=scenario&16 ? 0x314 : price + ((scenario&8)?0:-1);
        bool enabled=(scenario&1)!=0;
        confirmed=(scenario&2)!=0; target_cancel=(scenario&4)!=0; affected=(scenario&8)!=0;
        CpuState c=Setup(action,sp,enabled);
        assert(ActRaiserMiracle_Entry(&c,action));
        c.x_flag=1; assert(!ActRaiserMiracle_Entry(&c,action)); c.x_flag=0;
        assert(ActRaiserMiracle_Run(&c,action,&quote)==RECOMP_RETURN_NORMAL);
        bool paid=enabled && sp>=price && confirmed && (action>=8 || !target_cancel);
        assert(debits==(unsigned)paid && effects==debits && stories==(unsigned)(paid&&affected));
        assert(cpu_read16(&c,0,0x282)==sp-(paid?price:0));
        assert(c.S==0x1ef2 && c.PB==1 && c.DB==1 && c.m_flag && !c.x_flag);
        assert(c._flag_C==(!enabled || sp<price));
        assert(!g_cpu_return_scope);
        if (have_rom && region!=kArRegionalSource_Japan) {
          Trace expected[32]; memcpy(expected,trace,sizeof(trace)); unsigned expected_calls=calls;
          uint8_t expected_ram[sizeof(ram)]; memcpy(expected_ram,ram,sizeof(ram));
          CpuState native=Setup(action,sp,enabled); Native(&native,action);
          for (unsigned t=0;t<calls;++t) {
            const Trace *a=&expected[t], *b=&trace[t];
            if (a->pc!=b->pc || a->a!=b->a || a->x!=b->x || a->y!=b->y ||
                a->s!=b->s || a->frame!=b->frame || a->p!=b->p || a->pb!=b->pb || a->db!=b->db) {
              fprintf(stderr,"action %u scenario %u call %u: host %06x A%04x Y%04x P%02x frame%04x; native %06x A%04x Y%04x P%02x frame%04x\n",
                  action,scenario,t,a->pc,a->a,a->y,a->p,a->frame,b->pc,b->a,b->y,b->p,b->frame);
              abort();
            }
          }
          assert(calls==expected_calls);
          assert(!memcmp(&native,&c,sizeof(c)) && !memcmp(expected_ram,ram,sizeof(ram)));
        }
        ++cases;
      }
      /* A yielding/escaping native leaf must not incur payment or a forged
       * RTS. Test every leaf on the successful path, not only entry dialogue. */
      confirmed=affected=true; target_cancel=false;
      CpuState c=Setup(action,1000,true); ActRaiserMiracle_Run(&c,action,&quote);
      const unsigned count=calls;
      const RecompReturn escapes[]={RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_3,
        RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
      for (unsigned i=0;i<sizeof(escapes)/sizeof(escapes[0]);++i)
        for (stop_call=1;stop_call<=count;++stop_call) {
          c=Setup(action,1000,true); interruption=escapes[i];
          RecompReturn expected=interruption>=RECOMP_RETURN_TAILCALL ? interruption : interruption-1;
          assert(ActRaiserMiracle_Run(&c,action,&quote)==expected);
          const bool restores_pb=trace[stop_call-1].pc==0x03ca5e || trace[stop_call-1].pc==0x01b1fe;
          const uint8_t pb=restores_pb && interruption!=RECOMP_RETURN_PARKED_WAIT &&
              interruption!=RECOMP_RETURN_OWNED_UNWIND ? 1 : 0x42;
          assert(calls==stop_call && c.S==0x1abc && c.PB==pb && !g_cpu_return_scope);
        }
      stop_call=0;
    }
  }
  printf("PASS %u controller cases%s; native escape paths\n",cases,have_rom?" with US-ROM differential checks":"");
  return 0;
}
