#include "actraiser_death_heim_arena.h"
#include "actraiser_death_heim_runtime.h"
#include "actraiser_game.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"
#include "actraiser/regional/actraiser_regional_runtime.h"

enum {
  kFirstPlacedSlot = 0x0ae0,
  kReservedSlots = 32,
  kBossSlot = kFirstPlacedSlot + kReservedSlots * kActRaiserActionObjectStride,
};

/* The placement bytes are the original $0A room records. Coordinates for
 * the player are ground lines, as consumed by $95F0/$96A4; they are not the
 * live actor hot point. The ordinary initializer subtracts its down extent.
 * Room-local collision at these anchors is retained, including Northwall's
 * water and Kasandora's sloping pit. Land inside the boss viewport: the
 * ordinary entrances leave several bosses beyond the right screen edge.
 * Bloodpool uses the right platform so its native left-side entrance is
 * visible; Aitos uses the central platform beneath the dragon's entrance.
 * The dragon's $FF placement Y is intentional and its initializer wraps it
 * above the screen. Never clamp that boss coordinate to the map height. */
static const ActRaiserDeathHeimArena kArenas[] = {
    {1, 1, 3920, 720, 0xb28d, 0xad45, 0xfb, 0x2d, 0, 0x04},
    {2, 1, 4016, 416, 0xb55c, 0xb786, 0xf0, 0x24, 0, 0x06},
    {3, 2, 2624, 704, 0xb8d6, 0xc66f, 0xa6, 0x37, 0, 0x04},
    {4, 3,  248, 160, 0xbbc2, 0xd646, 0x1a, 0xff, 0, 0x0d},
    {5, 3,  384, 240, 0xbd9f, 0xd974, 0x1e, 0x0e, 0, 0x05},
    {6, 4,  368, 480, 0xc23d, 0xe7c6, 0x1c, 0x1d, 0, 0x07},
};

/* This exists only while the native force-blanked asset VM is on the stack.
 * It owns no gameplay progression and is reset with the runtime lifecycle. */
static const ActRaiserDeathHeimArena *s_loading;

extern RecompReturn bank_00_9557_M0X0(CpuState *cpu);
extern RecompReturn bank_02_BAF5_M1X0(CpuState *cpu);

/* Bloodpool's first placement is a hidden, persistent environment actor,
 * not an enemy: $0A:B4CB = 00 00 FF 07. Native type7 selects $00:B919,
 * which calls $02:9204 every update to build the water perspective table
 * and configure HDMA channel2. None of the other five target rooms has a
 * handler-only environment placement. Keep this actor before the boss so
 * the ordinary pre-fade actor pass establishes the raster before scanout. */
static RecompReturn SpawnEnvironment(CpuState *cpu, unsigned city) {
  if (city != 2) return RECOMP_RETURN_NORMAL;
  static const uint8_t placement[] = {0, 0, 0xff, 7};
  for (unsigned i = 0; i < sizeof(placement); ++i)
    if (cpu_read8(cpu, 0x0a, (uint16_t)(0xb4cb + i)) != placement[i])
      ActRaiserHleFatal("Death Heim Bloodpool environment differs from its native record");
  cpu->X = kFirstPlacedSlot;
  cpu_write16(cpu, 0, cpu->X + 0x34, 0);
  cpu_write16(cpu, 0, cpu->X + 0x36, 0);
  cpu_write16(cpu, 0, cpu->X + 0x38, 0xff);
  cpu->Y = 0xb4cb;
  cpu->A = 7;
  cpu_mirrors_to_p(cpu);
  cpu->P &= ~(CPU_P_N | CPU_P_Z);
  cpu_p_to_mirrors(cpu);
  const RecompReturn result = ActRaiserNativeCall(cpu, bank_00_9557_M0X0, 0, 0x9461, false);
  if (result != RECOMP_RETURN_NORMAL) return result;
  if (cpu->X != kFirstPlacedSlot || cpu->Y != 0xb4cb || cpu->DB != 0x0a ||
      cpu->m_flag || cpu->x_flag ||
      cpu_read16(cpu, 0, kFirstPlacedSlot) != 0x0800 ||
      cpu_read16(cpu, 0, kFirstPlacedSlot + 0x12) != 0xb919 ||
      cpu_read16(cpu, 0, kFirstPlacedSlot + 0x30) != 0)
    ActRaiserHleFatal("Death Heim Bloodpool environment initialization failed");
  cpu->X = kBossSlot;
  return RECOMP_RETURN_NORMAL;
}

const ActRaiserDeathHeimArena *ActRaiserDeathHeimArenas_Find(unsigned city) {
  return city >= 1 && city <= sizeof(kArenas) / sizeof(kArenas[0])
      ? &kArenas[city - 1] : NULL;
}

void ActRaiserDeathHeimArenas_Reset(void) { s_loading = NULL; }

bool ActRaiserDeathHeimArenas_Begin(CpuState *cpu, unsigned city) {
  const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(city);
  if (!cpu || !arena || s_loading) return false;
  cpu_write8(cpu, 0, kActRaiserWram_DestinationMapGroup, arena->city);
  cpu_write8(cpu, 0, kActRaiserWram_DestinationMap, arena->map);
  /* A new arena never consumes an earlier town's checkpoint. Actual life
   * retries are separate native loads and retain their retry marker. */
  cpu_write16(cpu, 0, 0x032c, 0);
  cpu_write16(cpu, 0, 0x032e, 0);
  cpu_write16(cpu, 0, 0x0330, 0);
  return true;
}

static bool Context(const CpuState *cpu, unsigned pb, unsigned db, bool byte_a) {
  return cpu && cpu->PB == pb && cpu->DB == db && !cpu->D && !cpu->emulation &&
      cpu->m_flag == byte_a && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}

static const ActRaiserDeathHeimArena *Current(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!cpu || !encounter || encounter->act != 1)
    return NULL;
  const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(encounter->city);
  return arena && cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == arena->city &&
      cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) == arena->map ? arena : NULL;
}

bool ActRaiserDeathHeimArenas_Loading(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!cpu || !s_loading || !encounter || encounter->act != 1 ||
      s_loading != ActRaiserDeathHeimArenas_Find(encounter->city))
    return false;
  const uint8_t map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  return cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == s_loading->city &&
      map >= 1 && map <= s_loading->map;
}

static RecompReturn Tail(uint32_t target, uint32_t source) {
  if (!cpu_hle_tailcall_request(target, source))
    ActRaiserHleFatal("Death Heim arena lost its native continuation");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_DeathHeimArenaAssetsBeginEntry(CpuState *cpu) {
  return !s_loading && Context(cpu, 2, 0, true) && Current(cpu);
}

RecompReturn ActRaiser_DeathHeimArenaAssetsBegin(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArenaAssetsBeginEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arena asset entry");
  s_loading = Current(cpu);
  cpu_write8(cpu, 0, kActRaiserWram_CurrentMap, 1);
  /* $B1F7 already pushed P and established M1/X0. Re-enter only the JSR
   * $B250 instruction; the guard now delegates it to native code. */
  return Tail(0x02b1fc, 0x02b1fc);
}

bool ActRaiser_DeathHeimArenaAssetsEndEntry(CpuState *cpu) {
  return Context(cpu, 2, 0, true) && ActRaiserDeathHeimArenas_Loading(cpu);
}

RecompReturn ActRaiser_DeathHeimArenaAssetsEnd(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArenaAssetsEndEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arena asset completion");
  const uint8_t map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  if (map < s_loading->map) {
    /* Complete the room's native character-animation capture before the
     * next script can request continuation ($DE bit7). Kasandora, Marahna
     * and Northwall inherit these frames from an earlier room. The asset
     * VM has finished its force-blanked VRAM uploads; $BAF5 reads the live
     * atlas at $DA into $7F:B800-$C7FF, or retains it for a continuation.
     * It requires M1/X0 and returns RTL with P restored. Our paired return
     * resumes at this script-end boundary, before advancing the map. The
     * final room still reaches the ordinary $00:8353/$8389 capture call. */
    const RecompReturn capture = ActRaiserNativeCall(cpu, bank_02_BAF5_M1X0,
        2, 0xb24d, true);
    if (capture != RECOMP_RETURN_NORMAL) return capture;
    cpu_write8(cpu, 0, kActRaiserWram_CurrentMap, (uint8_t)(map + 1));
    /* Native $B250 resets the script cursor for each room. All resource
     * commands, audio handshakes and PPU uploads execute in their usual VM. */
    return Tail(0x02b1fc, 0x02b24e);
  }
  /* A borrowed level supplies its environment, not its traversal clock.
   * Match this city's existing Death Heim rematch, including Marahna's
   * selected regional300/200 rule. Reading the native record also guards
   * the profile identity; the video/register bytes remain the arena's. */
  static const uint8_t profiles[] = {0x06, 0x0f, 0x15, 0x1c, 0x24, 0x2c};
  const uint8_t profile = profiles[s_loading->city - 1];
  const uint16_t native_bcd = cpu_read16(cpu, 2, (uint16_t)(0x893e + 28 * profile + 25));
  uint16_t resolved;
  if (native_bcd != (s_loading->city == 5 ? 0x0300 : 0x0200) ||
      !ActRaiserRegional_BeginActionRoom(profile, native_bcd, &resolved))
    ActRaiserHleFatal("Cannot initialize Death Heim arena timer policy");
  cpu_write16(cpu, 0, kActRaiserWram_ActionTimerLow, resolved);
  cpu_write8(cpu, 0, 0x00e5, 0x3b);
  cpu_write8(cpu, 0, 0x00e8, 0);
  s_loading = NULL;
  /* One original PHP frame, one original PLP/RTL, regardless of room count. */
  return Tail(0x02b24e, 0x02b24e);
}

bool ActRaiser_DeathHeimArenaPlayerEntry(CpuState *cpu) {
  return Context(cpu, 0, 0x0a, false) && Current(cpu) &&
      cpu->X == kActRaiserWram_PlayerObject;
}

RecompReturn ActRaiser_DeathHeimArenaPlayer(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArenaPlayerEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arena player entry");
  const ActRaiserDeathHeimArena *arena = Current(cpu);
  cpu_write16(cpu, 0, cpu->X + 0x34, arena->player_x);
  cpu_write16(cpu, 0, cpu->X + 0x36, arena->player_ground_y);
  cpu_write16(cpu, 0, cpu->X + 0x38, 0);
  cpu_write16(cpu, 0, 0x032e, 0);
  cpu_write16(cpu, 0, 0x0330, 0);
  /* Native type83 initializes the player, cached subject and camera origin.
   * $B727 subsequently builds the complete scrolled tilemap before fade-in.
   * Leave $032C to the ordinary retry policy at $981C. */
  return Tail(0x00939c, 0x009354);
}

bool ActRaiser_DeathHeimArenaPlacementsEntry(CpuState *cpu) {
  return Context(cpu, 0, 0x0a, false) && Current(cpu) && cpu->X == kFirstPlacedSlot;
}

RecompReturn ActRaiser_DeathHeimArenaPlacements(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArenaPlacementsEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arena placement entry");
  const ActRaiserDeathHeimArena *arena = Current(cpu);
  const uint8_t bytes[] = {arena->boss_x, arena->boss_y, arena->parameter, arena->type};
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    if (cpu_read8(cpu, 0x0a, (uint16_t)(arena->placement + i)) != bytes[i])
      ActRaiserHleFatal("Death Heim arena placement differs from its native record");
  /* Dedicated native arenas reserve32 records before the boss. Keep the same
   * record layout and root slot for all six, retaining only required native
   * environment actors within that range. No level enemies, pickups or wave
   * gates can interfere with this encounter or overwrite its retry anchor. */
  for (unsigned i = 0; i < kReservedSlots; ++i) {
    cpu_write16(cpu, 0, cpu->X, 0x4000);
    cpu->X += kActRaiserActionObjectStride;
  }
  const RecompReturn environment_result = SpawnEnvironment(cpu, arena->city);
  if (environment_result != RECOMP_RETURN_NORMAL) return environment_result;
  cpu_write16(cpu, 0, cpu->X + 0x34, (uint16_t)(arena->boss_x * 16));
  cpu_write16(cpu, 0, cpu->X + 0x36, (uint16_t)(arena->boss_y * 16));
  cpu_write16(cpu, 0, cpu->X + 0x38, arena->parameter);
  cpu->Y = arena->placement;
  cpu->A = arena->type;
  cpu_mirrors_to_p(cpu);
  cpu->P &= ~(CPU_P_N | CPU_P_Z);
  cpu_p_to_mirrors(cpu);
  const RecompReturn result = ActRaiserNativeCall(cpu, bank_00_9557_M0X0, 0, 0x9461, false);
  if (result != RECOMP_RETURN_NORMAL) return result;
  if (cpu->X != kBossSlot || cpu->Y != arena->placement || cpu->DB != 0x0a ||
      cpu->m_flag || cpu->x_flag ||
      cpu_read16(cpu, 0, kBossSlot + kActRaiserActionObject_SourceDescriptor) != arena->source ||
      !ActRaiserDeathHeim_AdmitEncounter(cpu, kBossSlot))
    ActRaiserHleFatal("Death Heim arena boss initialization failed");
  /* Hold the untouched native initializer behind the arena arrival beam.
   * Releasing it after the beam preserves multipart setup and native boss
   * entrances while the player lands with every positional gate satisfied. */
  if (!ActRaiserDeathHeim_BeginArrival(cpu, kBossSlot))
    ActRaiserHleFatal("Cannot begin Death Heim arena arrival");
  cpu->X += kActRaiserActionObjectStride;
  cpu_write16(cpu, 0, cpu->X, 0x8000);
  return Tail(0x009311, 0x00930e);
}

bool ActRaiser_DeathHeimArenaRetryEntry(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  const ActRaiserDeathHeimRushPhase phase = ActRaiserDeathHeim_Phase();
  return Context(cpu, 2, 0, true) && encounter && ActRaiserDeathHeim_Active() &&
      (phase == kActRaiserDeathHeimRush_Combat ||
       phase == kActRaiserDeathHeimRush_Final ||
       phase == kActRaiserDeathHeimRush_Retry ||
       (encounter->act == 1 && phase == kActRaiserDeathHeimRush_Preparing)) &&
      cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == encounter->map_group &&
      cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) == encounter->map &&
      cpu_read8(cpu, 0, kActRaiserWram_Lives) != 0 &&
      cpu_read16(cpu, 0, kActRaiserWram_ActionCameraSubject) == kActRaiserWram_PlayerObject &&
      (cpu_read16(cpu, 0, kActRaiserWram_PlayerFlags) & 0x0440) != 0;
}

RecompReturn ActRaiser_DeathHeimArenaRetry(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArenaRetryEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arena retry entry");
  /* Native $BCFC has already accepted a death with a spare life and cleared
   * movement, input and sword power. Replace only its ordinary-city retry
   * table result: that table otherwise sends later boss rooms backwards.
   * HP/life changes remain in the native transition, and zero-life exits
   * never reach this seam. $BD65 also owns the real retry marker increment. */
  const ActRaiserDeathHeimArena *arena = Current(cpu);
  const uint8_t map = arena ? arena->map : cpu_read8(cpu, 2, cpu->X);
  if (!ActRaiserDeathHeim_RetryEncounter(cpu))
    ActRaiserHleFatal("Cannot invalidate the previous Death Heim encounter attempt");
  cpu->A = (uint16_t)((cpu->A & 0xff00) | map);
  cpu_mirrors_to_p(cpu);
  cpu->P &= ~(CPU_P_N | CPU_P_Z);
  if (!map) cpu->P |= CPU_P_Z;
  if (map & 0x80) cpu->P |= CPU_P_N;
  cpu_p_to_mirrors(cpu);
  return Tail(0x02bd65, 0x02bd61);
}
