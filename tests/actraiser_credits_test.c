#include "actraiser/actraiser_credits.h"
#include "actraiser/actraiser_bg3_upload.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static int failures, uploads, expected_presented;
static uint8_t ram[0x20000];
static CpuState upload_input, upload_output;
static RecompReturn upload_result;

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  CHECK(bank == 0);
  return ram[address];
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) |
      (uint16)cpu_read8(cpu, bank, (uint16)(address + 1)) << 8;
}

/* Model the generated entry's re-entry predicate and native side effects.
 * Assert identity at the DMA boundary, before the wrapper returns to NMI. */
RecompReturn bank_02_AEEB_M1X0(CpuState *cpu) {
  ++uploads;
  CHECK(!ActRaiser_Bg3UploadEntry(cpu));
  CHECK(!memcmp(cpu, &upload_input, sizeof(*cpu)));
  CHECK(ActRaiserCredits_PresentedPage() == expected_presented);
  ram[0xf1] = 0;
  *cpu = upload_output;
  return upload_result;
}

static CpuState Cpu(void) {
  return (CpuState){.A = 0xa500, .X = 0x1234, .Y = 0x5678, .S = 0x1f0,
      .PB = 2, .DB = 0, .P = 0x24, .m_flag = 1, .host_return_valid = 1};
}

static void Select(unsigned page) {
  CpuState cpu = Cpu();
  cpu.A |= page;
  const CpuState before = cpu;
  CHECK(!ActRaiser_CreditsObserveSelection(&cpu));
  CHECK(!memcmp(&before, &cpu, sizeof(cpu)));
}

static void Wait(unsigned page, uint16_t return_address) {
  CpuState cpu = Cpu();
  cpu.A = 0x0800; /* Brightness is not the page number. */
  ram[cpu.S + 1] = return_address;
  ram[cpu.S + 2] = return_address >> 8;
  ram[cpu.S + 3] = page;
  const CpuState before = cpu;
  ActRaiserCredits_ObserveWait(&cpu);
  CHECK(!memcmp(&before, &cpu, sizeof(cpu)));
}

static void Upload(bool lower_rows) {
  CpuState cpu = Cpu();
  CHECK(ActRaiser_Bg3UploadEntry(&cpu));
  upload_input = cpu;
  upload_output = (CpuState){.A = 0xa501, .X = 0x05c0, .Y = 0x6789,
      .S = 0x1f2, .PB = 2, .DB = 0, .P = 0x21, .m_flag = 1, ._flag_C = 1};
  expected_presented = ActRaiserCredits_PresentedPage();
  ram[0xf1] = lower_rows ? 3 : 0; /* A dirty counter, not a page index. */
  const int before = uploads;
  CHECK(ActRaiser_Bg3Upload(&cpu) == upload_result);
  CHECK(uploads == before + 1);
  CHECK(!memcmp(&cpu, &upload_output, sizeof(cpu)));
  CHECK(!ram[0xf1]);
}

static void EnterScene(void) {
  ActRaiserBg3Upload_Reset();
  ram[kActRaiserWram_MapGroup] = 8;
  ram[kActRaiserWram_CurrentMap] = 1;
  upload_result = RECOMP_RETURN_NORMAL;
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
}

static void TestPageLifecycle(void) {
  EnterScene();
  Upload(true); /* No producer history must never infer page zero. */
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  for (unsigned page = 0; page < kActRaiserCreditsPageCount; ++page) {
    const int previous = ActRaiserCredits_PresentedPage();
    Select(page);
    for (unsigned fade = 0; fade < 16; ++fade) {
      Wait(page, 0xab3a); /* Outgoing fade: old page stays visible. */
      Upload(false);
      CHECK(ActRaiserCredits_PresentedPage() == previous);
    }
    ActRaiserCredits_ObserveClear();
    CHECK(ActRaiserCredits_PresentedPage() == previous);
    Wait(page, 0xab67); /* Copy completed, still no upload. */
    CHECK(ActRaiserCredits_PresentedPage() == previous);
    Upload(false); /* Top four rows alone do not publish the staged page. */
    CHECK(ActRaiserCredits_PresentedPage() == previous);
    Upload(true);
    CHECK(ActRaiserCredits_PresentedPage() == (int)page);
    for (unsigned fade = 0; fade < 16; ++fade) {
      Wait(page, 0xab67);
      Upload(false);
      CHECK(ActRaiserCredits_PresentedPage() == (int)page);
    }
    /* Hold, absent uploads, and presentation toggles need no new event. */
    CHECK(ActRaiserCredits_PresentedPage() == (int)page);
  }
  ActRaiserCredits_ObserveClear();
  Upload(false);
  CHECK(ActRaiserCredits_PresentedPage() == 19);
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  Wait(19, 0xab67); /* Later fade wait cannot undo the uploaded clear. */
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
}

static void TestMissingOrInvalidEvidence(void) {
  EnterScene();
  Select(2);
  Wait(3, 0xab67); /* Wrong saved page. */
  Wait(2, 0xab6a); /* Second fade wait is not the copy-completion seam. */
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  Wait(2, 0xab67);
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == 2);
  ActRaiserCredits_ObserveClear();
  Select(20);
  Wait(20, 0xab67);
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);

  CpuState cpu = Cpu();
  cpu.DB = 0x7f; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  cpu = Cpu(); cpu.D = 0x100; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  cpu = Cpu(); cpu.x_flag = 1; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  cpu = Cpu(); cpu.m_flag = 0; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  cpu = Cpu(); cpu.PB = 1; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  cpu = Cpu(); cpu.emulation = 1; CHECK(!ActRaiser_Bg3UploadEntry(&cpu));
  CHECK(!ActRaiser_Bg3UploadEntry(NULL));
  CHECK(!ActRaiser_CreditsObserveSelection(NULL));
  ActRaiserCredits_ObserveWait(NULL);
}

static void TestSceneResetAndNativeReturn(void) {
  EnterScene(); Select(1); Wait(1, 0xab67); Upload(true);
  ActRaiserCredits_ObserveScene(8, 1);
  CHECK(ActRaiserCredits_PresentedPage() == 1);
  ActRaiserCredits_ObserveScene(8, 2);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  ActRaiserCredits_ObserveScene(8, 1);
  Wait(1, 0xab67); Upload(true); /* Re-entry needs a fresh selection. */
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  Select(4); Wait(4, 0xab67); Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == 4);
  ActRaiserBg3Upload_Reset();
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  Select(5); Wait(5, 0xab67);
  upload_result = RECOMP_RETURN_OWNED_UNWIND;
  Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  upload_result = RECOMP_RETURN_NORMAL;
  Upload(true); /* Re-entry guard was released even on a nonlocal return. */
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
  Select(5); Wait(5, 0xab67); Upload(true);
  CHECK(ActRaiserCredits_PresentedPage() == 5);
  ram[kActRaiserWram_MapGroup] = 0;
  CpuState cpu = Cpu();
  CHECK(ActRaiser_Bg3UploadEntry(&cpu)); /* Shared SIM upload resets credits. */
  expected_presented = kActRaiserCreditsNoPage;
  upload_input = cpu;
  ram[0xf1] = 0;
  CHECK(ActRaiser_Bg3Upload(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(ActRaiserCredits_PresentedPage() == kActRaiserCreditsNoPage);
}

int main(void) {
  TestPageLifecycle();
  TestMissingOrInvalidEvidence();
  TestSceneResetAndNativeReturn();
  return failures ? 1 : 0;
}
