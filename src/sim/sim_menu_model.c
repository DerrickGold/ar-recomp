#include "sim/sim_menu_model.h"

#include <string.h>

const uint16_t kSimMenuCategoryPointers[6] = {
    0xf32e, 0xf332, 0xf336, 0xf33d, 0xf341, 0xf345};
const uint8_t kSimMenuCategoryActionCounts[6] = {2, 2, 5, 2, 2, 2};
const SimMenuAction kSimMenuActions[15] = {
    {"sim.menu.return_to_palace", 0, 0, 0, 0xf32f},
    {"sim.menu.sky_palace_movement", 0, 1, 0, 0xf330},
    {"sim.menu.building_direction", 1, 0, 0, 0xf333},
    {"sim.menu.listen", 1, 1, 0, 0xf334},
    {"sim.menu.lightning", 2, 0, 10, 0xf337},
    {"sim.menu.rain", 2, 1, 20, 0xf338},
    {"sim.menu.sun", 2, 2, 30, 0xf339},
    {"sim.menu.wind", 2, 3, 80, 0xf33a},
    {"sim.menu.earthquake", 2, 4, 160, 0xf33b},
    {"sim.menu.take_offering", 3, 0, 0, 0xf33e},
    {"sim.menu.use_offering", 3, 1, 0, 0xf33f},
    {"sim.menu.status_master", 4, 0, 0, 0xf342},
    {"sim.menu.status_cities", 4, 1, 0, 0xf343},
    {"sim.menu.progress_log", 5, 0, 0, 0xf346},
    {"sim.menu.message_speed", 5, 1, 0, 0xf347},
};

void SimMenuModel_ReleaseBarrier(SimMenuModel *m) {
  m->held=0xff; m->repeat_key=m->repeat_ticks=0;
}

void SimMenuModel_Open(SimMenuModel *m, uint64_t generation,
                       uint16_t selection) {
  memset(m, 0, sizeof(*m));
  m->generation = generation;
  m->phase = kSimMenu_Browse;
  for (unsigned i = 0; i < 6; ++i)
    if (selection == kSimMenuCategoryPointers[i]) m->category = i;
  for (unsigned i = 0; i < 15; ++i) {
    const SimMenuAction *a = &kSimMenuActions[i];
    if (selection != a->selection_pointer) continue;
    m->category = a->category;
    m->row[a->category] = a->row;
    m->submenu = true;
  }
  SimMenuModel_ReleaseBarrier(m);
}

uint8_t SimMenuModel_Action(const SimMenuModel *m) {
  if (m->category >= 6) return 0;
  for (unsigned i = 0; i < 15; ++i)
    if (kSimMenuActions[i].category == m->category &&
        kSimMenuActions[i].row == m->row[m->category]) return i + 1;
  return 0;
}

uint16_t SimMenuModel_Selection(const SimMenuModel *m) {
  const uint8_t action = SimMenuModel_Action(m);
  return m->submenu && action ? kSimMenuActions[action - 1].selection_pointer
                             : kSimMenuCategoryPointers[m->category % 6];
}

void SimMenuModel_Inventory(SimMenuModel *m, const uint8_t items[8]) {
  memcpy(m->items, items, 8);
  m->item_count = 0;
  /* Native $9239 compacts before this boundary. Keep slot identity intact. */
  while (m->item_count < 8 && items[m->item_count]) ++m->item_count;
  m->item_slot = 0;
  m->phase = kSimMenu_Inventory;
  SimMenuModel_ReleaseBarrier(m);
}

void SimMenuModel_Confirm(SimMenuModel *m) {
  m->phase = kSimMenu_Confirm;
  m->yes = true;
  SimMenuModel_ReleaseBarrier(m);
}

void SimMenuModel_EndDescription(SimMenuModel *m) {
  if (m->phase != kSimMenu_Describe) return;
  m->phase = m->return_phase;
  SimMenuModel_ReleaseBarrier(m);
}

SimMenuEvent SimMenuModel_Poll(SimMenuModel *m, uint8_t buttons) {
  if (m->held == 0xff) {
    if (!buttons) m->held=0;
    return kSimMenuEvent_None;
  }
  /* Opposing directions are neutral. A diagonal retains the dock's existing
   * horizontal priority and repeats only that axis. */
  if ((buttons&3)==3) buttons&=~3u;
  if ((buttons&12)==12) buttons&=~12u;
  uint8_t edge = buttons & ~m->held;
  m->held = buttons;
  uint8_t direction=buttons&3;
  if (!direction) direction=buttons&12;
  if ((m->phase!=kSimMenu_Browse && m->phase!=kSimMenu_Inventory) ||
      (buttons&(kSimMenuInput_Back|kSimMenuInput_Use|kSimMenuInput_Describe)))
    direction=0;
  if (m->phase==kSimMenu_Browse || m->phase==kSimMenu_Inventory)
    edge=(edge&~15u)|(edge&direction);
  if (!direction) m->repeat_key=m->repeat_ticks=0;
  else if (direction!=m->repeat_key) {
    m->repeat_key=direction; m->repeat_ticks=18;
    edge|=direction;
  } else if (m->repeat_ticks && !--m->repeat_ticks) {
    edge|=direction; m->repeat_ticks=6;
  }
  if (!edge || (m->phase != kSimMenu_Browse && m->phase != kSimMenu_Inventory &&
                m->phase != kSimMenu_Describe && m->phase != kSimMenu_Confirm))
    return kSimMenuEvent_None;
  if (edge & kSimMenuInput_Back) {
    if (m->phase == kSimMenu_Describe) {
      SimMenuModel_EndDescription(m);
      return kSimMenuEvent_Changed;
    }
    if (m->phase == kSimMenu_Confirm) {
      m->yes = false;
      return kSimMenuEvent_Use;
    }
    if (m->phase == kSimMenu_Browse && m->submenu) {
      m->submenu = false;
      return kSimMenuEvent_Changed;
    }
    return kSimMenuEvent_Close;
  }
  if (m->phase == kSimMenu_Describe)
    return edge & kSimMenuInput_Use ? kSimMenuEvent_Advance : kSimMenuEvent_None;
  if (m->phase == kSimMenu_Confirm) {
    if (edge & kSimMenuInput_Use) return kSimMenuEvent_Use;
    if (edge & (kSimMenuInput_Left | kSimMenuInput_Up)) m->yes = true;
    else if (edge & (kSimMenuInput_Right | kSimMenuInput_Down)) m->yes = false;
    else return kSimMenuEvent_None;
    return kSimMenuEvent_Changed;
  }
  if (edge & kSimMenuInput_Describe) {
    ++m->dialogue_generation;
    m->dialogue_source=0;
    m->return_phase = m->phase;
    m->phase = kSimMenu_Describe;
    SimMenuModel_ReleaseBarrier(m);
    return kSimMenuEvent_Describe;
  }
  if (edge & kSimMenuInput_Use) {
    if (m->phase == kSimMenu_Browse && !m->submenu) {
      m->submenu = true;
      return kSimMenuEvent_Changed;
    }
    return kSimMenuEvent_Use;
  }
  if (m->phase == kSimMenu_Inventory) {
    if (!m->item_count) return kSimMenuEvent_None;
    if (edge & (kSimMenuInput_Up | kSimMenuInput_Left))
      m->item_slot = (m->item_slot + m->item_count - 1) % m->item_count;
    else if (edge & (kSimMenuInput_Down | kSimMenuInput_Right))
      m->item_slot = (m->item_slot + 1) % m->item_count;
    else return kSimMenuEvent_None;
    return kSimMenuEvent_Changed;
  }
  if (edge & (kSimMenuInput_Left | kSimMenuInput_Right)) {
    m->category = (m->category + (edge & kSimMenuInput_Left ? 5 : 1)) % 6;
  } else if (edge & kSimMenuInput_Down) {
    if (m->submenu)
      m->row[m->category] = (m->row[m->category] + 1) %
                           kSimMenuCategoryActionCounts[m->category];
    m->submenu = true;
  } else if (edge & kSimMenuInput_Up) {
    if (!m->submenu) return kSimMenuEvent_None;
    if (m->row[m->category]) --m->row[m->category];
    else m->submenu = false;
  } else return kSimMenuEvent_None;
  return kSimMenuEvent_Changed;
}
