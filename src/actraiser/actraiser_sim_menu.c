#include "actraiser/actraiser_sim_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "actraiser/actraiser_localization_schedule.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"

typedef RecompReturn (*Routine)(CpuState *);
#define NATIVE_VARIANTS(pc) \
  extern RecompReturn bank_01_##pc##_M0X0(CpuState *); \
  extern RecompReturn bank_01_##pc##_M0X1(CpuState *); \
  extern RecompReturn bank_01_##pc##_M1X0(CpuState *); \
  extern RecompReturn bank_01_##pc##_M1X1(CpuState *); \
  static Routine const kNative##pc[] = {bank_01_##pc##_M0X0, \
      bank_01_##pc##_M0X1, bank_01_##pc##_M1X0, bank_01_##pc##_M1X1}
NATIVE_VARIANTS(81D7);
NATIVE_VARIANTS(8B7D);
extern RecompReturn bank_01_8D92_M1X0(CpuState *);
extern RecompReturn bank_01_8C43_M1X0(CpuState *);
extern RecompReturn bank_01_8C49_M1X0(CpuState *);
extern RecompReturn bank_01_8C79_M1X0(CpuState *);
extern RecompReturn bank_01_B52F_M1X0(CpuState *);
extern RecompReturn bank_01_B5CD_M1X0(CpuState *);
extern RecompReturn bank_01_8CCE_M1X0(CpuState *);
extern RecompReturn bank_01_8E29_M1X0(CpuState *);

static SimMenuModel s_menu;
static uint64_t s_generation;
static bool s_owner, s_action_guard, s_confirm_guard;
static bool s_input_guard;
static bool s_confirmation_cancelled;
static uint8_t s_action;
static uint16_t s_scene;
static SimMenuHelpPage s_help;
static bool s_description_aborted, s_description_fast, s_description_released;

bool ActRaiserSimMenu_DescriptionAborted(void) { return s_description_aborted; }
bool ActRaiserSimMenu_Describing(void) { return s_owner && s_menu.phase == kSimMenu_Describe; }
bool ActRaiserSimMenu_FastReveal(void) { return s_description_fast; }
void ActRaiserSimMenu_DescriptionWait(void) { s_description_fast=false; }

bool ActRaiser_SimMenuObserveFrame(CpuState *c) {
  if (c && s_menu.phase == kSimMenu_Describe) {
    const uint8_t buttons=cpu_read8(c,0,0xa1) & 0xc0;
    if (!buttons) s_description_released=true;
    if (s_description_released && (buttons & 0x40)) {
      s_description_released=false;
      s_description_aborted=true;
      ActRaiserLocalizationRuntime_ReturnDialogue();
    } else if (s_description_released && (buttons & 0x80)) {
      s_description_released=false;
      s_description_fast=true;
    }
  }
  return false; /* Observe only: $9284's full native frame service still runs. */
}

static unsigned Mode(const CpuState *c) {
  return ((c->m_flag & 1) << 1) | (c->x_flag & 1);
}

/* Calls only audited RTS leaves. Architectural state is restored; writes,
 * input sampling, animation and frame service remain native and observable. */
static uint8_t Call(CpuState *c, Routine routine, uint16_t a, uint16_t x,
                    uint16_t y, uint16_t return_address) {
  const uint16_t oa=c->A, ox=c->X, oy=c->Y, os=c->S, od=c->D;
  const uint8_t db=c->DB, pb=c->PB, p=c->P, m=c->m_flag, ix=c->x_flag,
      hr=c->host_return_valid, n=c->_flag_N, v=c->_flag_V, z=c->_flag_Z,
      carry=c->_flag_C, irq=c->_flag_I, decimal=c->_flag_D;
  c->A=a; c->X=x; c->Y=y; c->D=0; c->DB=1; c->PB=1;
  c->m_flag=1; c->x_flag=0; c->P=(c->P | 0x20) & ~0x10;
  cpu_write8(c,0,c->S--,return_address >> 8);
  cpu_write8(c,0,c->S--,return_address & 255);
  c->host_return_valid=1;
  const RecompReturn result=routine(c);
  if (result != RECOMP_RETURN_NORMAL || c->S != os) {
    fprintf(stderr,"[sim-menu] invalid native leaf return %d, stack %04x/%04x\n",
            result,c->S,os);
    abort();
  }
  const uint8_t value=(uint8_t)c->A;
  c->A=oa; c->X=ox; c->Y=oy; c->S=os; c->D=od;
  c->DB=db; c->PB=pb; c->P=p; c->m_flag=m; c->x_flag=ix;
  c->host_return_valid=hr; c->_flag_N=n; c->_flag_V=v; c->_flag_Z=z;
  c->_flag_C=carry; c->_flag_I=irq; c->_flag_D=decimal;
  return value;
}

static void Carry(CpuState *c, bool carry) {
  c->_flag_C=carry; c->P=(c->P & ~1u) | carry;
}

void ActRaiserSimMenu_Reset(void) {
  memset(&s_menu,0,sizeof(s_menu));
  memset(&s_help,0,sizeof(s_help));
  s_menu.generation=++s_generation;
  s_owner=s_action_guard=s_confirm_guard=false;
  s_input_guard=false;
  s_action=0; s_confirmation_cancelled=false;
  s_scene=0;
  s_description_aborted=s_description_fast=s_description_released=false;
}

bool ActRaiserSimMenu_OwnsInput(void) {
  return s_owner && (s_menu.phase == kSimMenu_Browse ||
      s_menu.phase == kSimMenu_Inventory || s_menu.phase == kSimMenu_Describe);
}

bool ActRaiserSimMenu_OwnsPresentation(void) {
  return s_owner && s_menu.phase != kSimMenu_Native &&
      s_menu.phase != kSimMenu_Closed;
}

void ActRaiserSimMenu_CopyModel(SimMenuModel *model) {
  if (model) {
    *model=s_menu;
    ArRegionalCostSnapshot prices;
    if (ActRaiserRegional_CopyPrices(&prices)) {
      for (unsigned action=5; action<=9; ++action) {
        ArRegionalCostRule rule;
        if (ActRaiserMiracle_Rule(action,&rule)) model->miracle_sp[action-5]=prices.price[rule];
      }
    }
  }
}

void ActRaiserSimMenu_CopyHelp(SimMenuHelpPage *page) { if(page) *page=s_help; }

void ActRaiserSimMenu_ObserveScene(uint16_t scene) {
  /* The departing town remains suppressed throughout its native fade. The
   * destination owns its own panels as soon as the game changes scenes. */
  if (s_owner && scene != s_scene) ActRaiserSimMenu_Reset();
}

void ActRaiserSimMenu_BeginDialogue(const CpuState *c) {
  if (!c || !s_owner || !s_action || s_action == 12 || s_action == 13 ||
      s_menu.phase == kSimMenu_Describe || ActRaiserSimMenu_SkipDialogue(c)) return;
  s_menu.phase=kSimMenu_Dialogue;
  ++s_menu.dialogue_generation;
  s_menu.dialogue_source=0;
  s_menu.dialogue_has_selector=false;
  if (c->PB!=1 || c->DB!=1 || c->D || !c->m_flag || c->x_flag) return;
  const uint16_t caller=cpu_read16((CpuState *)c,0,c->S+1);
  /* Identify the native question/selector call, not its translated wording.
   * Terminal acknowledgements, errors and outcomes remain plain dialogue. */
  static const struct { uint8_t action; uint16_t caller,source; } selectors[]={
    {5,0x82ae,0xfd15}, {6,0x8319,0xfdb9}, {7,0x8384,0xff57},
    {8,0x844f,0xfe2a}, {9,0x83ef,0xfec7},
    {14,0x8a9f,0xf99b}, {14,0x8abd,0xf9ba}, {15,0x8afa,0xfa7b},
  };
  for (unsigned i=0;i<sizeof(selectors)/sizeof(selectors[0]);++i)
    if (s_action==selectors[i].action && caller==selectors[i].caller &&
        c->Y==selectors[i].source) {
      s_menu.dialogue_has_selector=true;
      s_menu.dialogue_source=c->Y;
    }
}

void ActRaiserSimMenu_ClearDialogue(void) {
  if (s_owner && (s_menu.phase == kSimMenu_Dialogue ||
                 s_menu.phase == kSimMenu_MessageSpeed))
    s_menu.phase=kSimMenu_Handoff;
}

bool ActRaiser_SimMenuBrowseEntry(CpuState *c) {
  return c && g_settings.sim_menu_style == 1 && !c->x_flag &&
      c->DB == 1 && c->D == 0 && c->Y == 0xf34a &&
      cpu_read16(c,0,c->S+1) == 0x81be &&
      (s_owner || ActRaiserSimMenu_ArtworkAvailable());
}

static uint8_t Poll(CpuState *c) {
  uint8_t value=Call(c,bank_01_8C43_M1X0,0,0,0,0x8b98);
  /* Recorded pad X is reserved for the separately mapped Describe binding
   * only while this controller owns input. Native A1 omits that button. */
  if (cpu_read8(c,0,0x4218) & 0x40) value |= kSimMenuInput_Describe;
  return value;
}

static void Redraw(CpuState *c) {
  const uint16_t selection=SimMenuModel_Selection(&s_menu);
  cpu_write16(c,0,0x033a,selection);
  cpu_write16(c,0,0x0338,0xf32e);
  cpu_write16(c,0,0x00ea,0x212c);
  cpu_write8(c,0,0x00ec,0x17);
  Call(c,bank_01_8C49_M1X0,0,selection,0xf34a,0x8b8b);
  Call(c,bank_01_B52F_M1X0,0,selection,0xf34a,0x8b8e);
}

static void PaintHelp(CpuState *c) {
  for (unsigned row=0;row<6;++row) for(unsigned col=0;col<22;++col)
    cpu_write16(c,0x7f,0xb4ca+row*64+col*2,0x2020);
  for(unsigned g=0;g<s_help.revealed_glyphs;++g) {
    const uint32_t scalar=s_help.scalars[g];
    cpu_write16(c,0x7f,0xb4ca+s_help.row[g]*64+s_help.column[g]*2,
                 0x2000 | (scalar>=32 && scalar<128?scalar:'?'));
  }
  cpu_write16(c,0x7f,0xb674,
      s_help.more && s_help.revealed_glyphs==s_help.glyph_count?0x205f:0x2020);
  cpu_write8(c,0,0xf1,cpu_read8(c,0,0xf1)+1);
}

static void DescribeNeutral(CpuState *c, const char *id,const char *fallback) {
  ArDialogueSession session; ArDialogueSession_Init(&session);
  if(!ActRaiserLocalizationRuntime_BeginMenuHelp(&session,id,fallback)) return;
  Call(c,bank_01_8CCE_M1X0,0,0,0,0x82f8);
  size_t start=0; unsigned page_index=0,delay=0;
  bool build=true,fast=false,complete=false;
  ArDialoguePageSnapshot page;
  ArLanguagePackError error={{0}};
  while(s_menu.phase==kSimMenu_Describe) {
    if(!ArDialogueSession_GetPage(&session,&page)) break;
    if(build) {
      if(!SimMenuHelp_Build(&s_help,page.utf8,page.utf8_bytes,start,
                            page_index+1<page.page_count)) break;
      s_help.authored_page=page_index;
      snprintf(s_help.locale,sizeof(s_help.locale),"%s",page.locale?page.locale:"en-US");
      s_help.direction=page.direction==kArLanguageDirection_RightToLeft?
          kArTextDirection_RightToLeft:
          page.direction==kArLanguageDirection_LeftToRight?
              kArTextDirection_LeftToRight:kArTextDirection_Auto;
      if (!ActRaiserLocalizationRuntime_PrepareMenuHelpStyle(&page, &s_help)) break;
      build=false; complete=false; fast=false;
      SimMenuModel_ReleaseBarrier(&s_menu);
    }
    const unsigned speed = cpu_read8(c, 0, 0x0200);
    if(!complete && (!delay || fast || !speed)) {
      do {
        ArDialogueToken token;
        if(!ArDialogueSession_Next(&session,&token,&error)) goto finished;
        if(token.kind==kArDialogueToken_WaitStarted) {
          if(!fast) break;
          ArDialogueSession_TickWait(&session,session.state.wait_frames_remaining);
        }
        if(token.kind==kArDialogueToken_Control) goto finished;
        if(!ArDialogueSession_GetPage(&session,&page)) goto finished;
        while(s_help.revealed_glyphs<s_help.glyph_count &&
              s_help.source_ends[s_help.revealed_glyphs]<=page.revealed_utf8_bytes)
          ++s_help.revealed_glyphs;
        complete=s_help.revealed_glyphs==s_help.glyph_count &&
            s_help.source_end<page.utf8_bytes;
        if(token.kind==kArDialogueToken_PageComplete || token.kind==kArDialogueToken_End)
          complete=true;
        if(token.kind==kArDialogueToken_Blocked) break;
      } while((fast || !speed) && !complete);
      delay=speed;
    }
    PaintHelp(c);
    const SimMenuEvent event=SimMenuModel_Poll(&s_menu,Poll(c));
    if(delay) --delay;
    ArDialogueSession_TickWait(&session,1);
    if(event!=kSimMenuEvent_Advance) continue;
    if(!complete) {fast=true;continue;}
    if(s_help.source_end<page.utf8_bytes) start=s_help.source_end;
    else if(page_index+1<page.page_count) {
      /* Drain only the just-finished page's presentation tokens, then pay its
       * authored page boundary with this explicit acknowledgement. */
      while(!session.state.awaiting_page_advance && !session.state.terminal) {
        ArDialogueToken token;
        if(!ArDialogueSession_Next(&session,&token,&error) ||
            token.kind==kArDialogueToken_Control || token.kind==kArDialogueToken_Blocked)
          goto finished;
        ArDialogueSession_TickWait(&session,session.state.wait_frames_remaining);
      }
      if(!ArDialogueSession_AdvancePage(&session)) break;
      ++page_index; start=0;
    } else break;
    build=true;
  }
finished:
  ArDialogueSession_Destroy(&session);
  memset(&s_help,0,sizeof(s_help));
  Call(c,bank_01_8CCE_M1X0,0,0,0,0x82f8);
}

static void Describe(CpuState *c) {
  s_description_aborted=s_description_fast=s_description_released=false;
  const uint8_t action=SimMenuModel_Action(&s_menu);
  /* These are pure text entries. Calling the interpreter directly never
   * resumes the action handler's SP gate, confirmation or effect. */
  static const uint16_t source[5]={0xfc9c,0xfd25,0xfedc,0xfdc8,0xfe3a};
  static const uint16_t caller[5]={0x8295,0x8300,0x836b,0x8436,0x83d6};
  if (s_menu.return_phase == kSimMenu_Browse && s_menu.submenu &&
      action >= 5 && action <= 9) {
    Call(c,bank_01_8CCE_M1X0,0,0,0,0x82f8);
    Call(c,bank_01_8E29_M1X0,0,0,source[action-5],caller[action-5]);
    Call(c,bank_01_8CCE_M1X0,0,0,0,0x82f8);
  } else {
    char id[64]; const char *fallback;
    if(s_menu.return_phase==kSimMenu_Inventory) {
      const unsigned item=s_menu.items[s_menu.item_slot];
      snprintf(id,sizeof(id),"sim.help.item.%02u",item);
      fallback=SimMenuHelp_Item(item);
    } else if(!s_menu.submenu) {
      snprintf(id,sizeof(id),"sim.help.category.%u",s_menu.category);
      fallback=SimMenuHelp_Category(s_menu.category);
    } else {
      snprintf(id,sizeof(id),"sim.help.action.%02u",action);
      fallback=SimMenuHelp_Action(action);
    }
    DescribeNeutral(c,id,fallback);
  }
  SimMenuModel_EndDescription(&s_menu);
  s_description_aborted=s_description_fast=s_description_released=false;
}

RecompReturn ActRaiser_SimMenuBrowse(CpuState *c) {
  if (!s_owner) {
    s_owner=true;
    s_scene=cpu_read16(c,0,0x18);
    SimMenuModel_Open(&s_menu,++s_generation,c->X);
    if (getenv("AR_SIM_MENU_TRACE"))
      fprintf(stderr,"[sim-menu] open generation=%llu node=%04x DB=%02x S=%04x\n",
              (unsigned long long)s_generation,c->X,c->DB,c->S);
  }
  if (s_menu.phase != kSimMenu_Browse)
    SimMenuModel_Open(&s_menu,s_menu.generation,c->X);
  SimMenuModel_ReleaseBarrier(&s_menu);
  Redraw(c);
  for (;;) {
    if (g_settings.sim_menu_style != 1) {
      c->X=SimMenuModel_Selection(&s_menu);
      ActRaiserSimMenu_Reset();
      return kNative8B7D[Mode(c)](c);
    }
    const SimMenuEvent event=SimMenuModel_Poll(&s_menu,Poll(c));
    if (event != kSimMenuEvent_None && getenv("AR_SIM_MENU_TRACE"))
      fprintf(stderr,"[sim-menu] event=%d node=%04x action=%u phase=%d\n",
              event,SimMenuModel_Selection(&s_menu),SimMenuModel_Action(&s_menu),s_menu.phase);
    if (event == kSimMenuEvent_Describe) { Describe(c); Redraw(c); }
    else if (event == kSimMenuEvent_Changed) Redraw(c);
    else if (event == kSimMenuEvent_Close || event == kSimMenuEvent_Use) {
      c->X=SimMenuModel_Selection(&s_menu);
      c->A=(c->A & 0xff00) | SimMenuModel_Action(&s_menu);
      Carry(c,event == kSimMenuEvent_Close);
      c->S+=2;
      s_menu.return_phase=kSimMenu_Browse;
      s_menu.phase=kSimMenu_Handoff;
      if (event == kSimMenuEvent_Close) ActRaiserSimMenu_Reset();
      return RECOMP_RETURN_NORMAL;
    }
  }
}

bool ActRaiser_SimMenuActionEntry(CpuState *c) {
  if (s_action_guard) { s_action_guard=false; return false; }
  return c && (s_owner || ActRaiserRegional_MiracleEntry(c) || ActRaiserRegional_ReportCommandEntry(c)) &&
      cpu_read16(c,0,c->S+1) == 0x81c3;
}

RecompReturn ActRaiser_SimMenuAction(CpuState *c) {
  s_action=(uint8_t)c->A;
  if (s_action == 12 || s_action == 13) s_menu.phase=kSimMenu_Native;
  s_confirmation_cancelled=false;
  RecompReturn result;
  if (ActRaiserRegional_MiracleEntry(c)) result=ActRaiserRegional_RunMiracle(c);
  else if (ActRaiserRegional_ReportCommandEntry(c)) result=ActRaiserRegional_RunReportCommand(c);
  else {
    s_action_guard=true;
    result=kNative81D7[Mode(c)](c);
    s_action_guard=false;
  }
  if (!s_owner) { s_action=0; return result; } /* Native scene changes can retire us. */
  if (result == RECOMP_RETURN_NORMAL && s_confirmation_cancelled) {
    /* Native cancellation already performed its cleanup and release wait.
     * Ask the original owner to reopen at the same native node. */
    Carry(c,true);
    s_menu.phase=kSimMenu_Browse;
  } else if (result == RECOMP_RETURN_NORMAL && !c->_flag_C &&
             cpu_read16(c,0,0x1a) != cpu_read16(c,0,0x18)) {
    s_menu.phase=kSimMenu_Handoff;
  } else if (result != RECOMP_RETURN_NORMAL || !c->_flag_C) {
    ActRaiserSimMenu_Reset();
  } else s_menu.phase=kSimMenu_Browse;
  s_action=0;
  return result;
}

bool ActRaiser_SimMenuConfirmEntry(CpuState *c) {
  if (s_confirm_guard) { s_confirm_guard=false; return false; }
  return c && s_owner && ((s_action >= 5 && s_action <= 9) || s_action == 14) &&
      c->m_flag && !c->x_flag;
}

RecompReturn ActRaiser_SimMenuConfirm(CpuState *c) {
  SimMenuModel_Confirm(&s_menu);
  s_confirm_guard=true;
  const RecompReturn result=bank_01_8D92_M1X0(c);
  if (result == RECOMP_RETURN_NORMAL && s_action >= 5 && s_action <= 9)
    s_confirmation_cancelled=!c->_flag_C;
  s_menu.phase=kSimMenu_Handoff;
  return result;
}

bool ActRaiser_SimMenuConfirmInputEntry(CpuState *c) {
  if (s_input_guard) { s_input_guard=false; return false; }
  if (s_menu.phase == kSimMenu_Opening && g_settings.sim_menu_style != 1)
    ActRaiserSimMenu_Reset();
  /* $81AC polls until the opening face button is released, before $8B7D.
   * Its first $9284 frame service already presents the native menu. Claim
   * presentation at this exact caller, after the contextual lair check has
   * allowed a menu, while leaving the release wait and CPU state untouched. */
  if (c && !s_owner && g_settings.sim_menu_style == 1 &&
      c->PB == 1 && c->DB == 1 && c->D == 0 && c->m_flag && !c->x_flag &&
      cpu_read16(c,0,c->S+1) == 0x81ae &&
      cpu_read16(c,0,0x0338) == 0xf32e &&
      cpu_read16(c,0,0x033a) == 0xf32e && ActRaiserSimMenu_ArtworkAvailable()) {
    s_owner=true;
    s_scene=cpu_read16(c,0,0x18);
    SimMenuModel_Open(&s_menu,++s_generation,0xf32e);
    s_menu.phase=kSimMenu_Opening;
    if (getenv("AR_SIM_MENU_TRACE"))
      fprintf(stderr,"[sim-menu] opening generation=%llu before native release wait\n",
              (unsigned long long)s_generation);
  }
  if (c && s_owner && s_action == 15 && c->DB == 1 && c->D == 0 &&
      c->m_flag && !c->x_flag) {
    const uint16_t caller=cpu_read16(c,0,c->S+1);
    if (caller == 0x8b2a || caller == 0x8b31)
      s_menu.phase=kSimMenu_MessageSpeed;
  }
  return c && s_owner && s_menu.phase == kSimMenu_Confirm &&
      c->DB == 1 && c->D == 0 && c->m_flag && !c->x_flag;
}

RecompReturn ActRaiser_SimMenuConfirmInput(CpuState *c) {
  s_input_guard=true;
  const RecompReturn result=bank_01_8C43_M1X0(c);
  if (result == RECOMP_RETURN_NORMAL) {
    /* Up/Down retain native vertical selection. Keep Left/Right as aliases
     * for existing bindings/replays without changing the native PiP state. */
    uint8_t buttons=(uint8_t)c->A;
    if (buttons & 2) buttons=(buttons & ~3u) | 8;
    else if (buttons & 1) buttons=(buttons & ~3u) | 4;
    c->A=(c->A & 0xff00) | buttons;
    c->_flag_N=(buttons & 0x80) != 0;
    c->_flag_Z=buttons == 0;
  }
  return result;
}

bool ActRaiser_SimMenuInventoryEntry(CpuState *c) {
  if (!(c && s_owner && s_action == 11 && c->X == 0x0898 &&
      c->Y == 0xf08c && !c->x_flag &&
      cpu_read16(c,0,c->S+1) == 0x84ef)) return false;
  for (unsigned i = 0; i < 8; ++i)
    if (cpu_read8(c, 0, 0x02a2 + i) > 20) return false;
  return true;
}

RecompReturn ActRaiser_SimMenuInventory(CpuState *c) {
  uint8_t items[8];
  for (unsigned i=0;i<8;++i) items[i]=cpu_read8(c,0,0x02a2+i);
  SimMenuModel_Inventory(&s_menu,items);
  cpu_write16(c,0,8,0xf08c);
  for (;;) {
    cpu_write16(c,0,0x0a,s_menu.item_slot);
    Call(c,bank_01_8C79_M1X0,s_menu.items[s_menu.item_slot],0x0898,0x02a2,0x8d08);
    Call(c,bank_01_B5CD_M1X0,s_menu.item_slot,0x0898,0x02a2,0x8d0d);
    SimMenuEvent event;
    do { event=SimMenuModel_Poll(&s_menu,Poll(c)); } while(event == kSimMenuEvent_None);
    if (event == kSimMenuEvent_Describe) Describe(c);
    if (event != kSimMenuEvent_Close && event != kSimMenuEvent_Use) continue;
    if (event == kSimMenuEvent_Use &&
        cpu_read8(c, 0, 0x02a2 + s_menu.item_slot) != s_menu.items[s_menu.item_slot]) {
      for (unsigned i = 0; i < 8; ++i) items[i] = cpu_read8(c, 0, 0x02a2 + i);
      SimMenuModel_Inventory(&s_menu, items);
      if (s_menu.item_count) continue;
      event = kSimMenuEvent_Close;
    }
    /* Native $84FA reads this slot, calls $9C6E and owns ALL further dialogue,
     * effects, consumption and the special Bread/Wheat/Skull stack unwind. */
    c->A=(c->A & 0xff00) | s_menu.item_slot;
    Carry(c,event == kSimMenuEvent_Close);
    c->S+=2;
    s_menu.return_phase=event == kSimMenuEvent_Use?kSimMenu_Inventory:kSimMenu_Browse;
    s_menu.phase=kSimMenu_Handoff;
    return RECOMP_RETURN_NORMAL;
  }
}

bool ActRaiserSimMenu_SkipDialogue(const CpuState *c) {
  if (!c || !s_owner || s_menu.phase == kSimMenu_Describe ||
      c->DB != 1 || c->PB != 1 || c->D || !c->m_flag || c->x_flag) return false;
  /* Exact interpreter call sites, not text contents or a global fast-forward.
   * Optional description and pure target instruction only. The confirmation
   * question must remain in the native/localized window until Yes/No returns. */
  const uint16_t caller=cpu_read16((CpuState *)c,0,c->S+1);
  if (s_action==3) return caller==0x8249 && c->Y==0xfad0;
  if (s_action<5 || s_action>9) return false;
  static const uint16_t calls[5][3]={
    {0x8295,0x82ae,0x82b9}, {0x8300,0x8319,0x8324},
    {0x836b,0x8384,0x838f}, {0x8436,0x844f,0},
    {0x83d6,0x83ef,0}};
  static const uint16_t sources[5][3]={
    {0xfc9c,0xfd15,0xfce8}, {0xfd25,0xfdb9,0xfd8e},
    {0xfedc,0xff57,0xff26}, {0xfdc8,0xfe2a,0},
    {0xfe3a,0xfec7,0}};
  for (unsigned i=0;i<3;++i)
    if (i != 1 && calls[s_action-5][i] && caller == calls[s_action-5][i] &&
        c->Y==sources[s_action-5][i]) return true;
  return false;
}
