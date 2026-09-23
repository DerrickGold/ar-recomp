#include "actraiser_boss_rules.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"

static bool RootShape(CpuState *cpu,uint16_t original,uint16_t rematch,unsigned area,unsigned state,unsigned narrow) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->m_flag!=narrow || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  if(x<kActRaiserWram_ActionObjectTable ||
      x>=kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride ||
      (x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride)return false;
  const uint16_t source=cpu_read16(cpu,0,x+kActRaiserActionObject_SourceDescriptor);
  const unsigned map=cpu_read8(cpu,0,kActRaiserWram_MapGroup);
  return ((map==area && source==original) || (map==7 && source==rematch)) &&
      cpu_read16(cpu,0,x+kActRaiserActionObject_AnimationAddress)==0x5000 &&
      cpu_read8(cpu,0,x+kActRaiserActionObject_AnimationBank)==0x7e &&
      cpu_read16(cpu,0,x+kActRaiserActionObject_AnimationState)==state;
}
bool ActRaiser_WizardPauseEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_WizardPause)==0 &&
      RootShape(cpu,0xbdff,0xf6e2,2,0x0b,0);
}
RecompReturn ActRaiser_WizardPause(CpuState *cpu) {
  /* Only the new hold is skipped. Native position/HP decisions, first/second
   * form transitions and all rematch animation differences remain unchanged. */
  if(!ActRaiser_WizardPauseEntry(cpu) || !cpu_hle_tailcall_request(0x00be7e,0x00be78))
    ActRaiserHleFatal("Unsupported Wizard post-spread pause");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_MinotaurAxeOffsetEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_MinoAxeOffset)==48 &&
      RootShape(cpu,0xaf5d,0xf6ca,1,1,0);
}
RecompReturn ActRaiser_MinotaurAxeOffset(CpuState *cpu) {
  if(!ActRaiser_MinotaurAxeOffsetEntry(cpu))ActRaiserHleFatal("Unsupported Minotaur axe offset");
  cpu->A=(uint16_t)-48;
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  /* The original 8709 facing-relative helper owns Y's destination, including
   * the allocator's scratch record on a dropped shot. Do not allocate again
   * or create a host actor for that out-of-pool address. */
  if(!cpu_hle_tailcall_request(0x00afde,0x00afdb))
    ActRaiserHleFatal("Minotaur offset has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_TanzraClockEntry(CpuState *cpu) {
  if(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_TanzraClock)!=0 ||
      !RootShape(cpu,0xf80f,0xf80f,7,0xff,1))return false;
  const unsigned x=cpu->X;
  return cpu_read16(cpu,0,kActRaiserWram_MapGroup)==0x0807 &&
      cpu_read16(cpu,0,x)==0x0800 && cpu_read16(cpu,0,x+0x12)==0xf8f5 &&
      cpu_read16(cpu,0,x+0x14)==0xf8f5 && !cpu_read16(cpu,0,x+0x2c) &&
      cpu_read16(cpu,0,x+0x30)==0x32;
}
RecompReturn ActRaiser_TanzraClock(CpuState *cpu) {
  /* Skip only the 8-bit STZ E8. Keep elapsed time/divider, the high gate byte,
   * existing collision/cast clears, and the native REP/second-form entry. */
  if(!ActRaiser_TanzraClockEntry(cpu) || !cpu_hle_tailcall_request(0x00f8fe,0x00f8fc))
    ActRaiserHleFatal("Unsupported Tanzra second-form clock transition");
  return RECOMP_RETURN_TAILCALL;
}
static bool AntlionShape(CpuState *cpu,unsigned state) {
  return RootShape(cpu,0xc66f,0,3,state,0) && cpu_read16(cpu,0,kActRaiserWram_MapGroup)==0x0203;
}
static bool PlantRootShape(CpuState *cpu,unsigned *state) {
  if(!cpu)return false;
  const unsigned previous=cpu_read16(cpu,0,cpu->X+0x1a);
  if(!RootShape(cpu,0xd974,0,5,previous,0) || cpu_read16(cpu,0,0x18)!=0x0305 ||
      cpu_read16(cpu,0,cpu->X+0x3a) || (previous!=0 && previous!=1 && previous!=2 && previous!=3 && previous!=20))return false;
  *state=previous;return true;
}
bool ActRaiser_PlantPhaseEntry(CpuState *cpu) {
  unsigned previous;ArRegionalPlantPhase phase;
  if(!PlantRootShape(cpu,&previous))return false;
  return ArRegionalBoss_PlantPhase(ActRaiserRegional_BossSnapshot(),previous,&phase) ||
      (previous==20 && (cpu_read16(cpu,0,cpu->X+0x30)&0x20));
}
RecompReturn ActRaiser_PlantPhase(CpuState *cpu) {
  unsigned previous;if(!ActRaiser_PlantPhaseEntry(cpu) || !PlantRootShape(cpu,&previous))
    ActRaiserHleFatal("Unsupported Marahna plant phase");
  ArRegionalPlantPhase phase={2,99};
  (void)ArRegionalBoss_PlantPhase(ActRaiserRegional_BossSnapshot(),previous,&phase);
  uint16_t flags=cpu_read16(cpu,0,cpu->X+0x30);
  if(phase.state==20)flags|=0x20;
  else if(previous==20)flags&=(uint16_t)~0x20;
  cpu_write16(cpu,0,cpu->X+0x30,flags);
  /* Native 8669 owns repeat counts, decoding, wait frames and termination.
   * Reuse its actual D9DE JSR. After a cacheless debug restore, clear a
   * completed closed phase before falling back to the US open loop. */
  cpu->A=(uint16_t)((unsigned)phase.state<<8|phase.repetitions);
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00d9de,0x00d9db))ActRaiserHleFatal("Plant phase has no sequence continuation");
  return RECOMP_RETURN_TAILCALL;
}
static bool PharaohHeadShape(CpuState *cpu,unsigned state) {
  return RootShape(cpu,0xc1a2,0xf6fa,3,state,0) &&
      cpu_read8(cpu,0,kActRaiserWram_MapGroup+1)==(cpu_read8(cpu,0,kActRaiserWram_MapGroup)==3?6:4);
}
bool ActRaiser_PharaohHeadIdleEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PharaohHeads)==1 &&
      PharaohHeadShape(cpu,2);
}
RecompReturn ActRaiser_PharaohHeadIdle(CpuState *cpu) {
  if(!ActRaiser_PharaohHeadIdleEntry(cpu))ActRaiserHleFatal("Unsupported Pharaoh head idle");
  /* Reuse the real JSR8657 and its native return/yield frame. State3 is the
   * retained 120-update idle; allocation success or failure arrives here. */
  cpu->A=3;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00c2ce,0x00c2cb))ActRaiserHleFatal("Pharaoh idle has no continuation");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_PharaohHeadRepeatEntry(CpuState *cpu) {
  /* A native state3 return can only come from the active expanded loop.
   * Finish that idle even if a debug restore has lost its host policy cache.
   * No live-parent lookup: arrows can outlive the head that launched them. */
  return PharaohHeadShape(cpu,3) && cpu_read16(cpu,0,cpu->X+0x1e)==0xc2d0;
}
RecompReturn ActRaiser_PharaohHeadRepeat(CpuState *cpu) {
  if(!ActRaiser_PharaohHeadRepeatEntry(cpu) || !cpu_hle_tailcall_request(0x00c2a1,0x00c2d1))
    ActRaiserHleFatal("Unsupported Pharaoh head repeat");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_ViperChoiceEntry(CpuState *cpu) {
  const uint16_t choice=ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperChoice);
  return (choice==1 || choice==2) &&
      RootShape(cpu,0xe483,0xf72a,5,10,0) &&
      cpu_read8(cpu,0,kActRaiserWram_MapGroup+1)==(cpu_read8(cpu,0,kActRaiserWram_MapGroup)==5?8:6);
}
RecompReturn ActRaiser_ViperChoice(CpuState *cpu) {
  if(!ActRaiser_ViperChoiceEntry(cpu))ActRaiserHleFatal("Unsupported Viper lightning decision");
  /* JP LSR/BCS and PAL AND1/BNE choose the same half of RNG inputs,
   * but their accumulator and carry contracts are distinct. */
  const bool other=cpu->A&1;
  if(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperChoice)==1) {
    cpu->_flag_C=other;cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(other?CPU_P_C:0));
    cpu->A>>=1;
  } else cpu->A&=1;
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(other?0x00e4f7:0x00e4e0,0x00e4db))ActRaiserHleFatal("Viper choice has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
static bool DragonProjectileShape(CpuState *cpu,unsigned state) {
  return RootShape(cpu,0xd646,0,4,state,0) && cpu_read16(cpu,0,kActRaiserWram_MapGroup)==0x0304;
}
bool ActRaiser_DragonFlightBeginEntry(CpuState *cpu) {
  if(!cpu || ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_DragonProjectileFlight)!=5)return false;
  const unsigned state=cpu_read16(cpu,0,cpu->X+0x38);
  /* Children inherit the producer's state0, but their local word selects1/2. */
  return (state==1 || state==2) && DragonProjectileShape(cpu,0) &&
      cpu_read16(cpu,0,cpu->X+0x12)==0xa655;
}
bool ActRaiser_DragonFlightRepeatEntry(CpuState *cpu) {
  if(!cpu)return false;
  const unsigned local=cpu_read16(cpu,0,cpu->X+0x38),state=local&255,count=local>>8;
  /* Once initialized, the actor owns the repetition count. Honor it even
   * after a debug restore without a host policy cache; native A658 must
   * never receive a packed counter as an animation-state index. */
  return (state==1 || state==2) && count>=1 && count<=4 && DragonProjectileShape(cpu,state) &&
      cpu_read16(cpu,0,cpu->X+0x1e)==0xa65d;
}
static RecompReturn DragonSequence(CpuState *cpu,unsigned local,uint32_t from) {
  /* The native A655 projectile uses +38 only as its sequence selector. Its
   * high byte holds four extra repetitions until zero; 8657 does not use it.
   * Feed only the original low-byte state to the real JSR. No host timers,
   * animation-pointer mutation or invented native return addresses. */
  cpu_write16(cpu,0,cpu->X+0x38,(uint16_t)local);
  cpu->A=(uint16_t)(local&255);ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00a65b,from))ActRaiserHleFatal("Dragon projectile has no animation continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_DragonFlightBegin(CpuState *cpu) {
  if(!ActRaiser_DragonFlightBeginEntry(cpu))ActRaiserHleFatal("Unsupported dragon projectile birth");
  cpu_write16(cpu,0,cpu->X,0); /* original STZ */
  return DragonSequence(cpu,cpu_read16(cpu,0,cpu->X+0x38)|0x400,0x00a655);
}
RecompReturn ActRaiser_DragonFlightRepeat(CpuState *cpu) {
  if(!ActRaiser_DragonFlightRepeatEntry(cpu))ActRaiserHleFatal("Unsupported dragon projectile repeat");
  return DragonSequence(cpu,cpu_read16(cpu,0,cpu->X+0x38)-0x100,0x00a65e);
}
static void Compare(CpuState *cpu,uint16_t value) {
  cpu->_flag_C=cpu->A>=value;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
bool ActRaiser_AntlionTriggerEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_AntlionTrigger)==2304 && AntlionShape(cpu,0);
}
RecompReturn ActRaiser_AntlionTrigger(CpuState *cpu) {
  if(!ActRaiser_AntlionTriggerEntry(cpu))ActRaiserHleFatal("Unsupported Antlion introduction threshold");
  Compare(cpu,2304);
  if(!cpu_hle_tailcall_request(0x00c680,0x00c67d))ActRaiserHleFatal("Antlion trigger has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_AntlionVolleyEntry(CpuState *cpu) {
  return ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_AntlionStrategy)==1 && AntlionShape(cpu,4);
}
RecompReturn ActRaiser_AntlionVolley(CpuState *cpu) {
  /* The six native projectiles have already been allocated. Skip only state12;
   * the distance helper still owns cached-player/facing semantics. */
  if(!ActRaiser_AntlionVolleyEntry(cpu) || !cpu_hle_tailcall_request(0x00c71e,0x00c718))
    ActRaiserHleFatal("Unsupported Antlion post-volley hold");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_AntlionDecisionEntry(CpuState *cpu) {
  return ActRaiser_AntlionVolleyEntry(cpu);
}
RecompReturn ActRaiser_AntlionDecision(CpuState *cpu) {
  if(!ActRaiser_AntlionDecisionEntry(cpu))ActRaiserHleFatal("Unsupported Antlion post-volley decision");
  Compare(cpu,64);
  if(!cpu->_flag_C) {
    if(!cpu_hle_tailcall_request(0x00c726,0x00c721))ActRaiserHleFatal("Antlion near branch has no continuation");
    return RECOMP_RETURN_TAILCALL;
  }
  /* Native 86FA coroutine contract without fabricating a JSR return word:
   * halt motion, store delay60, resume the existing preparation at C6E3.
   * The original dispatcher decrements +24 and owns the 61-update wait;
   * death, room exit and slot reuse discard it with the native actor.
   * Use this boss's native RTS leaf to consume the real emulated return.
   * A bare NORMAL return would leave a dispatcher return word on the stack. */
  cpu_write16(cpu,0,cpu->X+0x06,0);cpu_write16(cpu,0,cpu->X+0x08,0);
  cpu_write16(cpu,0,cpu->X+0x24,60);cpu_write16(cpu,0,cpu->X+0x12,0xc6e3);
  cpu->A=0xc6e3;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00c682,0x00c721))ActRaiserHleFatal("Antlion wait has no native RTS owner");
  return RECOMP_RETURN_TAILCALL;
}
