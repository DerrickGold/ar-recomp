#include "actraiser/actraiser_angel.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "support/test_assert.h"

#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
static uint8_t s_ram[0x20000];
static uint32_t s_tail;
static unsigned s_animations, s_arrows, s_cameras;

int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) {
  (void)scope;
  (void)cpu;
  return 0;
}
int cpu_hle_tailcall_request(uint32_t target, uint32_t source) {
  assert(source == 0x019beb || source == 0x019bf4);
  s_tail = target;
  return 1;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  assert(bank == 0 || bank == 1 || bank == 0x7f);
  return s_ram[(bank == 0x7f ? 0x10000 : 0) + address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  assert(bank == 0 || bank == 1 || bank == 0x7f);
  s_ram[(bank == 0x7f ? 0x10000 : 0) + address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8_t)value);
  cpu_write8(cpu, bank, address + 1, (uint8_t)(value >> 8));
}
static void Put(unsigned address, uint16_t value) {
  s_ram[address] = (uint8_t)value;
  s_ram[address + 1] = (uint8_t)(value >> 8);
}
static uint16_t Word(unsigned address) {
  return s_ram[address] | s_ram[address + 1] << 8;
}
static CpuState Begin(void) {
  memset(s_ram, 0, sizeof(s_ram));
  ActRaiserAngel_Reset();
  s_animations = s_arrows = s_cameras = 0;
  Put(0x0aee, 128);
  Put(0x0af0, 128);
  CpuState cpu = {.PB = 1, .DB = 1, .S = 0x1ee, .X = 0x0ae4};
  cpu_p_to_mirrors(&cpu);
  return cpu;
}
RecompReturn bank_01_B4AF_M0X0(CpuState *cpu) {
  cpu->P &= ~CPU_P_M;
  cpu->m_flag = 0;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_B4AF_M1X0(CpuState *cpu) {
  return bank_01_B4AF_M0X0(cpu);
}
RecompReturn bank_01_AC70_M0X0(CpuState *cpu) {
  ++s_animations;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_CFF2_M0X0(CpuState *cpu) {
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_B23B_M0X0(CpuState *cpu) {
  /* Firing-state stand-in deliberately changes X to the arrow record just
   * like native allocation, and leaves cardinal facing untouched. */
  cpu->X = 0x0b0a;
  cpu->A = 0x4567;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_B41A_M0X0(CpuState *cpu) {
  ++s_arrows;
  assert(cpu->X == 0x0b0a);
  assert(cpu->A == 0x4567);
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_B4C6_M0X0(CpuState *cpu) {
  ++s_cameras;
  cpu->S += 3;
  return RECOMP_RETURN_NORMAL;
}

static void CompleteDecoder(CpuState *cpu) {
  assert(s_tail == 0x019c30 || s_tail == 0x019c34);
  if (s_tail == 0x019c34) {
    Put(0x0afe, 0);
    Put(0x0b00, 0);
    cpu->A = Word(0x19215);
    if (cpu->A) {
      cpu->A = (cpu->A & 0xff00) | 5;
      s_ram[0x0b06] = 5;
    }
  }
  cpu->P = s_ram[++cpu->S];
  cpu_p_to_mirrors(cpu);
  cpu->S += 3;
}

static void TestDigitalDecoder(void) {
  /* Independently decoded native tables, including opposing directions. */
  const int8_t directions[] = {-1, 2, 6, -1, 4, 3, 5, -1, 0, 1, 7, 0, 2, 2, 2, 0};
  const int16_t dx[] = {0, 2, 2, 2, 0, -2, -2, -2};
  const int16_t dy[] = {-2, -2, 0, 2, 2, 2, 0, -2};
  const uint8_t facing[] = {0, 0, 1, 2, 2, 2, 3, 0};
  for (unsigned bits = 0; bits < 16; ++bits) {
    for (unsigned picker = 0; picker < 2; ++picker) {
      for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned supplied = 0; supplied < 2; ++supplied) {
          CpuState cpu = Begin();
          cpu.P = (uint8_t)(CPU_P_C | CPU_P_V | (mode << 4));
          cpu_p_to_mirrors(&cpu);
          const uint8_t saved_p = cpu.P;
          const uint16_t stack = cpu.S;
          cpu.A = 0xab00 | bits;
          cpu.X = 0x1234;
          cpu.Y = 0x5678;
          s_ram[0xa1] = (uint8_t)bits;
          s_ram[0x0b06] = 3;
          Put(0x19215, (uint16_t)picker);
          const RecompReturn result = supplied ? ActRaiser_AngelDecodeDirection(&cpu) :
              ActRaiser_AngelDecodeInput(&cpu);
          assert(result == RECOMP_RETURN_TAILCALL);
          const int direction = directions[bits];
          assert(s_tail == (direction < 0 ? 0x019c34u : 0x019c30u));
          CompleteDecoder(&cpu);
          assert(cpu.P == saved_p && cpu.S == stack + 3);
          assert(Word(0x0afe) == (uint16_t)(direction < 0 ? 0 : dx[direction]));
          assert(Word(0x0b00) == (uint16_t)(direction < 0 ? 0 : dy[direction]));
          assert(s_ram[0x0b06] == (picker ? 5 : direction < 0 ? 3 : facing[direction]));
          assert(cpu.X == (unsigned)(direction < 0 ? bits : direction));
          assert(cpu.Y == (unsigned)(direction < 0 ? 0x78 : dy[direction] < 0 ? 0xff : 0));
        }
      }
    }
  }
}

static void Tick(CpuState *cpu, uint32_t input) {
  ActRaiserAngel_SetInput(input, 0, 1);
  cpu->S = 0x1ee;
  cpu->X = 0x0ae4;
  assert(ActRaiser_AngelDecodeInput(cpu) == RECOMP_RETURN_TAILCALL);
  CompleteDecoder(cpu);
  cpu->S = 0x1ee;
  cpu->X = 0x0ae4;
  assert(ActRaiser_AngelMove(cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu->S == 0x1f0);
}

static void TestAnalog(void) {
  CpuState cpu = Begin();
  uint32_t input = ActRaiserAngel_EncodeStick(32767, 0, 12);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 248 && Word(0x0af0) == 128);
  cpu = Begin();
  input = ActRaiserAngel_EncodeStick(16384, 0, 0);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 188 && Word(0x0af0) == 128);
  cpu = Begin();
  input = ActRaiserAngel_EncodeStick(32767, 32767, 12);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 212 && Word(0x0af0) == 212);
  assert(s_ram[0x0b06] < 4);
  cpu = Begin();
  input = ActRaiserAngel_EncodeStick(-32768, -32768, 12);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 44 && Word(0x0af0) == 44);
  cpu = Begin();
  input = ActRaiserAngel_EncodeStick(13000, 29000, 0);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 175 && Word(0x0af0) == 234);
  cpu = Begin();
  input = ActRaiserAngel_EncodeStick(1000, 0, 0);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 131);
  input = ActRaiserAngel_EncodeStick(1000, 1000, 12);
  assert(input == AR_ANGEL_STICK_ENABLED);
  for (unsigned i = 0; i < 60; ++i) Tick(&cpu, input);
  assert(Word(0x0aee) == 131 && Word(0x0af0) == 128);
  /* A teleport or town change cannot inherit accumulated fractional motion. */
  Tick(&cpu, ActRaiserAngel_EncodeStick(10000, 0, 0));
  Put(0x0aee, 200);
  Tick(&cpu, ActRaiserAngel_EncodeStick(10000, 0, 0));
  assert(Word(0x0aee) == 200);
  ActRaiserAngel_SetInput(input, 0, 2);
  Tick(&cpu, ActRaiserAngel_EncodeStick(10000, 0, 0));
  assert(Word(0x0aee) == 200);
}

static void TestBoundsAndKnockback(void) {
  CpuState cpu = Begin();
  Put(0x0aee, 495);
  Tick(&cpu, ActRaiserAngel_EncodeStick(32767, 0, 0));
  assert(Word(0x0aee) == 496);
  Tick(&cpu, ActRaiserAngel_EncodeStick(32767, 0, 0));
  assert(Word(0x0aee) == 496);
  Put(0x0af0, 1);
  Tick(&cpu, ActRaiserAngel_EncodeStick(0, -32768, 0));
  assert(Word(0x0af0) == 0);
  /* Digital movement rejects the native out-of-bounds step rather than clamping. */
  ActRaiserAngel_SetInput(0, 0, 1);
  Put(0x0aee, 495);
  Put(0x0afe, 2);
  cpu.S = 0x1ee;
  cpu.X = 0x0ae4;
  assert(ActRaiser_AngelMove(&cpu) == RECOMP_RETURN_NORMAL);
  assert(Word(0x0aee) == 495);
  Put(0x0af6, 3);
  Put(0x0afc, 10);
  Put(0x0b00, (uint16_t)-6);
  cpu.S = 0x1ee;
  assert(ActRaiser_AngelKnockback(&cpu) == RECOMP_RETURN_NORMAL);
  assert(Word(0x0af6) == 0 && Word(0x0afc) == 60);
  assert(Word(0x0af0) == 0);
}

static void TestFiringAndFreeze(void) {
  CpuState turning = Begin();
  s_ram[0xa1] = 0x40;
  for (unsigned i = 0; i < 20; ++i)
    Tick(&turning, ActRaiserAngel_EncodeStick(32767, 0, 0));
  assert(s_ram[0x0b06] == 0 && Word(0x0aee) == 168);
  s_ram[0xa1] = 0;
  Tick(&turning, ActRaiserAngel_EncodeStick(32767, 0, 0));
  assert(s_ram[0x0b06] == 1);
  for (unsigned state = 1; state <= 2; ++state) {
    CpuState cpu = Begin();
    Put(0x0af6, (uint16_t)state);
    s_ram[0x0b06] = 0; /* facing up while moving horizontally */
    ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 0, 1);
    assert(ActRaiser_AngelService(&cpu) == RECOMP_RETURN_NORMAL);
    assert(Word(0x0aee) == 130 && Word(0x0af0) == 128);
    assert(s_ram[0x0b06] == 0 && cpu.X == 0x0b0a && cpu.A == 0x4567);
    assert(cpu.S == 0x1f1 && s_arrows == 1 && s_cameras == 1);
  }
  CpuState cpu = Begin();
  Put(0x19750, 1);
  ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 0, 1);
  assert(ActRaiser_AngelService(&cpu) == RECOMP_RETURN_NORMAL);
  assert(Word(0x0aee) == 128 && s_arrows == 0 && s_cameras == 0);
  assert(s_animations == 1);
  /* Town dialogs enter the frozen service with DB=0 and an 8-bit A. */
  cpu = Begin();
  cpu.DB = 0;
  cpu.P = CPU_P_M | CPU_P_C;
  cpu_p_to_mirrors(&cpu);
  Put(0x19750, 1);
  assert(ActRaiser_AngelService(&cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu.DB == 0 && cpu.P == (CPU_P_M | CPU_P_C) && cpu.S == 0x1f1);
  cpu = Begin();
  Put(0x0af6, 1);
  ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 1, 1);
  assert(ActRaiser_AngelService(&cpu) == RECOMP_RETURN_NORMAL);
  assert(Word(0x0aee) == 128);
}

int main(void) {
  TestDigitalDecoder();
  TestAnalog();
  TestBoundsAndKnockback();
  TestFiringAndFreeze();
  puts("Angel movement: digital tables, analog speed, bounds, firing and freeze passed");
  return 0;
}
