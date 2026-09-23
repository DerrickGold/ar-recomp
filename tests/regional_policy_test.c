#include "regional/regional_costs.h"
#include "regional/regional_stock.h"
#include "settings_overlay_regions.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static const uint16_t kExpected[3][9] = {
  {1, 1, 1, 1, 10, 20, 30, 80, 160},
  {1, 2, 3, 4, 12, 16, 18, 24, 60},
  {1, 1, 1, 1, 10, 20, 30, 80, 160},
};
_Static_assert(kArRegionalSource_Count == 3 && kArRegionalCostRule_Count == 9,
               "extend the independent pricing oracle when adding rules");

static void CheckPolicies(void) {
  ArRegionalCostPolicy baseline, policy, other_save;
  CHECK(ArRegionalCosts_Init(&baseline, kArRegionalSource_US));
  CHECK(ArRegionalCosts_Init(&other_save, kArRegionalSource_Japan));
  const ArRegionalCostPolicy saved = other_save;
  CHECK(!ArRegionalCosts_Descriptor((ArRegionalCostRule)-1));
  CHECK(!ArRegionalCosts_Descriptor(kArRegionalCostRule_Count));
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *d = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    CHECK(d && d->key && d->key[0]);
    CHECK(d->group == (i < 4 ? kArRegionalCostGroup_Scrolls : kArRegionalCostGroup_Miracles));
    for (unsigned j = 0; j < i; ++j)
      CHECK(strcmp(d->key, ArRegionalCosts_Descriptor((ArRegionalCostRule)j)->key));
    for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
      CHECK(d->price[source] == kExpected[source][i]);
  }

  /* Every combination in this bounded pricing subset (3^9), not a claim of
   * cross-product validation for the remaining regional gameplay families. */
  for (unsigned combination = 0; combination < 19683; ++combination) {
    unsigned digits = combination;
    CHECK(ArRegionalCosts_Init(&policy, kArRegionalSource_US));
    uint16_t source_changes = 0, price_changes = 0;
    for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
      unsigned source = digits % 3;
      digits /= 3;
      CHECK(ArRegionalCosts_SetRule(&policy, (ArRegionalCostRule)i, (ArRegionalSource)source));
      if (source) source_changes |= (uint16_t)(1u << i);
      if (kExpected[source][i] != kExpected[0][i]) price_changes |= (uint16_t)(1u << i);
    }
    const ArRegionalCostPolicy before = policy;
    ArRegionalCostPreview preview;
    CHECK(ArRegionalCosts_Preview(&baseline, &policy, &preview));
    CHECK(preview.source_changes == source_changes);
    CHECK(preview.price_changes == price_changes);
    for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
      CHECK(preview.current.price[i] == kExpected[0][i]);
      CHECK(preview.requested.price[i] == kExpected[policy.source[i]][i]);
    }
    CHECK(!memcmp(&before, &policy, sizeof(policy)));
    CHECK(ArRegionalCosts_SetGroup(&policy, kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
    for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
      CHECK(policy.source[i] == (i < 4 ? before.source[i] : kArRegionalSource_Japan));
    ArRegionalSource source;
    CHECK(ArRegionalCosts_GroupSource(&policy, kArRegionalCostGroup_Miracles, &source));
    CHECK(source == kArRegionalSource_Japan);
    bool uniform = true;
    for (unsigned i = 1; i < 4; ++i) uniform &= policy.source[0] == policy.source[i];
    int summary = uniform ? (int)policy.source[0] : -1;
    for (int candidate = 0; summary < 0 && candidate < 3; ++candidate) {
      bool matches = true;
      for (unsigned i = 0; i < 4; ++i)
        matches &= kExpected[policy.source[i]][i] == kExpected[candidate][i];
      if (matches) summary = candidate;
    }
    CHECK(ArRegionalCosts_GroupSource(&policy, kArRegionalCostGroup_Scrolls, &source) == (summary >= 0));
    if (summary >= 0) CHECK((int)source == summary);
  }
  CHECK(!memcmp(&saved, &other_save, sizeof(saved)));

  policy = baseline;
  ArRegionalCostSnapshot in_flight;
  CHECK(ArRegionalCosts_Resolve(&policy, &in_flight));
  CHECK(ArRegionalCosts_SetGroup(&policy, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(in_flight.price[kArRegionalCost_Light] == 1);
  CHECK(ArRegionalCosts_SetGroup(&policy, kArRegionalCostGroup_Scrolls, kArRegionalSource_US));
  CHECK(!memcmp(&policy, &baseline, sizeof(policy)));

  CHECK(!ArRegionalCosts_Init(&policy, (ArRegionalSource)-1));
  CHECK(!ArRegionalCosts_SetRule(&policy, (ArRegionalCostRule)-1, kArRegionalSource_US));
  CHECK(!ArRegionalCosts_SetRule(&policy, kArRegionalCost_Fire, kArRegionalSource_Count));
  CHECK(!ArRegionalCosts_SetGroup(&policy, kArRegionalCostGroup_Count, kArRegionalSource_US));
  CHECK(!memcmp(&policy, &baseline, sizeof(policy)));
  policy.source[kArRegionalCost_Earthquake] = kArRegionalSource_Count;
  ArRegionalCostSnapshot snapshot = in_flight;
  CHECK(!ArRegionalCosts_Resolve(&policy, &snapshot));
  CHECK(!memcmp(&snapshot, &in_flight, sizeof(snapshot)));
  ArRegionalCostPreview preview, sentinel;
  memset(&preview, 0xa5, sizeof(preview));
  memcpy(&sentinel, &preview, sizeof(preview));
  CHECK(!ArRegionalCosts_Preview(&baseline, &policy, &preview));
  CHECK(!memcmp(&preview, &sentinel, sizeof(preview)));
  CHECK(!ArRegionalCosts_Preview(&policy, &baseline, &preview));
  CHECK(!memcmp(&preview, &sentinel, sizeof(preview)));
  CHECK(!ArRegionalCosts_Resolve(NULL, &snapshot));
  CHECK(!ArRegionalCosts_Resolve(&baseline, NULL));
  CHECK(!ArRegionalCosts_Init(NULL, kArRegionalSource_US));
  CHECK(!ArRegionalCosts_GroupSource(&baseline, kArRegionalCostGroup_Scrolls, NULL));
}

static void CheckDescriptions(void) {
  ArRegionalCostPolicy policy;
  for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
    for (int source = 0; source < kArRegionalSource_Count; ++source) {
      CHECK(ArRegionalCosts_Init(&policy, (ArRegionalSource)source));
      for (int group = 0; group < kArRegionalCostGroup_Count; ++group) {
        char text[2048] = "unchanged";
        CHECK(SettingsOverlayRegions_CostDescription((ArUiLocale)locale, &policy,
              (ArRegionalCostGroup)group, text, sizeof(text)));
        CHECK(text[0] && !strchr(text, '{') && !strchr(text, '}'));
        SettingsOverlayRegionBadge badge;
        CHECK(SettingsOverlayRegions_CostBadge(&policy, (ArRegionalCostGroup)group, &badge));
        CHECK(strstr(text, SettingsOverlayRegions_BadgeLabel((ArUiLocale)locale, badge)));
        for (unsigned i = group ? 4 : 0; i < (group ? 9u : 4u); ++i) {
          char number[8];
          snprintf(number, sizeof(number), "%u", kExpected[source][i]);
          CHECK(strstr(text, number));
        }
        strcpy(text, "unchanged");
        CHECK(!SettingsOverlayRegions_CostDescription((ArUiLocale)locale, &policy,
              (ArRegionalCostGroup)group, text, 4));
        CHECK(!strcmp(text, "unchanged"));
      }
    }
  }
  CHECK(ArRegionalCosts_Init(&policy, kArRegionalSource_US));
  CHECK(ArRegionalCosts_SetRule(&policy, kArRegionalCost_Light, kArRegionalSource_Japan));
  SettingsOverlayRegionBadge badge;
  CHECK(SettingsOverlayRegions_CostBadge(&policy, kArRegionalCostGroup_Scrolls, &badge));
  CHECK(badge == kOverlayRegionBadge_Mixed);
  char text[2048];
  CHECK(SettingsOverlayRegions_CostDescription(kArUiLocale_English, &policy,
        kArRegionalCostGroup_Scrolls, text, sizeof(text)));
  CHECK(strstr(text, "Custom scroll costs: Fire 1, Stardust 1, Aura 1, Light 4."));
  CHECK(strstr(text, "inventory and controls stay unchanged"));
  CHECK(strstr(text, "never a cast in progress"));
  CHECK(SettingsOverlayRegions_CostBadge(&policy, kArRegionalCostGroup_Miracles, &badge));
  CHECK(badge == kOverlayRegionBadge_US);
  /* Mixed provenance with identical prices is not a Custom gameplay rule. */
  CHECK(ArRegionalCosts_SetRule(&policy, kArRegionalCost_Rain, kArRegionalSource_Europe));
  CHECK(SettingsOverlayRegions_CostBadge(&policy, kArRegionalCostGroup_Miracles, &badge));
  CHECK(badge == kOverlayRegionBadge_US);
  CHECK(!SettingsOverlayRegions_CostBadge(NULL, kArRegionalCostGroup_Miracles, &badge));
  CHECK(badge == kOverlayRegionBadge_US);
  CHECK(!SettingsOverlayRegions_CostDescription(kArUiLocale_English, &policy,
        kArRegionalCostGroup_Count, text, sizeof(text)));
}

static void CheckStockEstimate(void) {
  const uint32_t cases[][5] = {
    {100, 200, 250, 65535, 150}, {230, 200, 250, 65535, 280},
    {0, 200, 250, 65535, 50}, {20, 250, 200, 65535, 0},
    {65530, 200, 250, 65535, 65535}, {0, 0, 0, 0, 0},
    {UINT32_MAX, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX},
    {0, UINT32_MAX, 0, UINT32_MAX, 0},
    {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uint32_t value = 123;
    CHECK(ArRegionalStock_Estimate(cases[i][0], cases[i][1], cases[i][2], cases[i][3], &value));
    CHECK(value == cases[i][4]);
  }
  for (uint32_t remaining = 0; remaining <= 65535; ++remaining) {
    uint32_t value;
    CHECK(ArRegionalStock_Estimate(remaining, 200, 250, 65535, &value));
    CHECK(value == (remaining > 65485 ? 65535 : remaining + 50));
    CHECK(ArRegionalStock_Estimate(remaining, 250, 200, 65535, &value));
    CHECK(value == (remaining < 50 ? 0 : remaining - 50));
    CHECK(ArRegionalStock_Estimate(remaining, 200, 200, 65535, &value));
    CHECK(value == remaining);
  }
  uint32_t sentinel = 123;
  CHECK(!ArRegionalStock_Estimate(256, 200, 250, 255, &sentinel));
  CHECK(!ArRegionalStock_Estimate(100, 256, 250, 255, &sentinel));
  CHECK(!ArRegionalStock_Estimate(100, 200, 256, 255, &sentinel));
  CHECK(!ArRegionalStock_Estimate(100, 200, 250, 255, NULL));
  CHECK(sentinel == 123);
}

static void CheckMenuAdapter(void) {
  ActRaiserRegionalRulesView view = {0};
  for (int group = 0; group < kActRaiserRegionalSetting_Count; ++group) {
    CHECK(SettingsOverlayRegions_RowKey((ActRaiserRegionalSettingGroup)group)[0]);
    for (int source = 0; source < kArRegionalSource_Count; ++source) {
      CHECK(ArRegionalCosts_Init(&view.requested.costs, (ArRegionalSource)source));
      CHECK(ArRegionalTimers_Init(&view.requested.timers, (ArRegionalSource)source));
      view.requested.retry_score = (ArRegionalSource)source;
      view.requested.town_wait = (ArRegionalSource)source;
      view.requested.fishing = (ArRegionalSource)source;
      CHECK(ArRegionalDevelopment_Init(&view.requested.development,(ArRegionalSource)source));
      CHECK(ArRegionalRecovery_Init(&view.requested.recovery, (ArRegionalSource)source));
      CHECK(ArRegionalQuake_Init(&view.requested.quake, (ArRegionalSource)source));
      view.requested.score_page = (ArRegionalSource)source;
      view.requested.lives_display = (ArRegionalSource)source;
      CHECK(ArRegionalSources_Init(&view.requested.sources,(ArRegionalSource)source));
      view.requested.skull_wait=(ArRegionalSource)source;
      CHECK(ArRegionalStory_Init(&view.requested.story,(ArRegionalSource)source));
      CHECK(ArRegionalTownStatus_Init(&view.requested.town_status,(ArRegionalSource)source));
      view.requested.level_goals=(ArRegionalSource)source;
      view.requested.construction=(ArRegionalSource)source;
      view.requested.arrival=(ArRegionalSource)source;
      view.requested.statue_volley=(ArRegionalSource)source;
      CHECK(ArRegionalSupport_Init(&view.requested.support,(ArRegionalSource)source));
      CHECK(ArRegionalSimCombat_Init(&view.requested.sim_combat,(ArRegionalSource)source));
      CHECK(ArRegionalSimAi_Init(&view.requested.sim_ai,(ArRegionalSource)source));
      CHECK(ArRegionalActionMotion_Init(&view.requested.action_motion,(ArRegionalSource)source));
      CHECK(ArRegionalEmitter_Init(&view.requested.emitters,(ArRegionalSource)source));
      CHECK(ArRegionalBoss_Init(&view.requested.bosses,(ArRegionalSource)source));
      CHECK(ArRegionalCollision_Init(&view.requested.collision,(ArRegionalSource)source));
      CHECK(ArRegionalPlatformSkull_Init(&view.requested.platform_skull,(ArRegionalSource)source));
      CHECK(ArRegionalActorStats_Init(&view.requested.actor_stats,(ArRegionalSource)source));
      view.requested.menu_return = (ArRegionalSource)source;
      view.requested.speed_range = (ArRegionalSource)source;
      view.requested.magic_gesture = (ArRegionalSource)source;
      view.requested.lair_seeds = (ArRegionalSource)source;
      view.requested.lair_reloads = (ArRegionalSource)source;
      view.lair_reload_ready = true;
      view.requested.house_credit = (ArRegionalSource)source;
      CHECK(ArRegionalScore_Init(&view.requested.score_feedback,(ArRegionalSource)source));
      view.lair_history_ready = true;
      ArRegionalSource next;
      CHECK(SettingsOverlayRegions_NextSource(&view, (ActRaiserRegionalSettingGroup)group, 1, &next));
      CHECK(next == (ArRegionalSource)((source + 1) % 3));
      CHECK(SettingsOverlayRegions_NextSource(&view, (ActRaiserRegionalSettingGroup)group, -1, &next));
      CHECK(next == (ArRegionalSource)((source + 2) % 3));
      for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
        char text[2048];
        CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale, &view,
            (ActRaiserRegionalSettingGroup)group, text, sizeof(text)));
        CHECK(text[0] && !strchr(text, '{') && !strchr(text, '}'));
        if (group == kActRaiserRegionalSetting_RoomTimes) {
          CHECK(strstr(text, source == kArRegionalSource_Japan ? "200/100" : "300/200"));
          CHECK(strstr(text, source == kArRegionalSource_Japan ? "100/100" : "200/200"));
        }
        if (group == kActRaiserRegionalSetting_TownWait)
          CHECK(strstr(text, source == kArRegionalSource_Japan ? "150" : "1"));
        if (group == kActRaiserRegionalSetting_Fishing)
          CHECK(strstr(text, source == kArRegionalSource_Japan ? "128" : "255"));
        if(group==kActRaiserRegionalSetting_Development)
          CHECK(strstr(text,source==kArRegionalSource_Japan?"480":"720"));
      }
    }
    for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
      CHECK(SettingsOverlayRegions_RowLabel((ArUiLocale)locale, (ActRaiserRegionalSettingGroup)group)[0]);
      for (int result = kActRaiserRegionalEdit_Invalid; result <= kActRaiserRegionalEdit_Applied; ++result)
        CHECK(SettingsOverlayRegions_EditStatus((ArUiLocale)locale, (ActRaiserRegionalEditResult)result)[0]);
    }
  }
  ArRegionalSource next;
  CHECK(!SettingsOverlayRegions_NextSource(&view, kActRaiserRegionalSetting_Count, 1, &next));
  CHECK(!SettingsOverlayRegions_NextSource(NULL, kActRaiserRegionalSetting_Miracles, 1, &next));
  CHECK(!SettingsOverlayRegions_NextSource(&view, kActRaiserRegionalSetting_Miracles, 1, NULL));
  CHECK(!SettingsOverlayRegions_RowKey(kActRaiserRegionalSetting_Count)[0]);
  CHECK(ArRegionalCosts_SetRule(&view.requested.costs, kArRegionalCost_Light, kArRegionalSource_Japan));
  CHECK(SettingsOverlayRegions_NextSource(&view, kActRaiserRegionalSetting_Scrolls, 1, &next));
  CHECK(next == kArRegionalSource_US);
  CHECK(ArRegionalTimers_SetRule(&view.requested.timers, kArRegionalTimer_MarahnaBoss, kArRegionalSource_Japan));
  CHECK(SettingsOverlayRegions_NextSource(&view, kActRaiserRegionalSetting_RoomTimes, -1, &next));
  CHECK(next == kArRegionalSource_Europe);
}

int main(void) {
  CheckPolicies();
  CheckDescriptions();
  CheckStockEstimate();
  CheckMenuAdapter();
  if (failures) fprintf(stderr, "%d regional policy failures\n", failures);
  else puts("regional policy: 19683 cost mixes, four UI languages, stock bounds passed");
  return failures ? 1 : 0;
}
