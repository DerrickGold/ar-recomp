#include "sim/sim_menu_model.h"
#include <assert.h>
#include <stdio.h>

static SimMenuEvent Press(SimMenuModel *m, uint8_t key) {
  SimMenuModel_Poll(m, 0);
  return SimMenuModel_Poll(m, key);
}

int main(void) {
  SimMenuModel m;
  SimMenuModel_Open(&m, 1, 0xf32e);
  assert(SimMenuModel_Poll(&m, kSimMenuInput_Use) == kSimMenuEvent_None);
  /* Every command is reachable, with the same ID and native node. */
  for (unsigned category = 0, action = 1; category < 6; ++category) {
    assert(m.category == category);
    Press(&m, kSimMenuInput_Down);
    for (unsigned row = 0; row < kSimMenuCategoryActionCounts[category]; ++row) {
      assert(SimMenuModel_Action(&m) == action);
      assert(SimMenuModel_Selection(&m) == kSimMenuActions[action - 1].selection_pointer);
      assert(Press(&m, kSimMenuInput_Use) == kSimMenuEvent_Use);
      ++action;
      if (row + 1 < kSimMenuCategoryActionCounts[category])
        Press(&m, kSimMenuInput_Down);
    }
    Press(&m, kSimMenuInput_Back);
    Press(&m, kSimMenuInput_Right);
  }
  assert(m.category == 0);
  /* Held navigation has a bounded initial delay and repeat cadence. Releasing
   * or changing direction resets it; held face buttons never auto-activate. */
  SimMenuModel_Open(&m,20,0xf32e);
  assert(Press(&m,kSimMenuInput_Right)==kSimMenuEvent_Changed && m.category==1);
  for (int i=0;i<17;++i)
    assert(SimMenuModel_Poll(&m,kSimMenuInput_Right)==kSimMenuEvent_None);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Right)==kSimMenuEvent_Changed && m.category==2);
  for (int i=0;i<5;++i)
    assert(SimMenuModel_Poll(&m,kSimMenuInput_Right)==kSimMenuEvent_None);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Right)==kSimMenuEvent_Changed && m.category==3);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Left)==kSimMenuEvent_Changed && m.category==2);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Left|kSimMenuInput_Right)==kSimMenuEvent_None);
  assert(Press(&m,kSimMenuInput_Right)==kSimMenuEvent_Changed);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Right|kSimMenuInput_Down)==kSimMenuEvent_None);
  assert(!m.submenu);
  assert(SimMenuModel_Poll(&m,kSimMenuInput_Down)==kSimMenuEvent_Changed && m.submenu);
  Press(&m,kSimMenuInput_Back);
  assert(Press(&m,kSimMenuInput_Use)==kSimMenuEvent_Changed && m.submenu);
  for (int i=0;i<90;++i)
    assert(SimMenuModel_Poll(&m,kSimMenuInput_Use)==kSimMenuEvent_None);
  /* Help never dispatches; restore the exact Sun node after every page. */
  SimMenuModel_Open(&m, 2, 0xf339);
  assert(Press(&m, kSimMenuInput_Describe) == kSimMenuEvent_Describe);
  assert(m.phase == kSimMenu_Describe);
  const uint32_t description_generation=m.dialogue_generation;
  assert(SimMenuModel_Poll(&m, kSimMenuInput_Use) == kSimMenuEvent_None);
  for (int i = 0; i < 8; ++i)
    assert(Press(&m, kSimMenuInput_Use) == kSimMenuEvent_Advance);
  assert(m.dialogue_generation==description_generation);
  Press(&m, kSimMenuInput_Back | kSimMenuInput_Use);
  assert(m.phase == kSimMenu_Browse && SimMenuModel_Selection(&m) == 0xf339);
  assert(SimMenuModel_Poll(&m, kSimMenuInput_Use) == kSimMenuEvent_None);
  SimMenuModel_Confirm(&m);
  assert(m.dialogue_generation==description_generation);
  assert(m.yes);
  assert(Press(&m, kSimMenuInput_Back) == kSimMenuEvent_Use && !m.yes);
  assert(SimMenuModel_Selection(&m) == 0xf339);
  /* Duplicate names/art do not merge item IDs or slots. */
  const uint8_t items[8] = {12, 13, 16, 17, 18, 18, 7, 8};
  SimMenuModel_Inventory(&m, items);
  for (int i = 0; i < 8; ++i) {
    assert(m.item_slot == i && m.items[m.item_slot] == items[i]);
    Press(&m, kSimMenuInput_Describe);
    SimMenuModel_EndDescription(&m);
    assert(m.phase == kSimMenu_Inventory && m.item_slot == i);
    Press(&m, kSimMenuInput_Down);
  }
  assert(m.item_slot == 0);
  /* Presentation-only phases cannot dispatch a second action or navigation. */
  const SimMenuPhase native_phases[]={kSimMenu_Native,kSimMenu_Dialogue,
      kSimMenu_MessageSpeed,kSimMenu_Handoff,kSimMenu_Opening};
  for (unsigned i=0;i<sizeof(native_phases)/sizeof(native_phases[0]);++i) {
    m.phase=native_phases[i];
    assert(Press(&m,kSimMenuInput_Use)==kSimMenuEvent_None);
    assert(Press(&m,kSimMenuInput_Back)==kSimMenuEvent_None);
    assert(Press(&m,kSimMenuInput_Describe)==kSimMenuEvent_None);
    assert(Press(&m,kSimMenuInput_Right)==kSimMenuEvent_None);
    assert(m.phase==native_phases[i] && m.item_slot==0);
  }
  puts("sim_menu_model: OK");
  return 0;
}
