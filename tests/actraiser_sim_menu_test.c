#include "actraiser/actraiser_sim_menu.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "sim/menu/sim_menu_localization.h"
#include "app/settings.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

Settings g_settings;
static uint8_t memory[0x20000], inputs[32];
static unsigned input_at, descriptions, actions, expected_action;
static bool artwork = true;
static uint32_t sound_site, sound_sites[64];
static unsigned sounds;
static bool interrupt_with_dialogue;
static bool interrupt_changes_scene;
static bool confirm_miracle;
static bool regional_report, keep_report_open;
static bool native_quick_test, native_full_flow, native_reset_during_poll;
static bool native_category_first;
static SimMenuPhase Phase(void);
void cpu_trace_block(CpuState *c, uint32_t site) {
  (void)c;
  sound_site = site;
}
static void MenuSound(CpuState *c) {
  assert((c->A & 255) == 7 && sounds < 64);
  sound_sites[sounds++] = sound_site;
}
void (*g_cpu_cop_hook)(CpuState *) = MenuSound;
static const uint16_t sources[] = {0xfc9c, 0xfd25, 0xfedc, 0xfdc8, 0xfe3a};
static const uint16_t callers[] = {0x8295, 0x8300, 0x836b, 0x8436, 0x83d6};
static const uint16_t questions[] = {0xfd15, 0xfdb9, 0xff57, 0xfe2a, 0xfec7};
static const uint16_t question_callers[] = {0x82ae, 0x8319, 0x8384, 0x844f, 0x83ef};

bool ActRaiserRegional_MiracleEntry(const CpuState *cpu) {
  (void)cpu;
  return false;
}
bool ActRaiserRegional_ReportCommandEntry(const CpuState *cpu) {
  return regional_report && cpu && (uint8_t)cpu->A >= 12 && (uint8_t)cpu->A <= 15;
}
RecompReturn ActRaiserRegional_RunReportCommand(CpuState *cpu) {
  assert(ActRaiserRegional_ReportCommandEntry(cpu));
  const bool native_report = (uint8_t)cpu->A <= 13;
  assert(Phase() == (native_report ? kSimMenu_Native : kSimMenu_Handoff));
  assert(ActRaiserSimMenu_OwnsPresentation() == !native_report);
  cpu->_flag_C = keep_report_open;
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu) {
  (void)cpu;
  assert(false);
  return RECOMP_RETURN_NORMAL;
}
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices) {
  *prices = (ArRegionalCostSnapshot){{1, 1, 1, 1, 10, 20, 30, 80, 160}};
  return true;
}
bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule) {
  if (action < 5 || action > 9 || !rule) return false;
  *rule = (ArRegionalCostRule)(action - 5 + kArRegionalCost_Lightning);
  return true;
}

static SimMenuPhase Phase(void) {
  SimMenuModel m;
  ActRaiserSimMenu_CopyModel(&m);
  return m.phase;
}

static bool DialogueHasSelector(void) {
  SimMenuModel m;
  ActRaiserSimMenu_CopyModel(&m);
  return m.dialogue_has_selector;
}

uint8 cpu_read8(CpuState *c, uint8 bank, uint16 address) {
  (void)c;
  return memory[(bank == 0x7f ? 0x10000 : 0) + address];
}
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 address) {
  return cpu_read8(c, bank, address) | cpu_read8(c, bank, address + 1) << 8;
}
void cpu_write8(CpuState *c, uint8 bank, uint16 address, uint8 value) {
  (void)c;
  memory[(bank == 0x7f ? 0x10000 : 0) + address] = value;
}
void cpu_write16(CpuState *c, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(c, bank, address, value);
  cpu_write8(c, bank, address + 1, value >> 8);
}
bool ActRaiserSimMenu_ArtworkAvailable(void) { return artwork; }
void ActRaiserLocalizationRuntime_ReturnDialogue(void) {}
bool ActRaiserLocalizationRuntime_BeginReadOnlyDialogue(ArDialogueSession *s, const char *id,
                                                        const char *fallback) {
  (void)s;
  (void)id;
  (void)fallback;
  return false;
}
size_t SimMenuLocalization_HelpRevealOffset(const SimMenuHelpPage *help,size_t source) {
  if(source<=help->source_start) return 0;
  size_t bytes=source-help->source_start; return bytes<help->bytes?bytes:help->bytes;
}
size_t SimMenuLocalization_HelpSourceOffset(const SimMenuHelpPage *help,size_t normalized) {
  return help->source_start+normalized;
}
bool SimMenuLocalization_PrepareHelp(const ArDialoguePageSnapshot *s, SimMenuHelpPage *p) {
  (void)s;
  (void)p;
  return true;
}
bool ArTextPresentation_Failed(uint64_t ticket) {
  (void)ticket;
  return false;
}
bool ArTextPresentation_PageEnd(uint64_t ticket, uint32_t start, uint32_t *end) {
  (void)ticket;
  (void)start;
  (void)end;
  return false;
}
void ArDialogueSession_Init(ArDialogueSession *s) { memset(s, 0, sizeof(*s)); }
void ArDialogueSession_Destroy(ArDialogueSession *s) { (void)s; }
bool ArDialogueSession_GetPage(const ArDialogueSession *s, ArDialoguePageSnapshot *p) {
  (void)s;
  (void)p;
  return false;
}
bool ArDialogueSession_Next(ArDialogueSession *s, ArDialogueToken *t, ArLanguagePackError *e) {
  (void)s;
  (void)t;
  (void)e;
  return false;
}
void ArDialogueSession_TickWait(ArDialogueSession *s, uint32_t f) {
  (void)s;
  (void)f;
}
bool ArDialogueSession_AdvancePage(ArDialogueSession *s) {
  (void)s;
  return false;
}

static RecompReturn Leaf(CpuState *c) {
  c->S += 2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_8C43_M1X0(CpuState *c) {
  assert(input_at < sizeof(inputs));
  if (native_quick_test) {
    assert(!ActRaiser_SimMenuConfirmInputEntry(c)); /* generated re-entry */
    if (native_reset_during_poll) {
      native_reset_during_poll=false;
      ActRaiserSimMenu_ObserveScene(0x0700);
    }
  }
  if (interrupt_with_dialogue && input_at == 1) {
    interrupt_with_dialogue = false;
    assert(Phase() == kSimMenu_Browse);
    ActRaiserSimMenu_BeginDialogue(c);
    assert(Phase() == kSimMenu_Dialogue && ActRaiserSimMenu_OwnsPresentation());
    assert(!DialogueHasSelector() && !ActRaiserSimMenu_OwnsInput());
    if (interrupt_changes_scene) ActRaiserSimMenu_ObserveScene(8);
    ActRaiserSimMenu_ClearDialogue();
    assert(Phase() == (interrupt_changes_scene ? kSimMenu_Closed : kSimMenu_Handoff));
  }
  const uint8_t buttons = inputs[input_at++];
  cpu_write8(c, 0, 0x4218, buttons & kSimMenuInput_Describe ? 0x40 : 0);
  c->A = (c->A & 0xff00) | (buttons & ~kSimMenuInput_Describe);
  return Leaf(c);
}
RecompReturn bank_01_8C49_M1X0(CpuState *c) { return Leaf(c); }
RecompReturn bank_01_8C79_M1X0(CpuState *c) { return Leaf(c); }
RecompReturn bank_01_B52F_M1X0(CpuState *c) { return Leaf(c); }
RecompReturn bank_01_B5CD_M1X0(CpuState *c) { return Leaf(c); }
RecompReturn bank_01_8CCE_M1X0(CpuState *c) { return Leaf(c); }
RecompReturn bank_01_8E29_M1X0(CpuState *c) {
  assert(c->Y == sources[expected_action - 5]);
  assert(cpu_read16(c, 0, c->S + 1) == callers[expected_action - 5]);
  assert(!ActRaiserSimMenu_SkipDialogue(c));
  assert(actions == 0 && ActRaiserSimMenu_OwnsInput());
  ++descriptions;
  /* The adapter must preserve its caller even if a native text leaf changes
   * flags, registers and widths. RAM remains owned by the native routine. */
  c->X = 0x4321;
  c->DB = 3;
  c->_flag_C = 1;
  c->x_flag = 1;
  return Leaf(c);
}
RecompReturn bank_01_8D92_M1X0(CpuState *c) {
  assert(!ActRaiser_SimMenuConfirmEntry(c)); /* generated re-entry guard */
  c->_flag_C = confirm_miracle && expected_action >= 5 && expected_action <= 9;
  return Leaf(c);
}
static RecompReturn Action(CpuState *c) {
  assert(!ActRaiser_SimMenuActionEntry(c));
  ++actions;
  if (native_quick_test) {
    assert(!ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation());
    CpuState text=*c;
    text.S-=2;
    if (expected_action >= 5 && expected_action <= 9) {
      text.Y=sources[expected_action-5];
      cpu_write16(&text,0,text.S+1,callers[expected_action-5]);
      assert(ActRaiserSimMenu_SkipDialogue(&text)==!native_full_flow);
      ActRaiserSimMenu_BeginDialogue(&text);
      assert(!ActRaiserSimMenu_OwnsPresentation());
      if (!ActRaiserSimMenu_SkipDialogue(&text)) ++descriptions;
      cpu_write16(&text,0,text.S+1,0x93b2);
      assert(!ActRaiserSimMenu_SkipDialogue(&text));
      text.Y=questions[expected_action-5];
      cpu_write16(&text,0,text.S+1,question_callers[expected_action-5]);
      assert(!ActRaiserSimMenu_SkipDialogue(&text));
      assert(!ActRaiser_SimMenuConfirmEntry(&text));
      if (expected_action <= 7) {
        static const uint16_t target[]={0xfce8,0xfd8e,0xff26};
        static const uint16_t caller[]={0x82b9,0x8324,0x838f};
        text.Y=target[expected_action-5];
        cpu_write16(&text,0,text.S+1,caller[expected_action-5]);
        assert(ActRaiserSimMenu_SkipDialogue(&text)==!native_full_flow);
      }
    } else if (expected_action == 3) {
      text.Y=0xfad0;
      cpu_write16(&text,0,text.S+1,0x8249);
      assert(ActRaiserSimMenu_SkipDialogue(&text)==!native_full_flow);
      text.Y=0xfaee;
      cpu_write16(&text,0,text.S+1,0x8253);
      assert(!ActRaiserSimMenu_SkipDialogue(&text));
    } else if (expected_action == 11) {
      /* Native inventory still needs its own choose-item prompt. */
      text.Y=0xf957;
      cpu_write16(&text,0,text.S+1,0x84c7);
      assert(!ActRaiserSimMenu_SkipDialogue(&text));
      text.X = 0x0898;
      text.Y = 0xf08c;
      cpu_write16(&text,0,text.S+1,0x84ef);
      assert(!ActRaiser_SimMenuInventoryEntry(&text));
    }
    c->_flag_C=keep_report_open;
    return Leaf(c);
  }
  if (expected_action < 5 || expected_action > 9) {
    CpuState text = *c;
    text.Y = 0xf99b;
    ActRaiserSimMenu_BeginDialogue(&text);
    if (expected_action == 12 || expected_action == 13) {
      assert(Phase() == kSimMenu_Native && !ActRaiserSimMenu_OwnsPresentation());
    } else {
      assert(Phase() == kSimMenu_Dialogue && ActRaiserSimMenu_OwnsPresentation());
      assert(!ActRaiserSimMenu_OwnsInput());
      if (expected_action == 3) {
        /* Direct the People skips its optional entry instruction, but its
         * native terminal message is ordinary dialogue even though the
         * semantic route was historically named "cancel_confirm". */
        ActRaiserSimMenu_ClearDialogue();
        text.Y = 0xfad0;
        cpu_write16(&text, 0, text.S + 1, 0x8249);
        assert(ActRaiserSimMenu_SkipDialogue(&text));
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(Phase() == kSimMenu_Handoff);
        text.Y = 0xfaee;
        cpu_write16(&text, 0, text.S + 1, 0x8253);
        assert(!ActRaiserSimMenu_SkipDialogue(&text));
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(Phase() == kSimMenu_Dialogue && !DialogueHasSelector());
        assert(!ActRaiserSimMenu_OwnsInput());
        assert(!ActRaiser_SimMenuConfirmEntry(&text));
      } else if (expected_action == 11) {
        ActRaiserSimMenu_ClearDialogue();
        text.Y = 0xf957;
        cpu_write16(&text, 0, text.S + 1, 0x84c7);
        assert(ActRaiserSimMenu_SkipDialogue(&text));
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(Phase() == kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
        cpu_write16(&text, 0, text.S + 1, 0x93b2);
        assert(!ActRaiserSimMenu_SkipDialogue(&text));
        static const uint16_t sources[] = {0xf96c, 0xf98f};
        static const uint16_t callers[] = {0x84bf, 0x84f7};
        for (unsigned i = 0; i < 2; ++i) {
          text.Y = sources[i];
          cpu_write16(&text, 0, text.S + 1, callers[i]);
          assert(!ActRaiserSimMenu_SkipDialogue(&text));
          ActRaiserSimMenu_BeginDialogue(&text);
          assert(Phase() == kSimMenu_Dialogue);
        }
      } else if (expected_action == 14) {
        static const uint16_t save_sources[] = {0xf99b, 0xf9ba, 0xf9e6, 0xfa10, 0xfa6a};
        static const uint16_t save_callers[] = {0x8a9f, 0x8abd, 0x8af0, 0x8acc, 0x8af0};
        for (unsigned i = 0; i < 5; ++i) {
          text.Y = save_sources[i];
          cpu_write16(&text, 0, text.S + 1, save_callers[i]);
          ActRaiserSimMenu_BeginDialogue(&text);
          assert(Phase() == kSimMenu_Dialogue && ActRaiserSimMenu_OwnsPresentation());
          assert(!ActRaiserSimMenu_OwnsInput());
          assert(DialogueHasSelector() == (i < 2));
        }
        assert(ActRaiser_SimMenuConfirmEntry(c));
        c->S -= 2;
        assert(ActRaiser_SimMenuConfirm(c) == RECOMP_RETURN_NORMAL);
        assert(!c->_flag_C); /* No must retain the native progress-log branch. */
      } else if (expected_action == 15) {
        text.Y = 0xfa7b;
        cpu_write16(&text, 0, text.S + 1, 0x8afa);
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(DialogueHasSelector());
        cpu_write16(&text, 0, text.S + 1, 0x8b31);
        assert(!ActRaiser_SimMenuConfirmInputEntry(&text));
        assert(Phase() == kSimMenu_MessageSpeed);
        ActRaiserSimMenu_BeginDialogue(&text); /* native sample/Back text */
        assert(Phase() == kSimMenu_Dialogue);
        assert(!DialogueHasSelector());
      }
      ActRaiserSimMenu_ClearDialogue();
      assert(Phase() == kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
    }
    if (expected_action == 1) cpu_write16(c, 0, 0x1a, 7);
    c->_flag_C = 0;
    return Leaf(c);
  }
  CpuState text = *c;
  text.S -= 2;
  text.Y = sources[expected_action - 5];
  cpu_write16(&text, 0, text.S + 1, callers[expected_action - 5]);
  assert(ActRaiserSimMenu_SkipDialogue(&text));
  cpu_write16(&text, 0, text.S + 1, 0x93b2); /* shared item dialogue wrapper */
  assert(!ActRaiserSimMenu_SkipDialogue(&text));
  text.Y = questions[expected_action - 5];
  cpu_write16(&text, 0, text.S + 1, question_callers[expected_action - 5]);
  assert(!ActRaiserSimMenu_SkipDialogue(&text));
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(Phase() == kSimMenu_Dialogue);
  assert(DialogueHasSelector());
  SimMenuModel question;
  ActRaiserSimMenu_CopyModel(&question);
  assert(question.dialogue_source == questions[expected_action - 5]);
  cpu_write16(&text, 0, text.S + 1, 0x93b2); /* A shared source is not this question. */
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(!DialogueHasSelector());
  SimMenuModel followup;
  ActRaiserSimMenu_CopyModel(&followup);
  assert(followup.dialogue_source == 0 &&
         followup.dialogue_generation > question.dialogue_generation);
  cpu_write16(&text, 0, text.S + 1, question_callers[expected_action - 5]);
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(ActRaiser_SimMenuConfirmEntry(c));
  c->S -= 2;
  assert(ActRaiser_SimMenuConfirm(c) == RECOMP_RETURN_NORMAL);
  assert(c->_flag_C == confirm_miracle);
  c->_flag_C = 0; /* Completed miracle, or native cancellation cleanup. */
  return Leaf(c);
}
#define VARIANTS(pc, body)                                                                         \
  RecompReturn bank_01_##pc##_M0X0(CpuState *c) { return body(c); }                                \
  RecompReturn bank_01_##pc##_M0X1(CpuState *c) { return body(c); }                                \
  RecompReturn bank_01_##pc##_M1X0(CpuState *c) { return body(c); }                                \
  RecompReturn bank_01_##pc##_M1X1(CpuState *c) { return body(c); }
VARIANTS(81D7, Action)
static uint8_t NativePoll(CpuState *c,uint16_t caller) {
  c->S-=2;
  cpu_write16(c,0,c->S+1,caller);
  const bool adapted=ActRaiser_SimMenuConfirmInputEntry(c);
  const uint16_t x=c->X,y=c->Y,s=c->S;
  assert((adapted ? ActRaiser_SimMenuConfirmInput(c) : bank_01_8C43_M1X0(c))==RECOMP_RETURN_NORMAL);
  assert(c->S==s+2 && c->X==x && c->Y==y);
  return (uint8_t)c->A;
}
static RecompReturn NativeBrowse(CpuState *c) {
  if (!native_quick_test) return Leaf(c);
  assert(!ActRaiser_SimMenuBrowseEntry(c)); /* generated re-entry */
  assert(ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation());
  CpuState other=*c;
  other.S-=2;
  /* The same poll leaf also serves dialogues, Palace menus and selectors. */
  const uint16_t other_callers[]={0x8d0d,0x8b2a,0x8b31,0x9265};
  for (unsigned i=0;i<sizeof(other_callers)/sizeof(*other_callers);++i) {
    cpu_write16(&other,0,other.S+1,other_callers[i]);
    assert(!ActRaiser_SimMenuConfirmInputEntry(&other));
  }
  cpu_write16(&other,0,other.S+1,0x8b98);
  other.DB=2;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&other));
  for (;;) {
    while (NativePoll(c,0x8b91)) {} /* Native wait for all held input. */
    uint8_t buttons;
    do { buttons=NativePoll(c,0x8b98); } while (!(buttons&0xc0) || (buttons&15));
    if (buttons&0x40) { c->_flag_C=1; return Leaf(c); }
    if (native_category_first) {
      native_category_first=false;
      ActRaiserSimMenu_BeginDialogue(c);
      assert(!ActRaiserSimMenu_OwnsPresentation());
      continue; /* Category help returns to navigation, without an action. */
    }
    c->A=(c->A&0xff00)|expected_action;
    c->_flag_C=0;
    return Leaf(c);
  }
}
VARIANTS(8B7D, NativeBrowse)

static void TestNativeQuickUse(void) {
  native_quick_test=true;
  g_settings.sim_menu_style=0;
  g_settings.native_menu_quick_use=true;
  artwork=false; /* This mode needs no modern art/presentation capture. */
  const unsigned commands[]={5,6,7,8,9,3,11,14,15};
  for (unsigned pass=0;pass<4;++pass) for (unsigned i=0;i<sizeof(commands)/sizeof(*commands);++i) {
    ActRaiserSimMenu_Reset();
    memset(memory,0,sizeof(memory));
    input_at=descriptions=actions=0;
    native_full_flow=(pass&1)!=0;
    keep_report_open=pass>=2;
    expected_action=commands[i];
    /* A held Describe on entry cannot activate anything. Then a fresh
     * Describe/Use chooses exactly one full/quick invocation. */
    const uint8_t sequence[]={kSimMenuInput_Describe,kSimMenuInput_Describe,0,
        native_full_flow?kSimMenuInput_Describe:kSimMenuInput_Use};
    memcpy(inputs,sequence,sizeof(sequence));
    CpuState c={.A=0x8100,.X=kSimMenuActions[expected_action-1].selection_pointer,
        .Y=0xf34a,.S=0x1f9,.PB=1,.DB=1,.m_flag=1};
    cpu_write16(&c,0,0x18,0x0600);
    cpu_write16(&c,0,c.S+1,0x81be);
    assert(ActRaiser_SimMenuBrowseEntry(&c));
    const uint16_t node=c.X;
    assert(ActRaiser_SimMenuBrowse(&c)==RECOMP_RETURN_NORMAL);
    assert(input_at==sizeof(sequence) && c.S==0x1fb && c.A==(0x8100|expected_action));
    assert(c.X==node && !ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation());
    c.S-=2;
    cpu_write16(&c,0,c.S+1,0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c)==RECOMP_RETURN_NORMAL);
    assert(actions==1 && c._flag_C==keep_report_open && c.S==0x1fb);
    assert(descriptions==(native_full_flow && expected_action>=5 && expected_action<=9));
    assert(!ActRaiserSimMenu_OwnsPresentation());
    c.S-=2;
    cpu_write16(&c,0,c.S+1,0x81c3);
    assert(!ActRaiser_SimMenuActionEntry(&c)); /* No policy leaks to a later action. */
  }
  /* Describe category help must not make the next ordinary Use run full flow. */
  ActRaiserSimMenu_Reset();
  input_at=descriptions=actions=0;
  expected_action=7;
  native_full_flow=false;
  native_category_first=true;
  const uint8_t category[]={0,kSimMenuInput_Describe,kSimMenuInput_Describe,0,kSimMenuInput_Use};
  memcpy(inputs,category,sizeof(category));
  CpuState c={.X=0xf336,.Y=0xf34a,.S=0x1f9,.PB=1,.DB=1,.m_flag=1};
  assert(ActRaiser_SimMenuBrowse(&c)==RECOMP_RETURN_NORMAL);
  assert(input_at==sizeof(category));
  c.S -= 2;
  cpu_write16(&c, 0, c.S + 1, 0x81c3);
  assert(ActRaiser_SimMenuActionEntry(&c));
  assert(ActRaiser_SimMenuAction(&c)==RECOMP_RETURN_NORMAL && descriptions==0);
  /* Back and a scene retirement leave no pending quick-use command. */
  for (unsigned reset=0;reset<2;++reset) {
    ActRaiserSimMenu_Reset();
    input_at = 0;
    inputs[0] = 0;
    inputs[1] = reset ? kSimMenuInput_Use : kSimMenuInput_Back;
    native_reset_during_poll=reset;
    c.S=0x1f9;
    assert(ActRaiser_SimMenuBrowse(&c)==RECOMP_RETURN_NORMAL);
    c.S -= 2;
    cpu_write16(&c, 0, c.S + 1, 0x81c3);
    assert(!ActRaiser_SimMenuActionEntry(&c));
  }
  ActRaiserSimMenu_Reset();
  native_quick_test=false;
  artwork=true;
  g_settings.native_menu_quick_use=false;
  g_settings.sim_menu_style=1;
}

static void TestOpeningPresentation(void) {
  ActRaiserSimMenu_Reset();
  memset(memory, 0, sizeof(memory));
  CpuState c = {.X = 0x212c, .Y = 0xf34a, .S = 0x1f9, .DB = 1, .PB = 1, .m_flag = 1};
  cpu_write16(&c, 0, 0x18, 0x0100);
  cpu_write16(&c, 0, 0x0338, 0xf32e);
  cpu_write16(&c, 0, 0x033a, 0xf32e);
  cpu_write16(&c, 0, c.S + 1, 0x81ae);
  g_settings.sim_menu_style = 0;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  g_settings.sim_menu_style = 1;
  artwork = false;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  artwork = true;
  cpu_write16(&c, 0, c.S + 1, 0x8b31); /* Message speed/shared input is not an opener. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  cpu_write16(&c, 0, c.S + 1, 0x81ae);
  cpu_write16(&c, 0, 0x0338, 0xf290); /* Palace descriptor must remain native. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  cpu_write16(&c, 0, 0x0338, 0xf32e);
  c.x_flag = 1;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  c.x_flag = 0;
  const CpuState before = c;
  static uint8_t before_memory[sizeof(memory)];
  memcpy(before_memory, memory, sizeof(memory));
  /* False means the original input/release leaf still executes. Claim only
   * presentation, without changing native state or accepting a menu action. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase() == kSimMenu_Opening && ActRaiserSimMenu_OwnsPresentation());
  assert(!ActRaiserSimMenu_OwnsInput());
  assert(!memcmp(&c, &before, sizeof(c)) && !memcmp(memory, before_memory, sizeof(memory)));
  SimMenuModel first, again;
  ActRaiserSimMenu_CopyModel(&first);
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  ActRaiserSimMenu_CopyModel(&again);
  assert(again.generation == first.generation);
  g_settings.sim_menu_style = 0; /* Settings can change during the release wait. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase() == kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
  g_settings.sim_menu_style = 1;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase() == kSimMenu_Opening);
  /* Once native release completes, browse takes input and Back behaves as
   * before. The opening press cannot accidentally dispatch an action. */
  c.X = 0xf32e;
  cpu_write16(&c, 0, c.S + 1, 0x81be);
  input_at = 0;
  inputs[0] = 0;
  inputs[1] = kSimMenuInput_Back;
  assert(ActRaiser_SimMenuBrowseEntry(&c));
  assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
  assert(c.S == 0x1fb && c._flag_C && Phase() == kSimMenu_Closed);
  cpu_write16(&c, 0, 0x033a, 0xf32e); /* Native opener resets the dock node. */
  c.S = 0x1f9;
  cpu_write16(&c, 0, c.S + 1, 0x81ae);
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(ActRaiserSimMenu_OwnsPresentation());
  ActRaiserSimMenu_ObserveScene(7);
  assert(Phase() == kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
}

static void TestMenuSounds(void) {
  ActRaiserSimMenu_Reset();
  memset(memory, 0, sizeof(memory));
  input_at = sounds = 0;
  const uint8_t sequence[] = {0, kSimMenuInput_Right, kSimMenuInput_Right, 0, kSimMenuInput_Down,
                              0, kSimMenuInput_Back};
  memcpy(inputs, sequence, sizeof(sequence));
  CpuState c = {.A = 0x8100,
                .X = 0xf32e,
                .Y = 0xf34a,
                .S = 0x1f9,
                .DB = 1,
                .PB = 1,
                .m_flag = 1,
                ._flag_N = 1,
                ._flag_V = 1};
  assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
  assert(sounds == 4); /* Open, category, action, cancel; held input is silent. */
  assert(sound_sites[0] == 0x018b82 && sound_sites[1] == 0x018b82 && sound_sites[2] == 0x018b82 &&
         sound_sites[3] == 0x018c15);
  assert(c.A == 0x8104 && c._flag_N && c._flag_V && c.Y == 0xf34a);

  for (unsigned cancel = 0; cancel < 2; ++cancel) {
    /* Inventory belongs to a live command; open that owner before entering. */
    c.S = 0x1f9;
    input_at = sounds = 0;
    inputs[0] = 0;
    inputs[1] = kSimMenuInput_Use;
    assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
    input_at = sounds = 0;
    const uint8_t inventory[] = {0, kSimMenuInput_Down, kSimMenuInput_Down, 0,
                                 cancel ? kSimMenuInput_Back : kSimMenuInput_Use};
    memcpy(inputs, inventory, sizeof(inventory));
    memory[0x2a2] = 1;
    memory[0x2a3] = 2;
    c.S = 0x1f9;
    assert(ActRaiser_SimMenuInventory(&c) == RECOMP_RETURN_NORMAL);
    assert(sounds == 3 && sound_sites[0] == 0x018d00 && sound_sites[1] == 0x018d00 &&
           sound_sites[2] == 0x018d00);
    assert(c.A == 0x8101 && c._flag_C == cancel && c.S == 0x1fb);
  }
}

static void TestTownEventDuringBrowse(void) {
  ActRaiserSimMenu_Reset();
  memset(memory, 0, sizeof(memory));
  input_at = sounds = 0;
  interrupt_with_dialogue = true;
  const uint8_t sequence[] = {0, kSimMenuInput_Use, kSimMenuInput_Use, 0, kSimMenuInput_Back};
  memcpy(inputs, sequence, sizeof(sequence));
  CpuState c = {.X = 0xf32e, .Y = 0xf34a, .S = 0x1f9, .DB = 1, .PB = 1, .m_flag = 1};
  assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
  assert(input_at == sizeof(sequence) && c._flag_C);
  assert(sounds == 2); /* The event's acknowledgement did not select an action. */
  assert(!ActRaiserSimMenu_OwnsPresentation());
  input_at = sounds = 0;
  c.S = 0x1f9;
  interrupt_with_dialogue = interrupt_changes_scene = true;
  assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
  assert(input_at == 2 && c._flag_C && c.S == 0x1fb);
  assert(!ActRaiserSimMenu_OwnsPresentation());
  interrupt_changes_scene = false;
}

static void TestRegionalReportReturns(void) {
  regional_report = true;
  /* US closes the command menu; JP returns to the same selected command. */
  for (unsigned pass = 0; pass < 8; ++pass) {
    expected_action = 12 + pass % 4;
    keep_report_open = pass >= 4;
    ActRaiserSimMenu_Reset();
    memset(memory, 0, sizeof(memory));
    input_at = sounds = 0;
    inputs[0] = 0;
    inputs[1] = kSimMenuInput_Use;
    const uint16_t node = kSimMenuActions[expected_action - 1].selection_pointer;
    CpuState c = {.X = node, .Y = 0xf34a, .S = 0x1f9, .DB = 1, .PB = 1, .m_flag = 1};
    cpu_write16(&c, 0, 0x18, 0x0100);
    assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
    c.S -= 2;
    cpu_write16(&c, 0, c.S + 1, 0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c) == RECOMP_RETURN_NORMAL);
    assert(c._flag_C == keep_report_open && c.S == 0x1fb);
    SimMenuModel model;
    ActRaiserSimMenu_CopyModel(&model);
    assert(model.phase == (keep_report_open ? kSimMenu_Browse : kSimMenu_Closed));
    assert(ActRaiserSimMenu_OwnsPresentation() == keep_report_open);
    assert(!keep_report_open || SimMenuModel_Selection(&model) == node);
  }
  regional_report = false;
  ActRaiserSimMenu_Reset();
}

int main(void) {
  TestNativeQuickUse();
  TestOpeningPresentation();
  TestMenuSounds();
  TestTownEventDuringBrowse();
  TestRegionalReportReturns();
  /* Exercise each miracle with No/Back, then Yes/completion. */
  for (unsigned pass = 0; pass < 10; ++pass) {
    expected_action = 5 + pass % 5;
    confirm_miracle = pass >= 5;
    ActRaiserSimMenu_Reset();
    memset(memory, 0, sizeof(memory));
    input_at = descriptions = actions = sounds = 0;
    const uint8_t sequence[] = {0, kSimMenuInput_Describe, 0, kSimMenuInput_Use};
    memcpy(inputs, sequence, sizeof(sequence));
    CpuState c = {.A = 0x8100,
                  .X = kSimMenuActions[expected_action - 1].selection_pointer,
                  .Y = 0xf34a,
                  .S = 0x1f9,
                  .DB = 1,
                  .PB = 1,
                  .m_flag = 1};
    /* Town navigation leaves the destination cleared. A completed miracle
     * must release presentation so native gameplay notices (PAUSE) can draw. */
    cpu_write16(&c, 0, 0x18, 0x0600);
    cpu_write16(&c, 0, c.S + 1, 0x81be);
    g_settings.native_menu_quick_use = false;
    g_settings.sim_menu_style = 0;
    assert(!ActRaiser_SimMenuBrowseEntry(&c));
    g_settings.sim_menu_style = 1;
    /* Enabling the native preference cannot change modern read-only help. */
    g_settings.native_menu_quick_use = pass >= 5;
    artwork = false;
    assert(!ActRaiser_SimMenuBrowseEntry(&c));
    artwork = true;
    assert(ActRaiser_SimMenuBrowseEntry(&c));
    const uint16_t node = c.X;
    assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
    assert(descriptions == 1 && actions == 0 && c.S == 0x1fb);
    assert(c.DB == 1 && !c.x_flag && c.m_flag && c.X == node && c.Y == 0xf34a);
    assert(c.A == (0x8100 | expected_action) && !c._flag_C);
    assert(sounds == 3 && sound_sites[0] == 0x018b82 && sound_sites[1] == 0x018c1c &&
           sound_sites[2] == 0x018c1c);
    c.S -= 2;
    cpu_write16(&c, 0, c.S + 1, 0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c) == RECOMP_RETURN_NORMAL);
    assert(actions == 1 && c._flag_C == !confirm_miracle && c.S == 0x1fb);
    assert(sounds == 3); /* Native confirmations retain their own audio. */
    SimMenuModel model;
    ActRaiserSimMenu_CopyModel(&model);
    if (confirm_miracle) {
      assert(model.phase == kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
    } else {
      assert(model.phase == kSimMenu_Browse && SimMenuModel_Selection(&model) == node);
    }
  }
  const unsigned commands[] = {1, 3, 11, 12, 13, 14, 15};
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    ActRaiserSimMenu_Reset();
    memset(memory, 0, sizeof(memory));
    expected_action = commands[i];
    input_at = sounds = 0;
    inputs[0] = 0;
    inputs[1] = kSimMenuInput_Use;
    CpuState c = {.X = kSimMenuActions[expected_action - 1].selection_pointer,
                  .Y = 0xf34a,
                  .S = 0x1f9,
                  .DB = 1,
                  .PB = 1,
                  .m_flag = 1};
    cpu_write16(&c, 0, 0x18, 0x0600);
    cpu_write16(&c, 0, c.S + 1, 0x81be);
    assert(ActRaiser_SimMenuBrowseEntry(&c));
    assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
    assert(Phase() == kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
    c.S -= 2;
    cpu_write16(&c, 0, c.S + 1, 0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c) == RECOMP_RETURN_NORMAL);
    assert(!c._flag_C);
    if (expected_action == 1) {
      ActRaiserSimMenu_ObserveScene(0x0600);
      assert(Phase() == kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
      /* A new script during the outgoing town's handoff is real dialogue,
       * even though the menu command has already returned. */
      ActRaiserSimMenu_BeginDialogue(&c);
      assert(Phase() == kSimMenu_Dialogue && !DialogueHasSelector());
      ActRaiserSimMenu_ClearDialogue();
      assert(Phase() == kSimMenu_Handoff);
      ActRaiserSimMenu_ObserveScene(7);
    }
    assert(Phase() == kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
  }
  ActRaiserSimMenu_Reset();
  assert(!ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation());
  puts("actraiser_sim_menu: OK");
  return 0;
}
