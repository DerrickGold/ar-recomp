#include "actraiser/actraiser_action_video_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kBankBytes = 0x10000,
  kScriptBank = 0x04,
  kScriptAddress = 0x8000,
  kVideoProfileAddress = 0x893E,
  kProfile = 0x03,
  kProfileBytes = 28,
  kEntryStack = 0x01E0,
};

static uint8_t wram[0x20000];
static uint8_t script_bank[kBankBytes];
static uint8_t bank02[kBankBytes];
static uint8_t ppu[0x40];
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  if (bank == 0x00 && address < 0x2000) return wram[address];
  if (bank == 0x7E) return wram[address];
  if (bank == 0x7F) return wram[0x10000u + address];
  if (bank == kScriptBank) return script_bank[address];
  if (bank == 0x02) return bank02[address];
  return 0;
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  const uint8 low = cpu_read8(cpu, bank, address);
  const uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1u));
  return (uint16)(low | ((uint16)high << 8));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  if (bank == 0x02 && address >= 0x2100 && address < 0x2140) {
    ppu[address - 0x2100] = value;
    return;
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

static uint8_t *Profile(void) {
  return bank02 + kVideoProfileAddress + kProfile * kProfileBytes;
}

static void ResetFixture(void) {
  memset(wram, 0xCC, sizeof(wram));
  memset(script_bank, 0, sizeof(script_bank));
  memset(bank02, 0, sizeof(bank02));
  memset(ppu, 0xCC, sizeof(ppu));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  wram[kActRaiserWram_CurrentMap] = 1;
  WriteWram16(0x00A2, kScriptAddress);
  wram[0x00A4] = kScriptBank;
  script_bank[kScriptAddress] = kProfile;

  static const uint8_t kFixture[kProfileBytes] = {
    0x11, 0x22, 0x33, 0x44, 0x0F, 0x0E, 0x01,
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
    0x81, 0x82, 0x83, 0x84, 0x55, 0xA0,
    0x01, 0x03, 0x04, 0x05, 0xB4, 0x06,
    0x34, 0x12, 0x01,
  };
  memcpy(Profile(), kFixture, sizeof(kFixture));
}

static void TestVideoProfile(void) {
  ResetFixture();
  CpuState cpu = MakeCpu();
  CHECK(ActRaiser_ActionVideoConfigHleEnabled(&cpu));
  CHECK(ActRaiser_ApplyActionVideoConfig(&cpu) == RECOMP_RETURN_NORMAL);

  CHECK(ppu[0x2C] == 0x11);
  CHECK(ppu[0x2E] == 0x11);
  CHECK(ppu[0x2D] == 0x22);
  CHECK(ppu[0x2F] == 0x22);
  CHECK(ppu[0x30] == 0x33);
  CHECK(ppu[0x31] == 0x44);
  CHECK(ppu[0x32] == 0xE0);
  CHECK(ppu[0x07] == 0x62);
  CHECK(ppu[0x08] == 0x73);
  CHECK(ppu[0x05] == 0x01);

  CHECK(ReadWram16(0x006A) == 0x3000);
  CHECK(ReadWram16(0x006E) == 0x2100);
  CHECK(ReadWram16(0x0072) == 0x2000);
  CHECK(ReadWram16(0x008F) == 0x3000);
  for (unsigned plane = 0; plane < 6; plane++) {
    CHECK(wram[0x003A + plane * 2u] == (uint8_t)(plane * 2u + 1u));
    CHECK(wram[0x003B + plane * 2u] == (uint8_t)(plane * 2u + 2u));
  }
  CHECK(wram[0x00BB] == 0x81);
  CHECK(wram[0x00BA] == 0x82);
  CHECK(wram[0x00B9] == 0x83);
  CHECK(wram[0x00BF] == 0x84);
  CHECK(wram[0x00C1] == 0xFF);
  CHECK(wram[0x00C4] == 0xA0);
  CHECK(wram[0x00C2] == 1);
  CHECK(wram[0x00C3] == 0);
  CHECK(wram[0x00C0] == 0);
  CHECK(wram[0x00C5] == 1);
  CHECK(wram[0x00C9] == 0x30);
  CHECK(wram[0x00C6] == 4);
  CHECK(wram[0x00CA] == 0x50);
  CHECK(wram[0x00C7] == 0);
  CHECK(wram[0x00C8] == 0);
  CHECK(ReadWram16(0x00DA) == 0x1000);
  CHECK(ReadWram16(0x00E1) == 0x0180);
  CHECK(wram[0x00DF] == 3);
  CHECK(wram[0x00DE] == 5);
  CHECK(ReadWram16(0x00E6) == 0x1234);
  CHECK(wram[0x00E5] == 0x3B);
  CHECK(wram[0x00E8] == 0);
  CHECK(ReadWram16(0x00F2) == 1);

  CHECK(cpu.A == 1);
  CHECK(cpu.X == kProfile * kProfileBytes);
  CHECK(cpu.Y == 1);
  CHECK(cpu.S == kEntryStack + k65816RtsStackBytes);
  CHECK(cpu.D == 0);
  CHECK(cpu.DB == 0);
  CHECK(cpu.PB == 0x02);
  CHECK(cpu.host_return_valid == 1);
  CHECK(cpu.P == (CPU_P_I | CPU_P_D | CPU_P_M | CPU_P_Z));
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
  CHECK(wram[kEntryStack] == 0);
  CHECK(ReadWram16(kEntryStack - 4) == 1);
  CHECK(ReadWram16(kEntryStack - 2) == 1);
}

static void TestPriorityClearAndC1Retention(void) {
  ResetFixture();
  Profile()[4] = 0;
  Profile()[18] = 0x40;
  Profile()[17] = 0x5A;
  wram[0x006A] = 0xAA;
  wram[0x006E] = 0xBB;
  wram[0x0072] = 0xCC;
  wram[0x008F] = 0xDD;
  CpuState cpu = MakeCpu();
  CHECK(ActRaiser_ApplyActionVideoConfig(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(wram[0x006A] == 0xAA && wram[0x006B] == 0x10);
  CHECK(wram[0x006E] == 0xBB && wram[0x006F] == 0x01);
  CHECK(wram[0x0072] == 0xCC && wram[0x0073] == 0x00);
  CHECK(wram[0x008F] == 0xDD && wram[0x0090] == 0x20);
  CHECK(wram[0x00C1] == 0x5A);
}

static void TestGuardedFallbacks(void) {
  ResetFixture();
  CpuState cpu = MakeCpu();
  for (unsigned profile = 0; profile <= 0xFF; profile++) {
    script_bank[kScriptAddress] = (uint8_t)profile;
    const bool expected = profile >= 0x03 && profile <= 0x2E &&
        profile != 0x08;
    CHECK(ActRaiser_ActionVideoConfigHleEnabled(&cpu) == expected);
  }
  script_bank[kScriptAddress] = kProfile;
  cpu.DB = 0x7E;
  CHECK(!ActRaiser_ActionVideoConfigHleEnabled(&cpu));
  cpu.DB = 0;
  cpu.D = 1;
  CHECK(!ActRaiser_ActionVideoConfigHleEnabled(&cpu));
  cpu.D = 0;
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  CHECK(!ActRaiser_ActionVideoConfigHleEnabled(&cpu));
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  CHECK(setenv("AR_ACTION_ROOM_VIDEO_HLE", "0", 1) == 0);
  CHECK(!ActRaiser_ActionVideoConfigHleEnabled(&cpu));
  CHECK(unsetenv("AR_ACTION_ROOM_VIDEO_HLE") == 0);
}

int main(void) {
  TestVideoProfile();
  TestPriorityClearAndC1Retention();
  TestGuardedFallbacks();
  if (failures) {
    printf("actraiser action video-config HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser action video-config HLE: all checks passed\n");
  return 0;
}
