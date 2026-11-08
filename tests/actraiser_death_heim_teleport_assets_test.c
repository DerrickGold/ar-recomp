#include "actraiser/actraiser_death_heim_teleport_assets.h"
#include "actraiser_game.h"
#include "snesrecomp/runner.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
static uint8_t s_rom[1024 * 1024];
static uint16_t s_vram[SR_PPU_VRAM_WORD_COUNT], s_cgram[SR_PPU_CGRAM_WORD_COUNT];
static uint8_t s_before_ram[kActRaiserWramSize];
static uint16_t s_before_vram[SR_PPU_VRAM_WORD_COUNT], s_before_cgram[SR_PPU_CGRAM_WORD_COUNT];
static CpuState s_cpu;
static uint64_t s_lifetime, s_reset_generation, s_load_generation;
static unsigned s_cgram_address, s_cgram_low;
static uint8_t s_object_select;
static bool s_cgram_second, s_reject_patch, s_stale_borrow;
static unsigned s_patch_count;
static int s_runner;

static void Word(unsigned at, uint16_t word) {
  g_ram[at] = (uint8_t)word;
  g_ram[at + 1] = (uint8_t)(word >> 8);
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu;
  if (bank == 0x7e || (bank == 0 && at < 0x2000)) return g_ram[at];
  assert(bank < 0x20 && at >= 0x8000);
  return s_rom[bank * 0x8000 + (at & 0x7fff)];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  return cpu_read8(cpu, bank, at) | (uint16)cpu_read8(cpu, bank, at + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 value) {
  (void)cpu;
  if (bank == 0x7e || (bank == 0 && at < 0x2000)) {
    g_ram[at] = value;
    return;
  }
  assert(bank == 0);
  if (at == 0x2101) s_object_select = value;
  else if (at == 0x2121) {
    s_cgram_address = value;
    s_cgram_second = false;
  } else if (at == 0x2122) {
    if (!s_cgram_second) s_cgram_low = value;
    else s_cgram[s_cgram_address++] = (uint16_t)(s_cgram_low | (value & 127) << 8);
    s_cgram_second = !s_cgram_second;
  } else assert(false);
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 value) {
  cpu_write8(cpu, bank, at, (uint8_t)value);
  cpu_write8(cpu, bank, at + 1, (uint8_t)(value >> 8));
}

static SrResult Query(SrRunnerHandle *runner, SrPpuStateSnapshot *out) {
  assert(runner == (SrRunnerHandle *)&s_runner);
  *out = (SrPpuStateSnapshot){.struct_size=sizeof(*out),
      .lifetime_generation=s_lifetime,.object_select=s_object_select};
  return SR_RESULT_OK;
}
static SrResult Generations(SrRunnerHandle *runner, SrGenerationSnapshot *out) {
  assert(runner == (SrRunnerHandle *)&s_runner);
  *out = (SrGenerationSnapshot){.struct_size=sizeof(*out),
      .lifetime_generation=s_lifetime,.reset_generation=s_reset_generation,
      .load_generation=s_load_generation};
  return SR_RESULT_OK;
}
static SrResult Borrow(SrRunnerHandle *runner, SrMemoryRegion region, SrBorrowedU16Span *out) {
  assert(runner == (SrRunnerHandle *)&s_runner);
  assert(region == SR_MEMORY_VRAM || region == SR_MEMORY_CGRAM);
  *out = (SrBorrowedU16Span){.struct_size=sizeof(*out),
      .lifetime_generation=s_lifetime + s_stale_borrow,
      .data=region == SR_MEMORY_VRAM ? s_vram : s_cgram,
      .element_count=region == SR_MEMORY_VRAM ? SR_PPU_VRAM_WORD_COUNT : SR_PPU_CGRAM_WORD_COUNT};
  return SR_RESULT_OK;
}
static SrResult Patch(SrRunnerHandle *runner, const SrPpuVramPatchRequest *request) {
  assert(runner == (SrRunnerHandle *)&s_runner);
  assert(request->lifetime_generation == s_lifetime && request->patch_count == 4096);
  ++s_patch_count;
  if (s_reject_patch) return SR_RESULT_INVALID_ARGUMENT;
  for (unsigned i = 0; i < request->patch_count; ++i) {
    assert(request->patches[i].word_address == 0x3000 + i);
    assert(request->patches[i].expected == s_vram[0x3000 + i]);
  }
  for (unsigned i = 0; i < request->patch_count; ++i)
    s_vram[0x3000 + i] = request->patches[i].replacement;
  ++s_lifetime; /* Production host mutations invalidate borrowed PPU views. */
  return SR_RESULT_OK;
}
const SnesRunnerApi *sr_runner_get_api(uint32_t version) {
  assert(version == SR_RUNNER_ABI_VERSION);
  static const SnesRunnerApi api = {.struct_size=sizeof(api),
      .query_generations=Generations,.query_ppu_state=Query,.borrow_u16_memory=Borrow,
      .compare_exchange_ppu_vram_words=Patch};
  return &api;
}

static uint8_t AssetByte(unsigned i, unsigned salt) {
  return (uint8_t)(i * 17 + salt);
}
static void Pack(unsigned at, unsigned size, unsigned salt) {
  s_rom[at] = (uint8_t)size;
  s_rom[at + 1] = (uint8_t)(size >> 8);
  unsigned bit = (at + 2) * 8;
  for (unsigned i = 0; i < size; ++i) {
    const unsigned token = 0x100 | AssetByte(i, salt);
    for (int shift = 8; shift >= 0; --shift, ++bit)
      s_rom[bit / 8] |= (uint8_t)(((token >> shift) & 1) << (7 - bit % 8));
  }
}
static void Remember(void) {
  memcpy(s_before_ram, g_ram, sizeof(g_ram));
  memcpy(s_before_vram, s_vram, sizeof(s_vram));
  memcpy(s_before_cgram, s_cgram, sizeof(s_cgram));
}
static void Unchanged(void) {
  assert(!memcmp(s_before_ram, g_ram, sizeof(g_ram)));
  assert(!memcmp(s_before_vram, s_vram, sizeof(s_vram)));
  assert(!memcmp(s_before_cgram, s_cgram, sizeof(s_cgram)));
}
static void Fixture(void) {
  ActRaiserDeathHeimTeleportAssets_Reset();
  memset(s_rom, 0, sizeof(s_rom));
  for (unsigned i = 0; i < sizeof(g_ram); ++i) g_ram[i] = (uint8_t)(i * 7 + 3);
  Word(kActRaiserWram_MapGroup, 0x0101);
  Word(0xea, 0);
  g_ram[0xcf] = 0;
  for (unsigned i = 0; i < SR_PPU_VRAM_WORD_COUNT; ++i) s_vram[i] = (uint16_t)(i * 3 + 5);
  for (unsigned i = 0; i < SR_PPU_CGRAM_WORD_COUNT; ++i) s_cgram[i] = (uint16_t)(i * 19);
  s_cpu = (CpuState){.A=0x1234,.X=0x1320,.Y=0x08a0,.S=0x01ff,.P=0x13};
  s_lifetime = 7;
  s_reset_generation = s_load_generation = 1;
  s_object_select = 9;
  s_reject_patch = s_stale_borrow = false;
  s_patch_count = 0;
  Pack(0xc0000, 4926, 31);
  Pack(0xb9058, 8192, 71);
  for (unsigned i = 0; i < 64; ++i) {
    s_rom[0xe64f8 + i * 2] = (uint8_t)i;
    s_rom[0xe64f9 + i * 2] = 0x20;
  }
  ActRaiserDeathHeimTeleportAssets_BindRunner((SrRunnerHandle *)&s_runner);
  Remember();
}

static void RoundTrip(void) {
  Fixture();
  const CpuState before = s_cpu;
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  assert(!memcmp(&before, &s_cpu, sizeof(before)) && s_object_select == 1);
  for (unsigned i = 0; i < sizeof(g_ram); ++i)
    assert(g_ram[i] == (i >= 0x4000 && i < 0x4000 + 4926 ?
        AssetByte(i - 0x4000, 31) : s_before_ram[i]));
  for (unsigned i = 0; i < SR_PPU_VRAM_WORD_COUNT; ++i) {
    const unsigned byte = (i - 0x3000) * 2;
    const uint16_t expected = i >= 0x3000 && i < 0x4000 ?
        (uint16_t)(AssetByte(byte,71) | (unsigned)AssetByte(byte+1,71) << 8) : s_before_vram[i];
    assert(s_vram[i] == expected);
  }
  for (unsigned i = 0; i < SR_PPU_CGRAM_WORD_COUNT; ++i)
    assert(s_cgram[i] == (i >= 128 && i < 192 ? 0x2000 + i - 128 : s_before_cgram[i]));
  assert(!ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  s_lifetime += 40; /* Native ticks and presentation mutations expire views, not ownership. */
  ActRaiserDeathHeimTeleportAssets_Restore(&s_cpu);
  assert(s_patch_count == 2 && s_object_select == 9);
  assert(!memcmp(&before, &s_cpu, sizeof(before)));
  Unchanged();
  ActRaiserDeathHeimTeleportAssets_Restore(&s_cpu);
  assert(s_patch_count == 2);
}

static void FailureIsAtomic(void) {
  Fixture();
  s_stale_borrow = true;
  assert(!ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  Unchanged();
  s_stale_borrow = false;
  s_rom[0xc0000] ^= 1;
  assert(!ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  Unchanged();
  s_rom[0xc0000] ^= 1;
  s_reject_patch = true;
  assert(!ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  Unchanged();
  assert(s_object_select == 9);
}

static void PendingNativeUploads(void) {
  Fixture();
  Word(0xea, 0x2101);
  g_ram[0xec] = 0x11;
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  assert(cpu_read16(&s_cpu,0,0xea) == 0 && s_object_select == 1);
  Word(0xea, 0x2101);
  g_ram[0xec] = 1;
  Word(0xcb, 0xe4f8);
  g_ram[0xcd] = 0x1c;
  g_ram[0xce] = 0x80;
  g_ram[0xcf] = 4;
  ActRaiserDeathHeimTeleportAssets_Restore(&s_cpu);
  assert(cpu_read16(&s_cpu,0,0xea) == 0 && g_ram[0xcf] == 0 && s_object_select == 0x11);
  Fixture();
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  Word(0xea, 0x2108);
  Word(0xcb, 0x9000);
  g_ram[0xcf] = 1;
  ActRaiserDeathHeimTeleportAssets_Restore(&s_cpu);
  assert(cpu_read16(&s_cpu,0,0xea) == 0x2108 && g_ram[0xcf] == 1);
}

static void AbandonmentOwnership(void) {
  Fixture();
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  ActRaiserDeathHeimTeleportAssets_Reset();
  Unchanged();
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  ++s_lifetime;
  ++s_reset_generation;
  Remember();
  ActRaiserDeathHeimTeleportAssets_Reset();
  Unchanged();
  assert(s_patch_count == 3);
  Fixture();
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  ++s_load_generation;
  Remember();
  ActRaiserDeathHeimTeleportAssets_Reset();
  Unchanged();
  assert(s_patch_count == 1);
  Fixture();
  assert(ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
  Word(kActRaiserWram_MapGroup, 0x0107);
  Remember();
  ActRaiserDeathHeimTeleportAssets_Reset();
  Unchanged();
  assert(s_patch_count == 1);
  ActRaiserDeathHeimTeleportAssets_BindRunner(NULL);
  assert(!ActRaiserDeathHeimTeleportAssets_Prepare(&s_cpu));
}

int main(void) {
  RoundTrip();
  FailureIsAtomic();
  PendingNativeUploads();
  AbandonmentOwnership();
  puts("Death Heim teleport resource tests passed");
  return 0;
}
