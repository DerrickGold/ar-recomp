/* Production eye projection with resident animation/composition fixtures.
 * No ROM, renderer, native gameplay or GPU is required. */
#include "actraiser/actraiser_death_heim_eyes.h"
#include "actraiser_game.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
static CpuState s_cpu;
static bool s_extended = true, s_donor;
static unsigned s_city = 1, s_mask = 3, s_donor_composition;
enum { kOwner = 0x08e0, kTable = 0x4300 };

static void Word(unsigned address, unsigned value) {
  g_ram[address] = (uint8_t)value;
  g_ram[address + 1] = (uint8_t)(value >> 8);
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  assert(bank == 0 || bank == 0x7e);
  return g_ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) |
      (uint16)cpu_read8(cpu, bank, address + 1) << 8;
}
unsigned ActRaiserDeathHeim_EyeActorCity(CpuState *cpu, uint16_t owner) {
  (void)cpu;
  return s_extended && owner == kOwner ? s_city : 0;
}
uint8_t ActRaiserDeathHeim_EyeMask(unsigned city) {
  assert(city == s_city);
  return (uint8_t)s_mask;
}
bool ActRaiserActorArt_Active(void) { return s_donor; }
bool ActRaiserActorArt_Draw(uint16_t base, uint16_t composition, unsigned visual,
    ActRaiserActorArtDraw *out) {
  assert(base == 0x4000);
  assert(composition == 0x4400 + visual * 32);
  s_donor_composition = composition;
  out->picture.count = g_ram[composition + 4];
  return true;
}
void ActRaiserActorArt_ResolvePart(const ActRaiserActorArtDraw *draw,
    unsigned index, bool flip_x, bool flip_y, int16_t left, int16_t top,
    ActRaiserActorArtPart *out) {
  assert(index < draw->picture.count);
  const unsigned part = s_donor_composition + 5 + index * 7;
  out->x = g_ram[part + 1 + flip_x] + left -
      g_ram[s_donor_composition + flip_x] + 2;
  out->y = g_ram[part + 3 + flip_y] + top -
      g_ram[s_donor_composition + 2 + flip_y] - 1;
  out->attributes = cpu_read16(&s_cpu, 0, part + 5) | 0x0200;
  out->large = false;
}

static void Sequence(unsigned state, unsigned address, bool dying, bool aitos) {
  Word(0x4002 + state * 2, address - 0x4000);
  static const uint8_t appear[][2] = {
    {1,89},{1,89},{2,5},{3,5},{4,5},{5,5},{4,7},{4,89},{4,57},
  };
  static const uint8_t fade[][2] = {
    {4,59},{4,5},{3,5},{2,5},{1,5},{24,5},{24,89},
  };
  const unsigned count = dying ? sizeof(fade) / 2 : sizeof(appear) / 2;
  for (unsigned i = 0; i < count; ++i) {
    unsigned visual = dying ? fade[i][0] : appear[i][0];
    if (aitos && visual != 24) visual += 11;
    g_ram[address + 4 * i] = (uint8_t)visual;
    g_ram[address + 4 * i + 1] = dying ? fade[i][1] : appear[i][1];
  }
  g_ram[address + count * 4] = 255;
}

static void Fixture(void) {
  memset(g_ram, 0, sizeof(g_ram));
  s_cpu = (CpuState){0};
  s_extended = true;
  s_donor = false;
  s_city = 1;
  s_mask = 3;
  Word(0x4000, kTable - 0x4000);
  Sequence(1, 0x4100, false, false);
  Sequence(2, 0x4140, true, false);
  Sequence(12, 0x4180, false, true);
  Sequence(13, 0x41c0, true, true);
  static const uint8_t aitos_tiles[] = {158,126,140,142,174};
  for (unsigned visual = 1; visual <= 16; ++visual) {
    if (visual > 5 && visual < 12) continue;
    const bool aitos = visual >= 12;
    const unsigned comp = 0x4400 + visual * 32;
    Word(kTable + visual * 2, comp - 0x4000);
    g_ram[comp] = aitos ? 0 : 8;
    g_ram[comp + 1] = 16;
    g_ram[comp + 3] = 8;
    g_ram[comp + 4] = aitos ? 2 : 3;
    for (unsigned index = 0; index < g_ram[comp + 4]; ++index) {
      /* Alternate the physical part order, as the native fade frames do. */
      const unsigned column = visual & 1 ? index : g_ram[comp + 4] - 1 - index;
      const unsigned part = comp + 5 + index * 7;
      g_ram[part + 1] = (uint8_t)(column * 8);
      g_ram[part + 2] = (uint8_t)((aitos ? 8 : 16) - column * 8);
      Word(part + 5, aitos ? aitos_tiles[visual - 12] + column :
          (visual - 1) * 3 + column);
    }
  }
  Word(kOwner + 0x32, 0xf408);
  Word(kActRaiserWram_GameFrame, 65000); /* Includes counter-wrap coverage. */
  ActRaiserDeathHeimEyes_BeginHub(&s_cpu, NULL, NULL);
}

static ActRaiserDeathHeimEyesDraw Draw(bool flip, unsigned age) {
  Word(kActRaiserWram_GameFrame, (65000 + age) & 0xffff);
  ActRaiserDeathHeimEyesDraw result;
  assert(ActRaiserDeathHeimEyes_Draw(&s_cpu, kOwner, flip, false,
      s_city == 4 ? 0 : flip ? 16 : 8, 0, &result));
  return result;
}
static bool HasTile(ActRaiserDeathHeimEyesDraw draw, unsigned tile) {
  for (unsigned i = 0; i < draw.count; ++i)
    if ((draw.parts[i].attributes & 0x01ff) == tile) return true;
  return false;
}

static void FixedScreenSides(void) {
  Fixture();
  for (unsigned city = 1; city <= 6; ++city) {
    if (city == 4) continue;
    s_city = city;
    for (unsigned flip = 0; flip <= 1; ++flip) {
      Word(kOwner + 0x32, flip ? 0xf3fa : 0xf408);
      s_mask = 3;
      assert(Draw(flip, 0).count == 3);
      s_mask = 1;
      ActRaiserDeathHeimEyesDraw left = Draw(flip, 0);
      assert(left.count == (flip ? 1 : 2));
      for (unsigned i = 0; i < left.count; ++i) assert(left.parts[i].x <= 8);
      s_mask = 2;
      ActRaiserDeathHeimEyesDraw right = Draw(flip, 0);
      assert(right.count == (flip ? 2 : 1));
      for (unsigned i = 0; i < right.count; ++i) assert(right.parts[i].x >= 8);
      s_mask = 0;
      assert(Draw(flip, 0).count == 0);
    }
  }
}

static void IndependentAnimations(void) {
  Fixture();
  const ActRaiserDeathHeimEncounter next = {.city=1,.act=2};
  const ActRaiserDeathHeimEncounter last = {.city=1,.act=1};
  s_mask = 2;
  ActRaiserDeathHeimEyes_BeginHub(&s_cpu, &next, &last);
  ActRaiserDeathHeimEyesDraw draw = Draw(false, 0);
  assert(draw.count == 3 && HasTile(draw,9) && HasTile(draw,10) && HasTile(draw,2));
  draw = Draw(false, 84);
  assert(draw.count == 1 && HasTile(draw,2));
  draw = Draw(false, 180);
  assert(draw.count == 1 && HasTile(draw,5));
  draw = Draw(false, 198);
  assert(draw.count == 1 && HasTile(draw,14));
  Fixture();
  s_mask = 0;
  const ActRaiserDeathHeimEncounter second = {.city=1,.act=2};
  ActRaiserDeathHeimEyes_BeginHub(&s_cpu, NULL, &second);
  draw = Draw(false, 0);
  assert(draw.count == 1 && HasTile(draw,11));
  assert(Draw(false, 84).count == 0);
}

static void AitosSingleEye(void) {
  for (unsigned first_act = 1; first_act <= 2; ++first_act) {
    Fixture();
    s_city = 4;
    Word(kOwner + 0x32, 0xf430);
    const ActRaiserDeathHeimEyesDraw before = Draw(false,0);
    const ActRaiserDeathHeimEncounter first = {.city=4,.act=(uint8_t)first_act};
    const ActRaiserDeathHeimEncounter second = {.city=4,.act=(uint8_t)(3-first_act)};
    s_mask = first_act == 1 ? 2 : 1;
    ActRaiserDeathHeimEyes_BeginHub(&s_cpu, &second, &first);
    ActRaiserDeathHeimEyesDraw draw = Draw(false, 0);
    assert(draw.count == 2 && HasTile(draw,142) && HasTile(draw,143));
    draw = Draw(false, 84);
    assert(draw.count == 2 && HasTile(draw,158) && HasTile(draw,159));
    draw = Draw(false, 180);
    assert(draw.count == 2 && HasTile(draw,126) && HasTile(draw,127));
    for (unsigned age = 0; age <= 300; ++age) {
      draw = Draw(false,age);
      assert(draw.count == 2);
      for (unsigned i = 0; i < draw.count; ++i) {
        const unsigned column = draw.parts[i].attributes & 1u;
        const unsigned match = (before.parts[0].attributes & 1u) == column ? 0 : 1;
        assert(draw.parts[i].x == before.parts[match].x);
        assert(draw.parts[i].y == before.parts[match].y);
      }
    }
    Word(kActRaiserWram_GameFrame, 65000);
    s_mask = 0;
    ActRaiserDeathHeimEyes_BeginHub(&s_cpu, NULL, &second);
    assert(Draw(false,0).count == 2);
    assert(Draw(false,84).count == 0);
  }
}

static void RegionalAndScope(void) {
  Fixture();
  const ActRaiserDeathHeimEyesDraw native = Draw(false,0);
  s_donor = true;
  const ActRaiserDeathHeimEyesDraw regional = Draw(false,0);
  assert(native.count == regional.count);
  for (unsigned i = 0; i < native.count; ++i) {
    assert(regional.parts[i].x == native.parts[i].x + 2);
    assert(regional.parts[i].y == native.parts[i].y - 1);
    assert(regional.parts[i].attributes == (native.parts[i].attributes | 0x0200));
  }
  ActRaiserDeathHeimEyesDraw draw;
  s_extended = false;
  assert(!ActRaiserDeathHeimEyes_Draw(&s_cpu,kOwner,false,false,8,0,&draw));
  s_extended = true;
  Word(kOwner + 0x32,0xf458); /* Central final statue is outside the feature. */
  assert(!ActRaiserDeathHeimEyes_Draw(&s_cpu,kOwner,false,false,8,0,&draw));
  Word(kOwner + 0x32,0xf494);
  const ActRaiserDeathHeimEncounter last = {.city=1,.act=1};
  ActRaiserDeathHeimEyes_BeginHub(&s_cpu,NULL,&last);
  assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == 0);
  Word(kOwner + kActRaiserActionObject_AnimationState,6);
  Word(kOwner + kActRaiserActionObject_Visual,25);
  assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == -6);
  s_city = 2;
  assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == 0);
}

static void ForeheadAndBurstAnchors(void) {
  for (unsigned city = 1; city <= 6; ++city)
    for (unsigned act = 1; act <= 2; ++act)
      for (unsigned mirrored = 0; mirrored <= 1; ++mirrored) {
        Fixture();
        s_city = city;
        Word(kOwner + 0x32, mirrored ? 0xf486 : 0xf494);
        const ActRaiserDeathHeimEncounter last = {.city=city,.act=act};
        ActRaiserDeathHeimEyes_BeginHub(&s_cpu,NULL,&last);
        Word(kOwner + kActRaiserActionObject_AnimationState,6);
        /* The native forehead part starts at the same anchor before and
         * after a victory, including the wait before the flare begins. */
        Word(kOwner + kActRaiserActionObject_Visual,0);
        assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == 0);
        for (unsigned visual = 25; visual <= 38; ++visual) {
          Word(kOwner + kActRaiserActionObject_Visual,visual);
          const int expected = city == 4 ? 0 : act == 1 ? -6 : 6;
          assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == expected);
        }
        Word(kOwner + kActRaiserActionObject_Visual,24);
        assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == 0);
        Word(kOwner + kActRaiserActionObject_Visual,25);
        Word(kOwner + kActRaiserActionObject_AnimationState,5);
        assert(ActRaiserDeathHeimEyes_FlareOffset(&s_cpu,kOwner) == 0);
      }
}

static void SurvivingEyeAnchors(void) {
  static const unsigned ages[] = {0,60,84,180,198,300};
  for (unsigned donor = 0; donor <= 1; ++donor)
    for (unsigned mirrored = 0; mirrored <= 1; ++mirrored)
      for (unsigned completed = 1; completed <= 2; ++completed) {
        Fixture();
        s_donor = donor != 0;
        Word(kOwner + 0x32, mirrored ? 0xf3fa : 0xf408);
        const ActRaiserDeathHeimEyesDraw before = Draw(mirrored,0);
        const ActRaiserDeathHeimEncounter next = {.city=1,.act=3-completed};
        const ActRaiserDeathHeimEncounter last = {.city=1,.act=completed};
        s_mask = 1u << (next.act - 1);
        ActRaiserDeathHeimEyes_BeginHub(&s_cpu,&next,&last);
        for (unsigned age = 0; age < sizeof(ages)/sizeof(ages[0]); ++age) {
          const ActRaiserDeathHeimEyesDraw after = Draw(mirrored,ages[age]);
          const bool source_right = (next.act == 2) != mirrored;
          const unsigned survivor_count = source_right ? 1 : 2;
          assert(after.count == (ages[age] < 84 ? 3 : survivor_count));
          unsigned survivors = 0;
          for (unsigned i = 0; i < after.count; ++i) {
            /* The fixture's tile modulo three identifies the native part,
             * independently of reordered compositions and glow brightness. */
            const unsigned column = (after.parts[i].attributes & 0x01ff) % 3;
            survivors += (column == 2) == source_right;
            unsigned match = 0;
            while (match < before.count &&
                (before.parts[match].attributes & 0x01ff) % 3 != column) ++match;
            assert(match < before.count);
            assert(after.parts[i].x == before.parts[match].x);
            assert(after.parts[i].y == before.parts[match].y);
          }
          assert(survivors == survivor_count);
        }
      }
}

int main(void) {
  FixedScreenSides();
  IndependentAnimations();
  AitosSingleEye();
  RegionalAndScope();
  ForeheadAndBurstAnchors();
  SurvivingEyeAnchors();
  puts("Death Heim eye projection tests passed");
  return 0;
}
