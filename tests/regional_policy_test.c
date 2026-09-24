#include "regional/regional_costs.h"
#include "regional/towns/regional_stock.h"
#include "settings_overlay/regional/regional_ui.h"
#include "settings_overlay/regional/regional_menu.h"

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

static void CheckProfileMenu(void) {
  ActRaiserRegionalRulesView view={0};
  CHECK(!OverlayRegionMenu_Count(kOverlayRegionPage_Count));
  CHECK(!OverlayRegionMenu_Row(kOverlayRegionPage_Presets,2));
  unsigned seen[kActRaiserRegionalSetting_Count]={0}, presets=0, difficulty=0;
  for(unsigned page=0;page<kOverlayRegionPage_Count;++page) {
    CHECK(OverlayRegionMenu_Count(page)>0 && OverlayRegionMenu_Count(page)<32);
    for(unsigned i=0;i<OverlayRegionMenu_Count(page);++i) {
      const OverlayRegionRow *row=OverlayRegionMenu_Row(page,i);
      char catalog_key[128];
      snprintf(catalog_key, sizeof(catalog_key), "overlay.region.menu.%s.label", row->key);
      CHECK(row->label_key && !strcmp(catalog_key, row->label_key));
      snprintf(catalog_key, sizeof(catalog_key), "overlay.region.menu.%s.help", row->key);
      CHECK(row->help_key && !strcmp(catalog_key, row->help_key));
      if(row->kind==kOverlayRegionRow_Preset) {
        ++presets;
        char key[96];
        snprintf(key, sizeof(key), "regional_profile_%s", ArRegionalProfiles_Key(row->group));
        CHECK(!strcmp(key, row->key)); // Screen labels and semantic IDs must agree.
      }
      if(row->kind==kOverlayRegionRow_Setting) ++seen[row->setting];
      if(row->kind==kOverlayRegionRow_Difficulty) ++difficulty;
      for(unsigned source=0;source<3;++source) {
        CHECK(ArRegionalProfiles_Expand(&view.requested,kArRegionalProfile_Gameplay,source,&view.requested));
        CHECK(ArRegionalProfiles_Expand(&view.requested,kArRegionalProfile_Presentation,source,&view.requested));
        CHECK(ArRegionalProfiles_Describe(&view.requested,view.profiles));
        CHECK(ArRegionalProfiles_Describe(&view.effective,view.active_profiles));
        CHECK(ArRegionalProfiles_Changes(&view.requested,&view.effective,&view.pending_groups));
        ActRaiserRegionalSettings_DescribeChoices(&view.requested,&view.effective,view.choices);
        for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
          const char *label=OverlayRegionMenu_Label(locale,row);
          CHECK(label && label[0] && !strstr(label,"overlay.region."));
          char text[2048];SettingsOverlayRegionBadge badge;
          CHECK(OverlayRegionMenu_Value(locale,&view,row,false,text,sizeof(text),&badge));
          if(row->kind!=kOverlayRegionRow_Difficulty) {
            CHECK(badge==source);
            CHECK(OverlayRegionMenu_RowSource(&view,row,false)==source);
            CHECK(OverlayRegionMenu_RowSource(&view,row,true)==0);
          }
          CHECK(OverlayRegionMenu_Description(locale,&view,row,text,sizeof(text)));
          CHECK(text[0] && !strstr(text,"overlay.region.") && !strchr(text,'{'));
          CHECK(!OverlayRegionMenu_Description(locale,&view,row,text,2));
        }
      }
    }
  }
  CHECK(presets==2 && difficulty==1);
  for(unsigned i=0;i<kActRaiserRegionalSetting_Count;++i) {
    const bool internal=i==kActRaiserRegionalSetting_DifficultyRules || i==kActRaiserRegionalSetting_DifficultyLevel ||
        i==kActRaiserRegionalSetting_LevelGoals || i==kActRaiserRegionalSetting_Story ||
        i==kActRaiserRegionalSetting_ActionStart || i==kActRaiserRegionalSetting_Recovery;
    CHECK(seen[i]==(internal?0:1)); // Every narrow feature is reachable, exactly once.
  }
  const OverlayRegionRow *art=OverlayRegionMenu_Row(kOverlayRegionPage_Presentation,1);
  char text[2048];SettingsOverlayRegionBadge badge;
  // A valid European donor supplies item art without requiring JP actor art.
  view.artwork_available=(1u<<kArRegionalArtwork_ActionItems);
  CHECK(OverlayRegionMenu_Value(0,&view,art,false,text,sizeof(text),&badge));
  CHECK(badge==kOverlayRegionBadge_Europe && !strstr(text,"partial"));
  CHECK(ArRegionalProfiles_Expand(&view.requested,kArRegionalProfile_Presentation,1,&view.requested));
  CHECK(ArRegionalProfiles_Describe(&view.requested,view.profiles));
  ActRaiserRegionalSettings_DescribeChoices(&view.requested,&view.effective,view.choices);
  CHECK(OverlayRegionMenu_Value(0,&view,art,false,text,sizeof(text),&badge));
  CHECK(badge==kOverlayRegionBadge_Japan && !strstr(text,"partial"));
  CHECK(ArRegionalProfiles_Changes(&view.requested, &view.effective, &view.pending_groups));
  for (unsigned locale = 0; locale < kArUiLocale_Count; ++locale) {
    CHECK(view.choices[art->setting].pending);
    CHECK(OverlayRegionMenu_Value(locale, &view, art, false, text, sizeof(text), &badge));
    CHECK(strchr(text, '*')); // Missing donor and queued activation coexist.
    CHECK(OverlayRegionMenu_Description(locale, &view, art, text, sizeof(text)));
    const char *authored = ArUiCatalog_Text(locale, art->help_key, NULL);
    const char *mechanics = strchr(authored, '\n');
    CHECK(mechanics && !strncmp(text, authored, (size_t)(mechanics - authored)));
    const char *notice = strstr(text, ArUiCatalog_Text(locale, "overlay.region.menu.media_missing", NULL));
    CHECK(notice && notice > text + (mechanics ? mechanics - authored : 0));
    CHECK(strstr(text, ArUiCatalog_Text(locale, "overlay.region.menu.activation_pending", NULL)));
    CHECK(OverlayRegionMenu_Value(locale, &view, art, true, text, sizeof(text), &badge));
    CHECK(!strchr(text, '*')); // The active readout is never itself pending.
  }
  view.artwork_available=63;view.actor_artwork_available=true;view.sequences_available=3;
  CHECK(OverlayRegionMenu_Value(0,&view,art,false,text,sizeof(text),&badge) && !strstr(text,"partial"));
  CHECK(OverlayRegionMenu_Description(0,&view,art,text,sizeof(text)));
  CHECK(!strstr(text, ArUiCatalog_Text(0, "overlay.region.menu.media_missing", NULL)));
  CHECK(*text && text[strlen(text) - 1] != '\n' && text[strlen(text) - 1] != ' ');
  view.population_pending=view.pending_profile=true;
  view.pending_profile_group=kArRegionalProfile_Gameplay;view.pending_population=1;
  CHECK(OverlayRegionMenu_Source(&view,kArRegionalProfile_Gameplay,false)==1);
  CHECK(OverlayRegionMenu_Source(&view,kArRegionalProfile_Combat,false)==1);
  CHECK(OverlayRegionMenu_Source(&view,kArRegionalProfile_Combat,true)==0);
  CHECK(OverlayRegionMenu_Description(0,&view,OverlayRegionMenu_Row(kOverlayRegionPage_Presets,0),text,sizeof(text)));
  CHECK(strstr(text,ArUiCatalog_Text(0,"overlay.region.menu.confirm_pending",NULL)));
  CHECK(ArRegionalDifficulty_Select(kArRegionalDifficultyChoice_Expert, &view.requested.difficulty));
  CHECK(ActRaiserRegionalSettings_DifficultyChoice(&view, false) == kArRegionalDifficultyChoice_Original);
  view.pending_population = kArRegionalSource_Europe;
  CHECK(ActRaiserRegionalSettings_DifficultyChoice(&view, false) == kArRegionalDifficultyChoice_Normal);
  CHECK(ActRaiserRegionalSettings_DifficultyChoice(&view, true) == kArRegionalDifficultyChoice_Original);
  view.population_pending = view.pending_profile = false;
  // Difficulty changes pending EU layout filtering, not authored US/JP counts.
  view.requested.placements.enemies = view.effective.placements.enemies = kArRegionalSource_Europe;
  ActRaiserRegionalSettings_DescribeChoices(&view.requested, &view.effective, view.choices);
  CHECK(view.choices[kActRaiserRegionalSetting_EnemyPlacements].pending);
  view.requested.placements.enemies = view.effective.placements.enemies = kArRegionalSource_US;
  ActRaiserRegionalSettings_DescribeChoices(&view.requested, &view.effective, view.choices);
  CHECK(!view.choices[kActRaiserRegionalSetting_EnemyPlacements].pending);
  // Difficulty selected on the US baseline must be real, not a dormant level.
  const OverlayRegionRow *difficulty_row=OverlayRegionMenu_Row(kOverlayRegionPage_Action,0);
  for(unsigned choice=0;choice<kArRegionalDifficultyChoice_Count;++choice) {
    CHECK(ArRegionalDifficulty_Select(choice,&view.requested.difficulty));
    CHECK(ArRegionalDifficulty_Choice(&view.requested.difficulty)==choice);
    ArRegionalDifficultySnapshot snapshot;
    CHECK(ArRegionalDifficulty_Resolve(&view.requested.difficulty,&snapshot));
    CHECK(snapshot.spawn_hp==(choice==0?0:choice==1?2:choice==2?1:3));
    CHECK(OverlayRegionMenu_Value(0,&view,difficulty_row,false,text,sizeof(text),&badge));
    CHECK(strstr(text,choice?"EU":"Original"));
    ArRegionalRules copy;
    CHECK(ArRegionalProfiles_Expand(&view.requested,kArRegionalProfile_Stage,1,&copy));
    CHECK(!memcmp(&copy.difficulty,&view.requested.difficulty,sizeof(copy.difficulty)));
    CHECK(ArRegionalProfiles_Expand(&view.requested,kArRegionalProfile_Combat,2,&copy));
    CHECK(!memcmp(&copy.difficulty,&view.requested.difficulty,sizeof(copy.difficulty)));
  }
}

static void CheckControlsPresentation(void) {
  for (unsigned locale = 0; locale < kArUiLocale_Count; ++locale)
    for (unsigned i = 0; i < OverlayRegionMenu_Count(kOverlayRegionPage_Controls); ++i) {
      const OverlayRegionRow *row = OverlayRegionMenu_Row(kOverlayRegionPage_Controls, i);
      ActRaiserRegionalRulesView view = {0};
      char previews[3][512], text[256];
      SettingsOverlayRegionBadge badge;
      for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
        view.choices[row->setting].source = source;
        CHECK(OverlayRegionMenu_Value(locale, &view, row, false, text, sizeof(text), &badge));
        CHECK(badge == source && !strcmp(text, SettingsOverlayRegions_BadgeCode(locale, badge)));
        CHECK(OverlayRegionMenu_Preview(locale, &view, row, previews[source], sizeof(previews[source])));
        CHECK(!strchr(previews[source], '{') && !strstr(previews[source], "overlay.region."));
        CHECK(!OverlayRegionMenu_Preview(locale, &view, row, text, 2));
        CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)) && !*text);
        CHECK(!OverlayRegionMenu_Note(locale, &view, row).attention);
      }
      CHECK(!strcmp(previews[kArRegionalSource_US], previews[kArRegionalSource_Europe]));
      CHECK(strcmp(previews[kArRegionalSource_US], previews[kArRegionalSource_Japan]));
      view.choices[row->setting].source = kArRegionalSource_Japan;
      view.choices[row->setting].pending = true;
      CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)) && *text);
      CHECK(strstr(text, "US") && !strchr(text, '*'));
      CHECK(OverlayRegionMenu_Note(locale, &view, row).attention);
      CHECK(!strcmp(OverlayRegionMenu_Note(locale, &view, row).text,
          ArUiCatalog_Text(locale, "overlay.region.note.pending", NULL)));
      view.choices[row->setting].active_source = kArRegionalSource_Japan;
      CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)) && !*text);
      CHECK(OverlayRegionMenu_Note(locale, &view, row).attention);
      view.choices[row->setting].active_source = kArRegionalSource_US;
      view.new_game = true;
      CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)) && !*text);
      CHECK(!OverlayRegionMenu_Note(locale, &view, row).attention);
      view.new_game = false;
      view.population_pending = view.pending_profile = true;
      view.pending_profile_group = kArRegionalProfile_Gameplay;
      view.pending_population = kArRegionalSource_Japan;
      CHECK(OverlayRegionMenu_Preview(locale, &view, row, text, sizeof(text)));
      CHECK(!strcmp(text, previews[kArRegionalSource_Japan]));
      CHECK(!strcmp(OverlayRegionMenu_Note(locale, &view, row).text,
          ArUiCatalog_Text(locale, "overlay.region.note.review", NULL)));
    }
  ActRaiserRegionalRulesView view = {0};
  const OverlayRegionRow *art = OverlayRegionMenu_Row(kOverlayRegionPage_Presentation, 0);
  CHECK(ArRegionalProfiles_Expand(&view.requested, kArRegionalProfile_Presentation,
      kArRegionalSource_Japan, &view.requested));
  CHECK(OverlayRegionMenu_Note(0, &view, art).attention);
  CHECK(!strcmp(OverlayRegionMenu_Note(0, &view, art).text,
      ArUiCatalog_Text(0, "overlay.region.note.media", NULL)));
}

static void CheckPresetStateLabels(void) {
  for (unsigned locale = 0; locale < kArUiLocale_Count; ++locale)
    for (unsigned i = 0; i < OverlayRegionMenu_Count(kOverlayRegionPage_Presets); ++i) {
      const OverlayRegionRow *row = OverlayRegionMenu_Row(kOverlayRegionPage_Presets, i);
      ActRaiserRegionalRulesView view = {0};
      char text[256], expected[256];
      for (unsigned source = 0; source <= kArRegionalSource_Count; ++source) {
        view.active_profiles[row->group].source = source;
        view.profiles[row->group].source = kArRegionalSource_Japan;
        view.population_pending = view.pending_profile = true;
        view.pending_profile_group = row->group;
        view.pending_population = kArRegionalSource_Europe;
        const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeCode(locale, source)}};
        CHECK(ArUiCatalog_Format(expected, sizeof(expected),
            ArUiCatalog_Text(locale, "overlay.region.current", NULL), args, 1));
        CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)));
        CHECK(!strcmp(text, expected)); // Not the queued or requested choice.
        CHECK(!OverlayRegionMenu_StateLabel(locale, &view, row, text, 2));
      }
      view.new_game = true;
      const ArUiTextArgument args[] = {{"region", "JP"}};
      CHECK(ArUiCatalog_Format(expected, sizeof(expected),
          ArUiCatalog_Text(locale, "overlay.region.draft", NULL), args, 1));
      CHECK(OverlayRegionMenu_StateLabel(locale, &view, row, text, sizeof(text)));
      CHECK(!strcmp(text, expected));
    }
}

int main(void) {
  CheckPolicies();
  CheckStockEstimate();
  CheckProfileMenu();
  CheckControlsPresentation();
  CheckPresetStateLabels();
  if (failures) fprintf(stderr, "%d regional policy failures\n", failures);
  else puts("regional policy: 19683 cost mixes, four UI languages, stock bounds passed");
  return failures ? 1 : 0;
}
