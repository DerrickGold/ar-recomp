#include "actraiser/actraiser_fishing.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) {
  (void)scope; (void)cpu; return 0;
}
static uint8_t ram[0x20000], before[0x20000], expected[0x20000], code[65536];
static unsigned calls, scene_calls, stop;
static RecompReturn escape;
static uint16_t frames[3];

uint8 cpu_read8(CpuState *c, uint8 bank, uint16 address) {
  (void)c; assert(bank == 0 || bank == 0x7f);
  return ram[(bank == 0x7f ? 0x10000 : 0) + address];
}
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 address) {
  return cpu_read8(c,bank,address) | cpu_read8(c,bank,address+1)<<8;
}
void cpu_write8(CpuState *c, uint8 bank, uint16 address, uint8 value) {
  (void)c; assert(bank == 0 || bank == 0x7f);
  ram[(bank == 0x7f ? 0x10000 : 0) + address] = value;
}
void cpu_write16(CpuState *c, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(c,bank,address,value); cpu_write8(c,bank,address+1,value>>8);
}
static void A8(CpuState *c, uint8_t v) { cpu_write_a8(c,v); ActRaiserCpuHle_SetNegativeZero8(c,v); }
static RecompReturn Leaf(CpuState *c, uint16_t target) {
  assert(calls < 3 && c->PB == 3 && c->DB == 0x7f && c->m_flag && !c->x_flag);
  frames[calls++] = cpu_read16(c,0,c->S+1);
  if (calls == stop) { c->PB=0x42; c->S=0x1789; return escape; }
  if (target == 0xf4df) { assert((uint8_t)c->A == 25); A8(c,ram[0x19102] & 0x40); }
  else if (target == 0xf4ea) { assert((uint8_t)c->A == 25); ram[0x19102] |= 0x40; A8(c,0x40); }
  else {
    assert(target == 0xca93 && c->X == 0xe5f6); ++scene_calls;
    /* Deliberate clobbers ensure the prefix does not fabricate saved registers. */
    c->X=0xcafe; c->Y=0x1234; A8(c,0xbc);
  }
  c->S+=2; return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_03_F4DF_M1X0(CpuState *c) { return Leaf(c,0xf4df); }
RecompReturn bank_03_F4EA_M1X0(CpuState *c) { return Leaf(c,0xf4ea); }
RecompReturn bank_03_CA93_M1X0(CpuState *c) { return Leaf(c,0xca93); }

/* Original bounded prefix, with native callees delegated to observable leaves.
 * JP code/scene/caller addresses are normalized to their verified US owners;
 * counter arithmetic, branches and the target immediate remain ROM-derived. */
static uint16_t Native(CpuState *c, bool jp) {
  uint16_t pc = jp ? 0xe359 : 0xe865;
  for (unsigned n=0; n<30; ++n) {
    const uint8_t op=code[pc++];
    uint16_t value=code[pc++];
    switch(op) {
      case 0xe2: c->P |= value; cpu_p_to_mirrors(c); break;
      case 0xa9: A8(c,value); break;
      case 0xa2:
        value |= code[pc++]<<8; c->X = jp && value == 0xe0f5 ? 0xe5f6 : value;
        ActRaiserCpuHle_SetNegativeZero16(c,c->X); break;
      case 0x20: {
        value |= code[pc++]<<8;
        if (jp) value = value == 0xefbb ? 0xf4df : value == 0xefc6 ? 0xf4ea : value == 0xc73c ? 0xca93 : 0;
        ActRaiserCpuHle_PushWord(c,pc-1+(jp?0x50c:0)); c->host_return_valid=1;
        assert(Leaf(c,value) == RECOMP_RETURN_NORMAL); break;
      }
      case 0xd0: if (!c->_flag_Z) pc += (int8_t)value; break;
      case 0xf0: return c->_flag_Z ? 0xe895 : 0xe88c;
      case 0x8f: case 0xaf:
        value |= code[pc++]<<8; assert(code[pc++] == 0x7f);
        if (op == 0x8f) cpu_write8(c,0x7f,value,c->A); else A8(c,cpu_read8(c,0x7f,value));
        break;
      case 0x1a: --pc; A8(c,(uint8_t)(c->A+1)); break;
      case 0xc9:
        c->_flag_C = (uint8_t)c->A >= value; c->P=(c->P&~1u)|c->_flag_C;
        ActRaiserCpuHle_SetNegativeZero8(c,(uint8_t)(c->A-value)); break;
      default: assert(!"unexpected fishing prefix opcode");
    }
  }
  assert(!"unterminated fishing prefix"); return 0;
}

static CpuState Setup(unsigned progress, bool initialized, unsigned flags) {
  memset(ram,0xa5,sizeof(ram)); calls=scene_calls=stop=0;
  ram[0x1916e]=progress; ram[0x19102]=initialized?0x41:1;
  CpuState c={.PB=3,.DB=0x7f,.A=0x5a10,.X=0x4321,.Y=0xdcae,.S=0x1efa,
      ._flag_C=flags&1,._flag_D=(flags>>1)&1,._flag_I=(flags>>2)&1,._flag_V=(flags>>3)&1};
  cpu_mirrors_to_p(&c); return c;
}
static unsigned Cases(bool native, bool jp) {
  unsigned count=0;
  for (unsigned init=0; init<2; ++init)
    for (unsigned progress=0; progress<256; ++progress)
      for (unsigned flags=0; flags<16; ++flags) {
        CpuState c=Setup(progress,init,flags), ref=c;
        memcpy(before,ram,sizeof(ram));
        uint16_t continuation=0;
        assert(ActRaiserFishing_Prefix(&c,jp?128:255,false,&continuation) == RECOMP_RETURN_NORMAL);
        const unsigned advanced=init?(uint8_t)(progress+1):1;
        assert(ram[0x1916e] == advanced && continuation == (advanced==(jp?128u:255u)?0xe895:0xe88c));
        assert(scene_calls == !init && calls == (init?1u:3u));
        assert(c.S == ref.S && c.m_flag && !c.x_flag && c._flag_D == ref._flag_D && c._flag_V == ref._flag_V);
        assert(frames[0] == 0xe86b && (init || (frames[1] == 0xe872 && frames[2] == 0xe87e)));
        if (native) {
          memcpy(expected,ram,sizeof(ram)); memcpy(ram,before,sizeof(ram)); calls=scene_calls=0;
          assert(Native(&ref,jp) == continuation);
          assert(!memcmp(&c,&ref,sizeof(c)) && !memcmp(expected,ram,sizeof(ram)));
        }
        ++count;
      }
  return count;
}
int main(int argc, char **argv) {
  assert(argc == 1 || argc == 6); unsigned count=Cases(false,false)+Cases(false,true);
  if (argc==6) for (unsigned i=1; i<6; ++i) {
    FILE *f=fopen(argv[i],"rb"); assert(f); assert(!fseek(f,0x18000,SEEK_SET));
    assert(fread(code+0x8000,1,0x8000,f) == 0x8000); fclose(f);
    count+=Cases(true,i==2);
  }
  for (unsigned init=0; init<2; ++init) for (unsigned p=0; p<256; ++p) {
    CpuState c=Setup(p,init,0); uint16_t target;
    assert(ActRaiserFishing_Prefix(&c,128,true,&target) == RECOMP_RETURN_NORMAL);
    assert(target == (init && p>=127 ? 0xe895 : 0xe88c));
    assert(ram[0x1916e] == (init ? (p>=128?p:p+1) : 1));
  }
  for (unsigned at=1; at<=3; ++at) for (unsigned token=0; token<6; ++token) {
    static const RecompReturn tokens[]={RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,RECOMP_RETURN_SKIP_3,
        RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
    CpuState c=Setup(200,false,0); stop=at; escape=tokens[token]; uint16_t out=0xbeef;
    assert(ActRaiserFishing_Prefix(&c,128,true,&out) == escape);
    assert(out==0xbeef && c.PB==0x42 && c.S==0x1789 && calls==at);
    assert(!g_cpu_return_scope);
  }
  CpuState valid=Setup(0,false,0);
  assert(ActRaiserFishing_Entry(&valid) && !ActRaiserFishing_Entry(NULL));
  for (unsigned i=0;i<6;++i) {
    CpuState bad=valid;
    if(i==0)bad.PB=0; if(i==1)bad.DB=0; if(i==2)bad.D=1;
    if(i==3)bad.m_flag=1; if(i==4)bad.x_flag=1; if(i==5)bad.emulation=1;
    assert(!ActRaiserFishing_Entry(&bad));
  }
  printf("fishing prefix: %u cases plus target-change/escape controls\n",count);
  return 0;
}
