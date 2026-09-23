#include "actraiser/actraiser_save_transaction.h"
#include "save_system.h"

#include <stdio.h>
#include <string.h>

static int failures, calls, history_checks;
void ActRaiserRegional_CheckLairHistory(CpuState *cpu) {
  ++history_checks;
  /* Audit sees the original writer's completed registers, before capture. */
  if (cpu->A!=0x5721 || cpu->DB!=0x42 || cpu->S!=0x1e03) ++failures;
}
static uint8_t image[kActRaiserSramSize], old[kActRaiserSramSize];
static RecompReturn outcome;
static const char *path = "actraiser-save-seam-test.srm";
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

/* This stub models native dispatch/return ownership, not native copy logic.
 * Real-ROM replay covers the latter. In particular the wrapper must delegate
 * once, preserve both entry widths and not invent another return frame. */
static RecompReturn Native(CpuState *cpu, bool m) {
  CHECK(cpu->m_flag == m);
  CHECK(!ActRaiser_SaveStoryEntry(cpu));
  ++calls;
  SaveError error = {{0}};
  uint8_t disk[kActRaiserSramSize];
  image[500] ^= 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, path, disk, &error));
  CHECK(!memcmp(disk, old, sizeof(disk)));
  Save_RecomputeChecksum(image);
  cpu->S += 3;
  cpu->A = 0x5721;
  cpu->DB = 0x42;
  return outcome;
}

RecompReturn bank_03_A656_M0X0(CpuState *cpu) { return Native(cpu, false); }
RecompReturn bank_03_A656_M1X0(CpuState *cpu) { return Native(cpu, true); }

int main(void) {
  CHECK(!ActRaiser_SaveStoryEntry(NULL));
  SaveError error = {{0}};
  for (int width = 0; width < 2; ++width) {
    for (int aborted = 0; aborted < 2; ++aborted) {
      memset(image, 0, sizeof(image));
      Save_RecomputeChecksum(image);
      memcpy(old, image, sizeof(old));
      CHECK(SaveSystem_Attach(image, sizeof(image), kSaveBackend_NativeSrm,
                             path, "unused.ini", &error));
      CHECK(SaveSystem_WriteActive(&error));
      CpuState cpu = {0};
      cpu.PB = 3; cpu.m_flag = width; cpu.S = 0x1e00;
      CHECK(ActRaiser_SaveStoryEntry(&cpu));
      cpu.x_flag = 1;
      CHECK(!ActRaiser_SaveStoryEntry(&cpu));
      cpu.x_flag = 0;
      outcome = aborted ? RECOMP_RETURN_PARKED_WAIT : RECOMP_RETURN_NORMAL;
      int before = calls;
      int checked_before = history_checks;
      CHECK(ActRaiser_SaveStory(&cpu) == outcome);
      CHECK(calls == before + 1);
      CHECK(history_checks == checked_before + !aborted);
      CHECK(cpu.A == 0x5721 && cpu.DB == 0x42 && cpu.S == 0x1e03);
      CHECK(SaveSystem_AutoPersistIfChanged(&error) == !aborted);
      uint8_t disk[kActRaiserSramSize];
      CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, path, disk, &error));
      CHECK(!memcmp(disk, aborted ? old : image, sizeof(disk)));
    }
  }
  remove(path);
  return failures ? 1 : 0;
}
