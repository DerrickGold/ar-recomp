#include "actraiser/actraiser_ar2_master.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "support/ar2_master_fixture.h"
#include <stdio.h>
#include <stdlib.h>

Settings g_settings;
uint8 g_ram[kActRaiserWramSize];
static uint32_t s_tail;
static bool s_grounded = true;
static unsigned s_vram_writes, s_palette_writes;
static uint8_t s_cgram[512];
static unsigned s_palette_cursor;
void (*g_cpu_cop_hook)(CpuState *cpu);
void (*g_cpu_brk_hook)(CpuState *cpu);
static uint32_t s_sound_site;
static unsigned s_swings, s_blocks;
static uint16_t s_block_object, s_block_attacker;

void cpu_trace_block(CpuState *cpu, uint32 site) {
  (void)cpu;
  s_sound_site = site;
}

static void SwingSound(CpuState *cpu) {
  assert((cpu->A & 0xFF) == 0x01 && s_sound_site == 0x009A91);
  assert(cpu->X == kActRaiserWram_PlayerObject);
  s_swings++;
}

static void BlockSound(CpuState *cpu) {
  assert((cpu->A & 0xFF) == 0x0C && s_sound_site == 0x008B24);
  s_block_object = cpu->X;
  s_block_attacker = cpu->Y;
  s_blocks++;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu;
  return bank == 0 ? g_ram[at] : (uint8)(at ^ bank);
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  return cpu_read8(cpu, bank, at) | (uint16)cpu_read8(cpu, bank, at + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 value) {
  (void)cpu;
  assert(bank == 0);
  if (at == 0x2118 || at == 0x2119)
    s_vram_writes++;
  else if (at == 0x2121)
    s_palette_cursor = value * 2u;
  else if (at == 0x2122) {
    s_cgram[s_palette_cursor++ % sizeof(s_cgram)] = value;
    s_palette_writes++;
  } else if (at < 0x2100 || at > 0x21FF)
    g_ram[at] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 value) {
  cpu_write8(cpu, bank, at, (uint8)value);
  cpu_write8(cpu, bank, at + 1, (uint8)(value >> 8));
}
int cpu_hle_tailcall_request(uint32 target, uint32 source) {
  (void)source;
  s_tail = target;
  return 1;
}
int cpu_invoke_rts_leaf(CpuState *cpu, RecompReturn (*leaf)(CpuState *), uint32 source) {
  (void)source;
  return leaf(cpu) == RECOMP_RETURN_NORMAL;
}
void ActRaiserHleFatal(const char *message, ...) {
  fprintf(stderr, "%s\n", message);
  abort();
}
bool ActRaiser_RegionalMagicGestureEntry(CpuState *cpu) {
  (void)cpu;
  return false;
}
RecompReturn bank_00_8FE7_M0X0(CpuState *cpu) {
  cpu->_flag_C = s_grounded;
  return RECOMP_RETURN_NORMAL;
}
#define STUB(name)                                                                                 \
  RecompReturn name(CpuState *cpu) {                                                               \
    (void)cpu;                                                                                     \
    return RECOMP_RETURN_NORMAL;                                                                   \
  }
STUB(bank_00_8538_M0X0)
STUB(bank_00_88C9_M0X0)
STUB(bank_00_8A3C_M0X0)
STUB(bank_00_8A3C_M1X0)
STUB(bank_00_8970_M0X0)
STUB(bank_00_8970_M1X0)
STUB(bank_00_9CF2_M0X0)

static void Put(unsigned at, uint16 value) { ByteOrder_WriteLe16(g_ram + at, value); }
static uint16 Get(unsigned at) { return ByteOrder_ReadLe16(g_ram + at); }

/* The game prepares presentation before scanning/drawing action objects. */
static bool PrepareDraw(CpuState *cpu, ActRaiserAr2MasterDraw *draw) {
  ActRaiserAr2Master_UpdatePresentation(cpu);
  return ActRaiserAr2Master_DrawObject(cpu, cpu->X, draw);
}

static void TestSwitch(CpuState *cpu, bool grounded) {
  const unsigned player = kActRaiserWram_PlayerObject;
  s_grounded = grounded;
  Put(player + kActRaiserActionObject_Handler, 0x9832);
  Put(player + kActRaiserActionObject_WorldX, 500);
  Put(player + kActRaiserActionObject_WorldY, 300);
  Put(player + kActRaiserActionObject_BottomExtent, 24);
  Put(player + kActRaiserActionObject_FlipAttributes, 0x4000);
  Put(player + kActRaiserActionObject_HitPoints, 12);
  Put(player + kActRaiserActionObject_Flags, 0x2001);
  Put(kActRaiserWram_GameFrame, Get(kActRaiserWram_GameFrame) + 1);
  g_settings.ar2_master = true;
  assert(ActRaiser_Ar2MasterControlEntry(cpu));
  assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9D4F);
  ActRaiserAr2MasterDraw draw;
  assert(PrepareDraw(cpu, &draw) && draw.count == 1);
  ActRaiserAr2Master_UploadVideo(cpu);
  assert(s_vram_writes && s_palette_writes);

  g_settings.ar2_master = false;
  /* The old OAM still needs the old tiles until the next controlled update. */
  unsigned before = s_vram_writes;
  ActRaiserAr2Master_UploadVideo(cpu);
  assert(s_vram_writes - before == 32);
  assert(ActRaiser_Ar2MasterControlEntry(cpu));
  assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9832);
  assert(!ActRaiser_Ar2MasterControlEntry(cpu));
  assert(!ActRaiser_Ar2MasterHitPassEntry(cpu));
  assert(!ActRaiser_Ar2MasterHitTestEntry(cpu));
  assert(!PrepareDraw(cpu, &draw));
  assert(Get(player + kActRaiserActionObject_WorldX) == 500);
  assert(Get(player + kActRaiserActionObject_WorldY) == 300);
  assert(Get(player + kActRaiserActionObject_HitPoints) == 12);
  assert(Get(player + kActRaiserActionObject_FlipAttributes) == 0x4000);
  assert(Get(player + kActRaiserActionObject_Flags) & 0x2000);
  before = s_vram_writes;
  ActRaiserAr2Master_UploadVideo(cpu);
  assert(s_vram_writes - before > 32); /* The original atlas is restored. */
  before = s_vram_writes;
  ActRaiserAr2Master_UploadVideo(cpu);
  assert(s_vram_writes == before);
}

static void ExpectPose(CpuState *cpu, unsigned animation, unsigned step) {
  ActRaiserAr2MasterDraw draw;
  assert(PrepareDraw(cpu, &draw) && draw.count == 1);
  const unsigned marker = animation * 2 + step;
  assert(draw.parts[0].x[0] == (int)marker + Get(cpu->X + kActRaiserActionObject_LeftExtent) - 8);
}

static void Tick(void) { Put(kActRaiserWram_GameFrame, Get(kActRaiserWram_GameFrame) + 1); }

static void RequestFade(unsigned step) {
  Put(0x00CB, (uint16_t)(0x8180 + step * 32));
  g_ram[0x00CD] = 0x0B;
  g_ram[0x00CE] = 192;
  g_ram[0x00CF] = 1;
}

/* The native palette uploader runs later in NMI. An AR1 request must reach it. */
static void NativePaletteUpload(CpuState *cpu) {
  assert(g_ram[0x00CF] == 1);
  cpu_write8(cpu, 0, 0x2121, g_ram[0x00CE]);
  for (unsigned i = 0; i < 32; i++)
    cpu_write8(cpu, 0, 0x2122, cpu_read8(cpu, g_ram[0x00CD], Get(0x00CB) + i));
  g_ram[0x00CF] = 0;
}

static void ExpectPalette(CpuState *cpu, unsigned step, bool ar2) {
  uint16_t native[8][16], expected[16];
  for (unsigned fade = 0; fade < 8; fade++)
    for (unsigned i = 0; i < 16; i++)
      native[fade][i] = cpu_read16(cpu, 0x0B, 0x8180 + fade * 32 + i * 2);
  if (ar2)
    Ar2MasterAssets_TranslateFade(ActRaiserAr2Master_Assets()->palette, native[0], native[7],
                                  native[step], expected);
  else
    memcpy(expected, native[step], sizeof(expected));
  for (unsigned i = 0; i < 16; i++)
    assert(ByteOrder_ReadLe16(s_cgram + 192 * 2 + i * 2) == expected[i]);
}

static void TestFadeSwitches(CpuState *cpu) {
  const uint16_t handlers[] = {0x97A6, 0xA2E5};
  for (unsigned sequence = 0; sequence < 2; sequence++) {
    ++g_ram[kActRaiserWram_CurrentMap];
    Put(kActRaiserWram_PlayerHandler, handlers[sequence]);
    Put(cpu->X + kActRaiserActionObject_Composition, 0);
    g_settings.ar2_master = false;
    ActRaiserAr2Master_UpdatePresentation(cpu);
    for (unsigned tick = 0; tick < 8; tick++) {
      const unsigned step = sequence ? 7 - tick : tick;
      RequestFade(step);
      /* AR1 receives the fade while selected, including the stone step. */
      ActRaiserAr2Master_UploadVideo(cpu);
      assert(g_ram[0x00CF] == 1);
      NativePaletteUpload(cpu);
      ExpectPalette(cpu, step, false);

      Tick();
      g_settings.ar2_master = true;
      ActRaiserAr2Master_UpdatePresentation(cpu);
      ActRaiserAr2Master_UploadVideo(cpu);
      ExpectPalette(cpu, step, true); /* No pending request when we switched. */

      /* While selected, AR2 consumes only its own native fade requests. */
      const unsigned next = sequence ? (step ? step - 1 : 0) : (step < 7 ? step + 1 : 7);
      RequestFade(next);
      ActRaiserAr2Master_UploadVideo(cpu);
      assert(g_ram[0x00CF] == 0);
      ExpectPalette(cpu, next, true);
      RequestFade(step);
      g_ram[0x00CE] = 0; /* Another palette must remain native. */
      ActRaiserAr2Master_UploadVideo(cpu);
      assert(g_ram[0x00CF] == 1);
      ExpectPalette(cpu, next, true);

      /* Switching back on the same NMI as a new fade restores that step. */
      RequestFade(step);
      g_settings.ar2_master = false;
      ActRaiserAr2Master_UpdatePresentation(cpu);
      ActRaiserAr2Master_UploadVideo(cpu);
      assert(g_ram[0x00CF] == 1);
      ExpectPalette(cpu, step, false);
      NativePaletteUpload(cpu);
    }
  }
}

static void TestPresentationUpdates(CpuState *cpu) {
  ++g_ram[kActRaiserWram_CurrentMap];
  Put(kActRaiserWram_PlayerHandler, 0x9DE1);
  Put(cpu->X + kActRaiserActionObject_AnimationState, 0);
  Put(kActRaiserWram_GameFrame, 0xFFFE);
  g_settings.ar2_master = true;
  ActRaiserAr2MasterDraw first, repeated;
  assert(PrepareDraw(cpu, &first));

  /* Time and setting changes do not make a draw advance or switch the pose.
   * Both native-height and margin passes consume the same prepared snapshot. */
  Tick();
  g_settings.ar2_master = false;
  uint8_t before[kActRaiserWramSize];
  memcpy(before, g_ram, sizeof(before));
  const CpuState saved = *cpu;
  for (unsigned i = 0; i < 20; i++) {
    assert(ActRaiserAr2Master_DrawObject(cpu, cpu->X, &repeated));
    assert(memcmp(&first, &repeated, sizeof(first)) == 0);
  }
  assert(memcmp(before, g_ram, sizeof(before)) == 0 && memcmp(&saved, cpu, sizeof(saved)) == 0);
  ActRaiserAr2Master_UpdatePresentation(cpu);
  assert(!ActRaiserAr2Master_DrawObject(cpu, cpu->X, &repeated));

  g_settings.ar2_master = true;
  ActRaiserAr2Master_UpdatePresentation(cpu);
  /* Advance across frame-counter wrap with no draws, then across a longer
   * skipped interval. Neither culling nor repeated rendering owns the clock. */
  for (unsigned i = 0; i < 3; i++) {
    Tick();
    ActRaiserAr2Master_UpdatePresentation(cpu);
  }
  assert(ActRaiserAr2Master_DrawObject(cpu, cpu->X, &repeated));
  assert(repeated.parts[0].x[0] == first.parts[0].x[0] + 1);
  Put(kActRaiserWram_PlayerHandler, 0x9C64);
  ActRaiserAr2Master_UpdatePresentation(cpu);
  assert(ActRaiserAr2Master_DrawObject(cpu, cpu->X, &first));
  Put(kActRaiserWram_GameFrame, Get(kActRaiserWram_GameFrame) + 12);
  ActRaiserAr2Master_UpdatePresentation(cpu);
  assert(ActRaiserAr2Master_DrawObject(cpu, cpu->X, &repeated));
  assert(repeated.parts[0].x[0] == first.parts[0].x[0] + 1);
}

static void TestNativePoses(CpuState *cpu) {
  g_settings.ar2_master = true;
  Put(cpu->X + kActRaiserActionObject_Composition, 0);
  Put(cpu->X + kActRaiserActionObject_AnimationState, 0);
  /* AR2 animation $00 opens with the front-facing raised-sword statue.
   * Hold that first step throughout arrival and the reverse stone fade. */
  const uint16_t statue_handlers[] = {0x97A6, 0x97C9, 0x97E4, 0xA1CE, 0xA2E5};
  for (unsigned i = 0; i < sizeof(statue_handlers) / sizeof(statue_handlers[0]); i++) {
    Put(kActRaiserWram_PlayerHandler, statue_handlers[i]);
    for (unsigned tick = 0; tick < 12; tick++) {
      Tick();
      ExpectPose(cpu, 0x00, 0);
    }
  }
  /* Magic still uses its independent side-facing cast animation. */
  Put(kActRaiserWram_PlayerHandler, 0x9DE1);
  for (unsigned tick = 0; tick < 12; tick++) {
    Tick();
    ExpectPose(cpu, 0x22, tick < 3 ? 0 : 1);
  }
}

static void TestHitReaction(CpuState *cpu) {
  s_grounded = true;
  Put(cpu->X + kActRaiserActionObject_AnimationState, 0);
  for (unsigned hit = 0; hit < 2; hit++) {
    Put(kActRaiserWram_PlayerHandler, 0x9832);
    Put(cpu->X + kActRaiserActionObject_Flags, 0x0001);
    Tick();
    assert(ActRaiser_Ar2MasterControlEntry(cpu));
    assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9D4F);
    ExpectPose(cpu, 0x01, 0);

    if (!hit) {
      /* Damage pending at control entry must give native knockback control. */
      Put(cpu->X + kActRaiserActionObject_Flags, 0x0009);
      Tick();
      assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9D4F);
    }
    /* The native tail can also enter hurt after AR2 has selected a normal
     * pose this frame. Its new handler must win immediately in either case. */
    Put(kActRaiserWram_PlayerHandler, 0x9C64);
    Put(cpu->X + kActRaiserActionObject_AnimationState, 0x0B);
    Put(cpu->X + kActRaiserActionObject_Flags, 0x0011);
    ExpectPose(cpu, 0x08, 0);
    for (unsigned tick = 1; tick < 12; tick++) {
      Tick();
      ExpectPose(cpu, 0x08, tick < 3 ? 0 : 1);
    }
    assert(Get(kActRaiserWram_PlayerHandler) == 0x9C64);
  }
  /* The same native handler family also owns death; death takes precedence. */
  Put(cpu->X + kActRaiserActionObject_AnimationState, 0x0E);
  Tick();
  ExpectPose(cpu, 0x05, 0);
}

static void ControlTick(CpuState *cpu, uint16_t buttons) {
  Put(0x4218, buttons);
  Tick();
  assert(ActRaiser_Ar2MasterControlEntry(cpu));
  assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9D4F);
}

static void StartSoundScenario(CpuState *cpu, bool grounded) {
  static uint8_t map;
  memset(g_ram, 0, sizeof(g_ram));
  g_ram[kActRaiserWram_MapGroup] = 1;
  g_ram[kActRaiserWram_CurrentMap] = ++map;
  g_settings.ar2_master = true;
  *cpu = (CpuState){.X = kActRaiserWram_PlayerObject, .A = 0xABCD};
  Put(kActRaiserWram_PlayerHandler, 0x9832);
  Put(cpu->X + kActRaiserActionObject_WorldX, 500);
  Put(cpu->X + kActRaiserActionObject_WorldY, 300);
  Put(cpu->X + kActRaiserActionObject_BottomExtent, 24);
  Put(cpu->X + kActRaiserActionObject_Flags, 1);
  Put(kActRaiserWram_InputEnableMask, 0xFFFF);
  s_grounded = grounded;
  s_swings = s_blocks = 0;
  ControlTick(cpu, 0);
  assert(s_swings == 0 && s_blocks == 0);
}

static void ExpectSwing(CpuState *cpu, uint16_t buttons) {
  const unsigned before = s_swings;
  const uint16_t a = cpu->A;
  ControlTick(cpu, buttons);
  assert(s_swings == before + 1 && cpu->A == a);
  /* Advancing poses and holding attack never replays the swing sound. */
  for (unsigned tick = 0; tick < 36; tick++)
    ControlTick(cpu, buttons);
  assert(s_swings == before + 1 && s_blocks == 0);
}

static unsigned DrawnAnimation(CpuState *cpu) {
  ActRaiserAr2MasterDraw draw;
  assert(PrepareDraw(cpu, &draw) && draw.count == 1);
  return (unsigned)(draw.parts[0].x[0] - Get(cpu->X + kActRaiserActionObject_LeftExtent) + 8) / 2;
}

static void TestSwingSounds(CpuState *cpu) {
  /* Standing, crouching, upward and airborne sword swings use COP $01. */
  const uint16_t attacks[] = {0x4000, 0x4400, 0x4800, 0x4000};
  for (unsigned i = 0; i < sizeof(attacks) / sizeof(attacks[0]); i++) {
    StartSoundScenario(cpu, i != 3);
    ExpectSwing(cpu, attacks[i]);
    ControlTick(cpu, 0);
    ExpectSwing(cpu, attacks[i]);
  }
  /* Both glide phases can swing. Reach them using ordinary jump input. */
  for (unsigned animation = 0x14; animation <= 0x15; animation++) {
    StartSoundScenario(cpu, true);
    ControlTick(cpu, 0x8000);
    s_grounded = false;
    for (unsigned tick = 0; tick < 20; tick++)
      ControlTick(cpu, 0x8000);
    ControlTick(cpu, 0);
    ControlTick(cpu, 0x8000);
    unsigned tick = 0;
    for (; tick < 100 && DrawnAnimation(cpu) != animation; tick++)
      ControlTick(cpu, 0);
    assert(tick < 100 && s_swings == 0);
    ExpectSwing(cpu, 0x4000);
  }
}

static void SetShot(unsigned object, uint16_t handler) {
  memset(g_ram + object, 0, kActRaiserActionObjectStride);
  Put(object + kActRaiserActionObject_Handler, handler);
  Put(object + kActRaiserActionObject_WorldX, 520);
  Put(object + kActRaiserActionObject_WorldY, 310);
  Put(object + kActRaiserActionObject_VelocityX, 0xFFFF);
  for (unsigned field = kActRaiserActionObject_LeftExtent;
       field <= kActRaiserActionObject_BottomExtent; field += 2)
    Put(object + field, 4);
}

static void Contact(CpuState *cpu) {
  const uint16_t a = cpu->A, x = cpu->X, y = cpu->Y;
  const uint8_t p = cpu->P;
  assert(ActRaiser_Ar2MasterContactEntry(cpu));
  assert(ActRaiser_Ar2MasterContact(cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu->A == a && cpu->X == x && cpu->Y == y && cpu->P == p);
}

static void TestBlockSounds(CpuState *cpu) {
  const unsigned shot = kActRaiserWram_PlayerObject + kActRaiserActionObjectStride;
  const unsigned shooter = shot + kActRaiserActionObjectStride;
  for (unsigned m_flag = 0; m_flag <= 1; m_flag++) {
    StartSoundScenario(cpu, true);
    cpu->m_flag = m_flag;
    cpu->P = m_flag ? CPU_P_M : 0;
    cpu->Y = 0x1234;
    Contact(cpu);
    assert(s_blocks == 0); /* Raising the shield alone is silent. */
    SetShot(shot, 0xA65D);
    Contact(cpu);
    assert(s_blocks == 1 && s_block_object == shot && s_block_attacker == cpu->X);
    assert(Get(shot + kActRaiserActionObject_Status) == 0x4000);
    Contact(cpu);
    assert(s_blocks == 1);

    SetShot(shot, 0xC389); /* A linked shot must still release its shooter. */
    Put(shot + kActRaiserActionObject_SpawnerBacklink, (uint16_t)shooter);
    Put(shooter + kActRaiserActionObject_LocalCounter, 1);
    Contact(cpu);
    assert(s_blocks == 2 && Get(shooter + kActRaiserActionObject_LocalCounter) == 0);
    assert(Get(shot + kActRaiserActionObject_Status) == 0x4000);

    SetShot(shot, 0xCF16); /* A persistent hazard sounds once per contact. */
    Contact(cpu);
    assert(s_blocks == 3 && Get(shot + kActRaiserActionObject_Status) == 0);
    Tick();
    Contact(cpu);
    assert(s_blocks == 3);
    Put(shot + kActRaiserActionObject_WorldX, 900);
    Contact(cpu);
    Put(shot + kActRaiserActionObject_WorldX, 520);
    Contact(cpu);
    assert(s_blocks == 4);

    /* Nearby stationary objects and unrecognized actors cannot make block sounds. */
    SetShot(shot, 0xA65D);
    Put(shot + kActRaiserActionObject_VelocityX, 0);
    Contact(cpu);
    SetShot(shot, 0x1234);
    Contact(cpu);
    assert(s_blocks == 4);

    cpu->m_flag = 0;
    cpu->P = 0;
    SetShot(shot, 0xCF16);
    Contact(cpu);
    assert(s_blocks == 5);
    ControlTick(cpu, 0);
    ControlTick(cpu, 0x4000);
    assert(!ActRaiser_Ar2MasterContactEntry(cpu));
    for (unsigned tick = 0; tick < 36; tick++)
      ControlTick(cpu, 0);
    Contact(cpu);
    assert(s_blocks == 6); /* Lowering and raising the shield admits a new impact. */
    g_settings.ar2_master = false;
    assert(ActRaiser_Ar2MasterControl(cpu) == RECOMP_RETURN_TAILCALL && s_tail == 0x9832);
    assert(!ActRaiser_Ar2MasterContactEntry(cpu));
    assert(s_blocks == 6);
  }
}

int main(void) {
  g_cpu_cop_hook = SwingSound;
  g_cpu_brk_hook = BlockSound;
  CpuState cpu = {0};
  cpu.X = kActRaiserWram_PlayerObject;
  g_ram[kActRaiserWram_MapGroup] = 1;
  ActRaiserAr2Master_Load("missing-ar2-master-test.armaster");
  g_settings.ar2_master = true;
  assert(!ActRaiserAr2Master_Available() && !ActRaiser_Ar2MasterControlEntry(&cpu));
  uint8_t data[kAr2MasterFixtureBytes];
  const size_t size = Ar2MasterFixture(data, true);
  const char *path = "ar2-master-runtime-test.armaster";
  FILE *file = fopen(path, "wb");
  assert(file && fwrite(data, 1, size, file) == size && fclose(file) == 0);
  ActRaiserAr2Master_Load(path);
  assert(remove(path) == 0); /* Gameplay uses the loaded pack, never the ROM. */
  assert(ActRaiserAr2Master_Available());
  TestSwitch(&cpu, true);
  TestSwitch(&cpu, false);
  /* Native damage/cast/death handlers keep ownership when changing appearance. */
  Put(kActRaiserWram_PlayerHandler, 0x9CC7);
  Put(kActRaiserWram_PlayerObject + kActRaiserActionObject_Flags, 0x2010);
  g_settings.ar2_master = true;
  ActRaiserAr2MasterDraw draw;
  assert(PrepareDraw(&cpu, &draw));
  g_settings.ar2_master = false;
  assert(!PrepareDraw(&cpu, &draw));
  assert(Get(kActRaiserWram_PlayerHandler) == 0x9CC7);
  assert(Get(kActRaiserWram_PlayerObject + kActRaiserActionObject_Flags) == 0x2010);
  TestFadeSwitches(&cpu);
  TestPresentationUpdates(&cpu);
  TestNativePoses(&cpu);
  TestHitReaction(&cpu);
  TestSwingSounds(&cpu);
  TestBlockSounds(&cpu);
  ActRaiserAr2Master_Shutdown();
  assert(!ActRaiserAr2Master_Available());
  assert(!ActRaiserAr2Master_DrawObject(&cpu, cpu.X, &draw));
  puts("actraiser_ar2_master_runtime: ok");
  return 0;
}
