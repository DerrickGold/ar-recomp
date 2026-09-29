/* Local-ROM collision audit. Link against the production game objects, replacing
 * only main. Execute original generated routines with caller-supplied WRAM.
 * Sound interrupts retain their ROM memory effect without opening audio devices.
 * No collision, damage, animation, movement or return-dispatch code is stubbed. */
#include <stdio.h>
#include <stdlib.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "actraiser/actraiser_rtl.h"

extern unsigned sr_diagnostic_trap_warning_count(void);

static void Brk(CpuState *cpu) { cpu_write8(cpu, 0, 0x035b, (uint8)cpu->A); }
static void Cop(CpuState *cpu) { cpu_write8(cpu, 0, 0x035a, (uint8)cpu->A); }

static int Read(const char *path, uint8 *data, size_t size) {
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  const int ok = fread(data, 1, size, file) == size && fgetc(file) == EOF;
  return fclose(file) == 0 && ok;
}

int main(int argc, char **argv) {
  if (argc != 10) return 99;
  static uint8 rom[1048576];
  if (!Read(argv[1], rom, sizeof(rom))) return 98;
  RtlRegisterGame(&kActRaiserGameModule);
  SnesInit(rom, sizeof(rom));
  if (!Read(argv[2], g_ram, kSnesWramSize)) return 97;
  const unsigned entry = strtoul(argv[4], NULL, 0);
  const unsigned repeat = strtoul(argv[9], NULL, 0);
  if (!repeat || repeat > 100) return 96;
  for (unsigned n = 0; n < repeat; ++n) {
    cpu_state_init(&g_cpu, g_ram);
    g_cpu.emulation = 0;
    g_cpu.P = strtoul(argv[8], NULL, 0);
    cpu_p_to_mirrors(&g_cpu);
    g_cpu.X = strtoul(argv[5], NULL, 0);
    g_cpu.Y = strtoul(argv[6], NULL, 0);
    g_cpu.A = strtoul(argv[7], NULL, 0);
    g_cpu.S = 0x1efd;
    g_cpu.host_return_valid = 1;
    g_cpu_brk_hook = Brk;
    g_cpu_cop_hook = Cop;
    cpu_write16(&g_cpu, 0, 0x1efe, 0x1234);
    WatchdogFrameStart();
    RecompReturn result = cpu_dispatch_paired_tail_from(&g_cpu, entry, g_cpu.S, 1, entry);
    if (result != RECOMP_RETURN_NORMAL || g_cpu.S != 0x1eff || sr_diagnostic_trap_warning_count()) {
      fprintf(stderr, "entry=%06x result=%d S=%04x\n", entry, result, g_cpu.S);
      return 95;
    }
  }
  FILE *file = fopen(argv[3], "wb");
  if (!file) return 94;
  const int ok = fwrite(g_ram, 1, kSnesWramSize, file) == kSnesWramSize;
  if (fclose(file) || !ok) return 93;
  return 0;
}
