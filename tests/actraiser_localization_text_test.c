#include "actraiser/actraiser_localization_text.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static uint8_t s_wram[0x20000];
static uint8_t s_bank01[0x10000];
static int s_failures;

#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++s_failures;                                                          \
  }                                                                        \
} while (0)

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  if (bank == 0 || bank == 0x7E) return s_wram[address];
  if (bank == 0x7F) return s_wram[0x10000u + address];
  if (bank == 1) return s_bank01[address];
  return 0;
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return (uint16_t)(cpu_read8(cpu, bank, address) |
                    ((uint16_t)cpu_read8(
                         cpu, bank, (uint16_t)(address + 1u)) << 8));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  if (bank == 0 || bank == 0x7E)
    s_wram[address] = value;
  else if (bank == 0x7F)
    s_wram[0x10000u + address] = value;
  else if (bank == 1)
    s_bank01[address] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8)value);
  cpu_write8(cpu, bank, (uint16_t)(address + 1u), (uint8)(value >> 8));
}

static CpuState MakeCpu(void) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0x0714;
  cpu.X = 0x4321;
  cpu.Y = 0xFA6A;
  cpu.S = 0x01E0;
  cpu.D = 0x0200;
  cpu.DB = 0x01;
  cpu.PB = 0x01;
  cpu.m_flag = 1;
  cpu.x_flag = 1;
  cpu.ram = s_wram;
  return cpu;
}

static void WriteReturnAddress(uint16_t stack, uint8_t bank,
                               uint16_t return_address) {
  s_wram[(uint16_t)(stack + 1u)] = (uint8_t)return_address;
  s_wram[(uint16_t)(stack + 2u)] = (uint8_t)(return_address >> 8);
  s_wram[(uint16_t)(stack + 3u)] = bank;
}

static void TestDialogueObservationIsReadOnly(void) {
  memset(s_wram, 0, sizeof(s_wram));
  memset(s_bank01, 0, sizeof(s_bank01));
  ActRaiserLocalizationText_ResetObservation();
  CpuState cpu = MakeCpu();
  WriteReturnAddress(cpu.S, 0x44, 0x8B66);
  cpu_write16(&cpu, 0, kActRaiserWram_GameFrame, 0x1234);
  cpu_write8(&cpu, 0, kActRaiserWram_MapGroup, 0);
  cpu_write8(&cpu, 0, kActRaiserWram_CurrentMap,
             kActRaiserNonActionMap_SkyPalace);
  const CpuState before = cpu;

  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  CHECK(memcmp(&before, &cpu, sizeof(cpu)) == 0);
  ActRaiserLocalizationTextObservation observation = {
    .struct_size = sizeof(observation),
  };
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.abi_version ==
        ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION);
  CHECK(observation.serial != 0);
  CHECK(observation.source_pc24 == 0x01FA6Au);
  CHECK(observation.cursor_pc24 == 0x01FA6Au);
  CHECK(observation.caller_pc24 == 0x018B67u);
  CHECK(observation.context_pc24 == 0);
  CHECK(observation.selector_x == 0x4321);
  CHECK(observation.game_frame == 0x1234);
  CHECK(observation.direct_page == 0x0200);
  CHECK(observation.map_number == kActRaiserNonActionMap_SkyPalace);

  s_bank01[0xFA6A] = 0x02;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  cpu.Y = 0xFA6B;
  s_bank01[0xFA6B] = 'A';
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.cursor_pc24 == 0x01FA6Bu);
  CHECK(observation.page_index == 1);
  CHECK(observation.page_unit_index == 1);
  CHECK(!observation.terminal);

  s_bank01[0xFA6B] = 0x00;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.page_unit_index == 2);
  CHECK(observation.terminal);
  CHECK(!ActRaiserLocalizationText_TerminalWasReplaced(&observation));

  /* The terminal byte starts the visible native acknowledgement wait. The
   * completed page remains current until subsequent UI composition proves it
   * was replaced. */
  cpu.PB = 0x02;
  cpu.DB = 0x01;
  cpu.Y = 0xF272;
  cpu.A = 0x0714;
  WriteReturnAddress(cpu.S, 0x01, 0xF100);
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&cpu));
  CHECK(ActRaiserLocalizationText_TerminalWasReplaced(&observation));
  observation.terminal = false;
  CHECK(!ActRaiserLocalizationText_TerminalWasReplaced(&observation));
  observation.abi_version = 0;
  observation.terminal = true;
  CHECK(!ActRaiserLocalizationText_TerminalWasReplaced(&observation));
}

static void TestDialogueWrapperContext(void) {
  ActRaiserLocalizationText_ResetObservation();
  memset(s_wram, 0, sizeof(s_wram));
  CpuState cpu = MakeCpu();

  /* Long-call wrappers preserve the outer caller bank from the JSL frame. */
  WriteReturnAddress(cpu.S, 0x03, 0xE0BD);
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper0(&cpu));
  WriteReturnAddress(cpu.S, 0, 0x932F);
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  ActRaiserLocalizationTextObservation observation = {
    .struct_size = sizeof(observation),
  };
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.caller_pc24 == 0x019330u);
  CHECK(observation.context_pc24 == 0x03E0BEu);

  /* Context is one-shot and a mismatched text caller discards it safely. */
  WriteReturnAddress(cpu.S, 0x03, 0xF000);
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper0(&cpu));
  WriteReturnAddress(cpu.S, 0, 0x8A00);
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.context_pc24 == 0);
  WriteReturnAddress(cpu.S, 0, 0x932F);
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.context_pc24 == 0);

  /* Bank-local JSR wrappers derive their context from PB. */
  WriteReturnAddress(cpu.S, 0, 0x85C0);
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper5(&cpu));
  WriteReturnAddress(cpu.S, 0, 0x93B1);
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.caller_pc24 == 0x0193B2u);
  CHECK(observation.context_pc24 == 0x0185C1u);
}

static void TestComposerRingAndDropSignal(void) {
  ActRaiserLocalizationText_ResetObservation();
  memset(s_wram, 0, sizeof(s_wram));
  CpuState cpu = MakeCpu();
  cpu.PB = 0x02;
  cpu.DB = 0x01;
  cpu.Y = 0xF272;
  cpu.A = 0x0714;
  WriteReturnAddress(cpu.S, 0x01, 0xF100);
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&cpu));

  ActRaiserLocalizationComposeObservation event[4];
  size_t count = 0;
  bool dropped = true;
  CHECK(ActRaiserLocalizationText_CopyComposeObservations(
      0, event, 4, &count, &dropped));
  CHECK(count == 1);
  CHECK(!dropped);
  CHECK(event[0].source_pc24 == 0x01F272u);
  CHECK(event[0].caller_pc24 == 0x01F101u);
  CHECK(event[0].destination == 0x0714);

  const uint64_t first_serial = event[0].serial;
  for (unsigned i = 0; i < 260; ++i) {
    cpu.Y = (uint16_t)(0xF300u + i);
    CHECK(!ActRaiser_LocalizationObserveTextCompose(&cpu));
  }
  count = 0;
  dropped = false;
  CHECK(ActRaiserLocalizationText_CopyComposeObservations(
      first_serial, event, 4, &count, &dropped));
  CHECK(dropped);
  CHECK(count == 4);
  CHECK(event[0].serial > first_serial);
  CHECK(event[1].serial == event[0].serial + 1u);
}

static void TestRejectionsAndReset(void) {
  ActRaiserLocalizationText_ResetObservation();
  ActRaiserLocalizationTextObservation observation = {0};
  CHECK(!ActRaiserLocalizationText_CopyObservation(&observation));
  observation.struct_size = sizeof(observation);
  CHECK(!ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(!ActRaiser_LocalizationObserveTextEntry(NULL));
  CHECK(!ActRaiser_LocalizationObserveTextByte(NULL));
  CHECK(!ActRaiser_LocalizationObserveTextCompose(NULL));
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper0(NULL));
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper6(NULL));
  CHECK(ActRaiser_LocalizationTextObserverTrap(NULL) == RECOMP_RETURN_NORMAL);
}

int main(void) {
  /* The composer serial deliberately remains monotonic across observation
   * resets. Exercise the zero-origin ring contract before the dialogue test
   * creates the replacement composer used by its terminal-wait assertion. */
  TestComposerRingAndDropSignal();
  TestDialogueObservationIsReadOnly();
  TestDialogueWrapperContext();
  TestRejectionsAndReset();
  if (s_failures) return 1;
  puts("localization text observation checks passed");
  return 0;
}
