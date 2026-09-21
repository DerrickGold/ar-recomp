#include "actraiser/actraiser_hud.h"
#include "actraiser/actraiser_bg3_upload.h"
#include "actraiser/actraiser_credits.h"
#include "actraiser_game.h"

#include <stdio.h>
#include <string.h>

static unsigned failures, uploads;
static uint8_t ram[0x20000];
static CpuState upload_input;
static RecompReturn upload_result = RECOMP_RETURN_NORMAL;
static ActRaiserHudOwner before_upload;
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; CHECK(bank == 0); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) |
      (uint16)cpu_read8(cpu, bank, address + 1) << 8;
}
static ActRaiserHudOwner Presented(void) {
  return ActRaiserHud_Presented(ram[0x18], ram[0x19]);
}
static void Expect(ActRaiserHudKind kind, bool enemy) {
  const ActRaiserHudOwner owner = Presented();
  CHECK(owner.kind == kind && owner.enemy == enemy);
}
RecompReturn bank_02_AEEB_M1X0(CpuState *cpu) {
  ++uploads;
  CHECK(!ActRaiser_Bg3UploadEntry(cpu));
  CHECK(!memcmp(cpu, &upload_input, sizeof(*cpu)));
  Expect(before_upload.kind, before_upload.enemy);
  ram[0xf1] = 0;
  cpu->S += 2;
  return upload_result;
}
static void Upload(bool lower_rows) {
  CpuState cpu = {.PB = 2, .m_flag = 1, .S = 0x1e0};
  CHECK(ActRaiser_Bg3UploadEntry(&cpu));
  upload_input = cpu;
  before_upload = Presented();
  ram[0xf1] = lower_rows ? 5 : 0;
  const unsigned count = uploads;
  CHECK(ActRaiser_Bg3Upload(&cpu) == upload_result);
  CHECK(uploads == count + 1 && cpu.S == 0x1e2 && !ram[0xf1]);
}
static void Template(uint8_t group, uint8_t map) {
  ram[0x18] = group; ram[0x19] = map;
  CpuState cpu = {.PB = 2, .m_flag = 1, .S = 0x1e0};
  const CpuState before = cpu;
  ActRaiserHud_ObserveTemplate(&cpu);
  CHECK(!memcmp(&cpu, &before, sizeof(cpu)));
}
static void Enemy(bool active) {
  CpuState cpu = {.S = 0x1e0, .X = 0x900};
  const CpuState before = cpu;
  CHECK(!(active ? ActRaiser_HudObserveEnemy(&cpu)
                 : ActRaiser_HudObserveEnemyClear(&cpu)));
  CHECK(!memcmp(&cpu, &before, sizeof(cpu)));
}
static void TestScenes(void) {
  for (unsigned group = 0; group <= 8; ++group) {
    const unsigned maps = group ? 1 : 9;
    for (unsigned map = 0; map <= maps; ++map) {
      ActRaiserBg3Upload_Reset();
      Template(group, map);
      Expect(kActRaiserHud_None, false); /* No upload yet. */
      Upload(false);
      const ActRaiserHudKind kind = group == 8 ? kActRaiserHud_None : group
          ? kActRaiserHud_Action : map >= 1 && map <= 8
          ? kActRaiserHud_Simulation : kActRaiserHud_None;
      Expect(kind, false);
    }
  }
  ActRaiserBg3Upload_Reset();
  Template(0, 8); Upload(true); Expect(kActRaiserHud_Simulation, false);
  for (unsigned pause = 0; pause < 60; ++pause) {
    ActRaiserHud_ObserveScene(0, 8); Upload(false);
    Expect(kActRaiserHud_Simulation, false);
  }
  /* Stale SIM words on the world map cannot carry ownership back to town. */
  CHECK(ActRaiserHud_Presented(0, 9).kind == kActRaiserHud_None);
  ActRaiserHud_ObserveScene(0, 9);
  ram[0x19] = 1; Upload(false); Expect(kActRaiserHud_None, false);
  Template(0, 1); Upload(true); Expect(kActRaiserHud_Simulation, false);
}
static void TestEnemyAndClears(void) {
  ActRaiserBg3Upload_Reset();
  Template(7, 8); Upload(true); Expect(kActRaiserHud_Action, false);
  Enemy(true); Expect(kActRaiserHud_Action, false);
  Upload(false); Expect(kActRaiserHud_Action, true);
  ram[0xee] = 0; Upload(false); Expect(kActRaiserHud_Action, true);
  Enemy(false); Expect(kActRaiserHud_Action, true);
  Upload(false); Expect(kActRaiserHud_Action, false);
  Enemy(true); Upload(false); Expect(kActRaiserHud_Action, true);
  Enemy(true); Upload(false); Expect(kActRaiserHud_Action, true); /* Refill */
  ram[0x1a] = 9; ram[0x1b] = 0; /* Pending world transition: outgoing fade. */
  ActRaiserHud_ObserveScene(7, 8); Upload(false);
  Expect(kActRaiserHud_Action, true);
  ActRaiserHud_ObserveClear(); Expect(kActRaiserHud_Action, true);
  Upload(false); Expect(kActRaiserHud_None, false);
  Enemy(true); Upload(false); Expect(kActRaiserHud_None, false);
  Template(1, 4); Upload(true); Expect(kActRaiserHud_Action, false);
  upload_result = RECOMP_RETURN_OWNED_UNWIND;
  Upload(false); Expect(kActRaiserHud_None, false);
  upload_result = RECOMP_RETURN_NORMAL;
  Upload(false); Expect(kActRaiserHud_None, false);
  Template(0, 8); Upload(true); Expect(kActRaiserHud_Simulation, false);
  Enemy(true); Upload(false); Expect(kActRaiserHud_Simulation, false);
}
int main(void) {
  TestScenes();
  TestEnemyAndClears();
  return failures ? 1 : 0;
}
