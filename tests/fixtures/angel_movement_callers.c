/* Local-ROM angel audit against production generated code. Native animation,
 * arrows, recovery continuations and camera follow all execute without stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_angel.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "support/test_assert.h"

extern unsigned sr_diagnostic_trap_warning_count(void);

static void Brk(CpuState *cpu) { cpu_write8(cpu, 0, 0x035b, (uint8)cpu->A); }
static void Cop(CpuState *cpu) { cpu_write8(cpu, 0, 0x035a, (uint8)cpu->A); }
static void Fatal(const char *message) {
  fprintf(stderr, "%s: A=%04x X=%04x Y=%04x S=%04x PB=%02x DB=%02x P=%02x\n",
          message, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.PB, g_cpu.DB, g_cpu.P);
  exit(1);
}

static void Dispatch(unsigned entry, bool long_call, uint8 p, uint8 db) {
  cpu_state_init(&g_cpu, g_ram);
  g_cpu.emulation = 0;
  g_cpu.P = p;
  cpu_p_to_mirrors(&g_cpu);
  g_cpu.PB = 1;
  g_cpu.DB = db;
  g_cpu.X = 0x0ae4;
  g_cpu.S = 0x1efd;
  g_cpu.host_return_valid = 1;
  g_cpu_brk_hook = Brk;
  g_cpu_cop_hook = Cop;
  cpu_write16(&g_cpu, 0, 0x1efe, 0x1234);
  if (long_call) cpu_write8(&g_cpu, 0, 0x1f00, 1);
  WatchdogFrameStart();
  const RecompReturn result = cpu_dispatch_paired_tail_from(&g_cpu, entry, g_cpu.S, 1, entry);
  assert(result == RECOMP_RETURN_NORMAL);
  assert(g_cpu.S == 0x1eff + long_call && g_cpu.PB == 1 && g_cpu.DB == db);
  assert(!sr_diagnostic_trap_warning_count());
}

static void Begin(unsigned facing) {
  memset(g_ram, 0, kSnesWramSize);
  cpu_state_init(&g_cpu, g_ram);
  ActRaiserAngel_Reset();
  g_ram[0x19] = 1;
  g_ram[0x0ae4] = 12;
  g_ram[0x0b0a] = 14;
  cpu_write16(&g_cpu, 0, 0x0aee, 128);
  cpu_write16(&g_cpu, 0, 0x0af0, 128);
  g_ram[0x0b06] = (uint8)facing;
  g_ram[0x286] = 10;
  Dispatch(0x01cff2, false, 0, 1);
}

static unsigned Word(unsigned at) { return g_ram[at] | g_ram[at + 1] << 8; }

static void Move(int x, int y, unsigned expected_x, unsigned expected_y) {
  Begin(0);
  for (unsigned n = 0; n < 60; ++n) {
    ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(x, y, 0), 0, 1);
    Dispatch(0x01b21b, true, 0, 1);
  }
  assert(Word(0x0aee) == expected_x && Word(0x0af0) == expected_y);
  assert(g_ram[0x0b06] < 4);
}

int main(int argc, char **argv) {
  if (argc != 2) return 99;
  static uint8 rom[1048576];
  FILE *file = fopen(argv[1], "rb");
  if (!file) return 98;
  const bool read = fread(rom, 1, sizeof(rom), file) == sizeof(rom) && fgetc(file) == EOF;
  if (fclose(file) || !read) return 97;
  RtlRegisterGame(&kActRaiserGameModule);
  SnesInit(rom, sizeof(rom));
  ActRaiserHleFatal_RegisterHostEscape(Fatal);
  Move(32767, 0, 248, 128);
  Move(16384, 0, 188, 128);
  Move(32767, 32767, 212, 212);
  Move(13000, 29000, 175, 234);
  /* Hold each cardinal shot while moving in a different direction, across
   * repeated complete shooting cycles. Native arrows must stay on one axis. */
  for (unsigned facing = 0; facing < 4; ++facing) {
    Begin(facing);
    unsigned shots = 0;
    for (unsigned n = 0; n < 60; ++n) {
      g_ram[0xa1] = 0x40;
      ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 0, 1);
      Dispatch(0x01b21b, true, 0, 1);
      assert(g_ram[0x0b06] == facing);
      assert(Word(0x0af0) == 128 && Word(0x0aee) == 130 + 2 * n);
      if (Word(0x0b1c)) {
        const int vx = (int16)Word(0x0b24), vy = (int16)Word(0x0b26);
        assert(!vx || !vy);
        assert(facing == 0 ? vy < 0 : facing == 1 ? vx > 0 : facing == 2 ? vy > 0 : vx < 0);
        ++shots;
      }
    }
    assert(shots);
    g_ram[0xa1] = 0;
    for (unsigned n = 0; n < 30; ++n) {
      ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 0, 1);
      Dispatch(0x01b21b, true, 0, 1);
    }
    assert(g_ram[0x0b06] == 1);
  }
  Begin(0);
  cpu_write16(&g_cpu, 0x7f, 0x9750, 1);
  ActRaiserAngel_SetInput(ActRaiserAngel_EncodeStick(32767, 0, 0), 0, 1);
  Dispatch(0x01b21b, true, CPU_P_M | CPU_P_C, 0);
  assert(g_cpu.P == (CPU_P_M | CPU_P_C) && Word(0x0aee) == 128);
  puts("Generated angel audit: analog speed, cardinal strafing, arrows and dialog ABI passed");
  return 0;
}
