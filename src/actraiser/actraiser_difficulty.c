#include "actraiser_difficulty.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_game.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

static bool Mode(const CpuState *cpu, unsigned bank, unsigned narrow) {
  return cpu && cpu->PB==bank && !cpu->DB && !cpu->D && cpu->m_flag==narrow &&
      !cpu->x_flag && !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D);
}
static bool Slot(unsigned x) {
  return x>=kActRaiserWram_ActionObjectTable &&
      x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
      !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride);
}
static RecompReturn Continue(uint32_t target, uint32_t origin) {
  if (!cpu_hle_tailcall_request(target,origin)) ActRaiserHleFatal("Difficulty prefix has no native owner");
  return RECOMP_RETURN_TAILCALL;
}
static void Compare(CpuState *cpu, uint16_t operand) {
  cpu->_flag_C=cpu->A>=operand;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-operand));
}
bool ActRaiser_DifficultySpawnEntry(CpuState *cpu) {
  const unsigned hp_mode=ActRaiserRegional_DifficultySnapshot().spawn_hp;
  if (hp_mode<1 || hp_mode>3 || !Mode(cpu,0,0) || !Slot(cpu->X) || cpu->Y<0x8000 || cpu->Y>0xfff4)
    return false;
  /* Selected descriptor Y, not +32 (which may still name the old occupant).
   * Earlier base-stat and fresh-flag policies have already composed here. */
  return cpu->A==cpu_read16(cpu,0,cpu->X+0x30) &&
      cpu_read16(cpu,0,cpu->X+0x16)==cpu_read16(cpu,0,cpu->Y) &&
      cpu_read8(cpu,0,cpu->X+0x18)==cpu_read8(cpu,0,cpu->Y+2) &&
      cpu_read16(cpu,0,cpu->X+0x1a)==cpu_read8(cpu,0,cpu->Y+6);
}
RecompReturn ActRaiser_DifficultySpawn(CpuState *cpu) {
  if (!ActRaiser_DifficultySpawnEntry(cpu)) ActRaiserHleFatal("Unsupported difficulty spawn");
  const ArRegionalDifficultySnapshot policy=ActRaiserRegional_DifficultySnapshot();
  const uint16_t flags=cpu->A;
  /* Reproduce PAL's BIT/CMP/INC/DEC flags, then rejoin the US initializer's
   * common zeroing/first-pose tail. No US Professional promotion runs twice. */
  cpu->_flag_Z=(flags&0x8231)==0;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_Z)|(cpu->_flag_Z?CPU_P_Z:0));
  if (! (flags&0x8231)) {
    cpu->A=policy.spawn_hp==1?2:policy.spawn_hp==2?1:3;
    Compare(cpu,2);
    if (cpu->A!=2) {
      Compare(cpu,3);
      cpu->A=cpu_read16(cpu,0,cpu->X+0x2c);
      Compare(cpu,policy.spawn_hp==2?2:1);
      const uint16_t hp=ArRegionalDifficulty_SpawnHp(&policy,flags,cpu->A);
      if (hp!=cpu->A) {
        cpu_write16(cpu,0,cpu->X+0x2c,hp);
        ActRaiserCpuHle_SetNegativeZero16(cpu,hp);
      }
    }
  }
  return Continue(0x00968f,0x00966f);
}
bool ActRaiser_DifficultyContactEntry(CpuState *cpu) {
  return ActRaiserRegional_DifficultySnapshot().contact_extra==1 && Mode(cpu,0,1) &&
      Slot(cpu->X) && Slot(cpu->Y) && cpu->X!=cpu->Y &&
      cpu_read16(cpu,0,0x8a)==cpu->Y && cpu->_flag_C &&
      (cpu_read16(cpu,0,cpu->Y+0x30)&8) && !(cpu_read16(cpu,0,cpu->X+0x30)&0x200);
}
RecompReturn ActRaiser_DifficultyContact(CpuState *cpu) {
  if (!ActRaiser_DifficultyContactEntry(cpu)) ActRaiserHleFatal("Unsupported difficulty contact");
  /* PAL adds in eight bits, then starts a separate SEC/SBC. Preserve wrap,
   * overflow and sign: the native following BPL owns its original HP clamp. */
  const uint8_t hp=(uint8_t)cpu->A, damage=(uint8_t)(cpu_read8(cpu,0,cpu->X+0x2a)+1u);
  const uint8_t value=(uint8_t)(hp-damage);
  cpu->A=(cpu->A&0xff00)|value;
  cpu->_flag_C=hp>=damage;
  cpu->_flag_V=((hp^damage)&(hp^value)&0x80)!=0;
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_C|CPU_P_V))|(cpu->_flag_C?CPU_P_C:0)|(cpu->_flag_V?CPU_P_V:0));
  ActRaiserCpuHle_SetNegativeZero8(cpu,value);
  return Continue(0x008a27,0x008a24);
}
bool ActRaiser_DifficultyTimerEntry(CpuState *cpu) {
  const unsigned reload=ActRaiserRegional_DifficultySnapshot().timer_reload;
  return (reload==71 || reload==47) && Mode(cpu,2,1) &&
      !cpu_read8(cpu,0,0xe8) && (cpu_read8(cpu,0,0xe5)&0x80);
}
RecompReturn ActRaiser_DifficultyTimer(CpuState *cpu) {
  if (!ActRaiser_DifficultyTimerEntry(cpu)) ActRaiserHleFatal("Unsupported difficulty countdown reload");
  cpu->A=(cpu->A&0xff00)|ActRaiserRegional_DifficultySnapshot().timer_reload;
  ActRaiserCpuHle_SetNegativeZero8(cpu,(uint8_t)cpu->A);
  return Continue(0x02bc8c,0x02bc8a);
}
bool ActRaiser_DifficultyDragonEntry(CpuState *cpu) {
  return ActRaiserRegional_DifficultySnapshot().skip_dragon_attack && Mode(cpu,0,0) && Slot(cpu->X) &&
      cpu_read16(cpu,0,0x18)==0x0304 && cpu_read16(cpu,0,cpu->X+0x32)==0xd646 &&
      cpu_read16(cpu,0,cpu->X+0x16)==0x5000 && cpu_read8(cpu,0,cpu->X+0x18)==0x7e &&
      cpu_read16(cpu,0,cpu->X+0x1a)==7 && !cpu_read16(cpu,0,cpu->X+0x3a);
}
RecompReturn ActRaiser_DifficultyDragon(CpuState *cpu) {
  /* Both real JSR D766 callers retain their frame. Return before allocation;
   * already-existing producers/children keep their native lifetime. */
  if (!ActRaiser_DifficultyDragonEntry(cpu)) ActRaiserHleFatal("Unsupported difficulty dragon producer");
  return Continue(0x00d76b,0x00d766);
}
