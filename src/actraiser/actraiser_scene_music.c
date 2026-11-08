#include "actraiser_scene_music.h"
#include "actraiser_death_heim_arena.h"
#include "actraiser_death_heim_runtime.h"
#include "actraiser_game.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
#include "regional/presentation/regional_music.h"

static bool BorrowedArena(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!cpu || !ActRaiserDeathHeim_Active() || !encounter || encounter->act != 1)
    return false;
  const unsigned map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  /* The loader replays every preceding room's asset declaration. Those
   * temporary rooms belong to the same borrowed arena and must stay silent. */
  return cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == encounter->map_group &&
      (map == encounter->map || ActRaiserDeathHeimArenas_Loading(cpu));
}

static bool ExactBorrowedArena(CpuState *cpu) {
  return BorrowedArena(cpu) && cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) ==
      ActRaiserDeathHeim_CurrentEncounter()->map;
}

static RecompReturn Tail(uint32_t target, uint32_t source) {
  if (!cpu_hle_tailcall_request(target, source))
    ActRaiserHleFatal("Scene music lost its native continuation");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_SceneMusicEntry(CpuState *cpu) {
  return cpu && !cpu->emulation && cpu->m_flag && !cpu->x_flag && !cpu->D &&
      cpu->PB==2 && cpu->DB==0 && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      cpu_read8(cpu,0,0x0334)==cpu_read8(cpu,0,0x008d);
}
RecompReturn ActRaiser_SceneMusic(CpuState *cpu) {
  uint8_t profile;
  if(!ActRaiser_SceneMusicEntry(cpu) || !ActRaiserRegional_BeginSceneMusic(&profile) || profile>1)
    ActRaiserHleFatal("Invalid regional scene music boundary");
  /* The command parser has consumed this complete song declaration. Keep
   * Death Heim's resident SPC image and playback cursor through every Act 1
   * prerequisite room and retry. No halt, upload or duplicate play command. */
  if (BorrowedArena(cpu)) return Tail(0x02b69b, 0x02b653);
  /* Only the accepted Fillmore cave declaration. The identical Japanese
   * Fillmore image is already present in the US cartridge at $18:947F.
   * Unrecognized/custom resources, boss music, track/variant selectors and
   * the resident source cache are not changed here. */
  if(ArRegionalMusic_UseFillmore(profile,cpu_read16(cpu,0,0x18)) &&
      cpu_read16(cpu,0,0xa5)==0xf69f && cpu_read8(cpu,0,0xa7)==0x0e &&
      cpu_read8(cpu,0,0x8c)==1 && cpu_read8(cpu,0,0x8d)==0) {
    cpu_write16(cpu, 0, 0xa5, 0x947f);
    cpu_write8(cpu, 0, 0xa7, 0x18);
  }
  /* LDX $A5 only. Native comparison suppresses redundant uploads; the native
   * continuation owns SPC handshakes, resident cache and playback command. */
  cpu->X=cpu_read16(cpu,0,0xa5);
  cpu_mirrors_to_p(cpu); /* Generated arithmetic owns the live flag mirrors. */
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_N|CPU_P_Z)) | (cpu->X?0:CPU_P_Z) |
      (cpu->X&0x8000?CPU_P_N:0));
  cpu_p_to_mirrors(cpu);
  return Tail(0x02b655, 0x02b653);
}

bool ActRaiser_DeathHeimBossMusicEntry(CpuState *cpu) {
  if (!cpu || cpu->emulation || cpu->m_flag || cpu->x_flag || cpu->D ||
      cpu->PB || cpu->DB || cpu->_flag_D || (cpu->P & CPU_P_D) || !ExactBorrowedArena(cpu))
    return false;
  const unsigned slot = cpu->X;
  if (slot < kActRaiserWram_ActionObjectTable ||
      slot >= kActRaiserWram_ActionObjectTable +
          kActRaiserActionObjectCount * kActRaiserActionObjectStride ||
      (slot - kActRaiserWram_ActionObjectTable) % kActRaiserActionObjectStride)
    return false;
  return !(cpu_read16(cpu, 0, slot + kActRaiserActionObject_Status) &
           kActRaiserObjectStatus_InactiveMask) &&
      cpu_read16(cpu, 0, slot + kActRaiserActionObject_Handler) == 0xa3fe &&
      !cpu_read16(cpu, 0, slot + kActRaiserActionObject_LocalCounter);
}

RecompReturn ActRaiser_DeathHeimBossMusic(CpuState *cpu) {
  if (!ActRaiser_DeathHeimBossMusicEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim boss music boundary");
  /* $A3FE is the dedicated music actor, not a BRK/COP sound effect. Skip its
   * attention/halt/upload/play states. Native completion clears the optional
   * boss-parent wait gate and retires the actor, preserving boss progression.
   * Ordinary stages, victory songs and the final boss retain their own path. */
  return Tail(0x00a4b4, 0x00a3fe);
}

bool ActRaiser_DeathHeimRetryMusicEntry(CpuState *cpu) {
  return cpu && !cpu->emulation && !cpu->m_flag && !cpu->x_flag && !cpu->D &&
      cpu->PB == 2 && !cpu->DB && !cpu->_flag_D && !(cpu->P & CPU_P_D) &&
      ExactBorrowedArena(cpu) && cpu_read8(cpu, 0, kActRaiserWram_Lives);
}

RecompReturn ActRaiser_DeathHeimRetryMusic(CpuState *cpu) {
  if (!ActRaiser_DeathHeimRetryMusicEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim retry music boundary");
  /* Native death leaves the song playing, but STZ $AC (M0) invalidates its
   * resident source. Borrowed arenas suppress level uploads, so preserve the
   * cache too: otherwise the next hub return needlessly restarts this song.
   * Skip only that store. Death SFX, animation and zero-life exits stay native. */
  return Tail(0x02bd16, 0x02bd14);
}
