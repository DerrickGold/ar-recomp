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
  plan.layer[0].role = kActionBgLayerRole_Playfield;
  plan.layer[0].source = kActionBgSource_WorldMap;
  plan.layer[0].world_width = 4096;
  plan.layer[0].world_height = 512;
  plan.layer[0].default_edge = kActionBgEdge_LiveWorld;
  plan.layer[1].role = kActionBgLayerRole_Backdrop;
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
  CHECK(Find(rows, count, "bg2.band0") != NULL);
  CHECK(Find(rows, count, "bg2.band0.horizontal") == NULL);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band0")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
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

static void TestStructuralBandAuthoring(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);

  /* The first added band occupies the largest uncovered interval, so the
   * existing lower band and new upper band are adjacent and deterministic. */
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band_add")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(Find(rows, count, "bg2.band0.anchor") != NULL);
  CHECK(Find(rows, count, "bg2.band1") != NULL);
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band0.end"), -1) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band_add")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(Find(rows, count, "bg2.band1.anchor") != NULL);

  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band1.edge"), +1) ==
        kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band1.motion"), +1) ==
        kActionBgTunerResult_Changed);
  /* This tiny world band is disjoint now but would cross band 0 as the camera
   * moves, so the editor rejects it. Converting the upper band is stable: a
   * World->Screen pair can only move farther apart after camera zero. */
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band1.anchor"), +1) ==
        kActionBgTunerResult_AtLimit);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band0")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band0.anchor"), +1) ==
        kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg_tuner.apply")) ==
        kActionBgTunerResult_Changed);

  ActionBgPlan applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].band_count == 3);
  CHECK(applied.layer[1].bands[0].anchor == kActionBgBandAnchor_World);
  CHECK(applied.layer[1].bands[0].y0 == 1 &&
        applied.layer[1].bands[0].y1 == 133);
  int resolved_y0 = 0, resolved_y1 = 0;
  CHECK(ActionBgLayerPlan_ResolveBand(
      &applied.layer[1], 0, &resolved_y0, &resolved_y1));
  CHECK(resolved_y0 == 0 && resolved_y1 == 132);
  CHECK(applied.layer[1].bands[1].anchor == kActionBgBandAnchor_Screen);
  CHECK(applied.layer[1].bands[1].motion == kActionBgMotion_NormalScroll);
  CHECK(ActionBgPlan_Validate(&applied));
  ActionBgPresentationPolicy presentation;
  CHECK(ActionBgPlan_CompilePresentation(&applied, &presentation));
  CHECK(presentation.band_count == 3);

  count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band1")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band1.delete")) ==
        kActionBgTunerResult_Changed);
  applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].band_count == 2);
  CHECK(ActionBgPlan_Validate(&applied));
}

static void TestStaleDraftIsAtomic(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2.band0")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg2.band0.anchor"), +1) ==
        kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_Change(Find(rows, count, "bg_tuner.apply"), +1) ==
        kActionBgTunerResult_Changed);

  /* A same-room canonical transition can narrow the source after the draft
   * was authored. The rejected world band must leave that new canonical plan
   * byte-exact so the caller can continue with its safe fallback. */
  ActionBgPlan transitioned = canonical;
  transitioned.layer[1].world_height = 128;
  CHECK(ActionBgPlan_Validate(&transitioned));
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &transitioned, (ActionBgTunerLimits){120, 120, 32, 32}));
  ActionBgPlan applied = transitioned;
  ActionBgPlan before = applied;
  CHECK(!ActionBgTuner_ApplyDraft(&applied));
  CHECK(!memcmp(&applied, &before, sizeof(applied)));
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
  wrong = canonical;
  wrong.layer[0].role = kActionBgLayerRole_Backdrop;
  before = wrong;
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

static void TestIgnoreSideBounds(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  canonical.layer[1].horizontal_extent = (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Fixed,
    .left = 12,
    .right = 20,
  };
  canonical.layer[1].bands[0].horizontal_extent =
      (ActionBgHorizontalExtent) {
        .mode = kActionBgExtent_Fixed,
        .left = 4,
        .right = 8,
      };
  CHECK(ActionBgPlan_Validate(&canonical));
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));

  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg2")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  ActionBgTunerRow *ignore = Find(rows, count, "bg2.ignore_side_bounds");
  CHECK(ignore != NULL);
  CHECK(!strcmp(ignore->value, "OFF"));
  CHECK(strstr(ActionBgTuner_RowHelp(ignore), "stored caps") != NULL);
  CHECK(ActionBgTuner_Change(ignore, +1) == kActionBgTunerResult_Changed);
  count = Rows(rows);
  ignore = Find(rows, count, "bg2.ignore_side_bounds");
  CHECK(ignore != NULL && !strcmp(ignore->value, "ON"));
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg_tuner.apply")) ==
        kActionBgTunerResult_Changed);

  ActionBgPlan applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(applied.layer[1].horizontal_extent.left == 0);
  CHECK(applied.layer[1].horizontal_extent.right == 0);
  CHECK(applied.layer[1].bands[0].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(applied.layer[1].bands[0].horizontal_extent.left == 0);
  CHECK(applied.layer[1].bands[0].horizontal_extent.right == 0);
  CHECK(ActionBgPlan_Validate(&applied));

  /* The shortcut is non-destructive: switching it off restores both the
   * layer cap and the more-specific row-band cap byte for byte. */
  CHECK(ActionBgTuner_Change(ignore, -1) == kActionBgTunerResult_Changed);
  applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(applied.layer[1].horizontal_extent.left == 12);
  CHECK(applied.layer[1].horizontal_extent.right == 20);
  CHECK(applied.layer[1].bands[0].horizontal_extent.mode ==
        kActionBgExtent_Fixed);
  CHECK(applied.layer[1].bands[0].horizontal_extent.left == 4);
  CHECK(applied.layer[1].bands[0].horizontal_extent.right == 8);

  CHECK(ActionBgTuner_Change(ignore, +1) == kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_ResetRow(ignore) == kActionBgTunerResult_Reset);
  count = Rows(rows);
  CHECK(!strcmp(Find(rows, count, "bg2.ignore_side_bounds")->value, "OFF"));
}

static void TestIgnoreVerticalBounds(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  canonical.layer[0].vertical_extent = (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Fixed,
    .top = 12,
    .bottom = 20,
  };
  CHECK(ActionBgPlan_Validate(&canonical));
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 64, 64}));

  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = Rows(rows);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg1")) ==
        kActionBgTunerResult_Changed);
  count = Rows(rows);
  ActionBgTunerRow *ignore = Find(
      rows, count, "bg1.ignore_vertical_bounds");
  CHECK(ignore != NULL && !strcmp(ignore->value, "OFF"));
  CHECK(strstr(ActionBgTuner_RowHelp(ignore), "Finite world edges") != NULL);
  CHECK(ActionBgTuner_Change(ignore, +1) == kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg_tuner.apply")) ==
        kActionBgTunerResult_Changed);

  ActionBgPlan applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[0].vertical_extent.mode ==
        kActionBgExtent_Available);
  CHECK(applied.layer[0].vertical_extent.top == 0);
  CHECK(applied.layer[0].vertical_extent.bottom == 0);

  /* Turning the shortcut back off restores the authored asymmetric cap. */
  CHECK(ActionBgTuner_Change(ignore, -1) == kActionBgTunerResult_Changed);
  applied = canonical;
  CHECK(ActionBgTuner_ApplyDraft(&applied));
  CHECK(applied.layer[0].vertical_extent.mode == kActionBgExtent_Fixed);
  CHECK(applied.layer[0].vertical_extent.top == 12);
  CHECK(applied.layer[0].vertical_extent.bottom == 20);
  CHECK(ActionBgTuner_Change(ignore, +1) == kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_ResetRow(ignore) == kActionBgTunerResult_Reset);
  count = Rows(rows);
  CHECK(!strcmp(Find(rows, count,
                     "bg1.ignore_vertical_bounds")->value, "OFF"));
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

static void TestRowCapacity(void) {
  ActionBgTuner_ResetSession();
  ActionBgPlan canonical = Plan();
  ActionBgLayerPlan *bg1 = &canonical.layer[0];
  bg1->horizontal_extent = (ActionBgHorizontalExtent) {
    .mode = kActionBgExtent_Fixed,
    .left = 32,
    .right = 40,
  };
  bg1->vertical_extent = (ActionBgVerticalExtent) {
    .mode = kActionBgExtent_Fixed,
    .top = 12,
    .bottom = 8,
  };
  for (int i = 0; i < kActionBgMaxBands; i++) {
    bg1->bands[i] = (ActionBgBand) {
      .y0 = (uint16_t)(i * 32),
      .y1 = (uint16_t)(i * 32 + 16),
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {
        .mode = kActionBgExtent_Fixed,
        .left = (uint16_t)(i * 4),
        .right = (uint16_t)(i * 4 + 4),
      },
    };
  }
  bg1->band_count = kActionBgMaxBands;
  CHECK(ActionBgPlan_Validate(&canonical));
  CHECK(ActionBgTuner_ObservePlan(
      2, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));

  ActionBgTunerRow rows[kActionBgTunerRowMax + 1];
  int count = ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
  ActionBgTunerRow *bg1_row = Find(rows, count, "bg1");
  CHECK(bg1_row != NULL);
  CHECK(ActionBgTuner_Activate(bg1_row) == kActionBgTunerResult_Changed);
  count = ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
  CHECK(ActionBgTuner_Activate(Find(rows, count, "bg1.band0")) ==
        kActionBgTunerResult_Changed);
  const int full_count = ActionBgTuner_BuildRows(
      rows, kActionBgTunerRowMax);
  CHECK(full_count > 0 && full_count <= kActionBgTunerRowMax);

  ActionBgTunerRow untouched;
  memset(&untouched, 0xa5, sizeof(untouched));
  for (int capacity = 1; capacity <= kActionBgTunerRowMax; capacity++) {
    memset(rows, 0xa5, sizeof(rows));
    count = ActionBgTuner_BuildRows(rows, capacity);
    CHECK(count == (capacity < full_count ? capacity : full_count));
    CHECK(!memcmp(&rows[capacity], &untouched, sizeof(untouched)));
  }
  CHECK(ActionBgTuner_BuildRows(rows, 0) == 0);
  CHECK(ActionBgTuner_BuildRows(NULL, kActionBgTunerRowMax) == 0);
}

int main(void) {
  TestDraftLifecycle();
  TestStructuralBandAuthoring();
  TestStaleDraftIsAtomic();
  TestResetAndAtomicity();
  TestIgnoreSideBounds();
  TestIgnoreVerticalBounds();
  TestGuideSegments();
  TestRowCapacity();
  if (failures) {
    fprintf(stderr, "action bg tuner: %d failure(s)\n", failures);
    return 1;
  }
  printf("action bg tuner: all checks passed\n");
  return 0;
}
