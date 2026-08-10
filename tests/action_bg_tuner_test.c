#include "action/action_bg_tuner.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
  failures++; \
} } while (0)

static ActionBgPlan Plan(void) {
  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  plan.layer[0].source = kActionBgSource_WorldMap;
  plan.layer[0].world_width = 4096;
  plan.layer[0].world_height = 512;
  plan.layer[0].default_edge = kActionBgEdge_LiveWorld;
  plan.layer[1].source = kActionBgSource_AuthenticViewport;
  plan.layer[1].world_width = 256;
  plan.layer[1].world_height = 256;
  plan.layer[1].default_edge = kActionBgEdge_Mirror;
  plan.layer[1].bands[0] = (ActionBgBand) {
    .y0 = 136,
    .y1 = 224,
    .edge = kActionBgEdge_Repeat,
  };
  plan.layer[1].band_count = 1;
  return plan;
}

static ActionBgTunerRow *Find(ActionBgTunerRow *rows, int count,
                              const char *key) {
  for (int i = 0; i < count; i++)
    if (!strcmp(rows[i].key, key)) return &rows[i];
  return NULL;
}

static int Rows(ActionBgTunerRow *rows) {
  return ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
}

static void TestDraftLifecycle(void) {
  ActionBgTuner_ResetSession();
  ActionBgTunerRow rows[kActionBgTunerRowMax];
  CHECK(Rows(rows) == 1);
  CHECK(!rows[0].selectable);

  ActionBgPlan canonical = Plan();
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
  int count = Rows(rows);
  CHECK(count == 7);
  CHECK(Find(rows, count, "bg_tuner.apply") != NULL);
  CHECK(!ActionBgTuner_DraftEnabled());

  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(Find(rows, count, "bg2.horizontal") != NULL);
  CHECK(Find(rows, count, "bg2.band0.horizontal") != NULL);

  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.horizontal"), +1) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(Find(rows, count, "bg2.left") != NULL);
  CHECK(!strcmp(Find(rows, count, "bg2.left")->value, "120 px"));
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.left"), -1) ==
        kActionBgTunerResult_Changed);

  /* Band Inherit -> Available removes the new layer cap only on water rows. */
  CHECK(ActionBgTuner_Change(
      Find(rows, count, "bg2.band0.horizontal"), +1) ==
      kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(!strcmp(Find(rows, count, "bg2.band0.horizontal")->value,
                "AVAILABLE"));

  ActionBgPlan applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg_tuner.apply")) ==
        kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_DraftEnabled());
  applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(applied.layer[1].horizontal_extent.left == 116);
  CHECK(applied.layer[1].horizontal_extent.right == 120);
  CHECK(applied.layer[1].bands[0].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(ActionBgPlan_Validate(&applied));
  ActionBgPresentationPolicy presentation;
  CHECK(ActionBgPlan_CompilePresentation(&applied, &presentation));

  /* The per-frame live marker is not a room transition and must not discard
   * the session draft when the same canonical plan is observed again. */
  ActionBgTuner_BeginFrame();
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
  CHECK(ActionBgTuner_DraftEnabled());
  applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].horizontal_extent.left == 116);

  /* A room change is an atomic A/B reset, while the live canonical updates. */
  ActionBgPlan next = canonical;
  next.layer[1].default_edge = kActionBgEdge_Clamp;
  CHECK(ActionBgTuner_ObservePlan(
      2, 2, &next, (ActionBgTunerLimits){120, 120, 32, 32}));
  CHECK(!ActionBgTuner_DraftEnabled());
  applied = next;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].default_edge == kActionBgEdge_Clamp);
}

static void TestResetAndAtomicity(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){64, 80, 12, 8}));
  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = Rows(rows);
  ActionBgTuner_Activate(Find(rows, count, "bg1"));
  count = Rows(rows);
  ActionBgTuner_Change(Find(rows, count, "bg1.vertical"), +1);
  count = Rows(rows);
  CHECK(!strcmp(Find(rows, count, "bg1.top")->value, "12 px"));
  ActionBgTuner_Change(Find(rows, count, "bg_tuner.guides"), +1);
  CHECK(ActionBgTuner_GuidesEnabled());
  ActionBgTuner_Change(Find(rows, count, "bg_tuner.apply"), +1);
  CHECK(ActionBgTuner_ResetRow(Find(rows, count, "bg1.top")) ==
        kActionBgTunerResult_Reset);
  count = Rows(rows);
  CHECK(Find(rows, count, "bg1.top") == NULL);

  ActionBgPlan wrong = canonical;
  wrong.layer[0].world_width++;
  ActionBgPlan before = wrong;
  CHECK(!ActionBgTuner_ApplyDraft(&wrong));
  CHECK(!memcmp(&wrong, &before, sizeof(wrong)));

  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg_tuner.reset")) ==
        kActionBgTunerResult_Reset);
  CHECK(!ActionBgTuner_DraftEnabled());
  CHECK(ActionBgTuner_GuidesEnabled());
  ActionBgTuner_BeginFrame();
  CHECK(!ActionBgTuner_IsLive());
  CHECK(!ActionBgTuner_GuidesEnabled());
}

static void TestGuideSegments(void) {
  ActionBgPlan plan = Plan();
  plan.layer[1].horizontal_extent = (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Fixed,
    .left = 48,
    .right = 64,
  };
  plan.layer[1].bands[0].horizontal_extent =
      (ActionBgHorizontalExtent){ .mode = kActionBgExtent_Available };
  plan.layer[1].vertical_extent = (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Fixed,
    .top = 12,
    .bottom = 4,
  };
  ActionBgTunerGuide guides[kActionBgTunerGuideMax];
  int count = ActionBgTuner_BuildGuides(
      &plan, guides, kActionBgTunerGuideMax);
  CHECK(count == 4);
  CHECK(guides[0].layer == 1 && guides[0].x0 == -48 &&
        guides[0].x1 == -48 && guides[0].y0 == 0 && guides[0].y1 == 136);
  CHECK(guides[1].x0 == 320 && guides[1].x1 == 320 &&
        guides[1].y0 == 0 && guides[1].y1 == 136);
  CHECK(guides[2].x0 == -48 && guides[2].x1 == 320 &&
        guides[2].y0 == -12 && guides[2].y1 == -12);
  CHECK(guides[3].x0 == -48 && guides[3].x1 == 320 &&
        guides[3].y0 == 228 && guides[3].y1 == 228);
  CHECK(ActionBgTuner_BuildGuides(NULL, guides,
                                  kActionBgTunerGuideMax) == 0);
}

int main(void) {
  TestDraftLifecycle();
  TestResetAndAtomicity();
  TestGuideSegments();
  if (failures) {
    fprintf(stderr, "action bg tuner: %d failure(s)\n", failures);
    return 1;
  }
  printf("action bg tuner: all checks passed\n");
  return 0;
}
