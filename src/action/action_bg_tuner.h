#ifndef ACTION_BG_TUNER_H
#define ACTION_BG_TUNER_H

#include <stdbool.h>
#include <stdint.h>

#include "action_bg_plan.h"

/* Developer-only, session-local authoring for action background presentation.
 * The tuner stores sparse overrides rather than a copied plan, so dynamic room
 * metadata (notably Death Heim) continues to come from the canonical planner.
 * Nothing is persisted or coupled to diorama-layers.ini. */

enum { kActionBgTunerRowMax = 32 };
enum {
  kActionBgTunerGuideMax = kActionBgPlanLayerCount *
      (2 * (2 * kActionBgMaxBands + 1) + 2),
};

typedef struct ActionBgTunerLimits {
  uint16_t left, right, top, bottom;
} ActionBgTunerLimits;

typedef enum ActionBgTunerRowKind {
  kActionBgTunerRow_Header = 0,
  kActionBgTunerRow_Apply,
  kActionBgTunerRow_Guides,
  kActionBgTunerRow_Layer,
  kActionBgTunerRow_Edge,
  kActionBgTunerRow_IgnoreSideBounds,
  kActionBgTunerRow_IgnoreVerticalBounds,
  kActionBgTunerRow_HorizontalMode,
  kActionBgTunerRow_Left,
  kActionBgTunerRow_Right,
  kActionBgTunerRow_VerticalMode,
  kActionBgTunerRow_Top,
  kActionBgTunerRow_Bottom,
  kActionBgTunerRow_BandHeader,
  kActionBgTunerRow_BandMode,
  kActionBgTunerRow_BandLeft,
  kActionBgTunerRow_BandRight,
  kActionBgTunerRow_Print,
  kActionBgTunerRow_Reset,
} ActionBgTunerRowKind;

typedef struct ActionBgTunerRow {
  ActionBgTunerRowKind kind;
  char key[48];
  char label[32];
  char value[24];
  int8_t layer;
  int8_t band;
  bool nested;
  bool selectable;
  bool separator_before;
} ActionBgTunerRow;

typedef enum ActionBgTunerResult {
  kActionBgTunerResult_Unchanged = 0,
  kActionBgTunerResult_Changed,
  kActionBgTunerResult_AtLimit,
  kActionBgTunerResult_Printed,
  kActionBgTunerResult_Reset,
} ActionBgTunerResult;

typedef struct ActionBgTunerGuide {
  int16_t x0, y0, x1, y1;
  uint8_t layer;
} ActionBgTunerGuide;

/* Frame lifecycle. BeginFrame removes stale live-room visibility; ObservePlan
 * publishes the current canonical action room and atomically resets sparse
 * draft overrides when the room changes. Guides remain a session preference,
 * while applying a draft always starts disabled in a new room. */
void ActionBgTuner_BeginFrame(void);
bool ActionBgTuner_ObservePlan(uint8_t map_group, uint8_t map_number,
                              const ActionBgPlan *canonical,
                              ActionBgTunerLimits limits);

/* Project the enabled sparse draft over the just-observed canonical plan.
 * Atomic: false leaves `plan` untouched. */
bool ActionBgTuner_ApplyDraft(ActionBgPlan *plan);

bool ActionBgTuner_IsLive(void);
bool ActionBgTuner_DraftEnabled(void);
bool ActionBgTuner_GuidesEnabled(void);
void ActionBgTuner_ResetSession(void);

/* Pure row-model boundary consumed by Settings > Layers > BG Extents. */
int ActionBgTuner_BuildRows(ActionBgTunerRow *out, int capacity);
ActionBgTunerResult ActionBgTuner_Change(const ActionBgTunerRow *row,
                                        int direction);
ActionBgTunerResult ActionBgTuner_Activate(const ActionBgTunerRow *row);
ActionBgTunerResult ActionBgTuner_ResetRow(const ActionBgTunerRow *row);
const char *ActionBgTuner_RowHelp(const ActionBgTunerRow *row);

/* Resolve fixed caps into line segments in authentic-screen coordinates.
 * Horizontal band overrides split vertical guide lines exactly where the PPU
 * policy changes. Available/Inherit regions emit no false boundary. */
int ActionBgTuner_BuildGuides(const ActionBgPlan *plan,
                              ActionBgTunerGuide *out, int capacity);

#endif  /* ACTION_BG_TUNER_H */
