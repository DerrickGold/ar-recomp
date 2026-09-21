#include "actraiser/actraiser_hud.h"

#include "actraiser_game.h"

static ActRaiserHudOwner s_staged, s_presented;

static ActRaiserHudKind SceneKind(uint8_t group, uint8_t map) {
  if (group >= kActRaiserActionMapGroup_First &&
      group <= kActRaiserActionMapGroup_Last)
    return kActRaiserHud_Action;
  if (group == kActRaiserMapGroup_NonAction &&
      map >= kActRaiserSimulationTown_First &&
      map <= kActRaiserNonActionMap_Temple)
    return kActRaiserHud_Simulation;
  return kActRaiserHud_None;
}

void ActRaiserHud_Reset(void) {
  s_staged = s_presented = (ActRaiserHudOwner){0};
}

void ActRaiserHud_ObserveScene(uint8_t group, uint8_t map) {
  /* Title/world/credits may retain the old words, but have no visible HUD.
   * Ordinary menus and pauses leave ownership alone. Pending destination
   * maps are intentionally ignored, preserving the outgoing fade. */
  if (SceneKind(group, map) == kActRaiserHud_None) ActRaiserHud_Reset();
}

ActRaiserHudOwner ActRaiserHud_Presented(uint8_t group, uint8_t map) {
  if (s_presented.kind != SceneKind(group, map))
    return (ActRaiserHudOwner){0};
  return s_presented;
}

void ActRaiserHud_ObserveTemplate(CpuState *cpu) {
  s_staged = (ActRaiserHudOwner){0};
  if (cpu && cpu->PB == 2 && cpu->D == 0 && !cpu->emulation)
    s_staged.kind = SceneKind(cpu_read8(cpu, 0, kActRaiserWram_MapGroup),
                             cpu_read8(cpu, 0, kActRaiserWram_CurrentMap));
}

void ActRaiserHud_ObserveClear(void) {
  s_staged = (ActRaiserHudOwner){0};
}

bool ActRaiser_HudObserveEnemy(CpuState *cpu) {
  if (cpu && cpu->PB == 0 && cpu->D == 0 && !cpu->x_flag &&
      !cpu->emulation && s_staged.kind == kActRaiserHud_Action)
    s_staged.enemy = true;
  return false; /* Execute the original label writer and health controller. */
}

bool ActRaiser_HudObserveEnemyClear(CpuState *cpu) {
  if (cpu && cpu->PB == 0 && cpu->D == 0 && !cpu->x_flag && !cpu->emulation)
    s_staged.enemy = false;
  return false;
}

void ActRaiserHud_ObserveUpload(void) {
  /* This DMA is unconditional: $F1 only gates the lower text rows. In
   * particular ENEMY creation/removal never increments that dirty counter. */
  s_presented = s_staged;
}
