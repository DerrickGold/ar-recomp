#include "actraiser_mode_entry.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_02_ABC4_M1X0(CpuState *cpu);
extern RecompReturn ActRaiser_WaitForVblank(CpuState *cpu);
static bool s_extended,s_continue,s_title_target;
static bool Entry(CpuState *cpu,unsigned db) {
  return cpu && cpu->PB==2 && cpu->DB==db && !cpu->D && cpu->m_flag &&
      !cpu->x_flag && !cpu->emulation;
}
static RecompReturn Tail(unsigned target,unsigned owner) {
  if(!cpu_hle_tailcall_request(target,owner))ActRaiserHleFatal("Mode entry has no native owner");
  return RECOMP_RETURN_TAILCALL;
}
static void Load8(CpuState *cpu,uint8_t value) {
  cpu->A=(cpu->A&0xff00)|value;ActRaiserCpuHle_SetNegativeZero8(cpu,value);
}
static bool UnlockedRequest(void) {
  uint8_t rules;return ActRaiserRegional_ModeEntry(false,&rules) && (rules&1);
}
bool ActRaiser_ModeTitleGateEntry(CpuState *cpu) {return Entry(cpu,2);}
RecompReturn ActRaiser_ModeTitleGate(CpuState *cpu) {
  if(!ActRaiser_ModeTitleGateEntry(cpu))ActRaiserHleFatal("Unsupported title checksum branch");
  uint8_t rules;
  if(!ActRaiserRegional_ModeEntry(true,&rules))ActRaiserHleFatal("Cannot capture title mode rules");
  /* The ORIGINAL checksum helper has just returned. Preserve its result for
   * Continue eligibility; never forge SRAM/checksums or completion markers. */
  s_continue=!cpu->_flag_C;s_extended=(rules&1)!=0;s_title_target=false;
  return Tail(s_continue || s_extended?0x02a72f:0x02a70f,0x02a70d);
}
bool ActRaiser_ModeLateMenuEntry(CpuState *cpu) {return Entry(cpu,2) && UnlockedRequest();}
RecompReturn ActRaiser_ModeLateMenu(CpuState *cpu) {
  if(!ActRaiser_ModeLateMenuEntry(cpu))ActRaiserHleFatal("Unsupported late title mode menu");
  uint8_t rules;
  if(!ActRaiserRegional_ModeEntry(true,&rules) || !(rules&1))ActRaiserHleFatal("Cannot capture late mode choice");
  s_extended=true;
  /* No-save Start prompt has accepted input. A newly enabled Action choice
   * enters the original selector instead of silently starting Story. */
  return Tail(0x02a72f,0x02a72d);
}
bool ActRaiser_ModeInitialLabelEntry(CpuState *cpu) {
  return cpu && cpu->PB==2 && cpu->DB==2 && !cpu->D && !cpu->m_flag &&
      !cpu->x_flag && !cpu->emulation && s_extended && !s_continue;
}
RecompReturn ActRaiser_ModeInitialLabel(CpuState *cpu) {
  if(!ActRaiser_ModeInitialLabelEntry(cpu))ActRaiserHleFatal("Unsupported initial mode label");
  cpu->Y=0xaa4a;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->Y);
  return Tail(0x02a74b,0x02a748);
}
bool ActRaiser_ModeInitialChoiceEntry(CpuState *cpu) {return Entry(cpu,2) && s_extended;}
RecompReturn ActRaiser_ModeInitialChoice(CpuState *cpu) {
  if(!ActRaiser_ModeInitialChoiceEntry(cpu))ActRaiserHleFatal("Unsupported initial mode choice");
  Load8(cpu,s_continue?1:0);cpu_write8(cpu,0,0x336,(uint8_t)cpu->A);
  /* The paired A748 prefix selects the same label before the native fade. */
  return Tail(0x02a756,0x02a751);
}
bool ActRaiser_ModeNextChoiceEntry(CpuState *cpu) {return Entry(cpu,2) && (s_extended || UnlockedRequest());}
RecompReturn ActRaiser_ModeNextChoice(CpuState *cpu) {
  if(!ActRaiser_ModeNextChoiceEntry(cpu))ActRaiserHleFatal("Unsupported mode selection");
  if(!s_extended) {
    uint8_t rules;
    if(!ActRaiserRegional_ModeEntry(true,&rules) || !(rules&1))ActRaiserHleFatal("Cannot extend title choices");
    s_extended=true;
  }
  const uint8_t choice=ArRegionalMode_NextChoice(cpu_read8(cpu,0,0x336),s_continue);
  cpu->X=choice;Load8(cpu,choice);
  return Tail(0x02a813,0x02a7e9);
}
bool ActRaiser_ModeGameOverEntry(CpuState *cpu) {
  uint8_t rules;
  return Entry(cpu,0) && cpu->S==0x1ff && cpu_read8(cpu,0,0x349) &&
      cpu_read8(cpu,0,0x349)<14 && ActRaiserRegional_ModeEntry(false,&rules) && (rules&2);
}
RecompReturn ActRaiser_ModeGameOver(CpuState *cpu) {
  if(!ActRaiser_ModeGameOverEntry(cpu))ActRaiserHleFatal("Unsupported Action Game Over return");
  uint8_t rules;
  if(!ActRaiserRegional_ModeEntry(true,&rules) || !(rules&2))ActRaiserHleFatal("Cannot capture Game Over mode rules");
  /* PAL waits once more after Start. Reuse the real AAEF wait call boundary,
   * then the retained US synchronous tilemap clear, with no fabricated JSR. */
  const RecompReturn waited=ActRaiserNativeCall(cpu,ActRaiser_WaitForVblank,2,0xaaf1,false);
  if(waited!=RECOMP_RETURN_NORMAL)return waited;
  cpu_write8(cpu,0,0x4200,1);cpu_write8(cpu,0,0x2100,0x80);
  if(!cpu_invoke_rts_leaf(cpu,bank_02_ABC4_M1X0,0x02abc4))
    ActRaiserHleFatal("Game Over map clear escaped its leaf contract");
  cpu_write8(cpu,0,0x349,0);cpu_write8(cpu,0,0x347,0);cpu_write8(cpu,0,0xe4,0);
  for(unsigned i=0;i<14;++i)cpu_write8(cpu,0x7f,0x6b18+i,0);
  cpu->X=14;Load8(cpu,0);cpu->_flag_C=1;cpu->P|=CPU_P_C;
  if(!ActRaiserRegional_ReturnToTitle())ActRaiserHleFatal("Cannot preserve new-run title choices");
  s_title_target=true;
  /* Keep AAFD's PHA. The destination and terminal PHX/RTL transfer below
   * retire the discarded host activations before entering native title setup. */
  return Tail(0x02aafd,0x02aaf9);
}
bool ActRaiser_ModeReturnTargetEntry(CpuState *cpu) {return Entry(cpu,0) && s_title_target && cpu->S==0x1fe;}
RecompReturn ActRaiser_ModeReturnTarget(CpuState *cpu) {
  if(!ActRaiser_ModeReturnTargetEntry(cpu))ActRaiserHleFatal("Unsupported title return destination");
  cpu->X=0x8023;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->X);
  return Tail(0x02ab03,0x02ab00);
}
bool ActRaiser_ModeReturnRootEntry(CpuState *cpu) {
  return ActRaiser_ModeReturnTargetEntry(cpu) && cpu->X==0x8023 && cpu_read8(cpu,0,0x1ff)==0;
}
RecompReturn ActRaiser_ModeReturnRoot(CpuState *cpu) {
  if(!ActRaiser_ModeReturnRootEntry(cpu))ActRaiserHleFatal("Unsupported terminal title transfer");
  /* Exact AB03 PHX / AB04 RTL stack writes and final S. This synthetic native
   * long jump has no surviving caller: do not nest a registry driver on it. */
  cpu_write16(cpu,0,0x1fd,cpu->X);cpu->S=0x1ff;
  if(!cpu_begin_reset_tail(cpu,0x008024,0x02ab04))
    ActRaiserHleFatal("Title transfer has no retired reset owner");
  s_title_target=false;
  return RECOMP_RETURN_OWNED_UNWIND;
}
