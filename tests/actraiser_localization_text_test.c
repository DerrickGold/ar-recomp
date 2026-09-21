#include "actraiser/actraiser_localization_text.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static uint8_t s_wram[0x20000];
static uint8_t s_bank01[0x10000];
static int s_failures;
static unsigned s_bank01_reads;
void ActRaiserCredits_ObserveClear(void) {}

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
  if (bank == 1) {
    ++s_bank01_reads;
    return s_bank01[address];
  }
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
  CHECK(observation.entry_compose_serial != 0);

  s_bank01[0xFA6A] = 0x02;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.awaiting_page_advance);
  const uint16_t saved_x = cpu.X;
  cpu.X = 0x0610;
  CHECK(!ActRaiser_LocalizationObserveContinuation(&cpu));
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.continuation_cell_valid);
  CHECK(observation.continuation_cell == 0x0308);
  cpu.X = saved_x;
  cpu.Y = 0xFA6B;
  s_bank01[0xFA6B] = 'A';
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.cursor_pc24 == 0x01FA6Bu);
  CHECK(observation.page_index == 1);
  CHECK(observation.window_start_page == 1);
  CHECK(observation.page_unit_index == 1);
  CHECK(!observation.awaiting_page_advance);
  CHECK(!observation.continuation_cell_valid);
  CHECK(!observation.terminal);

  /* Nonzero native text-state keeps rows across the same $02 command. */
  s_wram[0x0200] = 1;
  s_bank01[0xFA6B] = 0x02;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  s_bank01[0xFA6B] = 'B';
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.page_index == 2);
  CHECK(observation.window_start_page == 1);
  CHECK(observation.page_unit_index == 1);
  /* An explicit reset discards earlier continuations, not the session. */
  s_bank01[0xFA6B] = 0x05;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.window_start_page == 2);

  s_bank01[0xFA6B] = 0x01;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.yielded_to_menu);
  CHECK(!observation.terminal && !observation.awaiting_page_advance);
  CHECK(observation.control_pending);
  const uint16_t completed = observation.completed_control_count;
  ActRaiserLocalizationText_ObserveReturn();
  ActRaiserLocalizationText_ObserveReturn(); /* Idempotent, not another yield. */
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(!observation.control_pending);
  CHECK(observation.completed_control_count == completed + 1u);

  s_bank01[0xFA6B] = 0x00;
  CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
  observation.struct_size = sizeof(observation);
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(observation.page_unit_index == 4);
  CHECK(!observation.yielded_to_menu);
  CHECK(observation.terminal);
  CHECK(observation.terminal_compose_serial ==
        observation.entry_compose_serial);

  CHECK(!ActRaiser_LocalizationObserveMenuClear(&cpu));
  CHECK(!ActRaiserLocalizationText_CopyObservation(&observation));
  const CpuState before_clear = cpu;
  CHECK(!ActRaiser_LocalizationObserveGeneralClear(&cpu));
  CHECK(memcmp(&before_clear, &cpu, sizeof(cpu)) == 0);
}

static void TestControlAcknowledgementsAndClearAnchors(void) {
  ActRaiserLocalizationText_ResetObservation();
  memset(s_wram, 0, sizeof(s_wram));
  CpuState cpu = MakeCpu();
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  const struct {
    uint8_t code;
    uint16_t completed, clear_count, page;
    bool pending;
  } steps[] = {
      {0x05, 0, 1, 0, true},  /* Exposed reset, not yet completed. */
      {'A',  1, 1, 0, false},
      {0x03, 1, 1, 0, true},  /* Native fixed delay. */
      {0x04, 2, 1, 0, true},  /* Delay returned; toggle now pending. */
      {0x02, 3, 1, 0, false}, /* Continuation isn't a locked anchor. */
      {'B',  3, 0, 1, false}, /* Clear-style page discards reset identity. */
      {0x05, 3, 4, 1, true},  /* Reset can identify an intra-page boundary. */
      {'C',  4, 4, 1, false},
      {0x01, 4, 4, 1, true},  /* Yield exposed; no invented completion. */
  };
  for (size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); ++index) {
    s_bank01[cpu.Y] = steps[index].code;
    const CpuState before = cpu;
    uint8_t before_wram[sizeof(s_wram)];
    memcpy(before_wram, s_wram, sizeof(before_wram));
    CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
    CHECK(!memcmp(&before, &cpu, sizeof(cpu)));
    CHECK(!memcmp(before_wram, s_wram, sizeof(s_wram)));
    for (int repeat = 0; repeat < 3; ++repeat) {
      ActRaiserLocalizationTextObservation observation = {.struct_size = sizeof(observation)};
      CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
      CHECK(observation.completed_control_count == steps[index].completed);
      CHECK(observation.control_pending == steps[index].pending);
      CHECK(observation.window_start_control_count == steps[index].clear_count);
      CHECK(observation.page_index == steps[index].page);
    }
    ++cpu.Y;
  }
  CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
  ActRaiserLocalizationTextObservation observation = {.struct_size = sizeof(observation)};
  CHECK(ActRaiserLocalizationText_CopyObservation(&observation));
  CHECK(!observation.completed_control_count && !observation.control_pending &&
        !observation.window_start_control_count);
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
  CHECK(event[0].source_table_pc24 == 0);

  /* The indexed source resolver is a read-only seam before `$01:8C79`
   * destroys the logical selector while looking up a pointer. */
  cpu.DB = 1;
  cpu.D = 0x0200;
  cpu.A = 13;
  cpu_write16(&cpu, 0, 0x0208, 0xF08C);
  const CpuState before = cpu;
  CHECK(!ActRaiser_LocalizationObserveIndexedComposeSource(&cpu));
  CHECK(memcmp(&before, &cpu, sizeof(cpu)) == 0);
  cpu.Y = 0xF158;
  cpu.A = 0x0A12;
  WriteReturnAddress(cpu.S, 0x01, 0x8C92);
  CHECK(!ActRaiser_LocalizationObserveTextCompose(&cpu));
  count = 0;
  CHECK(ActRaiserLocalizationText_CopyComposeObservations(
      event[0].serial, event, 4, &count, &dropped));
  CHECK(count == 1);
  CHECK(event[0].source_table_pc24 == 0x01F08Eu);
  CHECK(event[0].source_selector == 12);

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
  CHECK(!ActRaiser_LocalizationObserveIndexedComposeSource(NULL));
  CHECK(!ActRaiser_LocalizationObserveTextCompose(NULL));
  CHECK(!ActRaiser_LocalizationObserveTextErase(NULL));
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper0(NULL));
  CHECK(!ActRaiser_LocalizationObserveDialogueWrapper6(NULL));
  CHECK(ActRaiser_LocalizationTextObserverTrap(NULL) == RECOMP_RETURN_NORMAL);
}

static void TestEraseFootprints(void) {
  const uint16_t destinations[] = {
      0x0B17, 0x0A12, 0x0C12, 0x0512, 0x0603, 0x1310,
      0x001F, 0x1F1F, 0x2000, 0xFF00,
  };
  const uint8_t records[][16] = {
      {0}, {'A', 'B', 'C', 0},
      {'A', 0x0D, 0x0D, 'B', 'C', 0},
      {0x09, 0x80, 0x0B, 0x20, 0x0D, 0x0D, 'X', 0},
      {'1', '2', '3', '4', '5', '6', 0x0D, '7', 0},
  };
  for (size_t d = 0; d < sizeof(destinations) / sizeof(destinations[0]); ++d) {
    for (size_t r = 0; r < sizeof(records) / sizeof(records[0]); ++r) {
      ActRaiserLocalizationText_ResetObservation();
      CpuState cpu = MakeCpu();
      cpu.x_flag = 0;
      cpu.A = destinations[d];
      cpu.Y = 0xFFFF;  /* The native 16-bit source cursor can wrap banks. */
      for (size_t i = 0; i < sizeof(records[r]); ++i)
        s_bank01[(uint16_t)(cpu.Y + i)] = records[r][i];
      CHECK(!ActRaiser_LocalizationObserveTextCompose(&cpu));
      CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
      ActRaiserLocalizationTextObservation text = {.struct_size = sizeof(text)};
      CHECK(ActRaiserLocalizationText_CopyObservation(&text));
      const uint64_t after_serial = text.entry_compose_serial;
      const CpuState before = cpu;
      static uint8_t wram_before[sizeof(s_wram)];
      memcpy(wram_before, s_wram, sizeof(s_wram));
      CHECK(!ActRaiser_LocalizationObserveTextErase(&cpu));
      CHECK(memcmp(&before, &cpu, sizeof(cpu)) == 0);
      CHECK(memcmp(wram_before, s_wram, sizeof(s_wram)) == 0);

      /* Independent byte-store oracle for $C1E9/$C1ED. $09/$80/$0B
       * have no special formatting meaning to the native eraser. */
      bool expected[32 * 32] = {0}, actual[32 * 32] = {0};
      uint16_t line = (uint16_t)((cpu.A >> 8) * 64u + (cpu.A & 255u) * 2u);
      uint16_t x = line;
      for (size_t i = 0; records[r][i]; ++i) {
        if (records[r][i] == 0x0D) {
          line += 64;
          x = line;
        } else {
          const uint16_t writes[] = {(uint16_t)(0xB000u + x),
                                     (uint16_t)(0xAFC0u + x)};
          for (size_t w = 0; w < 2; ++w)
            if (writes[w] >= 0xB000 && writes[w] < 0xB800)
              expected[(writes[w] - 0xB000) / 2u] = true;
          x += 2;
        }
      }
      ActRaiserLocalizationComposeObservation events[64];
      size_t count = 0;
      bool dropped = false;
      CHECK(ActRaiserLocalizationText_CopyComposeObservations(
          after_serial, events, 64, &count, &dropped));
      CHECK(!dropped);
      bool dialogue_erased = false;
      for (size_t i = 0; i < count; ++i) {
        const ActRaiserLocalizationComposeObservation *event = &events[i];
        CHECK(event->serial > after_serial);
        CHECK(event->clear_first_column + event->clear_column_count <= 32);
        CHECK(event->clear_first_row + event->clear_row_count <= 32);
        for (unsigned row = event->clear_first_row;
             row < event->clear_first_row + event->clear_row_count; ++row)
          for (unsigned col = event->clear_first_column;
               col < event->clear_first_column + event->clear_column_count; ++col)
            actual[row * 32 + col] = true;
        dialogue_erased |= event->clears_dialogue;
      }
      CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
      bool expected_dialogue_erased = false;
      for (unsigned row = 19; row < 25; ++row)
        for (unsigned col = 5; col < 29; ++col)
          expected_dialogue_erased |= expected[row * 32 + col];
      CHECK(dialogue_erased == expected_dialogue_erased);
      CHECK(ActRaiserLocalizationText_CopyObservation(&text) == !dialogue_erased);
    }
  }
  /* Malformed records cannot turn a read-only observer into an MMIO read or
   * an unbounded scan. Their native execution is still left untouched. */
  CpuState cpu = MakeCpu();
  cpu.A = 0;
  s_bank01_reads = 0;
  CHECK(!ActRaiser_LocalizationObserveTextErase(&cpu));
  CHECK(s_bank01_reads == 0);  /* Unsupported 8-bit indices: no inferred erase. */
  cpu.x_flag = 0;
  cpu.Y = 0x2100;
  s_bank01_reads = 0;
  CHECK(!ActRaiser_LocalizationObserveTextErase(&cpu));
  CHECK(s_bank01_reads == 0);
  cpu.Y = 0x9000;
  memset(s_bank01 + cpu.Y, 'X', 4096);
  s_bank01_reads = 0;
  CHECK(!ActRaiser_LocalizationObserveTextErase(&cpu));
  CHECK(s_bank01_reads == 2048);
}

static void TestDictionaryReturnControls(void) {
  for (uint8_t code = 0; code <= 5; ++code) {
    ActRaiserLocalizationText_ResetObservation();
    CpuState cpu = MakeCpu();
    const uint16_t first = cpu.Y;
    s_bank01[first] = 0x80;
    CHECK(!ActRaiser_LocalizationObserveTextEntry(&cpu));
    CHECK(!ActRaiser_LocalizationObserveTextByte(&cpu));
    /* Two dictionary words followed by a returned control. No intervening
     * function entry occurs in the real $8FFF -> $8FC5 loop. */
    cpu.Y += 3;
    cpu.A = (cpu.A & 0xff00u) | code;
    const CpuState before = cpu;
    ActRaiserLocalizationText_ObserveDecodedByte(&cpu, first);
    ActRaiserLocalizationText_ObserveDecodedByte(&cpu, first);
    CHECK(!memcmp(&cpu, &before, sizeof(cpu)));
    ActRaiserLocalizationTextObservation observed = {.struct_size = sizeof(observed)};
    CHECK(ActRaiserLocalizationText_CopyObservation(&observed));
    CHECK(observed.page_unit_index == 3);
    CHECK(observed.cursor_pc24 == (0x010000u | (uint16_t)(first + 2)));
    CHECK(observed.completed_control_count == 0);
    CHECK(observed.terminal == (code == 0));
    CHECK(observed.yielded_to_menu == (code == 1));
    CHECK(observed.awaiting_page_advance == (code == 2));
    CHECK(observed.control_pending == (code == 1 || code >= 3));
  }
}

int main(void) {
  TestDictionaryReturnControls();
  TestControlAcknowledgementsAndClearAnchors();
  /* The composer serial deliberately remains monotonic across observation
   * resets. Exercise the zero-origin ring contract before the dialogue test
   * creates the replacement composer used by its terminal-wait assertion. */
  TestComposerRingAndDropSignal();
  TestDialogueObservationIsReadOnly();
  TestDialogueWrapperContext();
  TestEraseFootprints();
  TestRejectionsAndReset();
  if (s_failures) return 1;
  puts("localization text observation checks passed");
  return 0;
}
