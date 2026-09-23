#include "actraiser/actraiser_quake.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) { (void)scope; (void)cpu; return 0; }
static uint8_t ram[65536], us[65536], regional[65536];
static unsigned random_value, draws, queued, feedback;
static RecompReturn escape;
uint8 cpu_read8(CpuState *c, uint8 bank, uint16 address) { (void)c; assert(bank == 0 || bank == 0x7f); return ram[address]; }
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 address) {
  return cpu_read8(c, bank, address) | cpu_read8(c, bank, address + 1) << 8;
}
void cpu_write8(CpuState *c, uint8 bank, uint16 address, uint8 value) {
  (void)c; assert(bank == 0 || bank == 0x7f); ram[address] = value;
}
void cpu_write16(CpuState *c, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(c, bank, address, value); cpu_write8(c, bank, address + 1, value >> 8);
}
static void A(CpuState *c, uint8_t value) { cpu_write_a8(c, value); ActRaiserCpuHle_SetNegativeZero8(c, value); }
static void Compare(CpuState *c, uint8_t value) {
  c->_flag_C = (uint8_t)c->A >= value; c->P = (c->P & ~1u) | c->_flag_C;
  ActRaiserCpuHle_SetNegativeZero8(c, (uint8_t)(c->A - value));
}
RecompReturn bank_03_AF65_M1X0(CpuState *c) {
  assert((uint8_t)c->A == 255 && c->PB == 3 && c->DB == 0x7f && c->m_flag && !c->x_flag);
  assert(cpu_read16(c, 0, c->S + 1) == 0xa341 && ram[c->S + 3] == 3);
  ++draws;
  if (escape) { c->PB = 0x42; c->S = 0x1234; return escape; }
  A(c, random_value); c->S += 3; return RECOMP_RETURN_NORMAL;
}
static CpuState Setup(unsigned flags) {
  memset(ram, 0xa5, sizeof(ram)); draws = queued = feedback = 0; escape = 0;
  CpuState c = {.PB = 3, .DB = 0x7f, .A = 0xcdef, .X = 0x8000, .Y = 0x1234, .S = 0x1ff0,
    .m_flag = 1, ._flag_C = flags & 1, ._flag_D = (flags >> 1) & 1,
    ._flag_I = (flags >> 2) & 1, ._flag_V = (flags >> 3) & 1};
  cpu_mirrors_to_p(&c); return c;
}

/* Optional ROM-decoded selector/continuation oracle. Helpers remain native in
 * production; here only their shared observable queue/callback contract is
 * modeled. This proves RNG consumption and call order, not house-credit math. */
static void Native(CpuState *c, const uint8_t *code, uint16_t pc) {
  for (unsigned step = 0; step < 32; ++step) {
    uint8_t op = code[pc++];
    if (op == 0x60) { c->S += 2; return; }
    uint16_t value = code[pc++];
    if (op == 0xa9) { A(c, value); continue; }
    if (op == 0x29) { A(c, c->A & value); continue; }
    if (op == 0xc9) { Compare(c, value); continue; }
    if (op == 0x90 || op == 0xf0 || op == 0xd0) {
      if ((op == 0x90 && !c->_flag_C) || (op == 0xf0 && c->_flag_Z) || (op == 0xd0 && !c->_flag_Z)) pc += (int8_t)value;
      continue;
    }
    value |= code[pc++] << 8;
    if (op == 0xbd) { A(c, ram[(uint16_t)(c->X + value)]); continue; }
    if (op == 0x22) {
      assert(code[pc++] == 3);
      c->host_return_valid = 1;
      if (value == 0xaf65 || value == 0xad2d) { assert((uint8_t)c->A == 255); ++draws; A(c, random_value); }
      else { assert(value == 0xb4a6 || value == 0xb22a); assert(queued == 7); ++feedback; }
      continue;
    }
    assert(op == 0x20 && (value == 0x9f05 || value == 0x9cb5));
    c->host_return_valid = 1;
    queued = (uint8_t)c->A; ram[c->X + 3] = (ram[c->X + 3] & 0x70) | queued;
    /* Native queue's PLA restores A and its N/Z after its AND/ORA work. */
    A(c, (uint8_t)c->A);
  }
  assert(!"selector failed to return within its bounded body");
}
static void Load(const char *path, uint8_t *code) {
  FILE *f = fopen(path, "rb"); assert(f);
  assert(!fseek(f, 3 * 0x8000, SEEK_SET));
  assert(fread(code + 0x8000, 1, 0x8000, f) == 0x8000); assert(!fclose(f));
}
static void Policies(void) {
  for (unsigned combination = 0; combination < 243; ++combination) {
    ArRegionalQuakePolicy p; unsigned n = combination;
    for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) { p.source[i] = n % 3; n /= 3; }
    ArRegionalQuakeSnapshot s; assert(ArRegionalQuake_Resolve(&p, &s));
    bool coherent = true;
    for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) {
      assert(s.random[i] == (p.source[i] == kArRegionalSource_Japan));
      coherent &= s.random[i] == s.random[0];
    }
    ArRegionalSource source; assert(ArRegionalQuake_GroupSource(&p, &source) == coherent);
  }
  ArRegionalQuakePolicy p = {0}, before = p;
  ArRegionalQuakeSnapshot s, sentinel; memset(&s, 0xa5, sizeof(s)); sentinel = s;
  assert(!ArRegionalQuake_Init(&p, kArRegionalSource_Count) && !memcmp(&p, &before, sizeof(p)));
  p.source[4] = kArRegionalSource_Count;
  assert(!ArRegionalQuake_Resolve(&p, &s) && !memcmp(&s, &sentinel, sizeof(s)));
  assert(!ArRegionalQuake_Descriptor(kArRegionalQuake_Count) && !ActRaiserQuake_SelectorPC(kArRegionalQuake_Count));
}
int main(int argc, char **argv) {
  Policies();
  const uint16_t jp_entries[] = {0x9e16, 0x9ef5, 0x9f9c, 0xa042, 0xa0ab};
  const uint32_t keep[] = {0x03a06f, 0x03a14b, 0x03a346, 0x03a346, 0x03a2e9};
  const uint32_t destroy[] = {0x03a075, 0x03a151, 0x03a1ee, 0x03a28a, 0x03a34c};
  if (argc > 1) { assert(argc == 6); Load(argv[1], us); Load(argv[2], regional); }
  unsigned cases = 0;
  for (unsigned rule = 0; rule < kArRegionalQuake_Count; ++rule)
    for (unsigned flags = 0; flags < 16; ++flags) for (unsigned value = 0; value < 256; ++value) {
      CpuState c = Setup(flags), reference = c; random_value = value;
      uint32_t target = 0;
      assert(ActRaiserQuake_Select(&c, rule, &target) == RECOMP_RETURN_NORMAL);
      assert(draws == 1 && target == (value < 128 ? destroy[rule] : keep[rule]));
      assert(c.S == reference.S && c.PB == 3 && c.DB == 0x7f && c.X == reference.X && c.Y == reference.Y);
      if (argc > 1) {
        Native(&c, us, (uint16_t)target);
        const unsigned actual_action = queued, actual_feedback = feedback;
        assert(actual_feedback == (rule == 0 && value < 128));
        Setup(flags); Native(&reference, regional, jp_entries[rule]);
        assert(draws == 1 && queued == actual_action && feedback == actual_feedback);
        assert(!memcmp(&reference, &c, sizeof(c)));
      }
      ++cases;
    }
  if (argc > 1) for (unsigned rom = 1; rom <= 5; ++rom) {
    if (rom == 2) continue;
    Load(argv[rom], regional);
    for (unsigned rule = 0; rule < kArRegionalQuake_Count; ++rule)
      for (unsigned subtype = 0; subtype < 256; ++subtype) {
        CpuState c = Setup(0); ram[c.X + 2] = subtype;
        Native(&c, regional, (uint16_t)ActRaiserQuake_SelectorPC(rule));
        bool preserved = rule == 0 ? (subtype & 0x30) == 0x20 : rule == 1 ? (subtype & 0x30) != 0 : rule != 4;
        assert(!draws && queued == (preserved ? 1u : 7u) && feedback == (rule == 0 && !preserved));
        ++cases;
      }
  }
  for (unsigned token = 1; token <= RECOMP_RETURN_OWNED_UNWIND; ++token) {
    CpuState c = Setup(0); escape = token; uint32_t target = 0xabcdef;
    assert(ActRaiserQuake_Select(&c, kArRegionalQuake_Houses, &target) == escape);
    assert(target == 0xabcdef && c.S == 0x1234 && draws == 1);
  }
  CpuState c = Setup(0); assert(ActRaiserQuake_SelectorEntry(&c));
  c.m_flag = 0; assert(!ActRaiserQuake_SelectorEntry(&c)); c.m_flag = 1;
  c.x_flag = 1; assert(!ActRaiserQuake_SelectorEntry(&c)); c.x_flag = 0;
  c.DB = 1; assert(!ActRaiserQuake_SelectorEntry(&c));
  printf("quake: %u selector cases, 243 mixed policies, native tails and callee escapes passed%s\n",
      cases, argc > 1 ? " against all five ROMs" : " (ROM-free)");
  return 0;
}
