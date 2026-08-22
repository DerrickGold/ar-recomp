#include "actraiser/actraiser_action_room_graphics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kBankBytes = 0x10000,
  kScriptBank = 0x04,
  kCharacterSourceBank = 0x05,
  kPaletteSourceBank = 0x06,
  kScriptAddress = 0x8000,
  kCharacterAddress = 0x9000,
  kPaletteAddress = 0xA000,
  kEntryStack = 0x01E0,
};

static uint8_t wram[0x20000];
static uint8_t script_bank[kBankBytes];
static uint8_t character_source_bank[kBankBytes];
static uint8_t palette_source_bank[kBankBytes];
static uint8_t vram[0x10000];
static uint8_t cgram[0x0200];
static uint16_t vram_word_address;
static uint8_t cgram_color_address;
static unsigned cgram_byte_phase;
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static uint8_t *BankMemory(uint8_t bank) {
  if (bank == kScriptBank) return script_bank;
  if (bank == kCharacterSourceBank) return character_source_bank;
  if (bank == kPaletteSourceBank) return palette_source_bank;
  return NULL;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  if (bank == 0x00 && address < 0x2000) return wram[address];
  if (bank == 0x7E) return wram[address];
  if (bank == 0x7F) return wram[0x10000u + address];
  const uint8_t *memory = BankMemory(bank);
  return memory ? memory[address] : 0;
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  const uint8 low = cpu_read8(cpu, bank, address);
  const uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1u));
  return (uint16)(low | ((uint16)high << 8));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  if (bank == 0x00) {
    switch (address) {
      case 0x2116:
        vram_word_address = (uint16_t)(
            (vram_word_address & 0xFF00u) | value);
        return;
      case 0x2117:
        vram_word_address = (uint16_t)(
            (vram_word_address & 0x00FFu) | ((uint16_t)value << 8));
        return;
      case 0x2118:
        vram[(uint16_t)(vram_word_address * 2u)] = value;
        return;
      case 0x2119:
        vram[(uint16_t)(vram_word_address * 2u + 1u)] = value;
        vram_word_address++;
        return;
      case 0x2121:
        cgram_color_address = value;
        cgram_byte_phase = 0;
        return;
      case 0x2122: {
        const uint16_t offset = (uint16_t)(
            (uint16_t)cgram_color_address * 2u + cgram_byte_phase);
        cgram[offset & 0x01FFu] = value;
        cgram_byte_phase ^= 1u;
        if (!cgram_byte_phase) cgram_color_address++;
        return;
      }
      default:
        break;
    }
  }
  if (bank == 0x00 && address < 0x2000)
    wram[address] = value;
  else if (bank == 0x7E)
    wram[address] = value;
  else if (bank == 0x7F)
    wram[0x10000u + address] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8)value);
  cpu_write8(cpu, bank, (uint16)(address + 1u), (uint8)(value >> 8));
}

static void WriteWram16(uint16_t address, uint16_t value) {
  wram[address] = (uint8_t)value;
  wram[(uint16_t)(address + 1u)] = (uint8_t)(value >> 8);
}

static uint16_t ReadWram16(uint16_t address) {
  return (uint16_t)(wram[address] |
                    ((uint16_t)wram[(uint16_t)(address + 1u)] << 8));
}

static uint32_t LinearAddress(uint8_t bank, uint16_t address) {
  return (uint32_t)bank * 0x8000u + (address & 0x7FFFu);
}

static void WriteLinearOperand(uint8_t *destination, uint8_t bank,
                               uint16_t address) {
  const uint32_t linear = LinearAddress(bank, address);
  destination[0] = (uint8_t)linear;
  destination[1] = (uint8_t)(linear >> 8);
  destination[2] = (uint8_t)(linear >> 16);
}

static void PutBits(uint8_t *bytes, size_t *bit, unsigned value,
                    unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const unsigned shift = count - 1u - i;
    if ((value >> shift) & 1u)
      bytes[*bit >> 3] |= (uint8_t)(1u << (7u - (*bit & 7u)));
    (*bit)++;
  }
}

static void PutLiteral(uint8_t *bytes, size_t *bit, uint8_t value) {
  PutBits(bytes, bit, 1, 1);
  PutBits(bytes, bit, value, 8);
}

static void PutMatch(uint8_t *bytes, size_t *bit, uint8_t source,
                     unsigned length) {
  PutBits(bytes, bit, 0, 1);
  PutBits(bytes, bit, source, 8);
  PutBits(bytes, bit, length - 2u, 4);
}

static void BuildAlternatingAsset(uint8_t *bank, uint16_t address,
                                  size_t output_size, uint8_t first,
                                  uint8_t second) {
  memset(bank + address, 0, 0x1000);
  bank[address] = (uint8_t)output_size;
  bank[(uint16_t)(address + 1u)] = (uint8_t)(output_size >> 8);
  uint8_t *stream = bank + address + 2u;
  size_t bit = 0;
  PutLiteral(stream, &bit, first);
  PutLiteral(stream, &bit, second);
  size_t produced = 2;
  uint8_t write_position = 0xF1;
  while (produced < output_size) {
    unsigned length = (unsigned)(output_size - produced);
    if (length > 17) length = 17;
    PutMatch(stream, &bit, (uint8_t)(write_position - 2u), length);
    write_position = (uint8_t)(write_position + length);
    produced += length;
  }
}

static CpuState MakeCpu(void) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0xA55A;
  cpu.X = 0x3456;
  cpu.Y = 0;
  cpu.S = kEntryStack;
  cpu.D = 0;
  cpu.DB = 0;
  cpu.PB = 0x02;
  cpu.m_flag = 1;
  cpu.x_flag = 0;
  cpu._flag_C = 1;
  cpu._flag_I = 1;
  cpu._flag_D = 1;
  cpu._flag_V = 1;
  cpu._flag_N = 1;
  cpu.ram = wram;
  cpu_mirrors_to_p(&cpu);
  return cpu;
}

static void ResetFixture(void) {
  memset(wram, 0xCC, sizeof(wram));
  memset(script_bank, 0, sizeof(script_bank));
  memset(character_source_bank, 0, sizeof(character_source_bank));
  memset(palette_source_bank, 0, sizeof(palette_source_bank));
  memset(vram, 0xCC, sizeof(vram));
  memset(cgram, 0xCC, sizeof(cgram));
  vram_word_address = 0;
  cgram_color_address = 0;
  cgram_byte_phase = 0;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  WriteWram16(0x00A2, kScriptAddress);
  wram[0x00A4] = kScriptBank;
}

static void CheckEntryContract(const CpuState *cpu, uint8_t saved_p,
                               uint16_t expected_x, uint16_t expected_a) {
  CHECK(cpu->P == saved_p);
  CHECK(cpu->m_flag == 1);
  CHECK(cpu->x_flag == 0);
  CHECK(cpu->S == kEntryStack + k65816RtsStackBytes);
  CHECK(cpu->Y == 6);
  CHECK(cpu->X == expected_x);
  CHECK(cpu->A == expected_a);
  CHECK(cpu->D == 0);
  CHECK(cpu->DB == 0);
  CHECK(cpu->PB == 0x02);
  CHECK(cpu->host_return_valid == 1);
  CHECK(wram[kEntryStack] == saved_p);
}

static void TestCharacterLoad(uint8_t end, uint8_t destination,
                              size_t output_size) {
  ResetFixture();
  BuildAlternatingAsset(character_source_bank, kCharacterAddress,
                        output_size, 0x12, 0x34);
  script_bank[kScriptAddress + 0] = 0x00;
  script_bank[kScriptAddress + 1] = end;
  script_bank[kScriptAddress + 2] = destination;
  WriteLinearOperand(&script_bank[kScriptAddress + 3],
                     kCharacterSourceBank, kCharacterAddress);

  CpuState cpu = MakeCpu();
  const uint8_t saved_p = cpu.P;
  CHECK(ActRaiser_ActionCharacterLoadHleEnabled(&cpu));
  CHECK(ActRaiser_LoadActionCharacters(&cpu) == RECOMP_RETURN_NORMAL);
  const size_t vram_destination = (size_t)destination << 9;
  for (size_t i = 0; i < output_size; i++) {
    const uint8_t expected = i & 1u ? 0x34 : 0x12;
    CHECK(vram[vram_destination + i] == expected);
    CHECK(wram[0x6000u + i] == expected);
  }
  CHECK(ReadWram16(0x0000) == 0);
  CHECK(ReadWram16(0x0002) == output_size);
  CHECK(ReadWram16(0x00B3) == output_size);
  CHECK(ReadWram16(0x00B5) == 0x6000);
  CHECK(wram[kEntryStack - 2] == destination);
  CHECK(wram[kEntryStack - 1] == 0);
  CHECK(wram[kEntryStack - 3] == 0x02);
  CHECK(wram[kEntryStack - 4] == 0xB2);
  CHECK(wram[kEntryStack - 5] == 0xFE);
  CheckEntryContract(&cpu, saved_p, (uint16_t)output_size, 0x3412);
}

static void TestPaletteLoad(void) {
  ResetFixture();
  for (unsigned i = 0; i < 0x80; i++)
    palette_source_bank[kPaletteAddress + i] = (uint8_t)(i ^ 0x5Au);
  script_bank[kScriptAddress + 0] = 0x00;
  script_bank[kScriptAddress + 1] = 0x40;
  script_bank[kScriptAddress + 2] = 0x40;
  WriteLinearOperand(&script_bank[kScriptAddress + 3],
                     kPaletteSourceBank, kPaletteAddress);

  CpuState cpu = MakeCpu();
  const uint8_t saved_p = cpu.P;
  CHECK(ActRaiser_ActionPaletteLoadHleEnabled(&cpu));
  CHECK(ActRaiser_LoadActionPalette(&cpu) == RECOMP_RETURN_NORMAL);
  for (unsigned i = 0; i < 0x80; i++)
    CHECK(cgram[0x80u + i] == (uint8_t)(i ^ 0x5Au));
  CHECK(ReadWram16(0x0000) == 0);
  CHECK(ReadWram16(0x0002) == 0x80);
  CHECK(ReadWram16(0x00A5) == kPaletteAddress);
  CHECK(wram[0x00A7] == kPaletteSourceBank);
  CHECK(wram[kEntryStack - 2] == 6);
  CHECK(wram[kEntryStack - 1] == 0);
  CheckEntryContract(&cpu, saved_p, 0x00A5, (0x7Fu ^ 0x5Au));
}

static void TestGuardedFallbacks(void) {
  ResetFixture();
  BuildAlternatingAsset(character_source_bank, kCharacterAddress,
                        0x2000, 0x12, 0x34);
  script_bank[kScriptAddress + 0] = 0;
  script_bank[kScriptAddress + 1] = 0x10;
  script_bank[kScriptAddress + 2] = 0x10;
  WriteLinearOperand(&script_bank[kScriptAddress + 3],
                     kCharacterSourceBank, kCharacterAddress);
  CpuState cpu = MakeCpu();
  CHECK(ActRaiser_ActionCharacterLoadHleEnabled(&cpu));
  script_bank[kScriptAddress + 2] = 0x20;
  CHECK(!ActRaiser_ActionCharacterLoadHleEnabled(&cpu));
  script_bank[kScriptAddress + 2] = 0x10;
  character_source_bank[kCharacterAddress + 1] = 0;
  CHECK(!ActRaiser_ActionCharacterLoadHleEnabled(&cpu));
  character_source_bank[kCharacterAddress] = 0x00;
  character_source_bank[kCharacterAddress + 1] = 0x20;
  cpu.DB = 0x7E;
  CHECK(!ActRaiser_ActionCharacterLoadHleEnabled(&cpu));
  cpu.DB = 0;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  CHECK(!ActRaiser_ActionCharacterLoadHleEnabled(&cpu));

  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  script_bank[kScriptAddress + 0] = 0;
  script_bank[kScriptAddress + 1] = 0x40;
  script_bank[kScriptAddress + 2] = 0x80;
  CHECK(ActRaiser_ActionPaletteLoadHleEnabled(&cpu));
  script_bank[kScriptAddress + 2] = 0x20;
  CHECK(!ActRaiser_ActionPaletteLoadHleEnabled(&cpu));

  CHECK(setenv("AR_ACTION_ROOM_GFX_HLE", "0", 1) == 0);
  script_bank[kScriptAddress + 2] = 0x80;
  CHECK(!ActRaiser_ActionPaletteLoadHleEnabled(&cpu));
  CHECK(unsetenv("AR_ACTION_ROOM_GFX_HLE") == 0);
}

int main(void) {
  TestCharacterLoad(0x10, 0x10, 0x2000);
  TestCharacterLoad(0x08, 0x50, 0x1000);
  TestPaletteLoad();
  TestGuardedFallbacks();
  if (failures) {
    printf("actraiser action-room graphics HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser action-room graphics HLE: all checks passed\n");
  return 0;
}
