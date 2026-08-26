#include "actraiser_rtl.h"

#include "audio_trace.h"
#include "cpu_state.h"
#include "spc_upload.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8 test_rom[0x80000];
uint8 g_ram[kSnesWramSize];
const uint8 *g_rom = test_rom;
const char *g_last_recomp_func;
Snes *g_snes;

static int failures;
static int lock_depth;
static int cycle_count;
static int producer;

static void check(bool condition, const char *message) {
  if (condition) return;
  fprintf(stderr, "ActRaiser SPC upload adapter failed: %s\n", message);
  ++failures;
}

uint8 *RomPtr(uint32 address) {
  size_t offset = ((size_t)(address >> 16) & 0x7fu) * 0x8000u +
      (address & 0x7fffu);
  return test_rom + (offset & (sizeof(test_rom) - 1u));
}

void RtlApuLock(void) { ++lock_depth; }
void RtlApuUnlock(void) { --lock_depth; }
void audio_trace_set_producer(int value) { producer = value; }
void apu_cycle(Apu *apu) {
  ++cycle_count;
  apu->spc->pc = 0x0460u;
}

static void test_source_and_stack_policy(void) {
  CpuState cpu;
  uint32 source = 0u;
  memset(&cpu, 0, sizeof(cpu));
  cpu.D = 0xff80u;
  g_ram[0x0025] = 0x34u;
  g_ram[0x0026] = 0x12u;
  g_ram[0x0027] = 0x08u;
  check(ActRaiser_SpcUploadSource(&cpu, &source) && source == 0x081234u,
        "direct-page source pointer wraps at 16 bits");
  g_last_recomp_func = "Func_02_9A56";
  check(ActRaiser_SpcUploadStackPop(&cpu) == 2,
        "$9A56 wrapper retains its two-byte stack adjustment");
  g_last_recomp_func = "Func_02_9964";
  check(ActRaiser_SpcUploadStackPop(&cpu) == 3,
        "ordinary upload wrappers retain their three-byte adjustment");
}

static void test_sample_pool_policy(Apu *apu, CpuState *cpu) {
  SrSpcUploadResult upload = {2u, 0u, 2u};
  size_t pool = (size_t)(RomPtr(0x088000u) - test_rom);
  memset(test_rom, 0, sizeof(test_rom));
  test_rom[2] = 1u;
  test_rom[3] = 0u;
  test_rom[pool] = 3u;
  test_rom[pool + 2u] = 0x10u;
  test_rom[pool + 3u] = 0x11u;
  test_rom[pool + 4u] = 0x12u;
  test_rom[pool + 5u] = 2u;
  test_rom[pool + 7u] = 0x20u;
  test_rom[pool + 8u] = 0x21u;
  g_ram[0x0358] = 0xfeu;
  g_ram[0x0359] = 0xffu;
  cpu->D = 0x0100u;
  g_last_recomp_func = "Func_02_9964";
  check(ActRaiser_SpcUploadCustomize(cpu, &upload, 0x088000u),
        "ActRaiser second-stage sample pool is accepted");
  check(apu->ram[0xfffe] == 0x20u && apu->ram[0xffff] == 0x21u &&
            apu->ram[0] == 0x10u && apu->ram[2] == 0x12u,
        "sample chunks pack and wrap in ARAM");
  check(g_ram[0x0102] == 0u && g_ram[0x0103] == 0u &&
            g_ram[0x0108] == 3u && g_ram[0x0109] == 0u,
        "upload result metadata is returned through direct page");
}

static void test_bootstrap_and_resident_completion(Apu *apu, Spc *spc) {
  static const uint8 entry[] = {
      0x20, 0xcd, 0xcf, 0xbd, 0xe8, 0x00, 0x5d, 0xaf,
      0xc8, 0xf0, 0xd0, 0xfb, 0xc5, 0xff, 0x11,
  };
  static const uint8 idle[] = {0xeb, 0xfd, 0xf0, 0xfc};
  memcpy(apu->ram + 0x0400u, entry, sizeof(entry));
  memcpy(apu->ram + 0x0460u, idle, sizeof(idle));
  spc->pc = 0x0400u;
  cycle_count = 0;
  ActRaiser_SpcUploadCommit(apu, 0x0400u, true);
  check(cycle_count == 1 && spc->pc == 0x0460u &&
            producer == AUDIO_TRACE_PRODUCER_UNKNOWN,
        "initial bootstrap advances synchronously to its idle loop");

  apu->ram[0x0f48] = 0xcdu;
  apu->ram[0x0f49] = 0x31u;
  apu->ram[0x0f4a] = 0xd8u;
  apu->ram[0x0f4b] = 0xf1u;
  apu->ram[0x0f4c] = 0x6fu;
  spc->pc = 0x0200u;
  ActRaiser_SpcUploadCommit(apu, 0u, false);
  spc->pc = 0x0f10u;
  ActRaiser_SpcUploaderCompleteTick();
  check(spc->pc == 0x0f48u && lock_depth == 0,
        "deferred resident upload resumes once the SPC reaches its wait");
}

int main(void) {
  Apu apu;
  Spc spc;
  Snes snes;
  Cart cart;
  CpuState cpu;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  memset(&snes, 0, sizeof(snes));
  memset(&cart, 0, sizeof(cart));
  memset(&cpu, 0, sizeof(cpu));
  apu.spc = &spc;
  spc.apu = &apu;
  snes.apu = &apu;
  snes.cart = &cart;
  cart.romSize = sizeof(test_rom);
  g_snes = &snes;
  test_source_and_stack_policy();
  test_sample_pool_policy(&apu, &cpu);
  test_bootstrap_and_resident_completion(&apu, &spc);
  if (failures == 0) puts("ActRaiser SPC upload adapter: PASS");
  return failures == 0 ? 0 : 1;
}
