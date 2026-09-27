/* Optional local-ROM contract fixture, linked by regional_hle_generated_test.py.
 * Uses production generated callers and real native callees. The scheduler RTS
 * cases prepare a pushed continuation with an inherited return owner. Prefix
 * cases stop at a specified native continuation, before unrelated game work.
 * Terminal exit from an execution checkpoint is permitted by the runtime API.
 * No generated C or ROM bytes are modified or committed by this fixture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "actraiser/actraiser_rtl.h"
void AuditPolicy(unsigned, bool);
extern unsigned sr_diagnostic_trap_warning_count(void);
static unsigned Flags(CpuState *c) {
  CpuState copy = *c;
  cpu_mirrors_to_p(&copy);
  return copy.P;
}
static unsigned kind, source, hooks, steps, stop, sounds;
static uint8_t before[kSnesWramSize];
/* The recovery native leaf sets $7F:7C05 to redraw the changed status. */
static void W(unsigned a, unsigned v) { cpu_write16(&g_cpu, a >= 0x2000 ? 0x7e : 0, a, v); }
static unsigned R(unsigned a) { return cpu_read16(&g_cpu, 0, a); }
void AuditHook(const char *n, CpuState *c) {
  ++hooks;
  fprintf(stderr, "HOOK %s A=%04x S=%04x X=%04x Y=%04x P=%02x\n", n, c->A, c->S, c->X, c->Y,
          Flags(c));
}
static void Finish(CpuState *c, unsigned pc) {
  printf(
      "RESULT kind=%u source=%u pc=%06x hooks=%u A=%04x X=%04x Y=%04x S=%04x P=%02x q4=%u q5=%u "
      "score=%04x lives=%u state=%u dx=%u delay=%u resume=%04x top=%u sounds=%u PB=%u DB=%u D=%u\n",
      kind, source, pc, hooks, c->A, c->X, c->Y, c->S, Flags(c), g_ram[0xb04], g_ram[0xb05],
      R(0x1f), g_ram[0x1c], R(0x8fa), R(0x8e6), R(0x904), R(0x8f2), R(0x8ec), sounds, c->PB, c->DB,
      c->D);
  for (unsigned a = 0; a < kSnesWramSize; a++)
    if (before[a] != g_ram[a]) {
      bool allowed = (a >= 0x1fe0 && a <= 0x1fff) || (a >= 0x8e0 && a < 0x920) ||
                     (a >= 0x80 && a < 0xb0) ||
                     (kind == 5 && (a == 0x32c || a == 0x32d || a == 0x1f || a == 0x20)) ||
                     (kind == 6 &&
                      ((a >= 0x282 && a < 0x288) || (a >= 0xafe && a <= 0xb06) || a == 0x17c05)) ||
                     (kind == 7 && (a == 0x1c || a == 0x1f || a == 0x20));
      if (!allowed) {
        fprintf(stderr, "unexpected memory write %05x %02x->%02x\n", a, before[a], g_ram[a]);
        exit(4);
      }
    }
  exit(sr_diagnostic_trap_warning_count() ? 2 : 0);
}
static void Checkpoint(CpuState *c, uint32 pc) {
  if (pc == stop) Finish(c, pc);
  if (++steps > 5000) {
    fprintf(stderr, "too many steps at %06x\n", pc);
    exit(3);
  }
}
static void UnexpectedBrk(CpuState *c) {
  (void)c;
  fputs("unexpected BRK in native caller fixture\n", stderr);
  exit(5);
}
static void Sound(CpuState *c) {
  /* Verify PAL's extra-life COP without initializing a host audio device. */
  if (kind != 7 || c->A != 0x8d) exit(6);
  ++sounds;
}
int main(int argc, char **argv) {
  if (argc != 5) return 99;
  kind = atoi(argv[1]);
  source = atoi(argv[2]);
  unsigned value = strtoul(argv[3], 0, 0);
  bool active = atoi(argv[4]);
  static uint8 rom[1048576];
  const char *path = getenv("AR_AUDIT_ROM");
  if (!path) return 98;
  FILE *f = fopen(path, "rb");
  if (!f || fread(rom, 1, sizeof(rom), f) != sizeof(rom)) return 98;
  fclose(f);
  static RtlGameModule module;
  static RtlGameExecutionApi execution;
  module = kActRaiserGameModule;
  execution = *module.execution;
  execution.struct_size = RTL_GAME_EXECUTION_API_V3_SIZE;
  execution.execution_checkpoint = Checkpoint;
  module.execution = &execution;
  RtlRegisterGame(&module);
  SnesInit(rom, sizeof(rom));
  memset(g_ram, 0, kSnesWramSize);
  AuditPolicy(source, active);
  cpu_state_init(&g_cpu, g_ram);
  g_cpu.emulation = 0;
  g_cpu.P = CPU_P_V | CPU_P_C;
  cpu_p_to_mirrors(&g_cpu);
  g_cpu.X = 0x8e0;
  g_cpu.Y = 0xc961;
  g_cpu.S = 0x1ffd;
  g_cpu.host_return_valid = 1;
  g_cpu_brk_hook = UnexpectedBrk;
  g_cpu_cop_hook = Sound;
  W(0x1ffe, 0x1234);
  W(0x8a, 0x8a0);
  W(0x8f6, 0x4000);
  g_ram[0x8f8] = 0x7e;
  unsigned entry = 0, hook = 0;
  switch (kind) {
  case 0:
    entry = 0xb3d8;
    stop = 0xb3e7;
    g_ram[0x18] = 1;
    W(0x912, 0xb3bf);
    W(0x8fa, 36);
    break;
  case 1:
    entry = 0xc408;
    stop = 0xc40d;
    g_ram[0x18] = 3;
    W(0x912, 0xc3a5);
    W(0x8fa, 12);
    g_cpu.A = value;
    break;
  case 2:
    entry = 0xc71e;
    stop = value < 64 ? 0xc726 : source == 1 ? 0xc682 : 0xc6e3;
    W(0x18, 0x0203);
    W(0x912, 0xc66f);
    W(0x8fa, 4);
    W(0x8f6, 0x5000);
    W(0x80, value);
    break;
  case 3:
    hook = 0xc718;
    stop = source == 1 ? 0xc71e : 0x8657;
    W(0x18, 0x0203);
    W(0x912, 0xc66f);
    W(0x8fa, 4);
    W(0x8f6, 0x5000);
    break;
  case 4:
    hook = 0xbe78;
    stop = source == 1 ? 0xbe7e : 0x86fa;
    g_ram[0x18] = 2;
    W(0x912, 0xbdff);
    W(0x8fa, 11);
    W(0x8f6, 0x5000);
    break;
  case 5:
    hook = 0x981c;
    stop = value ? (active ? 0x9826 : 0x8538) : 0x982f;
    W(0x32c, value);
    W(0x1f, 0x4567);
    break;
  case 6:
    entry = 0x1b252;
    stop = 0x1b281;
    g_cpu.PB = g_cpu.DB = 1;
    g_ram[0xb04] = g_ram[0xb05] = 3;
    g_ram[0x287] = 10;
    g_ram[0x286] = 4;
    W(0x284, 100);
    W(0x282, 40);
    W(0x88, value);
    break;
  case 7:
    entry = 0x8892;
    stop = 0x889c;
    W(0x90e, 1);
    W(0x1f, 0x1999);
    g_ram[0x349] = 1;
    g_ram[0x1c] = 2;
    break;
  case 8:
    entry = 0x95ed;
    stop = 0x96a1;
    g_ram[0x18] = 3;
    W(0x4000, 0x600);
    for (unsigned i = 0; i < 42; i++)
      W(0x4002 + 2 * i, 0x100);
    g_ram[0x4100] = 14;
    g_ram[0x4101] = 1;
    W(0x461c, 0x700);
    g_ram[0x4700] = g_ram[0x4701] = 16;
    g_ram[0x4702] = 32;
    g_ram[0x4703] = 24;
    break;
  default:
    return 97;
  }
  memcpy(before, g_ram, sizeof(before));
  WatchdogFrameStart();
  if (hook) {
    entry = 0x868f;
    g_cpu.S -= 2;
    W(g_cpu.S + 1, hook - 1);
    cpu_tailcall_inherit_return_context(0x1ffd, 1);
  }
  for (unsigned i = 0; i < g_dispatch_table_count; i++)
    if (g_dispatch_table[i].pc24 == entry) {
      RecompReturn (*native)(CpuState *) =
          g_dispatch_table[i].variant[2 * g_cpu.m_flag + g_cpu.x_flag];
      if (!native) return 96;
      const RecompReturn result = native(&g_cpu);
      fprintf(stderr, "unexpected return=%d before native continuation %06x\n", result, stop);
      return 95;
    }
  return 96;
}
