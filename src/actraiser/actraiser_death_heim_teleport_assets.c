#include "actraiser/actraiser_death_heim_teleport_assets.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/quintet_lzss.h"
#include "actraiser_game.h"
#include "snesrecomp/runner.h"
#include <string.h>

enum {
  kAnimationAddress = 0x4000,
  kAnimationBytes = 4926,
  kCharacterAddress = 0x3000, /* VRAM words: the native OBSEL=$01 second atlas. */
  kCharacterWords = 4096,
  kTeleportObjectSelect = 0x01,
  kPaletteAddress = 128,
  kPaletteWords = 64,
  kAnimationSource = 0xc0000,
  kCharacterSource = 0xb9058,
  kPaletteSource = 0xe64f8,
  kPaletteSourceAddress = 0x8000 | (kPaletteSource & 0x7fff),
  kPaletteSourceBank = kPaletteSource >> 15,
  kPaletteRows = kPaletteWords / 16,
};

static struct {
  bool active;
  CpuState *cpu;
  SrRunnerHandle *runner;
  uint64_t reset_generation, load_generation;
  uint16_t scene;
  uint8_t object_select;
  uint8_t animation[kAnimationBytes];
  uint16_t characters[kCharacterWords];
  uint16_t palette[kPaletteWords];
} s_saved;
static uint8_t s_animation[kAnimationBytes];
static uint8_t s_characters[kCharacterWords * 2];
static uint16_t s_palette[kPaletteWords];
static SrPpuVramWordPatch s_patches[kCharacterWords];
static SrRunnerHandle *s_runner;
static const SnesRunnerApi *s_api;

void ActRaiserDeathHeimTeleportAssets_BindRunner(SrRunnerHandle *runner) {
  ActRaiserDeathHeimTeleportAssets_Reset();
  s_runner = runner;
  s_api = runner ? sr_runner_get_api(SR_RUNNER_ABI_VERSION) : NULL;
}

static bool QueryPpu(SrPpuStateSnapshot *ppu) {
  if (!s_runner || !s_api || s_api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !s_api->query_ppu_state) return false;
  *ppu = (SrPpuStateSnapshot){.struct_size = sizeof(*ppu)};
  return s_api->query_ppu_state(s_runner, ppu) == SR_RESULT_OK;
}

static bool Generations(SrGenerationSnapshot *out) {
  if (!s_runner || !s_api || !s_api->query_generations) return false;
  *out = (SrGenerationSnapshot){.struct_size = sizeof(*out)};
  return s_api->query_generations(s_runner, out) == SR_RESULT_OK;
}

typedef struct Source {
  CpuState *cpu;
  uint32_t linear;
  size_t limit;
} Source;

static uint8_t RomByte(CpuState *cpu, uint32_t linear) {
  return cpu_read8(cpu, (uint8_t)(linear >> 15),
      (uint16_t)(0x8000 | (linear & 0x7fff)));
}

static bool ReadSource(void *context, size_t offset, uint8_t *out) {
  const Source *source = context;
  if (offset >= source->limit) return false;
  *out = RomByte(source->cpu, source->linear + (uint32_t)offset);
  return true;
}

static bool Decode(CpuState *cpu, uint32_t source, uint8_t *out, size_t size) {
  const unsigned declared = RomByte(cpu, source) | (unsigned)RomByte(cpu, source + 1) << 8;
  if (declared != size) return false;
  /* All-literal encoding is the longest valid stream for this output size. */
  Source reader = {cpu, source + 2, (size * 9 + 7) / 8};
  return QuintetLzss_DecompressReader(ReadSource, &reader, out, size, NULL);
}

static bool Views(SrPpuStateSnapshot *ppu, SrBorrowedU16Span *vram,
    SrBorrowedU16Span *cgram) {
  const SnesRunnerApi *api = s_api;
  if (!api || !s_runner ||
      api->struct_size < SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE ||
      !api->borrow_u16_memory || !api->compare_exchange_ppu_vram_words ||
      !QueryPpu(ppu)) return false;
  *vram = (SrBorrowedU16Span){.struct_size = sizeof(*vram)};
  *cgram = (SrBorrowedU16Span){.struct_size = sizeof(*cgram)};
  return api->borrow_u16_memory(s_runner, SR_MEMORY_VRAM, vram) == SR_RESULT_OK &&
      api->borrow_u16_memory(s_runner, SR_MEMORY_CGRAM, cgram) == SR_RESULT_OK &&
      vram->data && vram->element_count == SR_PPU_VRAM_WORD_COUNT &&
      cgram->data && cgram->element_count == SR_PPU_CGRAM_WORD_COUNT &&
      vram->lifetime_generation == ppu->lifetime_generation &&
      cgram->lifetime_generation == ppu->lifetime_generation;
}

static bool Patch(uint64_t lifetime) {
  const SrPpuVramPatchRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_VRAM_PATCH_ADDRESSES_SORTED,
    .lifetime_generation = lifetime,
    .patches = s_patches,
    .patch_count = kCharacterWords,
  };
  return s_api->compare_exchange_ppu_vram_words(s_runner, &request) == SR_RESULT_OK;
}

static void Palette(CpuState *cpu, const uint16_t *colors) {
  cpu_write8(cpu, 0, 0x2121, kPaletteAddress);
  for (unsigned i = 0; i < kPaletteWords; ++i) {
    cpu_write8(cpu, 0, 0x2122, (uint8_t)colors[i]);
    cpu_write8(cpu, 0, 0x2122, (uint8_t)(colors[i] >> 8));
  }
}

bool ActRaiserDeathHeimTeleportAssets_Prepare(CpuState *cpu) {
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram, cgram;
  SrGenerationSnapshot generations;
  if (!cpu || s_saved.active || !Generations(&generations) || !Views(&ppu, &vram, &cgram) ||
      !Decode(cpu, kAnimationSource, s_animation, sizeof(s_animation)) ||
      !Decode(cpu, kCharacterSource, s_characters, sizeof(s_characters))) return false;
  for (unsigned i = 0; i < kPaletteWords; ++i)
    s_palette[i] = (uint16_t)(RomByte(cpu, kPaletteSource + i * 2) |
        (unsigned)RomByte(cpu, kPaletteSource + i * 2 + 1) << 8);
  for (unsigned i = 0; i < kAnimationBytes; ++i)
    s_saved.animation[i] = cpu_read8(cpu, 0x7e, (uint16_t)(kAnimationAddress + i));
  memcpy(s_saved.characters, vram.data + kCharacterAddress, sizeof(s_saved.characters));
  memcpy(s_saved.palette, cgram.data + kPaletteAddress, sizeof(s_saved.palette));
  for (unsigned i = 0; i < kCharacterWords; ++i) {
    s_patches[i] = (SrPpuVramWordPatch){
      .word_address = (uint16_t)(kCharacterAddress + i),
      .expected = s_saved.characters[i],
      .replacement = (uint16_t)(s_characters[i * 2] | (unsigned)s_characters[i * 2 + 1] << 8),
    };
  }
  if (!Patch(ppu.lifetime_generation)) return false;
  /* No yield follows the atomic atlas replacement.
   * Include the entire blob, which extends into $5000's first 830 bytes.
   * Both banks are restored before the native boss initializer resumes. */
  s_saved.cpu = cpu;
  s_saved.runner = s_runner;
  /* Borrow lifetimes expire on every tick and host VRAM mutation. These
   * copied resources instead belong to the stable reset/load generation. */
  s_saved.reset_generation = generations.reset_generation;
  s_saved.load_generation = generations.load_generation;
  s_saved.scene = cpu_read16(cpu, 0, kActRaiserWram_MapGroup);
  s_saved.object_select = ppu.object_select;
  if (cpu_read16(cpu, 0, 0x00ea) == 0x2101) {
    /* An already queued arena OBSEL value is its effective next-frame
     * state. Preserve that value and prevent it undoing the beam's $01. */
    s_saved.object_select = cpu_read8(cpu, 0, 0x00ec);
    cpu_write16(cpu, 0, 0x00ea, 0);
  }
  s_saved.active = true;
  for (unsigned i = 0; i < kAnimationBytes; ++i)
    cpu_write8(cpu, 0x7e, (uint16_t)(kAnimationAddress + i), s_animation[i]);
  Palette(cpu, s_palette);
  cpu_write8(cpu, 0, 0x2101, kTeleportObjectSelect);
  return true;
}

void ActRaiserDeathHeimTeleportAssets_Restore(CpuState *cpu) {
  if (!s_saved.active) return;
  SrPpuStateSnapshot ppu = {0};
  SrBorrowedU16Span vram, cgram;
  SrGenerationSnapshot generations = {0};
  const bool views = Views(&ppu, &vram, &cgram);
  const bool owner = Generations(&generations) &&
      generations.reset_generation == s_saved.reset_generation &&
      generations.load_generation == s_saved.load_generation;
  const uint16_t scene = cpu ? cpu_read16(cpu, 0, kActRaiserWram_MapGroup) : 0;
  if (cpu != s_saved.cpu || s_runner != s_saved.runner || !views ||
      !owner || scene != s_saved.scene)
    ActRaiserHleFatal("Death Heim teleport resource owner changed: cpu=%u runner=%u views=%u "
        "reset=%llu/%llu load=%llu/%llu scene=%04x/%04x",
        cpu == s_saved.cpu, s_runner == s_saved.runner, views,
        (unsigned long long)generations.reset_generation,
        (unsigned long long)s_saved.reset_generation,
        (unsigned long long)generations.load_generation,
        (unsigned long long)s_saved.load_generation,
        scene, s_saved.scene);
  for (unsigned i = 0; i < kCharacterWords; ++i) {
    s_patches[i] = (SrPpuVramWordPatch){
      .word_address = (uint16_t)(kCharacterAddress + i),
      .expected = vram.data[kCharacterAddress + i],
      .replacement = s_saved.characters[i],
    };
  }
  if (!Patch(ppu.lifetime_generation))
    ActRaiserHleFatal("Cannot restore Death Heim arena character atlas");
  for (unsigned i = 0; i < kAnimationBytes; ++i)
    cpu_write8(cpu, 0x7e, (uint16_t)(kAnimationAddress + i), s_saved.animation[i]);
  Palette(cpu, s_saved.palette);
  cpu_write8(cpu, 0, 0x2101, s_saved.object_select);
  /* A native exit queues these same values for the next NMI. Retire only
   * teleport-owned requests so they cannot overwrite the restored arena. */
  if (cpu_read16(cpu, 0, 0x00ea) == 0x2101 &&
      cpu_read8(cpu, 0, 0x00ec) == kTeleportObjectSelect)
    cpu_write16(cpu, 0, 0x00ea, 0);
  if (cpu_read16(cpu, 0, 0x00cb) == kPaletteSourceAddress &&
      cpu_read8(cpu, 0, 0x00cd) == kPaletteSourceBank &&
      cpu_read8(cpu, 0, 0x00ce) == kPaletteAddress &&
      cpu_read8(cpu, 0, 0x00cf) == kPaletteRows)
    cpu_write8(cpu, 0, 0x00cf, 0);
  s_saved.active = false;
  s_saved.cpu = NULL;
}

void ActRaiserDeathHeimTeleportAssets_Reset(void) {
  if (!s_saved.active) return;
  SrGenerationSnapshot generations;
  if (s_runner == s_saved.runner && Generations(&generations) &&
      generations.reset_generation == s_saved.reset_generation &&
      generations.load_generation == s_saved.load_generation &&
      cpu_read16(s_saved.cpu, 0, kActRaiserWram_MapGroup) == s_saved.scene) {
    ActRaiserDeathHeimTeleportAssets_Restore(s_saved.cpu);
  } else {
    /* The new scene/lifetime has replaced native resources already. */
    s_saved.active = false;
    s_saved.cpu = NULL;
  }
}
