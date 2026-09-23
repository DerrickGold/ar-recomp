#include "actraiser_score_feedback.h"
#include "actraiser_cpu_hle_internal.h"
#include "regional/regional_score_feedback.h"

bool ActRaiserScoreFeedback_Entry(const CpuState *cpu) {
  return cpu && cpu->PB==3 && cpu->DB==0x7f && !cpu->D && !cpu->emulation &&
      !cpu->m_flag && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}

bool ActRaiserScoreFeedback_Route(CpuState *cpu, bool japanese, uint32_t *continuation) {
  if (!continuation || !ActRaiserScoreFeedback_Entry(cpu) || cpu->X>10 || (cpu->X&1)) return false;
  const uint16_t completed=cpu_read16(cpu,0x7f,(uint16_t)(0x6b18+cpu->X));
  ArRegionalScoreDestination destination;
  ArRegionalScore_Destination(japanese?kArRegionalSource_Japan:kArRegionalSource_US,completed,&destination);
  if (japanese) {
    /* JP LDA / CMP #2, not the US pair of DEC instructions. V is untouched. */
    cpu->A=completed;
    cpu->_flag_C=completed>=2;
    cpu->P=(uint8_t)((cpu->P&~CPU_P_C) | (cpu->_flag_C?CPU_P_C:0));
    ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(completed-2));
  } else {
    cpu->A=(uint16_t)(completed-1);
    if (cpu->A) --cpu->A;
    ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  }
  *continuation=destination==kArRegionalScoreDestination_Growth ? 0x03d0c7 :
      destination==kArRegionalScoreDestination_Stocks ? 0x03d0be : 0x03d0ce;
  return true;
}

bool ActRaiserScoreFeedback_ConvertJP(CpuState *cpu) {
  if (!ActRaiserScoreFeedback_Entry(cpu)) return false;
  const uint16_t bcd=cpu_read16(cpu,0,0x001f);
  uint16_t converted;
  if (!ArRegionalScore_Convert(kArRegionalSource_Japan,bcd,&converted)) return false;
  const unsigned decimal=(bcd&15) + ((bcd>>4)&15)*10 + ((bcd>>8)&15)*100 + (bcd>>12)*1000;
  const unsigned reduced=decimal>650 ? decimal-650 : 0;
  /* The original routine leaves the post-threshold BCD in Y, its low three
   * decimal digits in 7C05, and twice the quotient in 7C07. Preserve these
   * contracts even though the surrounding score wrapper restores registers. */
  cpu->Y=(uint16_t)((reduced%10) | ((reduced/10%10)<<4) |
                    ((reduced/100%10)<<8) | ((reduced/1000)<<12));
  cpu_write16(cpu,0x7f,0x7c05,(uint16_t)(reduced%1000));
  cpu_write16(cpu,0x7f,0x7c07,(uint16_t)(reduced/32*2));
  cpu->A=converted;
  cpu->_flag_C=cpu->_flag_V=cpu->_flag_D=0;
  cpu->P=(uint8_t)(cpu->P & ~(CPU_P_C|CPU_P_V|CPU_P_D));
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  return true;
}

bool ActRaiserScoreFeedback_Subtract(CpuState *cpu) {
  if (!ActRaiserScoreFeedback_Entry(cpu) || cpu->X>40 || (cpu->X&7)) return false;
  const uint16_t amount=cpu_read16(cpu,0,(uint16_t)(cpu->S+1));
  for (unsigned n=0; n<4; ++n) {
    const uint16_t address=(uint16_t)(0x96b8+cpu->X+2*n);
    const uint16_t before=cpu_read16(cpu,0x7f,address);
    const uint16_t subtracted=(uint16_t)(before-amount);
    const bool carry=before>=amount;
    cpu->_flag_C=carry;
    cpu->_flag_V=((before^amount)&(before^subtracted)&0x8000)!=0;
    cpu->P=(uint8_t)((cpu->P&~(CPU_P_C|CPU_P_V)) |
        (cpu->_flag_C?CPU_P_C:0) | (cpu->_flag_V?CPU_P_V:0));
    cpu->A=carry ? subtracted : 0; /* SBC / BCS / LDA #0 */
    ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
    cpu_write16(cpu,0x7f,address,cpu->A);
  }
  return true;
}
