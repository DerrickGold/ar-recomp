#include "actraiser/actraiser_sim_menu.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "settings.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;
static uint8_t memory[0x20000], inputs[32];
static unsigned input_at, descriptions, actions, expected_action;
static bool artwork = true;
static const uint16_t sources[] = {0xfc9c, 0xfd25, 0xfedc, 0xfdc8, 0xfe3a};
static const uint16_t callers[] = {0x8295, 0x8300, 0x836b, 0x8436, 0x83d6};
static const uint16_t questions[] = {0xfd15, 0xfdb9, 0xff57, 0xfe2a, 0xfec7};
static const uint16_t question_callers[] = {0x82ae, 0x8319, 0x8384, 0x844f, 0x83ef};

bool ActRaiserRegional_MiracleEntry(const CpuState *cpu) { (void)cpu; return false; }
bool ActRaiserRegional_ReportCommandEntry(const CpuState *cpu) { (void)cpu; return false; }
RecompReturn ActRaiserRegional_RunReportCommand(CpuState *cpu) { (void)cpu; assert(false); return RECOMP_RETURN_NORMAL; }
RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu) { (void)cpu; assert(false); return RECOMP_RETURN_NORMAL; }
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices) {
  *prices=(ArRegionalCostSnapshot){{1,1,1,1,10,20,30,80,160}}; return true;
}
bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule) {
  if (action<5 || action>9 || !rule) return false;
  *rule=(ArRegionalCostRule)(action-5+kArRegionalCost_Lightning); return true;
}

static SimMenuPhase Phase(void) {
  SimMenuModel m; ActRaiserSimMenu_CopyModel(&m); return m.phase;
}

static bool DialogueHasSelector(void) {
  SimMenuModel m; ActRaiserSimMenu_CopyModel(&m); return m.dialogue_has_selector;
}

uint8 cpu_read8(CpuState *c, uint8 bank, uint16 address) {
  (void)c; return memory[(bank == 0x7f ? 0x10000 : 0) + address];
}
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 address) {
  return cpu_read8(c, bank, address) | cpu_read8(c, bank, address + 1) << 8;
}
void cpu_write8(CpuState *c, uint8 bank, uint16 address, uint8 value) {
  (void)c; memory[(bank == 0x7f ? 0x10000 : 0) + address] = value;
}
void cpu_write16(CpuState *c, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(c, bank, address, value); cpu_write8(c, bank, address + 1, value >> 8);
}
bool ActRaiserSimMenu_ArtworkAvailable(void) { return artwork; }
void ActRaiserLocalizationRuntime_ReturnDialogue(void) {}
bool ActRaiserLocalizationRuntime_BeginMenuHelp(ArDialogueSession *s,
    const char *id, const char *fallback) { (void)s; (void)id; (void)fallback; return false; }
bool ActRaiserLocalizationRuntime_PrepareMenuHelpStyle(
    const ArDialoguePageSnapshot *s, const SimMenuHelpPage *p) { (void)s; (void)p; return true; }
void ArDialogueSession_Init(ArDialogueSession *s) { memset(s, 0, sizeof(*s)); }
void ArDialogueSession_Destroy(ArDialogueSession *s) { (void)s; }
bool ArDialogueSession_GetPage(const ArDialogueSession *s, ArDialoguePageSnapshot *p) {
  (void)s; (void)p; return false;
}
bool ArDialogueSession_Next(ArDialogueSession *s, ArDialogueToken *t, ArLanguagePackError *e) {
  (void)s; (void)t; (void)e; return false;
}
void ArDialogueSession_TickWait(ArDialogueSession *s, uint32_t f) { (void)s; (void)f; }
bool ArDialogueSession_AdvancePage(ArDialogueSession *s) { (void)s; return false; }

static RecompReturn Leaf(CpuState *c) { c->S += 2; return RECOMP_RETURN_NORMAL; }
RecompReturn bank_01_8C43_M1X0(CpuState *c) {
  assert(input_at < sizeof(inputs));
  const uint8_t buttons = inputs[input_at++];
  cpu_write8(c, 0, 0x4218, buttons & kSimMenuInput_Describe ? 0x40 : 0);
  c->A = buttons & ~kSimMenuInput_Describe;
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
  c->X = 0x4321; c->DB = 3; c->_flag_C = 1; c->x_flag = 1;
  return Leaf(c);
}
RecompReturn bank_01_8D92_M1X0(CpuState *c) {
  assert(!ActRaiser_SimMenuConfirmEntry(c)); /* generated re-entry guard */
  c->_flag_C = 0; /* native Back/No result */
  return Leaf(c);
}
static RecompReturn Action(CpuState *c) {
  assert(!ActRaiser_SimMenuActionEntry(c));
  ++actions;
  if (expected_action < 5 || expected_action > 9) {
    CpuState text=*c; text.Y=0xf99b;
    ActRaiserSimMenu_BeginDialogue(&text);
    if (expected_action==12 || expected_action==13) {
      assert(Phase()==kSimMenu_Native && !ActRaiserSimMenu_OwnsPresentation());
    } else {
      assert(Phase()==kSimMenu_Dialogue && ActRaiserSimMenu_OwnsPresentation());
      assert(!ActRaiserSimMenu_OwnsInput());
      if (expected_action==3) {
        /* Direct the People skips its optional entry instruction, but its
         * native terminal message is ordinary dialogue even though the
         * semantic route was historically named "cancel_confirm". */
        ActRaiserSimMenu_ClearDialogue();
        text.Y=0xfad0; cpu_write16(&text,0,text.S+1,0x8249);
        assert(ActRaiserSimMenu_SkipDialogue(&text));
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(Phase()==kSimMenu_Handoff);
        text.Y=0xfaee; cpu_write16(&text,0,text.S+1,0x8253);
        assert(!ActRaiserSimMenu_SkipDialogue(&text));
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(Phase()==kSimMenu_Dialogue && !DialogueHasSelector());
        assert(!ActRaiserSimMenu_OwnsInput());
        assert(!ActRaiser_SimMenuConfirmEntry(&text));
      } else if (expected_action==14) {
        static const uint16_t save_sources[]={0xf99b,0xf9ba,0xf9e6,0xfa10,0xfa6a};
        static const uint16_t save_callers[]={0x8a9f,0x8abd,0x8af0,0x8acc,0x8af0};
        for (unsigned i=0;i<5;++i) {
          text.Y=save_sources[i];
          cpu_write16(&text,0,text.S+1,save_callers[i]);
          ActRaiserSimMenu_BeginDialogue(&text);
          assert(DialogueHasSelector()==(i<2));
        }
        assert(ActRaiser_SimMenuConfirmEntry(c));
        c->S-=2;
        assert(ActRaiser_SimMenuConfirm(c)==RECOMP_RETURN_NORMAL);
        assert(!c->_flag_C); /* No must retain the native progress-log branch. */
      } else if (expected_action==15) {
        text.Y=0xfa7b; cpu_write16(&text,0,text.S+1,0x8afa);
        ActRaiserSimMenu_BeginDialogue(&text);
        assert(DialogueHasSelector());
        cpu_write16(&text,0,text.S+1,0x8b31);
        assert(!ActRaiser_SimMenuConfirmInputEntry(&text));
        assert(Phase()==kSimMenu_MessageSpeed);
        ActRaiserSimMenu_BeginDialogue(&text); /* native sample/Back text */
        assert(Phase()==kSimMenu_Dialogue);
        assert(!DialogueHasSelector());
      }
      ActRaiserSimMenu_ClearDialogue();
      assert(Phase()==kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
    }
    if (expected_action==1) cpu_write16(c,0,0x1a,7);
    c->_flag_C=0;
    return Leaf(c);
  }
  CpuState text = *c;
  text.S -= 2; text.Y = sources[expected_action - 5];
  cpu_write16(&text, 0, text.S + 1, callers[expected_action - 5]);
  assert(ActRaiserSimMenu_SkipDialogue(&text));
  cpu_write16(&text, 0, text.S + 1, 0x93b2); /* shared item dialogue wrapper */
  assert(!ActRaiserSimMenu_SkipDialogue(&text));
  text.Y=questions[expected_action-5];
  cpu_write16(&text,0,text.S+1,question_callers[expected_action-5]);
  assert(!ActRaiserSimMenu_SkipDialogue(&text));
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(Phase()==kSimMenu_Dialogue);
  assert(DialogueHasSelector());
  SimMenuModel question; ActRaiserSimMenu_CopyModel(&question);
  assert(question.dialogue_source==questions[expected_action-5]);
  cpu_write16(&text,0,text.S+1,0x93b2); /* A shared source is not this question. */
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(!DialogueHasSelector());
  SimMenuModel followup; ActRaiserSimMenu_CopyModel(&followup);
  assert(followup.dialogue_source==0 &&
         followup.dialogue_generation>question.dialogue_generation);
  cpu_write16(&text,0,text.S+1,question_callers[expected_action-5]);
  ActRaiserSimMenu_BeginDialogue(&text);
  assert(ActRaiser_SimMenuConfirmEntry(c));
  c->S -= 2;
  assert(ActRaiser_SimMenuConfirm(c) == RECOMP_RETURN_NORMAL);
  assert(!c->_flag_C);
  return Leaf(c);
}
#define VARIANTS(pc, body) \
  RecompReturn bank_01_##pc##_M0X0(CpuState *c) { return body(c); } \
  RecompReturn bank_01_##pc##_M0X1(CpuState *c) { return body(c); } \
  RecompReturn bank_01_##pc##_M1X0(CpuState *c) { return body(c); } \
  RecompReturn bank_01_##pc##_M1X1(CpuState *c) { return body(c); }
VARIANTS(81D7, Action)
VARIANTS(8B7D, Leaf)

static void TestOpeningPresentation(void) {
  ActRaiserSimMenu_Reset(); memset(memory,0,sizeof(memory));
  CpuState c={.X=0x212c,.Y=0xf34a,.S=0x1f9,.DB=1,.PB=1,.m_flag=1};
  cpu_write16(&c,0,0x18,0x0100);
  cpu_write16(&c,0,0x0338,0xf32e);
  cpu_write16(&c,0,0x033a,0xf32e);
  cpu_write16(&c,0,c.S+1,0x81ae);
  g_settings.sim_menu_style=0;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  g_settings.sim_menu_style=1; artwork=false;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  artwork=true;
  cpu_write16(&c,0,c.S+1,0x8b31); /* Message speed/shared input is not an opener. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  cpu_write16(&c,0,c.S+1,0x81ae);
  cpu_write16(&c,0,0x0338,0xf290); /* Palace descriptor must remain native. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  cpu_write16(&c,0,0x0338,0xf32e);
  c.x_flag=1;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(!ActRaiserSimMenu_OwnsPresentation());
  c.x_flag=0;
  const CpuState before=c;
  static uint8_t before_memory[sizeof(memory)];
  memcpy(before_memory,memory,sizeof(memory));
  /* False means the original input/release leaf still executes. Claim only
   * presentation, without changing native state or accepting a menu action. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase()==kSimMenu_Opening && ActRaiserSimMenu_OwnsPresentation());
  assert(!ActRaiserSimMenu_OwnsInput());
  assert(!memcmp(&c,&before,sizeof(c)) && !memcmp(memory,before_memory,sizeof(memory)));
  SimMenuModel first,again;
  ActRaiserSimMenu_CopyModel(&first);
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  ActRaiserSimMenu_CopyModel(&again);
  assert(again.generation==first.generation);
  g_settings.sim_menu_style=0; /* Settings can change during the release wait. */
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase()==kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
  g_settings.sim_menu_style=1;
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(Phase()==kSimMenu_Opening);
  /* Once native release completes, browse takes input and Back behaves as
   * before. The opening press cannot accidentally dispatch an action. */
  c.X=0xf32e;
  cpu_write16(&c,0,c.S+1,0x81be);
  input_at=0; inputs[0]=0; inputs[1]=kSimMenuInput_Back;
  assert(ActRaiser_SimMenuBrowseEntry(&c));
  assert(ActRaiser_SimMenuBrowse(&c)==RECOMP_RETURN_NORMAL);
  assert(c.S==0x1fb && c._flag_C && Phase()==kSimMenu_Closed);
  c.S=0x1f9; cpu_write16(&c,0,c.S+1,0x81ae);
  assert(!ActRaiser_SimMenuConfirmInputEntry(&c));
  assert(ActRaiserSimMenu_OwnsPresentation());
  ActRaiserSimMenu_ObserveScene(7);
  assert(Phase()==kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
}

int main(void) {
  TestOpeningPresentation();
  for (expected_action = 5; expected_action <= 9; ++expected_action) {
    ActRaiserSimMenu_Reset(); memset(memory, 0, sizeof(memory));
    input_at = descriptions = actions = 0;
    const uint8_t sequence[] = {0, kSimMenuInput_Describe, 0, kSimMenuInput_Use};
    memcpy(inputs, sequence, sizeof(sequence));
    CpuState c = {.A = 0x8100, .X = kSimMenuActions[expected_action - 1].selection_pointer,
      .Y = 0xf34a, .S = 0x1f9, .DB = 1, .PB = 1, .m_flag = 1};
    cpu_write16(&c, 0, c.S + 1, 0x81be);
    g_settings.sim_menu_style = 0;
    assert(!ActRaiser_SimMenuBrowseEntry(&c));
    g_settings.sim_menu_style = 1; artwork = false;
    assert(!ActRaiser_SimMenuBrowseEntry(&c));
    artwork = true;
    assert(ActRaiser_SimMenuBrowseEntry(&c));
    const uint16_t node = c.X;
    assert(ActRaiser_SimMenuBrowse(&c) == RECOMP_RETURN_NORMAL);
    assert(descriptions == 1 && actions == 0 && c.S == 0x1fb);
    assert(c.DB == 1 && !c.x_flag && c.m_flag && c.X == node && c.Y == 0xf34a);
    assert(c.A == (0x8100 | expected_action) && !c._flag_C);
    c.S -= 2; cpu_write16(&c, 0, c.S + 1, 0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c) == RECOMP_RETURN_NORMAL);
    assert(actions == 1 && c._flag_C && c.S == 0x1fb);
    SimMenuModel model; ActRaiserSimMenu_CopyModel(&model);
    assert(model.phase == kSimMenu_Browse && SimMenuModel_Selection(&model) == node);
  }
  const unsigned commands[]={1,3,11,12,13,14,15};
  for (unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);++i) {
    ActRaiserSimMenu_Reset(); memset(memory,0,sizeof(memory));
    expected_action=commands[i]; input_at=0;
    inputs[0]=0; inputs[1]=kSimMenuInput_Use;
    CpuState c={.X=kSimMenuActions[expected_action-1].selection_pointer,
      .Y=0xf34a,.S=0x1f9,.DB=1,.PB=1,.m_flag=1};
    cpu_write16(&c,0,0x18,0x0600); cpu_write16(&c,0,0x1a,0x0600);
    cpu_write16(&c,0,c.S+1,0x81be);
    assert(ActRaiser_SimMenuBrowseEntry(&c));
    assert(ActRaiser_SimMenuBrowse(&c)==RECOMP_RETURN_NORMAL);
    assert(Phase()==kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
    c.S-=2; cpu_write16(&c,0,c.S+1,0x81c3);
    assert(ActRaiser_SimMenuActionEntry(&c));
    assert(ActRaiser_SimMenuAction(&c)==RECOMP_RETURN_NORMAL);
    assert(!c._flag_C);
    if (expected_action==1) {
      ActRaiserSimMenu_ObserveScene(0x0600);
      assert(Phase()==kSimMenu_Handoff && ActRaiserSimMenu_OwnsPresentation());
      ActRaiserSimMenu_ObserveScene(7);
    }
    assert(Phase()==kSimMenu_Closed && !ActRaiserSimMenu_OwnsPresentation());
  }
  ActRaiserSimMenu_Reset();
  assert(!ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation());
  puts("actraiser_sim_menu: OK");
  return 0;
}
