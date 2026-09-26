#ifndef AR_ACTRAISER_HUD_H
#define AR_ACTRAISER_HUD_H
/* ActRaiserHud: tracks which native producer owns the HUD's BG3 rows (visible
 * only after their upload), independent of language and font, for the
 * localized HUD.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_hud_test.c */

#include "snesrecomp/game/cpu.h"

typedef enum ActRaiserHudKind {
  kActRaiserHud_None,
  kActRaiserHud_Action,
  kActRaiserHud_Simulation,
} ActRaiserHudKind;

typedef struct ActRaiserHudOwner {
  ActRaiserHudKind kind;
  bool enemy;
} ActRaiserHudOwner;

/* Native BG3 ownership, independent of language and font settings. These
 * producers run without yielding; their staging changes become visible only
 * after AEEB uploads the top four rows. No tiles or pixels identify an owner. */
void ActRaiserHud_Reset(void);
void ActRaiserHud_ObserveScene(uint8_t group, uint8_t map);
ActRaiserHudOwner ActRaiserHud_Presented(uint8_t group, uint8_t map);
void ActRaiserHud_ObserveTemplate(CpuState *cpu); /* $02:BA41, after clear observer */
void ActRaiserHud_ObserveClear(void);            /* $02:ABC4/$BA41 */
void ActRaiserHud_ObserveUpload(void);           /* completed $02:AEEB */
bool ActRaiser_HudObserveEnemy(CpuState *cpu);   /* $00:A4C3 */
bool ActRaiser_HudObserveEnemyClear(CpuState *cpu); /* $00:88F7 */

#endif
