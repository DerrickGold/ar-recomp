#include "actraiser_lair_history.h"
#include "byte_order.h"
#include "actraiser_cpu_hle_internal.h"

bool ActRaiserLairHistory_ReadSavedStocks(const uint8_t image[kActRaiserSramSize],
                                         uint16_t out[kArRegionalLairCount]) {
  if (!image || !out || !Save_ChecksumValid(image)) return false;
  for (unsigned i=0; i<kArRegionalLairCount; ++i)
    out[i]=ByteOrder_ReadLe16(image+0x1603+2*i);
  return true;
}

bool ActRaiserLairHistory_Entry(const CpuState *cpu) {
  return cpu && cpu->PB==3 && !cpu->D && !cpu->x_flag && !cpu->emulation &&
      !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
bool ActRaiserLairHistory_ProjectionEntry(const CpuState *cpu) {
  return cpu && (cpu->PB==0 || cpu->PB==3) && !cpu->D && !cpu->x_flag &&
      !cpu->emulation && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}

bool ActRaiserLairHouse_Units(CpuState *cpu, bool tiered) {
  if (!ActRaiserLairHistory_Entry(cpu) || cpu->m_flag) return false;
  cpu->A = tiered ? (cpu->A & 0xff) : 4;
  cpu->Y = cpu->A;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->Y);
  return true;
}

ArRegionalLairProjectionResult ActRaiserLairHistory_Project(ArRegionalLairHistory *history,
    CpuState *cpu, const ArRegionalLairAccounting *current,
    const ArRegionalLairAccounting *target) {
  if (!ActRaiserLairHistory_ProjectionEntry(cpu)) return kArRegionalLairProjection_Invalid;
  uint16_t native[kArRegionalLairCount], next[kArRegionalLairCount];
  for (unsigned i=0; i<kArRegionalLairCount; ++i)
    native[i]=cpu_read16(cpu,0x7f,(uint16_t)(0x96b8+2*i));
  const ArRegionalLairProjectionResult result=
      ArRegionalLairHistory_Project(history,current,target,native,next);
  if (result==kArRegionalLairProjection_Mismatch)
    ActRaiserLairHistory_Check(history,cpu,current);
  if (result!=kArRegionalLairProjection_Ready) return result;
  for (unsigned i=0; i<kArRegionalLairCount; ++i)
    if (native[i]!=next[i]) cpu_write16(cpu,0x7f,(uint16_t)(0x96b8+2*i),next[i]);
  return result;
}
static bool Matches(const ArRegionalLairHistory *history, CpuState *cpu,
                    const ArRegionalLairAccounting *policy, unsigned town) {
  for (unsigned i=town*4;i<town*4+4;++i) {
    uint16_t expected;
    if (!ArRegionalLairHistory_Read(history,policy,i,&expected) ||
        expected!=cpu_read16(cpu,0x7f,(uint16_t)(0x96b8+2*i))) return false;
  }
  return true;
}
bool ActRaiserLairHistory_Initialize(ArRegionalLairHistory *history, CpuState *cpu) {
  if (!cpu || !ArRegionalLairHistory_Valid(history) || history->initialized_towns) return false;
  ArRegionalLairHistory next={0}; const ArRegionalLairAccounting us={0};
  for (unsigned town=0;town<6;++town)
    if (!ArRegionalLairHistory_InitTown(&next,town) || !Matches(&next,cpu,&us,town)) return false;
  *history=next;
  return true;
}
bool ActRaiserLairHistory_Begin(ArRegionalLairHistory *history, CpuState *cpu,
    ActRaiserLairEvent event, const ArRegionalLairAccounting *policy,
    ActRaiserLairCapture *capture) {
  unsigned projection;
  if (!capture || !ActRaiserLairHistory_Entry(cpu) || !ArRegionalLairHistory_Valid(history) ||
      (unsigned)event>=kActRaiserLairEvent_Count ||
      !ArRegionalLairAccounting_Projection(policy,&projection)) return false;
  const unsigned town_word=cpu_read16(cpu,0x7f,0x7bfb);
  const unsigned town=event==kActRaiserLairEvent_Score ?
      (unsigned)cpu_read8(cpu,0,0x0341)-1 : town_word/2;
  if (town>=6 || (event!=kActRaiserLairEvent_Score && (town_word & 1)) ||
      !(history->initialized_towns & (1u<<town)) || history->diverged_towns & (1u<<town)) return false;
  if (!Matches(history,cpu,policy,town)) {
    ArRegionalLairHistory_MarkDiverged(history,town); return false;
  }
  ActRaiserLairCapture next={.event=event,.policy=*policy,.town=town};
  for (unsigned n=0;n<4;++n) {
    const uint16_t flags=cpu_read16(cpu,0x7f,(uint16_t)(0x95c8+town*8+n*2));
    if (flags & 0x8000) next.sealed_mask|=1u<<n;
    if (event==kActRaiserLairEvent_Kill) {
      /* BADD uses the first matching native actor-record address, even at
       * stock zero or with a sealed flag. Later matches must not also debit. */
      if (!next.candidates && cpu_read16(cpu,0x7f,(uint16_t)(0x9688+town*8+n*2))==cpu->X)
        next.candidates=1u<<n;
    } else if (event==kActRaiserLairEvent_Miracle && !(flags & 0xc000)) {
      const unsigned kind=cpu_read16(cpu,0x7f,0x90eb);
      if (kind!=2 && kind!=3 && !cpu_read16(cpu,0x7f,0x90f5)) next.candidates|=1u<<n;
    }
  }
  if (event==kActRaiserLairEvent_House)
    next.subtype=cpu_read8(cpu,cpu->DB,(uint16_t)(cpu->X+2));
  if (event==kActRaiserLairEvent_Score) {
    next.bcd_score=cpu_read16(cpu,0,0x001f);
    next.completed_acts=cpu_read16(cpu,0x7f,(uint16_t)(0x6b18+town*2));
  }
  *capture=next;
  return true;
}
bool ActRaiserLairHistory_End(ArRegionalLairHistory *history, CpuState *cpu,
                            const ActRaiserLairCapture *capture, RecompReturn result) {
  if (!cpu || !capture || capture->town>=6 || !ArRegionalLairHistory_Valid(history)) return false;
  ArRegionalLairHistory next=*history;
  bool valid=result==RECOMP_RETURN_NORMAL;
  if (valid) switch(capture->event) {
    case kActRaiserLairEvent_Kill:
    case kActRaiserLairEvent_Miracle:
      for(unsigned n=0;n<4 && valid;++n) if(capture->candidates & (1u<<n))
        valid=capture->event==kActRaiserLairEvent_Kill ?
            ArRegionalLairHistory_KillAttempt(&next,capture->town*4+n) :
            ArRegionalLairHistory_MiracleAttempt(&next,capture->town*4+n);
      break;
    case kActRaiserLairEvent_House:
      valid=ArRegionalLairHistory_HouseLost(&next,capture->town,capture->subtype,capture->sealed_mask);
      break;
    case kActRaiserLairEvent_Score:
      valid=ArRegionalLairHistory_SettleScore(&next,capture->town,capture->bcd_score,capture->completed_acts);
      break;
    default: valid=false; break;
  }
  if (!valid || !Matches(&next,cpu,&capture->policy,capture->town)) {
    ArRegionalLairHistory_MarkDiverged(history,capture->town); return false;
  }
  *history=next;
  return true;
}
bool ActRaiserLairHistory_Check(ArRegionalLairHistory *history, CpuState *cpu,
                              const ArRegionalLairAccounting *policy) {
  unsigned projection;
  if (!cpu || !ArRegionalLairHistory_Valid(history) ||
      !ArRegionalLairAccounting_Projection(policy,&projection)) return false;
  for (unsigned town=0;town<6;++town) {
    if (!(history->initialized_towns & (1u<<town)) || history->diverged_towns & (1u<<town)) continue;
    if (!Matches(history,cpu,policy,town)) ArRegionalLairHistory_MarkDiverged(history,town);
  }
  return !history->diverged_towns;
}
