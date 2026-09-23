#include "actraiser/actraiser_report_command.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_native_call.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], code[65536];
static unsigned calls, stop_at, seen[4];
static uint16_t frames[4];
static RecompReturn escape;
CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *s, CpuState *c) { (void)s; (void)c; return 0; }
uint8 cpu_read8(CpuState *c, uint8 b, uint16 a) { (void)c; (void)b; return ram[a]; }
uint16 cpu_read16(CpuState *c, uint8 b, uint16 a) { return cpu_read8(c,b,a) | cpu_read8(c,b,(uint16)(a+1)) << 8; }
void cpu_write8(CpuState *c, uint8 b, uint16 a, uint8 v) { (void)c; (void)b; ram[a]=v; }
void cpu_write16(CpuState *c, uint8 b, uint16 a, uint16 v) { cpu_write8(c,b,a,v); cpu_write8(c,b,(uint16)(a+1),v>>8); }

static RecompReturn Leaf(CpuState *c, unsigned id, unsigned size) {
  assert(calls < 4); frames[calls] = cpu_read16(c,0,c->S+1); seen[calls++] = id;
  assert(c->PB == (size == 3 ? 3 : 1));
  if (calls == stop_at) return escape;
  /* Observable native-owned work; deliberately clobber registers and flags.
   * The controller must not restore them or repeat a successful transaction. */
  c->A ^= (uint16_t)(id * 0x101); c->X += id; c->Y -= id;
  ActRaiserCpuHle_SetNegativeZero8(c, (uint8_t)c->A);
  c->S += size;
  return RECOMP_RETURN_NORMAL;
}
#define STUB(bank, pc, id, size) RecompReturn bank_##bank##_##pc##_M1X0(CpuState *c) { return Leaf(c,id,size); }
STUB(01,899B,1,2) STUB(03,BF8C,2,3) STUB(01,8A3F,3,2) STUB(03,8168,4,3)
STUB(01,8A9A,5,2) STUB(01,8AF5,6,2) STUB(01,8CB6,7,2) STUB(01,9270,8,2)
#undef STUB

static CpuState Initial(unsigned action, unsigned bits) {
  CpuState c = {.A = (uint16_t)(0x5a00 | action), .X = 0x200, .Y = 0x8000,
      .S = 0x1fee, .PB = 1, .DB = 1, .P = (uint8_t)((bits & 0xcf) | 0x20), .m_flag = 1};
  c._flag_N = !!(c.P & 0x80); c._flag_V = !!(c.P & 0x40);
  c._flag_D = !!(c.P & 8); c._flag_Z = !!(c.P & 2); c._flag_C = c.P & 1;
  return c;
}

/* Optional bounded ROM differential: execute just each region's four tiny
 * command wrappers, with the same observable callee contract. Real report,
 * save/quit and speed bodies are separately exercised by live input replays. */
static void Native(CpuState *c, unsigned action, bool jp) {
  static const uint16_t entry[2][4] = {{0x8530,0x853b,0x854a,0x8559},{0x84fa,0x84ff,0x8504,0x8509}};
  static const uint16_t target[2][8] = {
      {0x899b,0xbf8c,0x8a3f,0x8168,0x8a9a,0x8af5,0x8cb6,0x9270},
      {0x8978,0,0x89cf,0,0x8a35,0x8a8c,0,0}};
  uint16_t pc = entry[jp][action - 12];
  cpu_write_a8(c, action == 15 ? 1 : 0); ActRaiserCpuHle_SetNegativeZero8(c,(uint8_t)c->A);
  for (unsigned steps = 0; steps < 8; ++steps) {
    unsigned op = code[pc++];
    if (op == 0x60) { c->S += 2; return; }
    if (op == 0x18 || op == 0x38) {
      c->_flag_C = op == 0x38; c->P = (uint8_t)((c->P & ~1) | c->_flag_C); continue;
    }
    assert(op == 0x20 || op == 0x22);
    uint16_t dest = code[pc] | code[pc+1]<<8; pc += 2;
    const uint8_t bank = op == 0x22 ? code[pc++] : 1;
    unsigned id = 0;
    for (unsigned i = 0; i < 8; ++i) if (target[jp][i] && dest == target[jp][i]) id = i+1;
    assert(id);
    const uint16_t saved_s = c->S;
    if (op == 0x22) { cpu_write8(c,0,c->S--,1); c->PB=bank; }
    cpu_write8(c,0,c->S--,(pc-1)>>8); cpu_write8(c,0,c->S--,(uint8_t)(pc-1));
    assert(Leaf(c,id,op == 0x22 ? 3 : 2) == RECOMP_RETURN_NORMAL);
    assert(c->S == saved_s); c->PB = 1;
  }
  assert(!"native wrapper failed to return");
}

int main(int argc, char **argv) {
  static const unsigned sequence[4][4] = {{1,7,8,0},{2,3,7,8},{4,5,7,8},{6,7,8,0}};
  static const uint16_t expected_frames[4][4] = {
      {0x8532,0x8535,0x8538,0},{0x853e,0x8541,0x8544,0x8547},
      {0x854d,0x8550,0x8553,0x8556},{0x855b,0x855e,0x8561,0}};
  for (unsigned action=12; action<=15; ++action) for (unsigned keep=0; keep<2; ++keep)
    for (unsigned bits=0; bits<256; ++bits) {
      CpuState c=Initial(action,bits); calls=stop_at=0;
      assert(ActRaiserReportCommand_Entry(&c,action));
      assert(ActRaiserReportCommand_Run(&c,action,keep)==RECOMP_RETURN_NORMAL);
      unsigned count=(action==13 || action==14 ? 2 : 1)+(keep ? 0 : 2);
      assert(calls==count && !memcmp(seen,sequence[action-12],count*sizeof(*seen)));
      assert(!memcmp(frames,expected_frames[action-12],count*sizeof(*frames)));
      assert(c.S==0x1ff0 && c.PB==1 && c.DB==1 && c._flag_C==keep && (c.P&1)==keep);
      for (unsigned at=1; at<=count; ++at) for (unsigned token=1; token<=RECOMP_RETURN_OWNED_UNWIND; ++token) {
        c=Initial(action,bits); calls=0; stop_at=at; escape=(RecompReturn)token;
        RecompReturn result=ActRaiserReportCommand_Run(&c,action,keep);
        assert(calls==at && result==(token>=RECOMP_RETURN_TAILCALL ? escape : (RecompReturn)(token-1)));
      }
    }
  stop_at=0;
  for (int arg=1; arg<argc; ++arg) {
    assert(arg<=2); bool jp=arg==2;
    FILE *f=fopen(argv[arg],"rb"); assert(f);
    assert(!fseek(f,0x8000,SEEK_SET)); assert(fread(code+0x8000,1,0x8000,f)==0x8000); assert(!fclose(f));
    for (unsigned action=12; action<=15; ++action) for (unsigned bits=0; bits<256; ++bits) {
      /* JP wrappers do not call the extra US census/cache prep; compare
       * Master and speed owners here. City/save prefixes
       * stay US on purpose because the US report/save ABI requires that prep. */
      if (jp && (action==13 || action==14)) continue;
      CpuState hle=Initial(action,bits), native=hle;
      calls=0; assert(ActRaiserReportCommand_Run(&hle,action,jp)==RECOMP_RETURN_NORMAL);
      unsigned expected_calls=calls, expected[4]; memcpy(expected,seen,sizeof(expected));
      calls=0; Native(&native,action,jp);
      assert(calls==expected_calls && !memcmp(expected,seen,calls*sizeof(*seen)));
      assert(hle.A==native.A && hle.X==native.X && hle.Y==native.Y && hle.S==native.S && hle.P==native.P && hle.PB==native.PB);
    }
  }
  CpuState c=Initial(12,0);
  assert(!ActRaiserReportCommand_Entry(NULL,12));
  for (unsigned action=0; action<256; ++action) assert(ActRaiserReportCommand_Entry(&c,action)==(action>=12 && action<=15));
  c.DB=3; assert(!ActRaiserReportCommand_Entry(&c,12)); c.DB=1;
  c.m_flag=0; assert(!ActRaiserReportCommand_Entry(&c,12));
  puts("report commands: all actions/flags/return policies and callee escapes passed");
  return 0;
}
