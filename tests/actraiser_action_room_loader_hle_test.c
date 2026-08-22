#include "actraiser/actraiser_action_room_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kBankBytes = 0x10000,
  kScriptBank = 0x04,
  kMetatileSourceBank = 0x05,
  kMapSourceBank = 0x06,
  kScriptAddress = 0x8000,
  kAssetAddress = 0x9000,
  kEntryStack = 0x01E0,
};

static uint8_t wram[0x20000];
static uint8_t script_bank[kBankBytes];
static uint8_t metatile_source_bank[kBankBytes];
static uint8_t map_source_bank[kBankBytes];
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
  if (bank == kMetatileSourceBank) return metatile_source_bank;
  if (bank == kMapSourceBank) return map_source_bank;
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
  cpu.DB = 0x7E;
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
  memset(metatile_source_bank, 0, sizeof(metatile_source_bank));
  memset(map_source_bank, 0, sizeof(map_source_bank));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  WriteWram16(0x00A2, kScriptAddress);
  wram[0x00A4] = kScriptBank;
  wram[0x00AA] = 0x7E;
  WriteWram16(0x0046, kActRaiserWram_Bg1Map);
  WriteWram16(0x004A, kActRaiserWram_Bg2Map);
}

static void CheckEntryContract(const CpuState *cpu, uint8_t saved_p,
                               uint16_t expected_y, uint16_t expected_x) {
  CHECK(cpu->P == saved_p);
  CHECK(cpu->m_flag == 1);
  CHECK(cpu->x_flag == 0);
  CHECK(cpu->S == kEntryStack + k65816RtsStackBytes);
  CHECK(cpu->Y == expected_y);
  CHECK(cpu->X == expected_x);
  CHECK(cpu->A == 0);
  CHECK(cpu->D == 0);
  CHECK(cpu->DB == 0x7E);
  CHECK(cpu->PB == 0x02);
  CHECK(cpu->host_return_valid == 1);
  CHECK(wram[kEntryStack] == saved_p);
}

static void TestMetatileLoad(uint8_t selector) {
  ResetFixture();
  BuildAlternatingAsset(metatile_source_bank, kAssetAddress, 0x0800,
                        0x12, 0x34);
  script_bank[kScriptAddress + 0] = 0x00;
  script_bank[kScriptAddress + 1] = 0x20;
  script_bank[kScriptAddress + 2] = 0x00;
  script_bank[kScriptAddress + 3] = selector;
  WriteLinearOperand(&script_bank[kScriptAddress + 4],
                     kMetatileSourceBank, kAssetAddress);

  CpuState cpu = MakeCpu();
  const uint8_t saved_p = cpu.P;
  CHECK(ActRaiser_ActionMetatileLoadHleEnabled(&cpu));
  CHECK(ActRaiser_LoadActionMetatiles(&cpu) == RECOMP_RETURN_NORMAL);
  const uint16_t destination = selector == 1
      ? kActRaiserWram_Bg1MetatileDefinitions
      : kActRaiserWram_Bg2MetatileDefinitions;
  for (unsigned i = 0; i < 0x0800; i += 2) {
    CHECK(wram[destination + i] == 0x34);
    CHECK(wram[destination + i + 1] == 0x12);
  }
  CHECK(ReadWram16(0x0000) == 0);
  CHECK(ReadWram16(0x0002) == 0x0800);
  CHECK(ReadWram16(0x0004) == 0);
  CHECK(ReadWram16(0x0006) == 0);
  CHECK(ReadWram16(0x00B3) == 0x0800);
  CHECK(ReadWram16(0x00B5) == 0x6000);
  CheckEntryContract(&cpu, saved_p, 7, 0x3100);
}

static void TestMapLoad(uint8_t selector) {
  ResetFixture();
  map_source_bank[kAssetAddress + 0] = 2;
  map_source_bank[kAssetAddress + 1] = 1;
  BuildAlternatingAsset(map_source_bank, kAssetAddress + 2, 0x0200,
                        0x56, 0x78);
  script_bank[kScriptAddress] = selector;
  WriteLinearOperand(&script_bank[kScriptAddress + 1],
                     kMapSourceBank, kAssetAddress);

  CpuState cpu = MakeCpu();
  const uint8_t saved_p = cpu.P;
  CHECK(ActRaiser_ActionMapLoadHleEnabled(&cpu));
  CHECK(ActRaiser_LoadActionMap(&cpu) == RECOMP_RETURN_NORMAL);
  const uint16_t destination = selector == 1
      ? kActRaiserWram_Bg1Map : kActRaiserWram_Bg2Map;
  for (unsigned i = 0; i < 0x0200; i++)
    CHECK(wram[destination + i] == (i & 1u ? 0x78 : 0x56));
  const uint16_t dimension = selector == 1
      ? kActRaiserWram_Bg1Width : kActRaiserWram_Bg2Width;
  CHECK(ReadWram16(dimension) == 0x0200);
  CHECK(ReadWram16((uint16_t)(dimension + 2u)) == 0x0100);
  CHECK(ReadWram16(0x0000) == 0x0200);
  CHECK(ReadWram16(0x0002) == 0x0100);
  CHECK(ReadWram16(0x0004) == selector);
  CHECK(ReadWram16(0x00B3) == 0x0200);
  CHECK(ReadWram16(0x00B5) == destination);
  CheckEntryContract(&cpu, saved_p, 4, selector == 1 ? 0 : 4);
}

static void TestGuardedFallbacks(void) {
  ResetFixture();
  script_bank[kScriptAddress + 0] = 0;
  script_bank[kScriptAddress + 1] = 0x20;
  script_bank[kScriptAddress + 2] = 0;
  script_bank[kScriptAddress + 3] = 1;
  CpuState cpu = MakeCpu();
  CHECK(ActRaiser_ActionMetatileLoadHleEnabled(&cpu));
  script_bank[kScriptAddress + 3] = 4;
  CHECK(!ActRaiser_ActionMetatileLoadHleEnabled(&cpu));
  script_bank[kScriptAddress + 3] = 1;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  CHECK(!ActRaiser_ActionMetatileLoadHleEnabled(&cpu));

  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  script_bank[kScriptAddress] = 1;
  CHECK(ActRaiser_ActionMapLoadHleEnabled(&cpu));
  script_bank[kScriptAddress] = 0x81;
  CHECK(!ActRaiser_ActionMapLoadHleEnabled(&cpu));
  script_bank[kScriptAddress] = 0;
  CHECK(!ActRaiser_ActionMapLoadHleEnabled(&cpu));

  CHECK(setenv("AR_ACTION_ROOM_LOAD_HLE", "0", 1) == 0);
  script_bank[kScriptAddress] = 1;
  CHECK(!ActRaiser_ActionMapLoadHleEnabled(&cpu));
  CHECK(unsetenv("AR_ACTION_ROOM_LOAD_HLE") == 0);
}

int main(void) {
  TestMetatileLoad(1);
  TestMetatileLoad(2);
  TestMapLoad(1);
  TestMapLoad(2);
  TestGuardedFallbacks();
  if (failures) {
    printf("actraiser action-room loader HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser action-room loader HLE: all checks passed\n");
  return 0;
}
