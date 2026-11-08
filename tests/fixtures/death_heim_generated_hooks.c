/* Optional local-ROM integration observer. The production application, native
 * generated code, room loader, PPU and audio all run unchanged. This replaces
 * only oracle_trace.o in an isolated executable. Controlled lethal completion
 * enters the original generic boss-death handler; no victory hook is called
 * directly. Timeout probes wait for the real death, retry and admission paths. */
#define OracleTrace_CompleteTick DeathHeimTest_OriginalCompleteTick
#include "../../src/dev/oracle_trace.c"
#undef OracleTrace_CompleteTick

#include "actraiser/actraiser_death_heim_runtime.h"
#include "app/settings.h"
#include "present/render_comparison.h"

static FILE *s_test_events;
static bool s_test_configured;
static bool s_test_resources_seeded;
static bool s_test_timeout;
static bool s_test_retry_seen;
static int s_test_saved_hp_cheat;
static unsigned s_test_age;
static unsigned s_test_kill_age;
static unsigned s_test_serial;
static unsigned s_test_last_id = 255;
static unsigned s_test_last_phase = 255;
static unsigned s_test_last_scene = 65535;
static unsigned s_test_visible_frames;
static unsigned s_test_first_visible;
static unsigned s_test_first_active;
static bool s_test_native_active;
static bool s_test_attack;
static bool s_test_failed;
static unsigned s_test_beam_state;
static unsigned s_test_root;

enum { kObserveFrames = 600, kAppearanceDeadline = 120 };

static unsigned TestWord(unsigned address) {
  return g_ram[address] | (unsigned)g_ram[address + 1] << 8;
}

static void TestWriteWord(unsigned address, unsigned value) {
  g_ram[address] = (uint8_t)value;
  g_ram[address + 1] = (uint8_t)(value >> 8);
}

static int TestOption(const char *name, int fallback) {
  const char *text = getenv(name);
  return text ? atoi(text) : fallback;
}

/* Read native geometry rather than treating an allocated actor as visible.
 * Signed world subtraction also handles Aitos's intentionally wrapped Y. */
static bool TestVisible(unsigned owner) {
  if (!owner || (TestWord(owner) & 0x6c00)) return false;
  const int x = (int16_t)(TestWord(owner + 2) - TestWord(0x22));
  const int y = (int16_t)(TestWord(owner + 4) - TestWord(0x24));
  return x - (int)TestWord(owner + 0x0a) < 256 &&
      x + (int)TestWord(owner + 0x0e) >= 0 &&
      y - (int)TestWord(owner + 0x0c) < 224 &&
      y + (int)TestWord(owner + 0x10) >= 0;
}

static bool TestFamily(unsigned owner, unsigned root) {
  return owner != root && TestWord(owner) < 0x4000 &&
      TestWord(owner + 0x32) == TestWord(root + 0x32) &&
      TestWord(owner + 0x16) == 0x5000 && g_ram[owner + 0x18] == 0x7e &&
      TestWord(owner + 0x3a) == root;
}

static bool TestBodyVisible(unsigned id, unsigned root) {
  if (TestVisible(root)) return true;
  /* Dragon body segments are boss artwork. Ordinary missiles and the
   * centaur's HP-bar child must not make an offscreen boss pass this check. */
  if (id == 6 && root)
    for (unsigned at = 0x0ae0; at < 0x1aa0; at += 0x40)
      if (TestFamily(at, root) && (TestWord(at + 0x30) & 0x4000) && TestVisible(at))
        return true;
  return false;
}

static bool TestNativeActive(unsigned id, unsigned root) {
  if (!root || TestWord(root + 0x16) != 0x5000) return false;
  if (id >= 12 || (id & 1)) return true;
  static const unsigned first[] = {0xadb0,0xb7f1,0xc6da,0xd693,0xd9d5,0xe7fb};
  static const unsigned last[] = {0xae99,0xb891,0xc778,0xd701,0xd9e3,0xe884};
  unsigned pc = TestWord(root + 0x12);
  if (pc == 0x8661 || pc == 0x8683) pc = TestWord(root + 0x1e);
  return pc >= first[id / 2] && pc < last[id / 2];
}

static bool TestAttack(unsigned id, unsigned root) {
  if (!root || !TestNativeActive(id, root)) return false;
  if (id >= 12 || (id & 1)) return true;
  const unsigned state = TestWord(root + 0x1a);
  /* Native charge/volley states, or their emitted attack actors. These are
   * downstream of the original positional and music-introduction gates. */
  if (id == 0 && ((TestWord(root + 0x30) & 0x0800) || state == 10)) return true;
  if (id == 6 && (state == 5 || state == 7 || state == 9)) return true;
  if (id == 10 && state >= 7 && state <= 10) return true;
  for (unsigned at = 0x0ae0; at < 0x1aa0; at += 0x40) {
    if (!TestFamily(at, root) || (TestWord(at) & 0x0800)) continue;
    unsigned pc = TestWord(at + 0x12);
    if (pc == 0x8661 || pc == 0x8683) pc = TestWord(at + 0x1e);
    if ((id == 0 && pc >= 0xaeba && pc < 0xaf21) ||
        (id == 2 && pc >= 0xb8e9 && pc < 0xb919) ||
        (id == 4 && pc >= 0xc778 && pc < 0xc7a0) ||
        (id == 6 && pc >= 0xd78a && pc < 0xd7ba) ||
        (id == 8 && ((pc >= 0xd9f2 && pc < 0xda2e) ||
                    (pc >= 0xdaeb && pc < 0xdafd))) ||
        (id == 10 && pc >= 0xe884 && pc < 0xe8b5)) return true;
  }
  return false;
}

static void TestEvent(const char *event, unsigned id, unsigned phase,
                      unsigned owner) {
  if (!s_test_events) {
    const char *path = getenv("AR_DEATH_HEIM_TEST_EVENTS");
    s_test_events = path ? fopen(path, "w") : NULL;
    if (!s_test_events) abort();
  }
  fprintf(s_test_events,
          "{\"event\":\"%s\",\"gf\":%u,\"host\":%u,\"id\":%u,\"phase\":%u,"
          "\"group\":%u,\"map\":%u,\"age\":%u,\"owner\":%u,"
          "\"hp\":%u,\"max_hp\":%u,\"lives\":%u,\"magic\":%u,"
          "\"timer\":%u,\"progress\":%u,\"retry\":%u,"
          "\"player\":[%u,%u],\"camera\":[%u,%u],\"boss\":[%u,%u],"
          "\"source\":%u,\"handler\":%u,\"postdeath\":%u,"
          "\"animation\":%u,\"bank\":%u,\"state\":%u,\"visual\":%u,\"resume\":%u,"
          "\"flags\":%u,\"status\":%u,\"boss_hp\":%u,\"input_mask\":%u,"
          "\"player_status\":%u,\"player_handler\":%u,\"controller\":%u,"
          "\"music_source\":%u,\"native_active\":%u,\"attack\":%u,\"body_visible\":%u,"
          "\"visible_frames\":%u,\"first_visible_age\":%u,\"comparison_view\":%u,"
          "\"eyes\":[%u,%u,%u,%u,%u,%u]}\n",
          event, TestWord(0x88), (unsigned)snes_frame_counter, id, phase, g_ram[0x18], g_ram[0x19],
          s_test_age, owner, g_ram[0x1d], g_ram[0x1e], g_ram[0x1c],
          g_ram[0x21], TestWord(0xe6), TestWord(0x347), TestWord(0x32c),
          TestWord(0x8a2), TestWord(0x8a4), TestWord(0x22), TestWord(0x24),
          owner ? TestWord(owner + 2) : 0, owner ? TestWord(owner + 4) : 0,
          owner ? TestWord(owner + 0x32) : 0, owner ? TestWord(owner + 0x12) : 0,
          owner ? TestWord(owner + 0x14) : 0,
          owner ? TestWord(owner + 0x16) : 0, owner ? g_ram[owner + 0x18] : 0,
          owner ? TestWord(owner + 0x1a) : 0, owner ? TestWord(owner + 0x20) : 0,
          owner ? TestWord(owner + 0x1e) : 0, owner ? TestWord(owner + 0x30) : 0,
          owner ? TestWord(owner) : 0, owner ? TestWord(owner + 0x2c) : 0,
          TestWord(0xf4), TestWord(0x8a0), TestWord(0x8b2), TestWord(0xa0),
          TestWord(0xab) | (unsigned)g_ram[0xad] << 16,
          TestNativeActive(id, owner), TestAttack(id, owner), TestBodyVisible(id, owner),
          s_test_visible_frames, s_test_first_visible,
          (unsigned)RenderComparison_PresentView(),
          ActRaiserDeathHeim_EyeMask(1), ActRaiserDeathHeim_EyeMask(2),
          ActRaiserDeathHeim_EyeMask(3), ActRaiserDeathHeim_EyeMask(4),
          ActRaiserDeathHeim_EyeMask(5), ActRaiserDeathHeim_EyeMask(6));
  fflush(s_test_events);
}

static unsigned TestRoot(const ActRaiserDeathHeimEncounter *encounter) {
  /* Native child allocation copies the source, HP and animation. Capture
   * the unparented root once, including its arrival beam, and keep that
   * exact owner through Tanzra's second form and Antlion's backlink reuse.
   * Never replace a retired root with a surviving child. */
  if (!s_test_root) {
    for (unsigned owner = 0x0ae0; owner < 0x1aa0; owner += 0x40) {
      const unsigned parent = TestWord(owner + 0x3a);
      const bool child = parent >= 0x06a0 && parent < 0x1aa0 &&
          (parent - 0x06a0) % 0x40 == 0;
      const bool arrival = TestWord(owner + 0x16) == 0x4000 &&
          TestWord(owner + 0x1a) == 14 && TestWord(owner + 0x30) == 0x0030;
      if (!child && TestWord(owner + 0x32) == encounter->source &&
          ((TestWord(owner + 0x30) & 0x4000) || arrival) &&
          (TestWord(owner + 0x16) == 0x5000 || arrival) &&
          g_ram[owner + 0x18] == 0x7e && TestWord(owner) < 0x4000) {
        s_test_root = owner;
        break;
      }
    }
  }
  const unsigned owner = s_test_root;
  return owner && TestWord(owner + 0x32) == encounter->source &&
      (TestWord(owner + 0x16) == 0x5000 || TestWord(owner + 0x16) == 0x4000) &&
      g_ram[owner + 0x18] == 0x7e && TestWord(owner) < 0x4000 ? owner : 0;
}

static void TestSnapshot(unsigned id) {
  const char *directory = getenv("AR_DEATH_HEIM_TEST_DIRECTORY");
  char prefix[1024];
  if (!directory || snprintf(prefix, sizeof(prefix),
                             "%s/arena%02u-e%02u-age%u",
                             directory, s_test_serial++, id, s_test_age) >=
                        (int)sizeof(prefix)) abort();
  ActRaiser_FullSnapshot(prefix);
}

void OracleTrace_CompleteTick(void) {
  DeathHeimTest_OriginalCompleteTick();
  const unsigned frame = TestWord(0x88);
  const unsigned scene = TestWord(0x18);
  const int roster = TestOption("AR_DEATH_HEIM_TEST_ROSTER", 1);
  const bool shuffle = TestOption("AR_DEATH_HEIM_TEST_SHUFFLE", 0) != 0;
  const int retry_id = TestOption("AR_DEATH_HEIM_TEST_RETRY", -1);
  if (!s_test_configured && frame != 0x5555 && frame >= 450) {
    ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
    config.roster = (uint8_t)roster;
    config.shuffle_order = shuffle;
    config.seed = (uint32_t)TestOption("AR_DEATH_HEIM_TEST_SEED", 1);
    if (!ActRaiserDeathHeim_Configure(&config)) abort();
    s_test_configured = true;
    TestEvent("configure", 255, ActRaiserDeathHeim_Phase(), 0);
  }

  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  unsigned phase = ActRaiserDeathHeim_Phase();
  /* Native Standard/off deliberately has no custom run. Identify its actual
   * loaded rematch for the same diagnostic completion controls. */
  if (!encounter && roster == 0 && !shuffle && g_ram[0x18] == 7 &&
      g_ram[0x19] >= 2 && g_ram[0x19] <= 8) {
    encounter = ActRaiserDeathHeimRush_Encounter(
        g_ram[0x19] == 8 ? 12 : (g_ram[0x19] - 2) * 2 + 1);
    phase = g_ram[0x19] == 8 ? kActRaiserDeathHeimRush_Final
                           : kActRaiserDeathHeimRush_Combat;
  }
  const unsigned id = encounter ? encounter->id : 255;
  if (s_test_timeout && phase == kActRaiserDeathHeimRush_Retry)
    s_test_retry_seen = true;
  if (s_test_retry_seen && s_test_saved_hp_cheat &&
      (phase == kActRaiserDeathHeimRush_Combat ||
       phase == kActRaiserDeathHeimRush_Final)) {
    g_settings.cheat_inf_hp = s_test_saved_hp_cheat;
    s_test_saved_hp_cheat = 0;
  }
  if (id != s_test_last_id || phase != s_test_last_phase || scene != s_test_last_scene) {
    TestEvent("transition", id, phase, 0);
    if (id != s_test_last_id || scene != s_test_last_scene) s_test_root = 0;
    s_test_last_id = id;
    s_test_last_phase = phase;
    s_test_last_scene = scene;
    s_test_age = 0;
    s_test_kill_age = 0;
    s_test_visible_frames = s_test_first_visible = s_test_first_active = 0;
    s_test_native_active = s_test_attack = s_test_failed = false;
    s_test_beam_state = 0;
  }
  if (!encounter || g_ram[0x18] != encounter->map_group ||
      g_ram[0x19] != encounter->map) return;

  const unsigned owner = TestRoot(encounter);
  const unsigned beam = owner && TestWord(owner + 0x16) == 0x4000 &&
      (TestWord(owner + 0x1a) == 14 || TestWord(owner + 0x1a) == 15)
      ? TestWord(owner + 0x1a) : 0;
  /* Keep the beam and native waiting states visible to the verifier even
   * while the host says Preparing. Combat is not evidence of activation. */
  TestEvent("sample", id, phase, owner);
  if (beam && beam != s_test_beam_state) {
    TestSnapshot(id);
    TestEvent(beam == 14 ? "arrival_beam" : "departure_beam", id, phase, owner);
  }
  s_test_beam_state = beam;
  if (phase != kActRaiserDeathHeimRush_Combat && phase != kActRaiserDeathHeimRush_Final)
    return;
  ++s_test_age;
  if (!s_test_kill_age && owner && TestWord(owner + 0x16) == 0x5000) {
    if (TestBodyVisible(id, owner)) {
      if (!s_test_first_visible) s_test_first_visible = s_test_age;
      ++s_test_visible_frames;
    }
    if (TestNativeActive(id, owner)) {
      if (!s_test_first_active) s_test_first_active = s_test_age;
      s_test_native_active = true;
    }
    s_test_attack |= TestAttack(id, owner);
  }
  if (s_test_age == 60 || s_test_age == kObserveFrames) {
    TestSnapshot(id);
    TestEvent("fight", id, phase, owner);
  }
  if (retry_id >= 0 && !s_test_resources_seeded && s_test_age == 60) {
    g_ram[0x1c] = 3;
    g_ram[0x1d] = 24;
    g_ram[0x1e] = 24;
    g_ram[0x21] = 3;
    s_test_resources_seeded = true;
    TestEvent("resources", id, phase, owner);
  }
  if (!s_test_timeout && id == (unsigned)retry_id && s_test_age == kObserveFrames) {
    TestWriteWord(0xe6, 0);
    g_ram[0xe5] = 0;
    g_ram[0xe8] = 0;
    s_test_timeout = true;
    s_test_saved_hp_cheat = g_settings.cheat_inf_hp;
    g_settings.cheat_inf_hp = 0;
    TestEvent("timeout", id, phase, owner);
  }
  if (!s_test_kill_age && s_test_age == kObserveFrames && id < 12 && !(id & 1) &&
      (!s_test_native_active || s_test_first_active > kAppearanceDeadline ||
       !s_test_attack || !s_test_first_visible ||
       s_test_first_visible > kAppearanceDeadline || s_test_visible_frames < 30)) {
    s_test_failed = true;
    TestEvent("acceptance_failed", id, phase, owner);
  }
  if (!s_test_failed && (id != (unsigned)retry_id || s_test_retry_seen) &&
      s_test_age >= kObserveFrames && owner && TestWord(owner + 0x16) == 0x5000 &&
      TestWord(owner + 0x2c) > 0 && TestWord(0xf4) == 0xffff &&
      (!s_test_kill_age || s_test_age - s_test_kill_age >= kObserveFrames)) {
    TestEvent("lethal", id, phase, owner);
    TestWriteWord(owner + 0x2c, 0);
    TestWriteWord(owner + 0x12, 0xa54a);
    s_test_kill_age = s_test_age;
  }
}
