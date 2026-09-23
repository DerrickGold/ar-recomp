#include "actraiser/actraiser_recovery.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *s, CpuState *c) { (void)s; (void)c; return 0; }
static uint8_t ram[65536], initial[65536], expected[65536], code[65536];
static unsigned calls, stop;
static RecompReturn escape;
typedef struct Trace { uint32_t leaf; uint16_t a, x, y, s, frame; uint8_t p, db; } Trace;
static Trace trace[2], reference_trace[2];
uint8 cpu_read8(CpuState *c, uint8 bank, uint16 address) {
  (void)c; assert(bank == 0 || bank == 1 || bank == 0x7f); return ram[address];
}
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 address) {
  return cpu_read8(c, bank, address) | cpu_read8(c, bank, address + 1) << 8;
}
void cpu_write8(CpuState *c, uint8 bank, uint16 address, uint8 value) {
  (void)c; assert(bank == 0 || bank == 1 || bank == 0x7f); ram[address] = value;
}
void cpu_write16(CpuState *c, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(c, bank, address, value); cpu_write8(c, bank, address + 1, value >> 8);
}
static void A(CpuState *c, uint16_t value) {
  cpu_write_a_m(c, value);
  if (c->m_flag) ActRaiserCpuHle_SetNegativeZero8(c, value);
  else ActRaiserCpuHle_SetNegativeZero16(c, value);
}
static RecompReturn Leaf(CpuState *c, uint32_t leaf, unsigned bytes) {
  assert(calls < 2);
  trace[calls++] = (Trace){leaf, c->A, c->X, c->Y, c->S, cpu_read16(c, 0, c->S + 1), c->P, c->DB};
  if (calls == stop) { c->PB = 0x42; c->S = 0x1234; return escape; }
  if (leaf == 0x03afbd) {
    const uint8_t divisor = (uint8_t)c->A;
    c->A = (uint16_t)((c->X % divisor) << 8); A(c, (uint8_t)(c->X / divisor));
  } else {
    /* Native saturation helpers remain native; observable common contract. */
    const unsigned field = leaf == 0x03b482 ? 0x286 : 0x282;
    ++ram[field]; A(c, ram[field]); c->Y = 0x4321;
  }
  c->S += bytes;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_03_AFBD_M1X0(CpuState *c) { return Leaf(c, 0x03afbd, 2); }
RecompReturn bank_03_B482_M1X0(CpuState *c) { return Leaf(c, 0x03b482, 3); }
RecompReturn bank_03_B456_M1X0(CpuState *c) { return Leaf(c, 0x03b456, 3); }

/* Bounded, ROM-decoded prefix oracle. PAL maxima relocate by two bytes;
 * Japanese HP fields relocate by one. Native helpers are common observed
 * contracts, not an independent implementation of their game behavior. */
static void Native(CpuState *c, uint16_t pc, uint16_t end, unsigned region) {
  for (unsigned n = 0; n < 100 && pc != end; ++n) {
    const uint8_t op = code[pc++];
    if (op == 0xaa) { c->X = cpu_read_a_m(c); ActRaiserCpuHle_SetNegativeZero16(c, c->X); continue; }
    if (op == 0x1a) { A(c, cpu_read_a_m(c) + 1); continue; }
    uint16_t value = code[pc++];
    if (op == 0xe2 || op == 0xc2) {
      c->P = op == 0xe2 ? c->P | value : c->P & ~value; cpu_p_to_mirrors(c); continue;
    }
    if (op == 0xd0 || op == 0xf0 || op == 0x90) {
      if ((op == 0xd0 && !c->_flag_Z) || (op == 0xf0 && c->_flag_Z) || (op == 0x90 && !c->_flag_C))
        pc += (int8_t)value;
      continue;
    }
    if (op == 0xa5) { A(c, cpu_read8(c, 0, value)); continue; }
    if (op == 0xa9 || op == 0x29 || op == 0xc9) {
      if (!c->m_flag) value |= code[pc++] << 8;
      if (op == 0xa9) A(c, value);
      else if (op == 0x29) A(c, cpu_read_a_m(c) & value);
      else {
        c->_flag_C = cpu_read_a_m(c) >= value; c->P = (c->P & ~1u) | c->_flag_C;
        if (c->m_flag) ActRaiserCpuHle_SetNegativeZero8(c, cpu_read_a_m(c) - value);
        else ActRaiserCpuHle_SetNegativeZero16(c, c->A - value);
      }
      continue;
    }
    value |= code[pc++] << 8;
    if (op == 0x20 || op == 0x22) {
      const uint8_t old_pb = c->PB;
      uint8_t bank = op == 0x22 ? code[pc++] : c->PB;
      if (op == 0x22) cpu_write8(c, 0, c->S--, old_pb);
      ActRaiserCpuHle_PushWord(c, pc - 1); c->PB = bank; c->host_return_valid = 1;
      assert(Leaf(c, ((uint32_t)bank << 16) | value, op == 0x22 ? 3 : 2) == RECOMP_RETURN_NORMAL);
      c->PB = old_pb;
      continue;
    }
    uint8_t bank = c->DB;
    if (op == 0xaf || op == 0x8f) bank = code[pc++];
    if (region == 2 && (value == 0x286 || value == 0x289)) value -= 2;
    if (region == 1 && (value == 0x285 || value == 0x286)) ++value;
    if (op == 0xad || op == 0xaf) { A(c, c->m_flag ? cpu_read8(c, bank, value) : cpu_read16(c, bank, value)); continue; }
    if (op == 0xcd) {
      const uint8_t rhs = cpu_read8(c, bank, value);
      c->_flag_C = (uint8_t)c->A >= rhs; c->P = (c->P & ~1u) | c->_flag_C;
      ActRaiserCpuHle_SetNegativeZero8(c, (uint8_t)(c->A - rhs)); continue;
    }
    if (op == 0xce || op == 0xee) {
      const uint8_t next = cpu_read8(c, bank, value) + (op == 0xee ? 1 : -1);
      cpu_write8(c, bank, value, next); ActRaiserCpuHle_SetNegativeZero8(c, next); continue;
    }
    if (op == 0x8d || op == 0x8f) {
      if (c->m_flag) cpu_write8(c, bank, value, (uint8_t)c->A);
      else cpu_write16(c, bank, value, c->A);
      continue;
    }
    fprintf(stderr, "unexpected %02x at %04x\n", op, pc - 1); assert(0);
  }
  assert(pc == end);
}
static CpuState Setup(bool cycle, unsigned flags) {
  memset(ram, 0x55, sizeof(ram)); memset(trace, 0, sizeof(trace)); calls = stop = 0;
  CpuState c = {.PB = cycle ? 3 : 1, .DB = cycle ? 0x7f : 1,
      .A = 0xa123, .X = 0x0ae4, .Y = 0x6789, .S = 0x1ff0,
      ._flag_C = flags & 1, ._flag_D = (flags >> 1) & 1,
      ._flag_I = (flags >> 2) & 1, ._flag_V = (flags >> 3) & 1};
  cpu_mirrors_to_p(&c);
  return c;
}
static unsigned Prefixes(bool native, unsigned region, bool cycle) {
  const ArRegionalRecoverySnapshot us = {true, 0};
  const uint16_t maxima[] = {0, 1, 7, 24, 255, 999, 65535};
  unsigned count = 0;
  for (unsigned f = 0; f < 16; ++f) for (unsigned a = 0; a < 7; ++a)
    for (unsigned b = 0; b < 7; ++b) for (unsigned frame = 0; frame < (cycle ? 1u : 16u); ++frame) {
      CpuState c = Setup(cycle, f);
      cpu_write16(&c, 0, 0x284, maxima[a]); ram[0x287] = (uint8_t)maxima[b];
      ram[0x0b04] = (uint8_t)maxima[a]; ram[0x0b05] = (uint8_t)maxima[b]; ram[0x88] = frame;
      CpuState ref = c; memcpy(initial, ram, sizeof(ram));
      assert((cycle ? ActRaiserRecovery_Cycle(&c, &us) : ActRaiserRecovery_Drain(&c, &us)) == RECOMP_RETURN_NORMAL);
      if (native) {
        const unsigned expected_calls = calls;
        memcpy(expected, ram, sizeof(ram)); memcpy(reference_trace, trace, sizeof(trace));
        memcpy(ram, initial, sizeof(ram)); calls = 0;
        Native(&ref, cycle ? 0x8271 : 0xb257, cycle ? 0x8298 : 0xb281, region);
        assert(calls == expected_calls && !memcmp(trace, reference_trace, calls * sizeof(Trace)));
        assert(!memcmp(&ref, &c, sizeof(c)) && !memcmp(expected, ram, sizeof(ram)));
      }
      ++count;
    }
  return count;
}
static unsigned Motion(bool native) {
  unsigned count = 0;
  const ArRegionalRecoverySnapshot jp = {false, 60};
  for (unsigned state = 0; state < 16; ++state) for (unsigned phase = 0; phase < 60; ++phase)
    for (unsigned hp = 0; hp < 26; ++hp) {
      CpuState c = Setup(false, hp & 15); c.m_flag = c.x_flag = 1; c.X &= 255; c.Y &= 255;
      cpu_mirrors_to_p(&c); ram[0xaf6] = state; ram[0xb04] = phase;
      ram[0xb05] = 0; ram[0x286] = hp; ram[0x287] = 24;
      CpuState ref = c; memcpy(initial, ram, sizeof(ram));
      ActRaiserRecovery_Motion(&c, &jp, false);
      if (native) {
        memcpy(expected, ram, sizeof(ram)); memcpy(ram, initial, sizeof(ram));
        ref.P &= ~0x20; cpu_p_to_mirrors(&ref);
        Native(&ref, 0x9bfd, 0x9c26, 1);
        assert(!memcmp(&ref, &c, sizeof(c)) && !memcmp(expected, ram, sizeof(ram)));
      }
      assert(ram[0xb05] == 0);
      assert(ram[0xb04] == (state == 4 ? phase : (phase + 1) % 60));
      assert(ram[0x286] == hp + (state != 4 && phase == 59 && hp != 24));
      ++count;
    }
  return count;
}
static void MixedAndSwitches(void) {
  for (unsigned sp = 0; sp < 3; ++sp) for (unsigned hp = 0; hp < 3; ++hp) {
    const ArRegionalRecoveryPolicy policy = {{sp, hp}};
    ArRegionalRecoverySnapshot snapshot; assert(ArRegionalRecovery_Resolve(&policy, &snapshot));
    CpuState c = Setup(true, 0); cpu_write16(&c, 0, 0x284, 200); ram[0x287] = 24;
    ram[0xb04] = 17; ram[0xb05] = 13;
    assert(ActRaiserRecovery_Cycle(&c, &snapshot) == RECOMP_RETURN_NORMAL);
    assert(ram[0xb04] == (snapshot.angel_calls ? 17 : 6));
    assert(ram[0xb05] == (snapshot.cycle_sp ? 20 : 13));
    c.PB = c.DB = 1; c.m_flag = c.x_flag = 1; ram[0xaf6] = 0;
    const uint8_t old_sp = ram[0xb05];
    for (unsigned tick = 0; tick < 120; ++tick) {
      c.m_flag = 1; ActRaiserRecovery_Motion(&c, &snapshot, tick & 1);
      assert(ram[0xb05] == old_sp);
    }
    for (unsigned changed = 0; changed < 4; ++changed) {
      ram[0xb04] = 17; ram[0xb05] = 13; ram[0x286] = 7; ram[0x282] = 90;
      ActRaiserRecovery_Reconcile(&c, changed);
      assert(ram[0xb04] == (changed & 2 ? 0 : 17));
      assert(ram[0xb05] == (changed & 1 ? 0 : 13));
      assert(ram[0x286] == 7 && ram[0x282] == 90);
    }
  }
}
static void Escapes(void) {
  const ArRegionalRecoverySnapshot us = {true, 0};
  const RecompReturn tokens[] = {RECOMP_RETURN_SKIP_1, RECOMP_RETURN_SKIP_2, RECOMP_RETURN_SKIP_3,
      RECOMP_RETURN_TAILCALL, RECOMP_RETURN_PARKED_WAIT, RECOMP_RETURN_OWNED_UNWIND};
  for (unsigned cycle = 0; cycle < 2; ++cycle) for (unsigned at = 1; at <= 2; ++at)
    for (unsigned t = 0; t < 6; ++t) {
      CpuState c = Setup(cycle, 0); ram[0x88] = 0; stop = at; escape = tokens[t];
      assert((cycle ? ActRaiserRecovery_Cycle(&c, &us) : ActRaiserRecovery_Drain(&c, &us)) == escape);
      assert(calls == at && c.S == 0x1234 && !g_cpu_return_scope);
      assert(c.PB == (!cycle && escape != RECOMP_RETURN_PARKED_WAIT && escape != RECOMP_RETURN_OWNED_UNWIND ? 1 : 0x42));
    }
}
static void OwnershipAndEntries(void) {
  const ArRegionalRecoverySnapshot jp = {true, 60};
  for (unsigned stock = 0; stock < 256; ++stock) for (unsigned phase = 0; phase < 60; ++phase) {
    CpuState c = Setup(false, 0); c.m_flag = c.x_flag = 1;
    ram[0xb04] = phase; ram[0xb05] = stock; ram[0xaf6] = 0;
    ram[0x286] = 3; ram[0x287] = 24;
    ActRaiserRecovery_Motion(&c, &jp, false);
    assert(ram[0xb04] == (phase + 1) % 60 && ram[0xb05] == stock);
    assert(ram[0x286] == (phase == 59 ? 4 : 3));
  }
  assert(!ActRaiserRecovery_CycleEntry(NULL) && !ActRaiserRecovery_DrainEntry(NULL) &&
         !ActRaiserRecovery_MotionEntry(NULL));
  for (unsigned shape = 0; shape < 16; ++shape) {
    CpuState c = Setup(true, 0);
    c.m_flag = !!(shape & 1); c.x_flag = !!(shape & 2);
    c.D = !!(shape & 4); c.emulation = !!(shape & 8);
    assert(ActRaiserRecovery_CycleEntry(&c) == (shape == 0));
    c.PB = c.DB = 1;
    assert(ActRaiserRecovery_DrainEntry(&c) == (shape == 0));
    assert(ActRaiserRecovery_MotionEntry(&c) == (shape == 3));
  }
  ArRegionalRecoveryPolicy bad = {{kArRegionalSource_Count, kArRegionalSource_US}};
  ArRegionalRecoverySnapshot sentinel = {true, 42};
  assert(!ArRegionalRecovery_Resolve(&bad, &sentinel) && sentinel.cycle_sp && sentinel.angel_calls == 42);
  assert(!ArRegionalRecovery_Descriptor(kArRegionalRecovery_Count));
  assert(!ArRegionalRecovery_Init(&bad, kArRegionalSource_Count));
}
static void ReadBank(const char *path, unsigned bank) {
  FILE *file = fopen(path, "rb"); assert(file);
  assert(!fseek(file, bank * 0x8000, SEEK_SET));
  assert(fread(code + 0x8000, 1, 0x8000, file) == 0x8000); fclose(file);
}
int main(int argc, char **argv) {
  assert(argc == 1 || argc == 6);
  unsigned count = Prefixes(false, 0, true) + Prefixes(false, 0, false) + Motion(false);
  if (argc == 6) for (unsigned i = 1; i <= 5; ++i) {
    if (i == 2) { ReadBank(argv[i], 1); count += Motion(true); continue; }
    ReadBank(argv[i], 3); count += Prefixes(true, i > 2 ? 2 : 0, true);
    if (i == 1) { ReadBank(argv[i], 1); count += Prefixes(true, 0, false); }
  }
  MixedAndSwitches(); Escapes(); OwnershipAndEntries();
  printf("recovery: %u native/reference cases, mixed policies and escapes passed\n", count);
  return 0;
}
