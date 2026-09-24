#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser_game.h"
#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
static unsigned failures, builds, uploads;
static RecompReturn native_result = RECOMP_RETURN_NORMAL;
static CpuState native_input;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", \
    __FILE__, __LINE__, #x); ++failures; } } while (0)
extern bool ActRaiser_SimSpriteBuildEntry(CpuState *);
extern bool ActRaiser_SpriteUploadEntry(CpuState *);
extern RecompReturn ActRaiser_SimSpriteBuild(CpuState *);
extern RecompReturn ActRaiser_SpriteUpload(CpuState *);

static void Expect(uint8_t group, uint8_t map, ActRaiserSpriteRole role,
                   unsigned first, unsigned count) {
  const ActRaiserSpriteOwnership owner = ActRaiserSpriteOwnership_Presented(group, map);
  uint8_t actual_first = 255, actual_count = 255;
  CHECK(ActRaiserSpriteOwnership_Range(&owner, role, &actual_first, &actual_count) == (count != 0));
  CHECK(actual_first == first && actual_count == count);
}

RecompReturn bank_01_ACD9_M0X0(CpuState *cpu) {
  ++builds;
  CHECK(!ActRaiser_SimSpriteBuildEntry(cpu));
  CHECK(!memcmp(cpu, &native_input, sizeof(*cpu)));
  ActRaiserSpriteOwnership_RecordSim(0x083e, 0x25, 24, 40);
  cpu->S += 3;
  return native_result;
}
RecompReturn bank_01_ACD9_M1X0(CpuState *cpu) {
  return bank_01_ACD9_M0X0(cpu);
}
RecompReturn bank_02_ACA3_M1X0(CpuState *cpu) {
  ++uploads;
  CHECK(!ActRaiser_SpriteUploadEntry(cpu));
  CHECK(!memcmp(cpu, &native_input, sizeof(*cpu)));
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0); /* Not published early. */
  cpu->S += 2;
  return native_result;
}
static void TestNativeWrappers(void) {
  for (unsigned m = 0; m < 2; ++m) {
    ActRaiserSpriteOwnership_Reset();
    g_ram[0x18] = 0; g_ram[0x19] = 7;
    CpuState cpu = {.PB = 1, .DB = 0x7f, .m_flag = m, .S = 0x1e0};
    native_input = cpu;
    CHECK(ActRaiser_SimSpriteBuildEntry(&cpu));
    CHECK(ActRaiser_SimSpriteBuild(&cpu) == RECOMP_RETURN_NORMAL);
    CHECK(cpu.S == 0x1e3 && builds == m + 1);
    Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
    cpu = (CpuState){.PB = 2, .m_flag = 1, .S = 0x1e0};
    native_input = cpu;
    CHECK(ActRaiser_SpriteUploadEntry(&cpu));
    CHECK(ActRaiser_SpriteUpload(&cpu) == RECOMP_RETURN_NORMAL);
    CHECK(cpu.S == 0x1e2 && uploads == m + 1);
    Expect(0, 7, kActRaiserSprite_HudIcon, 6, 4);
  }
  CHECK(!ActRaiser_SpriteUploadEntry(NULL));
  CpuState cpu = {.PB = 2, .DB = 0x7f, .m_flag = 1};
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.DB = 0; cpu.D = 0x100;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.D = 0; cpu.x_flag = 1;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.x_flag = 0; cpu.emulation = 1;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.emulation = 0; cpu.m_flag = 0;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.PB = 1; cpu.emulation = 1;
  CHECK(!ActRaiser_SimSpriteBuildEntry(&cpu));
  cpu.emulation = 0; cpu.S = 0x1e0; native_input = cpu;
  native_result = RECOMP_RETURN_TAILCALL;
  CHECK(ActRaiser_SimSpriteBuild(&cpu) == native_result);
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
  native_result = RECOMP_RETURN_NORMAL;
}

static void TestPublication(void) {
  uint8_t shadow[kActRaiserSpriteShadowBytes];
  memset(shadow, 0x5a, sizeof(shadow)); /* No recognizable sprite art. */
  ActRaiserSpriteOwnership_Reset();
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x083e, 2, 44, 60);
  ActRaiserSpriteOwnership_Complete(shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 11, 4);
  /* Pause: repeated upload retains ownership. Beginning another frame alone
   * cannot change what is already visible. */
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x083e, 4, 0, 12);
  Expect(0, 1, kActRaiserSprite_HudIcon, 11, 4);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 3);
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
  shadow[543] ^= 1; /* Unobserved writer, including high OAM. */
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0);
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x0a00, 2, 0, 16);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0); /* World record is not HUD. */
  ActRaiserSpriteOwnership_Begin(0, 9, 3);
  ActRaiserSpriteOwnership_RecordSim(0x06a0, 0x33, 0, 28);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 9, shadow);
  ActRaiserSpriteOwnership_Begin(0, 9, 4);
  ActRaiserSpriteOwnership owner = ActRaiserSpriteOwnership_Presented(0, 9);
  CHECK(owner.valid && owner.location == 3);
  ActRaiserSpriteOwnership_Complete(shadow); /* All-hidden animation. */
  ActRaiserSpriteOwnership_Upload(0, 9, shadow);
  owner = ActRaiserSpriteOwnership_Presented(0, 9);
  CHECK(owner.valid && owner.location == 4);
  Expect(0, 9, kActRaiserSprite_WorldLabel, 0, 0);
}

static void TestArtIndependentRoles(void) {
  uint8_t shadow[kActRaiserSpriteShadowBytes] = {0};
  const uint16_t eyes[] = {0xf3fa,0xf408,0xf430,0xf458,0xf486,0xf494,0xf4b2};
  for (unsigned parts = 1; parts <= 6; ++parts) {
    ActRaiserSpriteOwnership_Begin(7, 1, 0);
    for (unsigned i = 0; i < 7; ++i)
      ActRaiserSpriteOwnership_RecordAction(eyes[i], (5+i*parts)*4, (5+(i+1)*parts)*4);
    ActRaiserSpriteOwnership_Complete(shadow);
    ActRaiserSpriteOwnership_Upload(7, 1, shadow);
    Expect(7, 1, kActRaiserSprite_StatueEyes, 5, 7*parts);
    Expect(7, 2, kActRaiserSprite_StatueEyes, 0, 0);
  }
  for (unsigned spell = 0; spell < 4; ++spell) {
    ActRaiserSpriteOwnership_Begin(0, 7, 0);
    ActRaiserSpriteOwnership_RecordSim(0x081a, 0x25+spell, 0, 4); /* Menu copy. */
    ActRaiserSpriteOwnership_RecordSim(0x083e, 0x25+spell, 4, spell ? 8 : 20);
    ActRaiserSpriteOwnership_Complete(shadow);
    ActRaiserSpriteOwnership_Upload(0, 7, shadow);
    Expect(0, 7, kActRaiserSprite_HudIcon, 1, spell ? 1 : 4);
  }
  ActRaiserSpriteOwnership_Begin(1, 1, 0);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 0, 4);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 8, 12);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(1, 1, shadow);
  Expect(1, 1, kActRaiserSprite_HudIcon, 0, 0); /* Never capture a gap. */
  ActRaiserSpriteOwnership_Begin(1, 1, 0);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 0, 516);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(1, 1, shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(1, 1).valid);
  ActRaiserSpriteOwnership_Reset();
}
int main(void) {
  TestNativeWrappers(); TestPublication(); TestArtIndependentRoles();
  return failures ? 1 : 0;
}
