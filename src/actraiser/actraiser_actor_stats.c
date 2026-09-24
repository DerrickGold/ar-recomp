#include "actraiser_actor_stats.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_platform_skull.h"
#include "actraiser_cast_hold.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"
#include "randomizer.h"
typedef struct SpawnOwner { uint16_t source,actor; } SpawnOwner;
/* US initializer addresses stay in this game adapter, never the portable
 * policy. Sorted for lookup at birth only; direct-handler records excluded. */
static const SpawnOwner kOwners[]={
  {0xa9da,0x0102},
  {0xad45,0x0104},
  {0xaf5d,0x010a},
  {0xb0b4,0x010e},
  {0xb155,0x0110},
  {0xb191,0x0112},
  {0xb1af,0x0113},
  {0xb379,0x0117},
  {0xb61a,0x0204},
  {0xbba8,0x0223},
  {0xbd2a,0x0225},
  {0xbdff,0x0227},
  {0xc1a2,0x030b},
  {0xc3a5,0x0306},
  {0xc45f,0x0307},
  {0xc66f,0x0304},
  {0xc80e,0x030c},
  {0xc961,0x0310},
  {0xc9be,0x0311},
  {0xca8b,0x0312},
  {0xcb7b,0x0316},
  {0xceec,0x0405},
  {0xcf2e,0x0406},
  {0xcf9e,0x0408},
  {0xd025,0x0409},
  {0xd033,0x040a},
  {0xd2a2,0x040b},
  {0xd382,0x040c},
  {0xd4a1,0x0414},
  {0xd5b1,0x0416},
  {0xd5c0,0x0417},
  {0xd646,0x040d},
  {0xdc46,0x050b},
  {0xde08,0x0507},
  {0xe0ba,0x0512},
  {0xe18e,0x0513},
  {0xe254,0x0514},
  {0xe292,0x0515},
  {0xe2a6,0x0516},
  {0xe2bd,0x0517},
  {0xe3a1,0x0520},
  {0xe952,0x0609},
  {0xed4b,0x0616},
  {0xefc8,0x061a},
  {0xefd6,0x061b},
  {0xf0e6,0x061c},
  {0xf161,0x0608},
  {0xf6ca,0x0700},
  {0xf6e2,0x0701},
  {0xf6fa,0x0702},
  {0xf712,0x0703},
  {0xf72a,0x0704},
  {0xf751,0x0715},
  {0xf760,0x0705},
};
static bool Plan(CpuState *cpu,uint16_t *hp,uint16_t *attack) {
  if(!ActRaiserRegional_ActorStatsEnabled() || !cpu || cpu->PB || cpu->DB || cpu->D ||
      cpu->m_flag || cpu->x_flag || cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  if(x<kActRaiserWram_ActionObjectTable ||
      x>=kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride ||
      (x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride)return false;
  unsigned lo=0,hi=sizeof(kOwners)/sizeof(kOwners[0]);
  while(lo<hi){const unsigned mid=lo+(hi-lo)/2;if(kOwners[mid].source<cpu->Y)lo=mid+1;else hi=mid;}
  if(lo==sizeof(kOwners)/sizeof(kOwners[0]) || kOwners[lo].source!=cpu->Y ||
      (kOwners[lo].actor>>8)!=cpu_read8(cpu,0,kActRaiserWram_MapGroup))return false;
  /* Y is the selected initializer. Its source pointer is installed only after
   * this routine returns. Require the copied animation identity, never +32. */
  if(cpu_read16(cpu,0,x+0x16)!=cpu_read16(cpu,0,cpu->Y) ||
      cpu_read8(cpu,0,x+0x18)!=cpu_read8(cpu,0,cpu->Y+2) ||
      cpu_read16(cpu,0,x+0x1a)!=cpu_read8(cpu,0,cpu->Y+6))return false;
  const uint16_t copied_hp=cpu_read16(cpu,0,x+0x2c),copied_attack=cpu_read16(cpu,0,x+0x2a);
  RandomizerSpawnStatBasis basis;
  /* Native allocation copied the already-scaled US ROM. Select the regional
   * base from pristine bytes, then apply that same scale exactly once. This
   * preserves the policy's stock-value guards, even after rounding/clamping. */
  if(!Randomizer_SpawnStatBasis(cpu->Y&0x7fff,copied_hp,copied_attack,&basis) ||
      !ActRaiserRegional_ActorStats(kOwners[lo].actor,basis.hp,basis.attack,hp,attack))return false;
  *hp=Randomizer_ScaleStat((uint8_t)*hp,basis.scale.hp_percent);
  *attack=Randomizer_ScaleStat((uint8_t)*attack,basis.scale.attack_percent);
  return *hp!=copied_hp || *attack!=copied_attack;
}
bool ActRaiser_ActorStatsEntry(CpuState *cpu) {
  uint16_t hp,attack;return Plan(cpu,&hp,&attack) || ActRaiser_PlatformSkullSpawnEntry(cpu) || ActRaiser_CastHoldSpawnEntry(cpu);
}
RecompReturn ActRaiser_ActorStats(CpuState *cpu) {
  uint16_t hp,attack;const bool stats=Plan(cpu,&hp,&attack),skull=ActRaiser_PlatformSkullSpawnEntry(cpu),hold=ActRaiser_CastHoldSpawnEntry(cpu);
  if(!stats && !skull && !hold)ActRaiserHleFatal("Unsupported regional actor initializer");
  if(stats){cpu_write16(cpu,0,cpu->X+0x2c,hp);cpu_write16(cpu,0,cpu->X+0x2a,attack);}
  if(skull)return ActRaiser_PlatformSkullSpawn(cpu);
  if(hold)return ActRaiser_CastHoldSpawn(cpu);
  cpu->A=cpu_read16(cpu,0,cpu->X+0x30);ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00966f,0x00966c))ActRaiserHleFatal("Actor stats have no native continuation");
  return RECOMP_RETURN_TAILCALL;
}

static bool TanzraChild(CpuState *cpu,uint16_t handler,uint16_t flags) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  if(x<kActRaiserWram_ActionObjectTable ||
      x>=kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride ||
      (x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride)return false;
  /* The native allocator copies the owner's source/animation identity and
   * installs this child handler. Do not require the parent's current state:
   * it may already have advanced, and no parent memory needs to be touched. */
  return cpu_read16(cpu,0,kActRaiserWram_MapGroup)==0x0807 &&
      cpu_read16(cpu,0,x+0x32)==0xf80f && cpu_read16(cpu,0,x+0x16)==0x5000 &&
      cpu_read8(cpu,0,x+0x18)==0x7e && cpu_read16(cpu,0,x+0x12)==handler &&
      cpu_read16(cpu,0,x)==0 && cpu_read16(cpu,0,x+0x30)==flags;
}
static uint16_t TanzraMinionHp(void) {
  const uint16_t base=ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionHp);
  return base==1 || base==2 ? Randomizer_ScaleStat((uint8_t)base,Randomizer_AppliedStatScale().hp_percent) : UINT16_MAX;
}
static uint16_t TanzraProjectileAttack(void) {
  const uint16_t base=ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraProjectileAttack);
  return base>=3 && base<=5 ? Randomizer_ScaleStat((uint8_t)base,Randomizer_AppliedStatScale().attack_percent) : UINT16_MAX;
}
bool ActRaiser_TanzraMinionHpEntry(CpuState *cpu) {
  const uint16_t hp=TanzraMinionHp();
  return hp!=UINT16_MAX && hp!=2 &&
      TanzraChild(cpu,0xfc8d,0) && cpu->A==2;
}
bool ActRaiser_TanzraMinionRewardEntry(CpuState *cpu) {
  return ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionReward)==1 &&
      TanzraChild(cpu,0xfc8d,0) && cpu->A==2;
}
bool ActRaiser_TanzraProjectileAttackEntry(CpuState *cpu) {
  const uint16_t attack=TanzraProjectileAttack();
  return attack!=UINT16_MAX && attack!=3 && TanzraChild(cpu,0xfd25,0x20) && cpu->A==0x20;
}
RecompReturn ActRaiser_TanzraMinionHp(CpuState *cpu) {
  if(!ActRaiser_TanzraMinionHpEntry(cpu))ActRaiserHleFatal("Unsupported Tanzra minion HP initializer");
  /* Replace STA's value, not the live accumulator: the following independent
   * reward store must still see the native A=2. STA does not modify flags. */
  cpu_write16(cpu,0,cpu->X+0x2c,TanzraMinionHp());
  if(!cpu_hle_tailcall_request(0x00fc99,0x00fc96))ActRaiserHleFatal("Minion HP has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_TanzraMinionReward(CpuState *cpu) {
  if(!ActRaiser_TanzraMinionRewardEntry(cpu))ActRaiserHleFatal("Unsupported Tanzra minion reward initializer");
  cpu_write16(cpu,0,cpu->X+0x2e,1);
  if(!cpu_hle_tailcall_request(0x00fc9c,0x00fc99))ActRaiserHleFatal("Minion reward has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_TanzraProjectileAttack(CpuState *cpu) {
  if(!ActRaiser_TanzraProjectileAttackEntry(cpu))ActRaiserHleFatal("Unsupported Tanzra projectile initializer");
  cpu->A=TanzraProjectileAttack();
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00fd31,0x00fd2e))ActRaiserHleFatal("Tanzra projectile has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
