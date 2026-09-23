#define _POSIX_C_SOURCE 200809L

#include "diorama_layer_editor.h"
#include "action/action_bg_tuner.h"
#include "display_geometry.h"
#include "input_map.h"
#include "host/host_display_status.h"
#include "render_capabilities.h"
#include "settings.h"
#include "randomizer.h"
#include "settings_overlay.h"
#include "settings_overlay_artwork.h"
#include "settings_overlay_localization.h"
#include "settings_overlay_layers_localization.h"
#include "localization/interface_text.h"
#include "performance_overlay.h"
#include "platform/sdl/render_sdl_internal.h"
#include "sim/sim_town_terrain.h"
#ifdef AR_OVERLAY_UI_FONT
#include "platform/sdl/text_rasterizer_sdl.h"
#include "host/font_resources.h"
#include "localization/unicode_grapheme.h"
static ArHostFontResources s_font_store;
#endif

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

uint8 g_ram[0x20000];
/* kSettingCat_Graphics's GpuShadersActive() availability gate reads this
 * (main.c's real runtime state); this harness has no renderer, so it's
 * never actually true here. */
bool g_gpu_shaders_active;
/* W4-2: present.c owns the real value (latched when a renderer rejects the rim
 * mask blend mode); stubbed true here so the row's availability is exercised. */
bool Present_SimRimMaskSupported(void) { return true; }
bool Present_EffectRendererSupported(void) { return true; }
/* Host-side diorama geometry rebind; no renderer in this harness. */
void Diorama_OnModeChanged(void) {}
static int s_failures;
static int s_action_calls;
static const SettingDesc *s_action_desc;
static int s_inspector_info_calls;
static bool s_fake_manual_available = true;

static bool FakeManualAvailable(void) {
  return s_fake_manual_available;
}

static const SettingsOverlayManualHooks kFakeManualHooks = {
  .available = FakeManualAvailable,
};

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static bool ActionObserved(const SettingDesc *desc) {
  s_action_calls++;
  s_action_desc = desc;
  return true;
}

static void InspectorInfo(char *buffer, size_t buffer_size) {
  s_inspector_info_calls++;
  snprintf(buffer, buffer_size,
           "SCENE Fillmore sim  $18/$19 $00/$01\n"
           "GF $1234  HOST 5678  PAUSE MENU\n"
           "CAM $0080,$0040  MAP 512X512\n"
           "PPU MODE 1  MAIN $17 SUB $00\n"
           "MUSIC Fillmore  SONG $01 AUTH");
}

/* The overlay's nav column lists SECTIONS and each section has a tab bar, so
 * these walk to a named destination instead of counting keypresses — the old
 * "press Down four times" style broke every time a row landed above the one
 * under test. */
enum {
  kSection_Video = 0,
  kSection_Action3D,
  kSection_Town3D,
  kSection_Audio,
  kSection_Controls,
  kSection_Cheats,
  kSection_Save,
  /* The in-game manual: a player-facing section, so it sits ahead of System's
   * host commands. */
  kSection_Manual,
  kSection_System,
  kSection_Localization,
  /* Developer-only until a seeded run has been played end to end, so it sits
   * with Layers rather than among the player sections. */
  kSection_Randomizer,
  /* Developer-only, so it is present in the nav column only while
   * show_debug_settings is on. Last on purpose: every section above keeps the
   * same ordinal whether it is shown or hidden. */
  kSection_Layers,
};
/* Sections a player sees, and the total with developer tools revealed. Named so
 * the assertions below say which one they mean rather than repeating an enum
 * arithmetic expression that reads the same for both. */
enum {
  /* The first hidden section marks the end of the player-visible run; both
   * sections past it are debug-gated. */
  kPlayerSectionCount = kSection_Randomizer,
  kDebugSectionCount = kSection_Layers + 1,
  kPlayerSectionCountWithoutManual = kPlayerSectionCount - 1,
  kSystemVisibleOrdinalWithoutManual = kSection_System - 1,
};

/* Call from the nav column (not inside a submenu). */
static void NavToSection(int target) {
  for (int guard = 0; guard < 24; guard++) {
    int selected = -1;
    CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
    if (selected == target) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"section not reachable");
}

/* A build with no staged PDF must not advertise a dead Manual destination.
 * Exercise this through the same availability hook used by main.c, including
 * both directions around the hole so section movement cannot land on it. */
static void CheckManualSectionAvailability(void) {
  g_settings.show_debug_settings = false;
  SettingsOverlay_Refresh();
  s_fake_manual_available = true;
  SettingsOverlay_Refresh();

  int selected = -1;
  int total = -1;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(total == kPlayerSectionCount);

  NavToSection(kSection_Save);
  s_fake_manual_available = false;
  SettingsOverlay_Refresh();
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected == kSection_Save);
  CHECK(total == kPlayerSectionCountWithoutManual);

  /* DOWN skips the hidden raw Manual section and lands on System. Its visible
   * ordinal closes the gap, followed by Localization and then Video. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSystemVisibleOrdinalWithoutManual);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSection_Localization - 1);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSection_Video);

  /* UP must likewise skip the missing section. Restoring availability inserts
   * Manual back ahead of System and makes it reachable again. */
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSystemVisibleOrdinalWithoutManual);
  s_fake_manual_available = true;
  SettingsOverlay_Refresh();
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected == kSection_System);
  CHECK(total == kPlayerSectionCount);
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSection_Manual);

  /* Restore the fixture state expected by the exhaustive menu checks below. */
  g_settings.show_debug_settings = true;
  SettingsOverlay_Refresh();
  NavToSection(kSection_Video);
}

/* Tabs are remembered per section, so step forward (wrapping) until the
 * wanted one is active rather than assuming we start at zero. */
static void NavToTab(int target) {
  for (int guard = 0; guard < 12; guard++) {
    int active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, NULL));
    if (active == target) return;
    /* ']' is the layout-independent next-tab key; the primary L/R keys follow
     * the player's own SNES bindings and are exercised separately below. */
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
  }
  CHECK(!"tab not reachable");
}

static void RowToKey(const char *key) {
  for (int guard = 0; guard < 80; guard++) {
    if (!strcmp(SettingsOverlay_SelectedKey(), key)) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"row not reachable");
}

static ActRaiserRegionalRulesView s_fake_region;
static bool s_fake_region_active;
static unsigned s_region_edits;
static bool FakeRegionalView(ActRaiserRegionalRulesView *out) {
  if (!s_fake_region_active) return false;
  *out = s_fake_region;
  return true;
}
static ActRaiserRegionalEditResult FakeRegionalEdit(const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, ArRegionalSource source) {
  ++s_region_edits;
  CHECK(view->revision == s_fake_region.revision);
  CHECK(!memcmp(view->campaign, s_fake_region.campaign, sizeof(view->campaign)));
  CHECK(s_fake_region.editable);
  if(group==kActRaiserRegionalSetting_Population) {
    s_fake_region.population_pending=true;s_fake_region.pending_population=source;
    return kActRaiserRegionalEdit_Deferred;
  }
  if (group == kActRaiserRegionalSetting_RoomTimes)
    CHECK(ArRegionalTimers_Init(&s_fake_region.requested.timers, source));
  else if (group == kActRaiserRegionalSetting_RetryScore)
    s_fake_region.requested.retry_score = source;
  else if (group == kActRaiserRegionalSetting_TownWait)
    s_fake_region.requested.town_wait = source;
  else if (group == kActRaiserRegionalSetting_Fishing)
    s_fake_region.requested.fishing = source;
  else if(group==kActRaiserRegionalSetting_Development)
    CHECK(ArRegionalDevelopment_Init(&s_fake_region.requested.development,source));
  else if (group == kActRaiserRegionalSetting_Recovery)
    CHECK(ArRegionalRecovery_Init(&s_fake_region.requested.recovery, source));
  else if (group == kActRaiserRegionalSetting_Quake)
    CHECK(ArRegionalQuake_Init(&s_fake_region.requested.quake, source));
  else if (group == kActRaiserRegionalSetting_ScorePage)
    s_fake_region.requested.score_page = source;
  else if (group == kActRaiserRegionalSetting_LivesDisplay)
    s_fake_region.requested.lives_display = source;
  else if (group == kActRaiserRegionalSetting_Sources)
    CHECK(ArRegionalSources_Init(&s_fake_region.requested.sources,source));
  else if (group == kActRaiserRegionalSetting_SkullWait)
    s_fake_region.requested.skull_wait=source;
  else if (group == kActRaiserRegionalSetting_Story)
    CHECK(ArRegionalStory_Init(&s_fake_region.requested.story,source));
  else if (group == kActRaiserRegionalSetting_LairReloads)
    s_fake_region.requested.lair_reloads=source;
  else if (group == kActRaiserRegionalSetting_TownStatus)
    ArRegionalTownStatus_Init(&s_fake_region.requested.town_status,source);
  else if (group == kActRaiserRegionalSetting_LevelGoals)
    s_fake_region.requested.level_goals=source;
  else if (group == kActRaiserRegionalSetting_SimCombat)
    ArRegionalSimCombat_Init(&s_fake_region.requested.sim_combat,source);
  else if (group == kActRaiserRegionalSetting_SimAi)
    ArRegionalSimAi_Init(&s_fake_region.requested.sim_ai,source);
  else if (group == kActRaiserRegionalSetting_Construction)
    s_fake_region.requested.construction=source;
  else if(group==kActRaiserRegionalSetting_Arrival)
    s_fake_region.requested.arrival=source;
  else if(group==kActRaiserRegionalSetting_ActionMotion)
    ArRegionalActionMotion_Init(&s_fake_region.requested.action_motion,source);
  else if(group==kActRaiserRegionalSetting_Emitters)
    ArRegionalEmitter_Init(&s_fake_region.requested.emitters,source);
  else if(group==kActRaiserRegionalSetting_StatueVolley)
    s_fake_region.requested.statue_volley=source;
  else if(group==kActRaiserRegionalSetting_Bosses)
    ArRegionalBoss_Init(&s_fake_region.requested.bosses,source);
  else if(group==kActRaiserRegionalSetting_Collision)
    ArRegionalCollision_Init(&s_fake_region.requested.collision,source);
  else if(group==kActRaiserRegionalSetting_PlatformSkull)
    ArRegionalPlatformSkull_Init(&s_fake_region.requested.platform_skull,source);
  else if(group==kActRaiserRegionalSetting_ActorStats)
    ArRegionalActorStats_Init(&s_fake_region.requested.actor_stats,source);
  else if (group == kActRaiserRegionalSetting_MenuReturn)
    s_fake_region.requested.menu_return = source;
  else if (group == kActRaiserRegionalSetting_SpeedRange)
    s_fake_region.requested.speed_range = source;
  else if (group == kActRaiserRegionalSetting_MagicGesture)
    s_fake_region.requested.magic_gesture = source;
  else if (group == kActRaiserRegionalSetting_LairReserves)
    s_fake_region.requested.lair_seeds = source;
  else if (group == kActRaiserRegionalSetting_HouseCredit)
    s_fake_region.requested.house_credit = source;
  else if (group == kActRaiserRegionalSetting_ScoreFeedback)
    CHECK(ArRegionalScore_Init(&s_fake_region.requested.score_feedback,source));
  else
    CHECK(ArRegionalCosts_SetGroup(&s_fake_region.requested.costs,
        group == kActRaiserRegionalSetting_Scrolls ? kArRegionalCostGroup_Scrolls : kArRegionalCostGroup_Miracles,
        source));
  ++s_fake_region.revision;
  return kActRaiserRegionalEdit_Applied;
}

static void CheckRegionalControls(SDL_Renderer *renderer, SDL_Surface *surface) {
  SettingsOverlay_Close();
  SettingsOverlay_Open();
  const Settings before = g_settings;
  SettingsOverlayRegionalHooks hooks = {FakeRegionalView, FakeRegionalEdit};
  SettingsOverlay_SetRegionalHooks(&hooks);
  NavToSection(kSection_Localization);
  NavToTab(3);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_no_campaign"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 0);
  s_fake_region_active = true;
  s_fake_region = (ActRaiserRegionalRulesView){.revision = 7, .editable = true, .lair_history_ready = true, .campaign = {42}};
  CHECK(ArRegionalCosts_Init(&s_fake_region.requested.costs, kArRegionalSource_US));
  s_fake_region.effective.costs = s_fake_region.requested.costs;
  SettingsOverlay_Refresh();
  RowToKey("regional_scroll_prices");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 1);
  CHECK(s_fake_region.requested.costs.source[kArRegionalCost_Light] == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.costs.source[kArRegionalCost_Light] == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_miracle_prices"));
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(s_region_edits == 2);
  CHECK(s_fake_region.requested.costs.source[kArRegionalCost_Rain] == kArRegionalSource_Europe);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_room_times"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 3);
  CHECK(s_fake_region.requested.timers.source[0] == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.timers.source[0] == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_retry_score"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 4);
  CHECK(s_fake_region.requested.retry_score == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.retry_score == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_town_wait"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 5);
  CHECK(s_fake_region.requested.town_wait == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.town_wait == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_fishing"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 6);
  CHECK(s_fake_region.requested.fishing == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.fishing == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_development"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==7);
  CHECK(s_fake_region.requested.development.source[0]==kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.development.source[0]==kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_recovery"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 8);
  CHECK(s_fake_region.requested.recovery.source[0] == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.recovery.source[0] == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_quake"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 9);
  CHECK(s_fake_region.requested.quake.source[0] == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.quake.source[0] == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_score_page"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 10);
  CHECK(s_fake_region.requested.score_page == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.score_page == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_menu_return"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 11);
  CHECK(s_fake_region.requested.menu_return == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.menu_return == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_speed_range"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 12);
  CHECK(s_fake_region.requested.speed_range == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.speed_range == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_magic_gesture"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 13);
  CHECK(s_fake_region.requested.magic_gesture == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.magic_gesture == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_lair_reserves"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 14);
  CHECK(s_fake_region.requested.lair_seeds == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.lair_seeds == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_house_credit"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 15);
  CHECK(s_fake_region.requested.house_credit == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.house_credit == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_score_feedback"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 16);
  for(unsigned i=0;i<kArRegionalScore_Count;++i) {
    CHECK(s_fake_region.requested.score_feedback.source[i] == kArRegionalSource_Japan);
    CHECK(s_fake_region.effective.score_feedback.source[i] == kArRegionalSource_US);
  }
  /* Four rows at the default 640-wide description panel. Do not silently
   * clip the new timing/one-reward caveats, including internal mixed bundles. */
  for(unsigned combination=0;combination<81;++combination)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    unsigned digits=combination;
    for(unsigned i=0;i<kArRegionalScore_Count;++i) {
      preview.requested.score_feedback.source[i]=(ArRegionalSource)(digits%3);digits/=3;
    }
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,
        kActRaiserRegionalSetting_ScoreFeedback,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    if(at!=length)fprintf(stderr,"score help overflow: bundle=%u locale=%u remaining=%s\n",combination,locale,help+at);
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_lives_display"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 17);
  CHECK(s_fake_region.requested.lives_display == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.lives_display == kArRegionalSource_US);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_sources"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 18);
  for(unsigned combination=0;combination<9;++combination)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    preview.requested.sources.source[0]=(ArRegionalSource)(combination%3);
    preview.requested.sources.source[1]=(ArRegionalSource)(combination/3);
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,
        kActRaiserRegionalSetting_Sources,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  for(unsigned i=0;i<kArRegionalSourceItem_Count;++i) {
    CHECK(s_fake_region.requested.sources.source[i]==kArRegionalSource_Japan);
    CHECK(s_fake_region.effective.sources.source[i]==kArRegionalSource_US);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_skull_wait"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 19);
  CHECK(s_fake_region.requested.skull_wait == kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.skull_wait == kArRegionalSource_US);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    preview.requested.skull_wait=(ArRegionalSource)source;
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,
        kActRaiserRegionalSetting_SkullWait,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_story"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_region_edits == 20);
  for(unsigned i=0;i<3;++i)CHECK(s_fake_region.requested.story.source[i]==kArRegionalSource_Japan &&
      s_fake_region.effective.story.source[i]==kArRegionalSource_US);
  for(unsigned n=0;n<27;++n)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;unsigned digits=n;
    for(unsigned i=0;i<3;++i){preview.requested.story.source[i]=(ArRegionalSource)(digits%3);digits/=3;}
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,
        kActRaiserRegionalSetting_Story,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_lair_reloads"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==21 && s_fake_region.requested.lair_reloads==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    preview.requested.lair_reloads=(ArRegionalSource)source;preview.lair_reload_ready=true;
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,
        kActRaiserRegionalSetting_LairReloads,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_town_status"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==22 && s_fake_region.requested.town_status.source[0]==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    ArRegionalTownStatus_Init(&preview.requested.town_status,(ArRegionalSource)source);
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_TownStatus,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_level_goals"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==23 && s_fake_region.requested.level_goals==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;preview.requested.level_goals=(ArRegionalSource)source;
    char help[2048];
    CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_LevelGoals,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;
      CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_sim_combat"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==24 && s_fake_region.requested.sim_combat.source[0]==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    ArRegionalSimCombat_Init(&preview.requested.sim_combat,(ArRegionalSource)source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_SimCombat,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_sim_ai"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==25 && s_fake_region.requested.sim_ai.source[0]==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    ArRegionalSimAi_Init(&preview.requested.sim_ai,(ArRegionalSource)source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_SimAi,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_construction"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==26 && s_fake_region.requested.construction==kArRegionalSource_Japan);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    preview.requested.construction=(ArRegionalSource)source;
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Construction,help,sizeof(help)));
    size_t at=0,length=strlen(help);
    for(unsigned row=0;row<4 && at<length;++row) {
      ArInterfaceTextLine line;CHECK(ArInterfaceText_WrapLine(help+at,length-at,98,kArInterfaceTextMaximumBytes,&line));
      if(!line.consumed)break;
      at+=line.consumed;
    }
    CHECK(at==length);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_population"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==27 && s_fake_region.population_pending && s_fake_region.pending_population==kArRegionalSource_Japan);
  CHECK(s_fake_region.effective.support.source[0]==kArRegionalSource_US);
  for(unsigned source=0;source<3;++source)for(unsigned locale=0;locale<kArUiLocale_Count;++locale) {
    ActRaiserRegionalRulesView preview=s_fake_region;
    preview.population_pending=true;preview.pending_population=(ArRegionalSource)source;
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Population,help,sizeof(help)));
    CHECK(help[0] && !strchr(help,'{'));
    const uint16_t counts[6]={128,128,128,128,128,128};
    CHECK(SettingsOverlayRegions_PopulationConfirmation((ArUiLocale)locale,(ArRegionalSource)source,counts,help,sizeof(help)));
    CHECK(strstr(help,"128") && !strchr(help,'{'));
    /* Smallest supported 464x208 logical overlay: 66 cells x8 lines. */
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<8 && used<length;++line) {
      ArInterfaceTextLine slice;
      CHECK(ArInterfaceText_WrapLine(help+used,length-used,66,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length);
    const uint16_t empty[6]={0};
    CHECK(SettingsOverlayRegions_PopulationConfirmation((ArUiLocale)locale,(ArRegionalSource)source,empty,help,sizeof(help)));
    CHECK(help[0] && !strchr(help,'{'));
    CHECK(!SettingsOverlayRegions_PopulationConfirmation((ArUiLocale)locale,(ArRegionalSource)source,counts,help,2));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_arrival"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==28 && s_fake_region.requested.arrival==kArRegionalSource_Japan);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned locked=0;locked<2;++locked)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;preview.arrival_locked=locked;preview.requested.arrival=source;
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Arrival,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_action_motion"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==29 && s_fake_region.requested.action_motion.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalActionMotion_Init(&preview.requested.action_motion,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_ActionMotion,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_emitters"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==30 && s_fake_region.requested.emitters.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalEmitter_Init(&preview.requested.emitters,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Emitters,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_statue_volley"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==31 && s_fake_region.requested.statue_volley==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;preview.requested.statue_volley=source;
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_StatueVolley,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_boss_rules"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==32 && s_fake_region.requested.bosses.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalBoss_Init(&preview.requested.bosses,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Bosses,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_collision"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==33 && s_fake_region.requested.collision.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalCollision_Init(&preview.requested.collision,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_Collision,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_platform_skull"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==34 && s_fake_region.requested.platform_skull.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalPlatformSkull_Init(&preview.requested.platform_skull,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_PlatformSkull,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(),"regional_actor_stats"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT,true,false));
  CHECK(s_region_edits==35 && s_fake_region.requested.actor_stats.source[0]==1);
  for(unsigned locale=0;locale<kArUiLocale_Count;++locale)for(unsigned source=0;source<3;++source) {
    ActRaiserRegionalRulesView preview=s_fake_region;ArRegionalActorStats_Init(&preview.requested.actor_stats,source);
    char help[2048];CHECK(SettingsOverlayRegions_ViewDescription((ArUiLocale)locale,&preview,kActRaiserRegionalSetting_ActorStats,help,sizeof(help)));
    size_t used=0,length=strlen(help);
    for(unsigned line=0;line<4 && used<length;++line) {
      ArInterfaceTextLine slice;CHECK(ArInterfaceText_WrapLine(help+used,length-used,98,kArInterfaceTextMaximumBytes,&slice));
      CHECK(slice.consumed);used+=slice.consumed;
    }
    CHECK(used==length && !strchr(help,'{'));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "regional_scroll_prices")); /* no global reset row */
  s_fake_region.editable = false;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_region_edits == 35);
  CHECK(!memcmp(&before, &g_settings, sizeof(before)));
  s_fake_region.editable = true;
  SettingsOverlay_Close(); /* clear transient status for the preview */
  SettingsOverlay_Open();
  NavToSection(kSection_Localization);
  NavToTab(3);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  if (getenv("AR_OVERLAY_REGIONAL_TIME_PREVIEW")) RowToKey("regional_room_times");
  if (getenv("AR_OVERLAY_REGIONAL_RETRY_PREVIEW")) RowToKey("regional_retry_score");
  if (getenv("AR_OVERLAY_REGIONAL_WAIT_PREVIEW")) RowToKey("regional_town_wait");
  if (getenv("AR_OVERLAY_REGIONAL_FISHING_PREVIEW")) RowToKey("regional_fishing");
  if(getenv("AR_OVERLAY_REGIONAL_DEVELOPMENT_PREVIEW"))RowToKey("regional_development");
  if (getenv("AR_OVERLAY_REGIONAL_RECOVERY_PREVIEW")) RowToKey("regional_recovery");
  if (getenv("AR_OVERLAY_REGIONAL_QUAKE_PREVIEW")) RowToKey("regional_quake");
  if (getenv("AR_OVERLAY_REGIONAL_SCORE_PREVIEW")) RowToKey("regional_score_page");
  if (getenv("AR_OVERLAY_REGIONAL_RETURN_PREVIEW")) RowToKey("regional_menu_return");
  if (getenv("AR_OVERLAY_REGIONAL_SPEED_PREVIEW")) RowToKey("regional_speed_range");
  if (getenv("AR_OVERLAY_REGIONAL_GESTURE_PREVIEW")) RowToKey("regional_magic_gesture");
  if (getenv("AR_OVERLAY_REGIONAL_LAIR_PREVIEW")) RowToKey("regional_lair_reserves");
  const char *preview_row = getenv("AR_OVERLAY_REGIONAL_ROW_PREVIEW");
  if (preview_row && preview_row[0]) RowToKey(preview_row);
  if (renderer && surface) {
    for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
      g_settings.interface_language = locale;
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      CHECK(SDL_RenderClear(renderer));
      SettingsOverlay_Render((ArRenderRectI){0, 0, surface->w, surface->h});
      CHECK(SDL_RenderPresent(renderer));
      const char *preview = getenv("AR_OVERLAY_REGIONAL_TEST_BMP");
      if (preview && preview[0]) {
        char path[1024];
        const int length = snprintf(path, sizeof(path), "%s-%s.bmp", preview,
                                     ArUiCatalog_LocaleTag((ArUiLocale)locale));
        CHECK(length > 0 && length < (int)sizeof(path));
        if (length > 0 && length < (int)sizeof(path)) CHECK(SDL_SaveBMP(surface, path));
      }
    }
    g_settings.interface_language = before.interface_language;
  }
  SettingsOverlay_SetRegionalHooks(NULL);
  SettingsOverlay_Close();
}

static bool s_dump_catalog;
static void CheckCatalogEntry(const char *key, const char *english) {
  if (s_dump_catalog) {
    printf("%s\t%s\n", key, english);
    return;
  }
  for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
    const char *text = ArUiCatalog_Text((ArUiLocale)locale, key, NULL);
    if (!text[0] || (!locale && strcmp(text, english))) {
      fprintf(stderr, "catalog missing/drifted: %s (%s)\n", key,
              ArUiCatalog_LocaleTag((ArUiLocale)locale));
      CHECK(false);
    }
  }
}

static void CheckLayerHelpCatalog(void) {
  char key[128];
  for (int kind = kActionBgTunerRow_Header; kind <= kActionBgTunerRow_Reset; ++kind) {
    ActionBgTunerRow row = {.kind = (ActionBgTunerRowKind)kind};
    snprintf(key, sizeof(key), "overlay.layer.action.help.%d", kind);
    CheckCatalogEntry(key, ActionBgTuner_RowHelp(&row));
  }
  CheckCatalogEntry("overlay.layer.diorama.help.header", DioramaLayerEditor_RowHelp(
      kDioramaEditorRow_Header, kDioramaEditorParam_None, kDioramaDepth_Flat));
  CheckCatalogEntry("overlay.layer.diorama.help.reset", DioramaLayerEditor_RowHelp(
      kDioramaEditorRow_ResetRoom, kDioramaEditorParam_None, kDioramaDepth_Flat));
  for (int shape = 0; shape < kDioramaDepth_StrategyCount; ++shape) {
    snprintf(key, sizeof(key), "overlay.layer.diorama.help.shape.%d", shape);
    CheckCatalogEntry(key, DioramaLayerEditor_RowHelp(kDioramaEditorRow_Plane,
        kDioramaEditorParam_None, (DioramaDepthStrategy)shape));
  }
  for (int param = kDioramaEditorParam_Depth; param <= kDioramaEditorParam_Order; ++param) {
    snprintf(key, sizeof(key), "overlay.layer.diorama.help.param.%d", param);
    CheckCatalogEntry(key, DioramaLayerEditor_RowHelp(kDioramaEditorRow_Param,
        (DioramaEditorParam)param, kDioramaDepth_Flat));
  }
}

static void CheckLayerCaptionParity(const char *label, const char *value,
                                     const SettingsOverlayLayerText *text,
                                     ArUiLocale locale) {
  CHECK(text->label[0] && text->help && text->help[0]);
  if (locale == kArUiLocale_English) {
    /* The authentic atlas uppercases ASCII. Capitalization of keyed captions
     * may differ; words, quantities, scope and expansion indicators may not. */
    CHECK(!SDL_strcasecmp(label, text->label));
    CHECK(!SDL_strcasecmp(value, text->value));
  }
}

static void CheckLayerRowPresentation(void) {
  const struct { const char *family; int count; } enums[] = {
    {"action.edge", kActionBgEdge_RawWrap + 1},
    {"action.motion", kActionBgMotion_NormalScroll + 1},
    {"action.anchor", kActionBgBandAnchor_World + 1},
    {"action.extent", kActionBgExtent_Fixed + 1},
    {"action.source", kActionBgSource_AuthenticViewport + 1},
    {"action.role", kActionBgLayerRole_Backdrop + 1},
    {"diorama.shape", kDioramaDepth_StrategyCount},
    {"diorama.direction", kDioramaStack_DirectionCount},
  };
  for (size_t f = 0; f < sizeof(enums) / sizeof(enums[0]); ++f) {
    for (int v = 0; v < enums[f].count; ++v) {
      char key[128];
      snprintf(key, sizeof(key), "overlay.layer.%s.%d", enums[f].family, v);
      for (int locale = 0; locale < kArUiLocale_Count; ++locale)
        CHECK(ArUiCatalog_Text((ArUiLocale)locale, key, NULL)[0]);
    }
  }
  DioramaLayerOrderTable table = {0};
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 1, 2);
  CHECK(room);
  if (!room) return;
  DioramaEditorContext context = {.room_live = true, .map_group = 1, .map_number = 2};
  unsigned params = 0;
  for (int shape = 0; shape < kDioramaDepth_StrategyCount; ++shape) {
    for (int p = 0; p < DioramaLayerOrder_PlaneCount(); ++p) {
      int plane = DioramaLayerOrder_PlaneAt(p);
      context.selected_plane = plane;
      DioramaLayerEditor_SetStrategy(&room->planes[plane], (DioramaDepthStrategy)shape);
      if (shape == kDioramaDepth_Stack) {
        room->planes[plane].set_stack_density = true;
        room->planes[plane].stack_density = 14.0f;
      }
      DioramaEditorRow rows[kDioramaEditorRowMax];
      int count = DioramaLayerEditor_BuildRows(&table, &context, 0, rows, kDioramaEditorRowMax);
      for (int i = 0; i < count; ++i) {
        DioramaEditorRow before = rows[i];
        CHECK(rows[i].room_live && rows[i].map_group == 1 && rows[i].map_number == 2);
        if (rows[i].param) params |= 1u << rows[i].param;
        if (rows[i].kind == kDioramaEditorRow_Param || rows[i].kind == kDioramaEditorRow_ParamEnum) {
          char key[128];
          if (rows[i].param == kDioramaEditorParam_Copies && rows[i].strategy == kDioramaDepth_Voxel)
            snprintf(key, sizeof(key), "overlay.layer.diorama.slices");
          else snprintf(key, sizeof(key), "overlay.layer.diorama.param.%d", rows[i].param);
          CheckCatalogEntry(key, rows[i].label);
        }
        for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
          SettingsOverlayLayerText text;
          SettingsOverlay_LocalizedDioramaRow((ArUiLocale)locale, &rows[i], &text);
          CheckLayerCaptionParity(rows[i].label, rows[i].value, &text, (ArUiLocale)locale);
          CHECK(!memcmp(&before, &rows[i], sizeof(before)));
        }
      }
    }
  }
  CHECK(params == ((1u << (kDioramaEditorParam_Order + 1)) - 2));
  context.room_live = false;
  DioramaEditorRow offline[kDioramaEditorRowMax];
  CHECK(DioramaLayerEditor_BuildRows(NULL, &context, 0, offline, kDioramaEditorRowMax) == 1);
  CHECK(!offline[0].room_live);
  SettingsOverlayLayerText text;
  SettingsOverlay_LocalizedDioramaRow(kArUiLocale_French, &offline[0], &text);
  CHECK(!strcmp(text.label, ArUiCatalog_Text(kArUiLocale_French, "overlay.layer.diorama.enter", NULL)));

  ActionBgTuner_ResetSession();
  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  plan.layer[0].role = kActionBgLayerRole_Playfield;
  plan.layer[0].source = kActionBgSource_WorldMap;
  plan.layer[0].world_width = 4096;
  plan.layer[0].world_height = 512;
  plan.layer[0].horizontal_extent = (ActionBgHorizontalExtent){kActionBgExtent_Fixed, 32, 48};
  plan.layer[0].vertical_extent = (ActionBgVerticalExtent){kActionBgExtent_Fixed, 16, 24};
  plan.layer[0].band_count = 1;
  plan.layer[0].bands[0] = (ActionBgBand){.y0 = 136, .y1 = 224,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {kActionBgExtent_Fixed, 16, 24}};
  CHECK(ActionBgTuner_ObservePlan(1, 2, &plan, (ActionBgTunerLimits){120,120,64,64}));
  ActionBgTunerRow layer = {.kind = kActionBgTunerRow_Layer, .layer = 0, .band = -1, .selectable = true};
  ActionBgTunerRow band = {.kind = kActionBgTunerRow_BandHeader, .layer = 0, .band = 0, .selectable = true};
  CHECK(ActionBgTuner_Activate(&layer) == kActionBgTunerResult_Changed);
  CHECK(ActionBgTuner_Activate(&band) == kActionBgTunerResult_Changed);
  ActionBgTunerRow rows[kActionBgTunerRowMax];
  int count = ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
  unsigned kinds = 0;
  for (int i = 0; i < count; ++i) kinds |= 1u << rows[i].kind;
  CHECK(kinds == ((1u << (kActionBgTunerRow_Reset + 1)) - 1));
  /* Changing the owner after building a snapshot must not change its captions.
   * This catches accidental presentation-time access to the live draft. */
  ActionBgTuner_ResetSession();
  for (int i = 0; i < count; ++i) {
    ActionBgTunerRow before = rows[i];
    if (rows[i].kind != kActionBgTunerRow_Header && rows[i].kind != kActionBgTunerRow_Layer &&
        rows[i].kind != kActionBgTunerRow_BandHeader) {
      char key[128];
      snprintf(key, sizeof(key), "overlay.layer.action.label.%d", rows[i].kind);
      CheckCatalogEntry(key, rows[i].label);
    }
    for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
      SettingsOverlay_LocalizedActionBgRow((ArUiLocale)locale, &rows[i], &text);
      CheckLayerCaptionParity(rows[i].label, rows[i].value, &text, (ArUiLocale)locale);
      CHECK(!memcmp(&before, &rows[i], sizeof(before)));
    }
  }
}

static void CheckCompleteDescriptorCatalog(void) {
  /* Compile the real registry, including macros and debug-only settings. A
   * second source parser would miss conditional/expanded descriptors. */
  for (int i = 0; i < g_setting_desc_count; ++i) {
    const SettingDesc *desc = &g_setting_descs[i];
    char key[192];
    snprintf(key, sizeof(key), "setting.%s.label", desc->key);
    CheckCatalogEntry(key, desc->label);
    if (desc->tooltip && desc->tooltip[0]) {
      snprintf(key, sizeof(key), "setting.%s.help", desc->key);
      CheckCatalogEntry(key, desc->tooltip);
    }
    for (int v = 0; desc->enum_labels && v < desc->enum_count; ++v) {
      snprintf(key, sizeof(key), "setting.%s.value.%d", desc->key, v);
      CheckCatalogEntry(key, desc->enum_labels[v]);
    }
  }
  /* Controller names come from the input owner; adding a named button/axis
   * requires a caption without affecting the numeric persisted identity. */
  for (int kind = kInputBind_PadButton; kind <= kInputBind_PadAxis; ++kind) {
    int count = kind == kInputBind_PadButton ? SDL_GAMEPAD_BUTTON_COUNT
                                            : SDL_GAMEPAD_AXIS_COUNT;
    for (int code = 0; code < count; ++code) {
      for (int negative = 0; negative <= (kind == kInputBind_PadAxis); ++negative) {
        uint32 binding = INPUT_BIND_MAKE(kind, code, negative);
        const char *name = InputMap_BindingName(binding);
        if (!name || !name[0]) continue;
        char key[80];
        snprintf(key, sizeof(key), "overlay.binding.%s.%d.%d",
                 kind == kInputBind_PadButton ? "button" : "axis", code, negative);
        CheckCatalogEntry(key, name);
      }
    }
  }
}

static void CheckDynamicInterfaceValues(void) {
  Settings before = g_settings;
  int hz = HostDisplayStatus_NominalRefreshHz();
  bool vsync = HostDisplayStatus_VsyncActive();
  char value[512], serialized[512];
  struct { const char *setting; const char *text; const char *key; } cases[] = {
    {"hud_scale_percent", "match", "overlay.value.match_game"},
    {"menu_scale_percent", "auto", "overlay.value.auto"},
    {"save_master_hp", "leave-as-is", "overlay.value.leave"},
    {"save_angel_hp_current", "leave-as-is", "overlay.value.leave"},
    {"save_score_fillmore_1", "leave-as-is", "overlay.value.leave"},
    {"save_player_name", "", "overlay.value.leave"},
    {"bind_key_up", "Unbound", "overlay.binding.unbound"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const SettingDesc *desc = Settings_Find(cases[i].setting);
    CHECK(desc && Settings_SetText(desc, cases[i].text) != kSettingChange_Rejected);
    for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
      SettingsOverlay_LocalizedValue((ArUiLocale)locale, desc, value, sizeof(value));
      CHECK(!strcmp(value, ArUiCatalog_Text((ArUiLocale)locale, cases[i].key, NULL)));
    }
  }
  /* User/device/keycap text is literal. No runtime matching of English labels,
   * no catalog strings written to settings.ini or save fields. */
  snprintf(g_settings.save_player_name, sizeof(g_settings.save_player_name), "Auto");
  g_settings.save_master_hp = 15;
  g_settings.save_angel_hp_current = 1;
  CHECK(Settings_SetText(Settings_Find("save_score_fillmore_1"), "210") !=
        kSettingChange_Rejected);
  const char *literal[] = {"save_player_name", "save_master_hp", "save_angel_hp_current",
                          "save_score_fillmore_1"};
  for (size_t i = 0; i < sizeof(literal) / sizeof(literal[0]); ++i) {
    const SettingDesc *desc = Settings_Find(literal[i]);
    Settings_FormatValue(desc, serialized, sizeof(serialized));
    SettingsOverlay_LocalizedValue(kArUiLocale_Japanese, desc, value, sizeof(value));
    CHECK(!strcmp(serialized, value));
  }
  const SettingDesc *refresh = Settings_Find("refresh_mode");
  g_settings.refresh_mode = kRefreshMode_Vsync;
  HostDisplayStatus_SetVsyncActive(false);
  SettingsOverlay_LocalizedValue(kArUiLocale_French, refresh, value, sizeof(value));
  CHECK(!strcmp(value, ArUiCatalog_Text(kArUiLocale_French, "overlay.value.vsync_unavailable", NULL)));
  HostDisplayStatus_SetNominalRefreshHz(144);
  HostDisplayStatus_SetVsyncActive(true);
  SettingsOverlay_LocalizedValue(kArUiLocale_Japanese, refresh, value, sizeof(value));
  CHECK(!strcmp(value, "Vsync 144Hz"));
  Settings_FormatValue(refresh, serialized, sizeof(serialized));
  CHECK(!strcmp(serialized, "Vsync"));
  const SettingDesc *binding = Settings_Find("bind_key_up");
  CHECK(Settings_SetText(binding, "Key 4 A") != kSettingChange_Rejected);
  SettingsOverlay_LocalizedValue(kArUiLocale_French, binding, value, sizeof(value));
  CHECK(!strcmp(value, "Touche A"));
  Settings_FormatValue(binding, serialized, sizeof(serialized));
  CHECK(!strcmp(serialized, "Key 4 A"));
  CHECK(Settings_SetText(binding, "Pad R-Stick Up") != kSettingChange_Rejected);
  SettingsOverlay_LocalizedValue(kArUiLocale_German, binding, value, sizeof(value));
  CHECK(!strcmp(value, "Pad R-Stick hoch"));
  Settings_FormatValue(binding, serialized, sizeof(serialized));
  CHECK(!strcmp(serialized, "Pad R-Stick Up"));
  /* The harness has no connected controllers: test both hotplug-following and
   * explicit disconnected slot captions without depending on local hardware. */
  g_settings.input_gamepad_slot = 0;
  SettingsOverlay_LocalizedValue(kArUiLocale_Japanese,
      Settings_Find("input_gamepad_slot"), value, sizeof(value));
  CHECK(!strcmp(value, "最初の接続"));
  g_settings.input_gamepad_slot = 7;
  SettingsOverlay_LocalizedValue(kArUiLocale_French,
      Settings_Find("input_gamepad_slot"), value, sizeof(value));
  CHECK(!strcmp(value, "Manette 7 (déconnectée)"));
  const SettingsLocalizationPack packs[] = {
    {.id = "literal", .name = "common.save {slot}", .locale = "en-CA", .manifest = "pack.ini"},
  };
  CHECK(Settings_SetLocalizationPacks(packs, 1));
  g_settings.localization_content = 2;
  SettingsOverlay_LocalizedValue(kArUiLocale_Japanese,
      Settings_Find("localization_content"), value, sizeof(value));
  CHECK(!strcmp(value, "common.save {slot} (en-CA) [literal]"));
  CHECK(Settings_SetLocalizationPacks(NULL, 0));
  /* Small buffers must never split a translated UTF-8 caption. */
  g_settings.menu_scale_percent = 0;
  char tiny[4] = {0};
  CHECK(SettingsOverlay_LocalizedValue(kArUiLocale_Japanese,
      Settings_Find("menu_scale_percent"), tiny, sizeof(tiny)) == 6);
  CHECK(!strcmp(tiny, "自"));
  HostDisplayStatus_SetNominalRefreshHz(hz);
  HostDisplayStatus_SetVsyncActive(vsync);
  g_settings = before;
}

static void CheckDecisions(SDL_Renderer *renderer, SDL_Surface *surface) {
  SettingsOverlay_Close();
  CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_None);
  const int language=g_settings.interface_language;
  for (int locale=0; locale<kArUiLocale_Count; ++locale) {
    g_settings.interface_language=locale;
    CHECK(SettingsOverlay_BeginDecision("overlay.region.continue_title",
        "overlay.region.legacy_estimate","overlay.region.acknowledge"));
    CHECK(SettingsOverlay_IsOpen());
    CHECK(!SettingsOverlay_BeginDecision("a","b","c"));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Pending);
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,true)); /* Held key never consents. */
    CHECK(SettingsOverlay_IsOpen());
    SettingsOverlay_Render((ArRenderRectI){0,0,surface->w,surface->h});
    SDL_RenderPresent(renderer);
    const char *directory=getenv("AR_OVERLAY_DECISION_PREVIEW_DIR");
    if(directory && *directory) {
      char path[1024];
      snprintf(path,sizeof(path),"%s/continue-%s.bmp",directory,ArUiCatalog_LocaleTag((ArUiLocale)locale));
      CHECK(SDL_SaveBMP(surface,path));
    }
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,false)); /* Default is Cancel. */
    CHECK(!SettingsOverlay_IsOpen());
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Cancelled);
    char body[2048];const uint16_t removed[6]={128,128,128,128,128,128};
    CHECK(SettingsOverlayRegions_PopulationConfirmation((ArUiLocale)locale,kArRegionalSource_Japan,removed,body,sizeof(body)));
    CHECK(SettingsOverlay_BeginDecisionText("overlay.region.population_label",body,"overlay.region.population_accept"));
    memset(body,'X',strlen(body)); /* Overlay owns its copy, not the caller's buffer. */
    SettingsOverlay_Render((ArRenderRectI){0,0,surface->w,surface->h});SDL_RenderPresent(renderer);
    if(directory && *directory) {
      char path[1024];snprintf(path,sizeof(path),"%s/population-%s.bmp",directory,ArUiCatalog_LocaleTag((ArUiLocale)locale));
      CHECK(SDL_SaveBMP(surface,path));
    }
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,false));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Cancelled);
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_None);
    CHECK(SettingsOverlay_BeginDecision("overlay.region.continue_title",
        "overlay.region.legacy_estimate","overlay.region.acknowledge"));
    CHECK(SettingsOverlay_HandleKey(SDLK_UP,true,false));
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,false));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Accepted);
    CHECK(!SettingsOverlay_IsOpen());
    CHECK(SettingsOverlay_BeginDecision("overlay.region.continue_title",
        "overlay.region.adoption_failed","overlay.decision.retry"));
    CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE,true,false));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Cancelled);
    CHECK(SettingsOverlay_BeginNotice("overlay.region.population_label",
        "overlay.region.population_complete","overlay.region.acknowledge"));
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN,true,false));
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,true));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Pending);
    SettingsOverlay_Render((ArRenderRectI){0,0,surface->w,surface->h});SDL_RenderPresent(renderer);
    CHECK(SettingsOverlay_HandleKey(SDLK_RETURN,true,false));
    CHECK(SettingsOverlay_TakeDecisionResult()==kOverlayDecision_Accepted);
  }
  CHECK(!SettingsOverlay_BeginDecision(NULL,"b","c"));
  g_settings.interface_language=language;
  SettingsOverlay_Open();
}

static void CheckInterfaceCatalogs(SDL_Renderer *renderer, SDL_Surface *surface) {
  CheckCompleteDescriptorCatalog();
  CheckLayerHelpCatalog();
  CheckLayerRowPresentation();
  CheckDynamicInterfaceValues();
  Settings before = g_settings;
  const SettingDesc *language = Settings_Find("interface_language");
  const SettingDesc *font = Settings_Find("localization_font_sampling");
  CHECK(language && language->category == kSettingCat_Interface);
  NavToSection(kSection_Localization);
  NavToTab(2);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("interface_language");
  int native_sampling = g_settings.localization_font_sampling;
  char value[256], serialized[256];
  for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
    CHECK(Settings_SetText(language, ArUiCatalog_LocaleTag((ArUiLocale)locale)) !=
          kSettingChange_Rejected);
    CHECK(g_settings.localization_content == before.localization_content);
    CHECK(g_settings.localization_presentation == before.localization_presentation);
    CHECK(g_settings.localization_font_sampling == native_sampling);
#ifdef AR_OVERLAY_UI_FONT
    CHECK(SettingsOverlay_InterfaceLocale() == (ArUiLocale)locale);
#else
    CHECK(SettingsOverlay_InterfaceLocale() == kArUiLocale_English);
#endif
    Settings_FormatValue(language, serialized, sizeof(serialized));
    CHECK(!strcmp(serialized, ArUiCatalog_LocaleTag((ArUiLocale)locale)));
    SettingsOverlay_LocalizedValue((ArUiLocale)locale, font, value, sizeof(value));
    Settings_FormatValue(font, serialized, sizeof(serialized));
    CHECK(!strcmp(serialized, native_sampling == 0 ? "Crisp" : "Smooth"));
    CHECK(!strcmp(SettingsOverlay_LocalizedLabel((ArUiLocale)locale, language),
        ArUiCatalog_Text((ArUiLocale)locale, "setting.interface_language.label", NULL)));
    CHECK(!strcmp(SettingsOverlay_LocalizedGameChangeHeading(
                      (ArUiLocale)locale,
                      kSettingGameChange_OriginalBugFix),
                  ArUiCatalog_Text((ArUiLocale)locale,
                      "overlay.group.original_bug_fixes", NULL)));
    CHECK(!strcmp(SettingsOverlay_LocalizedGameChangeHeading(
                      (ArUiLocale)locale,
                      kSettingGameChange_QualityOfLife),
                  ArUiCatalog_Text((ArUiLocale)locale,
                      "overlay.group.quality_of_life", NULL)));
    /* Traversal, help, and reset prompts use the actual shaped overlay. The
     * same loop also runs in no-TTF builds, which must remain English/readable. */
    for (int tab = 0; tab < 4; ++tab) {
      NavToTab(tab);
      if (renderer && surface) {
        SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
        CHECK(SDL_RenderClear(renderer));
        SettingsOverlay_Render((ArRenderRectI){0, 0, surface->w, surface->h});
        CHECK(SDL_RenderPresent(renderer));
        const char *prefix = getenv("AR_OVERLAY_CATALOG_TEST_PREFIX");
        if (prefix && prefix[0]) {
          char path[1024];
          int size = snprintf(path, sizeof(path), "%s-%s-%d.bmp", prefix,
                               ArUiCatalog_LocaleTag((ArUiLocale)locale), tab);
          CHECK(size > 0 && size < (int)sizeof(path));
          if (size > 0 && size < (int)sizeof(path)) CHECK(SDL_SaveBMP(surface, path));
        }
      }
    }
  }
  /* Keyed presentation must not translate user-authored values or custom
   * formatting; even a pack named exactly like a catalog key is just data. */
  SettingDesc copy = *font;
  copy.key = "unknown_user_setting";
  copy.label = "common.save";
  CHECK(!strcmp(SettingsOverlay_LocalizedLabel(kArUiLocale_Japanese, &copy), "common.save"));
  SettingsOverlay_LocalizedValue(kArUiLocale_Japanese, &copy, value, sizeof(value));
  Settings_FormatValue(&copy, serialized, sizeof(serialized));
  CHECK(!strcmp(value, serialized));
  NavToTab(2);
  RowToKey("interface_language");
  /* Use the real menu change/persistence path too, not just direct formatting.
   * Feedback must occupy the description region without covering the title. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  if (renderer && surface) {
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_Render((ArRenderRectI){0, 0, surface->w, surface->h});
    CHECK(SDL_RenderPresent(renderer));
    const char *prefix = getenv("AR_OVERLAY_CATALOG_TEST_PREFIX");
    if (prefix && prefix[0]) {
      char path[1024];
      int size = snprintf(path, sizeof(path), "%s-feedback.bmp", prefix);
      CHECK(size > 0 && size < (int)sizeof(path));
      if (size > 0 && size < (int)sizeof(path)) CHECK(SDL_SaveBMP(surface, path));
    }
  }
  g_settings = before;
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  NavToTab(0);
  NavToSection(kSection_Video);
}

static uint8_t *ReadOptionalRom(size_t *size_out) {
  const char *path = getenv("AR_OVERLAY_TEST_ROM");
  *size_out = 0;
  if (!path || !path[0]) return NULL;
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *data = (uint8_t *)malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size_out = (size_t)size;
  return data;
}

/* The gamepad menu gate remains connection-based. Keyboard arbitration is
 * activity-based in Auto: an idle connected pad does not lock out a keyboard,
 * but a native pad event wins over a simultaneous Steam-generated key. */
static bool MenuGamepadOwns(int input_device, int gamepad_count) {
  return input_device != kInputDevice_Keyboard && gamepad_count > 0;
}
static void CheckMenuDeviceGateTruthTable(void) {
  const struct {
    int input_device;
    int gamepad_count;
    bool pad_input_active;
    bool gamepad_enabled;
    bool keyboard_active;
  } rows[] = {
      /* input_device,       pads, active, gamepad, keyboard */
      { kInputDevice_Auto,     0, false, false, true  },
      { kInputDevice_Auto,     1, false, true,  true  },
      { kInputDevice_Auto,     1, true,  true,  false },
      { kInputDevice_Keyboard, 0, false, false, true  },
      { kInputDevice_Keyboard, 2, true,  false, true  },
      { kInputDevice_Gamepad,  1, false, true,  false },
      { kInputDevice_Gamepad,  0, false, false, true  },
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    bool pad = MenuGamepadOwns(rows[i].input_device, rows[i].gamepad_count);
    bool kbd = InputMap_ShouldAcceptKeyboard(
        (InputDeviceMode)rows[i].input_device, rows[i].gamepad_count > 0,
        rows[i].pad_input_active);
    CHECK(pad == rows[i].gamepad_enabled);
    CHECK(kbd == rows[i].keyboard_active);
    /* At least one device is always active — never a total lockout. */
    CHECK(pad || kbd);
  }

  const uint32 keyboard = (1u << kInputAction_Up) |
                          (1u << kInputAction_B);
  const uint32 gamepad = (1u << kInputAction_Right);
  CHECK(InputMap_ArbitrateState(kInputDevice_Auto, true, true,
                                keyboard, gamepad) == gamepad);
  CHECK(InputMap_ArbitrateState(kInputDevice_Auto, true, false,
                                keyboard, gamepad) == keyboard);
  CHECK(InputMap_ArbitrateState(kInputDevice_Gamepad, false, false,
                                keyboard, gamepad) == keyboard);
}

/* ── Layer editor harness ────────────────────────────────────────────────
 *
 * The overlay reaches the override table through injected hooks precisely so
 * this test can supply its own without linking diorama.c (which would drag in
 * the PPU and the SDL render path). These fakes are the whole reason the
 * indirection exists. */
static DioramaLayerOrderTable s_fake_layer_table;
static bool s_fake_room_live;
static uint8_t s_fake_group = 0x01;   /* Fillmore */
static uint8_t s_fake_map = 0x02;     /* act 2, the reported room */
static uint8_t s_fake_section = kDioramaLayerSection_Room;
static int s_fake_saves;
static uint16_t s_fake_cgram[kSettingsOverlayLayerPaletteEntries];

static DioramaLayerOrderTable *FakeLayerTable(void) {
  return &s_fake_layer_table;
}

static bool FakeLayerRoom(uint8_t *group, uint8_t *map, uint8_t *section) {
  if (!s_fake_room_live) return false;
  if (group) *group = s_fake_group;
  if (map) *map = s_fake_map;
  if (section) *section = s_fake_section;
  return true;
}

static bool FakeLayerSave(void) {
  s_fake_saves++;
  return true;
}

static bool FakeLayerPalette(
    uint16_t out_cgram[kSettingsOverlayLayerPaletteEntries]) {
  memcpy(out_cgram, s_fake_cgram, sizeof(s_fake_cgram));
  return true;
}

/* Drive the Layers section the way a player would: keys only, no direct calls
 * into the row model. What is asserted is the WIRING -- that a keypress reaches
 * the override table, that the cursor never rests on a caption, that the section
 * disappears without debug settings, and that an edit persists. The row model
 * itself is covered by tests/diorama_layer_editor_test.c. */
static void CheckLayerEditorSection(void) {
  SettingsOverlay_SetLayerEditorHooks(FakeLayerTable, FakeLayerRoom,
                                      FakeLayerSave);
  SettingsOverlay_SetLayerPaletteProvider(FakeLayerPalette);
  memset(&s_fake_layer_table, 0, sizeof(s_fake_layer_table));
  for (int i = 0; i < kSettingsOverlayLayerPaletteEntries; i++)
    s_fake_cgram[i] = (uint16_t)i;
  s_fake_room_live = true;
  s_fake_section = kDioramaLayerSection_Room;
  s_fake_saves = 0;

  /* THE GATE the feature was asked for: developer-only means the section is not
   * in the nav column at all for a player, not merely that its rows are. */
  g_settings.show_debug_settings = false;
  SettingsOverlay_Refresh();
  int total = -1;
  CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, &total));
  CHECK(total == kPlayerSectionCount);

  /* Stepping DOWN from the last visible section must WRAP to the first, not walk
   * onto the hidden one. Asserted from the section above it, since a wrap that
   * landed on Layers would report an out-of-range position rather than 0.
   *
   * Without this the only evidence would be positional, and Layers is last -- so
   * its raw index and its visible position coincide and a MoveSection that
   * happily lands on a hidden section looks identical to one that skips it.
   *
   * Driven from the last PLAYER-visible section, which is the property under
   * test -- not that section's name. */
  NavToSection(kSection_Localization);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  int wrapped = -1;
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, &total));
  CHECK(wrapped == 0);
  CHECK(total == kPlayerSectionCount);
  /* UP from the first reaches Localization, not a hidden developer section. */
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_Localization);

  /* The randomizer is gated at BOTH levels: its section is debug_only so the
   * nav column omits it (asserted by the count above), and its rows are
   * debug-only so nothing can surface them from another tab. Checked with the
   * flag still off, which is the state a player ships with. */
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_enable")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_statue_drops")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_lair_types")));

  g_settings.show_debug_settings = true;
  SettingsOverlay_Refresh();
  /* Production initializes the randomizer before the overlay can open. Give
   * this isolated menu test the same stable capability before asserting that
   * its master row is available. */
  {
    static uint8_t fake_rom[0x100000];
    CHECK(Randomizer_Init(fake_rom, (uint32_t)sizeof fake_rom));
  }
  CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, &total));
  CHECK(total == kDebugSectionCount);
  /* ...and revealed by the same flag, so the gate is a switch and not a wall. */
  CHECK(Settings_IsMenuVisible(Settings_Find("rando_enable")));
  /* With debug on, DOWN from Localization reaches the first hidden section and one
   * more DOWN reaches the last -- which is what pins the reported ordinal to
   * the VISIBLE numbering that the nav column draws in. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_Randomizer);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_Layers);

  /* The randomizer section: four tabs, and every row the player edits must be
   * reachable by name. Its rows are gated on the master being on and on the ROM
   * snapshot existing, so this drives the master first -- otherwise the gated
   * rows are legitimately absent and "reachable" would prove nothing. Seed sits
   * on tab 0 beside the master; the per-area rows are on their own tabs. */
  NavToSection(kSection_Randomizer);
  {
    int tabs = 0;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 4);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* into the rows */
  NavToTab(0);
  RowToKey("rando_enable");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* master on */
  CHECK(g_settings.rando_enable);
  NavToTab(0);
  RowToKey("rando_seed");
  RowToKey("rando_reroll");
  NavToTab(1);
  RowToKey("rando_enemy_hp");
  RowToKey("rando_enemy_types");
  NavToTab(2);
  RowToKey("rando_statue_drops");
  RowToKey("rando_statue_spots");
  NavToTab(3);
  RowToKey("rando_lair_spots");
  RowToKey("rando_lair_types");
  /* Turning the master back off must retract the gated rows, which is what
   * stops the menu offering edits that would do nothing. */
  NavToTab(0);
  RowToKey("rando_enable");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(!g_settings.rando_enable);
  CHECK(!Settings_IsAvailable(Settings_Find("rando_seed")));
  /* Back out to the nav column so the sections below start where they expect. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  NavToSection(kSection_Layers);
  /* Tab 0 is Fillmore, which is where the fake room is. */
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* open submenu */

  /* The cursor must never rest on the room caption, which is row 0 and is not
   * selectable -- so opening the submenu has already stepped past it onto a real
   * row. Asserted by name rather than by index. */
  CHECK(strcmp(SettingsOverlay_SelectedKey(), "") != 0);

  /* The list must contain NO row the editor does not own. Two ways it could:
   * the synthetic "Reset <section> defaults" row (which has no registry
   * categories to act on here), and a settings descriptor matched by row index
   * because SelectedDesc walked the registry. Walking the whole list and
   * requiring every row to report an editor-shaped key catches both -- an
   * off-by-one row past the end reports "" and a descriptor reports its own key,
   * neither of which is a plane token. */
  {
    int rows = 0;
    CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, NULL));
    /* Step through more rows than any tab has, checking each landing. */
    for (int i = 0; i < 48; i++) {
      const char *key = SettingsOverlay_SelectedKey();
      CHECK(strcmp(key, "") != 0);
      CHECK(strcmp(key, "reset_section_defaults") != 0);
      /* Every editor key is a plane token, a "token.param", or the room reset. */
      const bool known = !strcmp(key, "layer_reset_room") ||
                         DioramaLayerOrder_PlaneFromToken(key) >= 0 ||
                         strchr(key, '.') != NULL;
      CHECK(known);
      if (Settings_Find(key)) CHECK(!"editor row matched a settings descriptor");
      rows++;
      CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
    }
    CHECK(rows == 48);
  }

  /* Cycle the water plane's shape. Navigating by name, because the row list's
   * shape changes with the active shape and a keypress count would break. */
  RowToKey("bg2hi");
  const int saves_before = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_fake_saves > saves_before);   /* the edit was persisted */
  const DioramaRoomOverride *room =
      DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(room != NULL);
  CHECK(DioramaLayerOrder_RoomIsActive(room));
  /* One Right from FLAT is RAKE, and cycling a plane expands it -- so its depth
   * row now exists and is reachable. Both are contracts, not incidentals: the
   * expansion is what puts the parameters under the cursor after a change. */
  const DioramaPlaneOverride *water = &room->planes[kDioramaPlane_Bg2Hi];
  CHECK(DioramaLayerEditor_StrategyOfPlane(water) == kDioramaDepth_Rake);
  RowToKey("bg2hi.depth");
  /* Stepping the depth row moves the rake itself, not some other key. */
  const float rake_before = water->rake;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(water->rake > rake_before);

  /* The menu's reset verb is SNES Y, whose kDefaults keyboard binding is A --
   * not SDLK_Y. Clearing the depth removes the SHAPE, since
   * a rake of zero is not a shape, so the plane reads FLAT again. */
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  CHECK(DioramaLayerEditor_StrategyOfPlane(water) == kDioramaDepth_Flat);

  /* Author two planes, then confirm the room reset clears both at once. */
  RowToKey("bg2hi");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  RowToKey("bg1");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(DioramaLayerOrder_RoomIsActive(room));

  RowToKey("layer_reset_room");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* confirm */

  /* A room with no overrides left must be INACTIVE, so Resolve returns the
   * built-in table and the unedited-game guarantee holds. */
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(!room || !DioramaLayerOrder_RoomIsActive(room));

  /* Base BG1/BG2 expose a live 16x16 CGRAM picker. The selection stores the
   * index (not the sampled RGB), so palette animation remains live. Starting at
   * $00, Right, Right, Down lands on $12. */
  RowToKey("bg2");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("bg2.transparent");
  const int saves_before_palette = s_fake_saves;
  /* Opening and cancelling the picker is read-only: it must not consume one
   * of the bounded room override slots before a colour is confirmed. */
  CHECK(!DioramaLayerOrder_Find(
      &s_fake_layer_table, s_fake_group, s_fake_map));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(!DioramaLayerOrder_Find(
      &s_fake_layer_table, s_fake_group, s_fake_map));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  const DioramaPlaneOverride *filled =
      room ? &room->planes[SR_PPU_OVERLAY_BG2] : NULL;
  CHECK(filled && filled->set_transparent_fill);
  CHECK(filled && filled->transparent_fill_kind ==
                      kDioramaTransparentFill_Cgram);
  CHECK(filled && filled->transparent_fill_cgram == 0x12);
  CHECK(s_fake_saves > saves_before_palette);
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(room == NULL);  /* clearing the final key recycles the bounded slot */

  /* The production room hook also carries a camera-local section. Edits while
   * that section is live must land in its refining record, never the base room. */
  s_fake_section = kDioramaLayerSection_AitosWaterfall;
  RowToKey("backdrop");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* expand */
  RowToKey("backdrop.source");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  const DioramaRoomOverride *scoped = DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section);
  CHECK(scoped != NULL);
  CHECK(scoped && scoped->planes[kDioramaPlane_Backdrop].set_source);
  CHECK(scoped && scoped->planes[kDioramaPlane_Backdrop].source ==
                      DioramaLayerOrder_ActionBgSource(0x01, 0x01, 1));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(!room || !DioramaLayerOrder_RoomIsActive(room));

  /* Left in a scoped section authors OFF, rather than merely clearing the
   * local key and exposing an inherited base-room fill again. */
  DioramaRoomOverride *base = DioramaLayerOrder_FindOrAdd(
      &s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(base != NULL);
  if (base) {
    base->planes[SR_PPU_OVERLAY_BG2].set_transparent_fill = true;
    base->planes[SR_PPU_OVERLAY_BG2].transparent_fill_kind =
        kDioramaTransparentFill_Black;
  }
  RowToKey("bg2");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("bg2.transparent");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  scoped = DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section);
  CHECK(scoped &&
        scoped->planes[SR_PPU_OVERLAY_BG2].set_transparent_fill);
  CHECK(scoped &&
        scoped->planes[SR_PPU_OVERLAY_BG2].transparent_fill_kind ==
            kDioramaTransparentFill_None);
  DioramaTransparentFill effective_fill = kDioramaTransparentFill_Black;
  uint8_t effective_cgram = 0xff;
  CHECK(DioramaLayerOrder_ResolveTransparentFill(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section,
      SR_PPU_OVERLAY_BG2, &effective_fill, &effective_cgram));
  CHECK(effective_fill == kDioramaTransparentFill_None);
  RowToKey("layer_reset_room");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section));
  DioramaLayerOrder_ResetRoom(
      &s_fake_layer_table, s_fake_group, s_fake_map);
  s_fake_section = kDioramaLayerSection_Room;

  /* The final Layers tab is a separate, session-only action-BG tuner. It uses
   * the live canonical plan even when Diorama itself is not the provider, and
   * must never write diorama-layers.ini through the hooks above. */
  {
    ActionBgPlan canonical;
    ActionBgPlan_InitNative(&canonical);
    canonical.layer[0].role = kActionBgLayerRole_Playfield;
    canonical.layer[0].source = kActionBgSource_WorldMap;
    canonical.layer[0].world_width = 4096;
    canonical.layer[0].world_height = 512;
    canonical.layer[0].default_edge = kActionBgEdge_LiveWorld;
    canonical.layer[1].role = kActionBgLayerRole_Backdrop;
    canonical.layer[1].source = kActionBgSource_AuthenticViewport;
    canonical.layer[1].world_width = 256;
    canonical.layer[1].world_height = 256;
    canonical.layer[1].default_edge = kActionBgEdge_Mirror;
    canonical.layer[1].vertical_extent = (ActionBgVerticalExtent) {
      .mode = kActionBgExtent_Fixed, .top = 8, .bottom = 12,
    };
    ActionBgTuner_ResetSession();
    CHECK(ActionBgTuner_ObservePlan(
        1, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
    const int saves_before_bg_tuner = s_fake_saves;
    NavToTab(kDioramaEditorLevelCount);
    RowToKey("bg_tuner.apply");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(ActionBgTuner_DraftEnabled());
    RowToKey("bg2");
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    RowToKey("bg2.ignore_side_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    ActionBgPlan unbounded = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&unbounded));
    CHECK(unbounded.layer[1].horizontal_extent.mode ==
          kActionBgExtent_Available);
    RowToKey("bg2.ignore_vertical_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    unbounded = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&unbounded));
    CHECK(unbounded.layer[1].vertical_extent.mode ==
          kActionBgExtent_Available);
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    RowToKey("bg2.ignore_side_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    RowToKey("bg2.horizontal");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    RowToKey("bg2.left");
    CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
    ActionBgPlan applied = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&applied));
    CHECK(applied.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
    CHECK(applied.layer[1].horizontal_extent.left == 116);
    RowToKey("bg_tuner.guides");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(ActionBgTuner_GuidesEnabled());
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    CHECK(!ActionBgTuner_GuidesEnabled());
    RowToKey("bg_tuner.reset");
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    CHECK(!ActionBgTuner_DraftEnabled());
    CHECK(s_fake_saves == saves_before_bg_tuner);
  }

  /* A level the player is not in explains itself rather than editing something.
   * Bloodpool is tab 1; the fake room is Fillmore. */
  NavToTab(1);
  const int saves_at_foreign = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_fake_saves == saves_at_foreign);   /* nothing authored */

  /* And with no room live at all, no tab edits anything. */
  NavToTab(0);
  s_fake_room_live = false;
  const int saves_offline = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_fake_saves == saves_offline);
  s_fake_room_live = true;

  /* Turning debug settings off while standing IN the editor must move focus out
   * rather than leave the cursor on a hidden section. */
  g_settings.show_debug_settings = false;
  SettingsOverlay_Refresh();
  int selected = -1;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected != kSection_Layers);
  CHECK(total == kPlayerSectionCount);
  g_settings.show_debug_settings = true;
  SettingsOverlay_Refresh();

  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));   /* leave submenu */
  /* Leave no hooks behind: later blocks drive other sections. */
  SettingsOverlay_SetLayerEditorHooks(NULL, NULL, NULL);
  SettingsOverlay_SetLayerPaletteProvider(NULL);
  ActionBgTuner_ResetSession();
}

static void CheckPerformanceOverlay(ArRenderDevice *device, SDL_Renderer *renderer,
                                     SDL_Surface *surface) {
  PerformanceSnapshot sample = {.revision = 1, .ready = true, .fps = 40,
      .frame_mean_ms = 25, .frame_p95_ms = 27, .frame_max_ms = 29};
  sample.context = (PerformanceContext){.scene = kPerformanceScene_World,
      .width = surface->w, .height = surface->h, .map_number = 9};
  for (int i = 0; i < kPerformanceStage_Count; i++)
    sample.stages[i] = (PerformanceValue){.mean_ms = .25, .maximum_ms = .5, .calls = 40};
  const ArRenderExtentI output = {surface->w, surface->h};
  for (int level = 0; level <= 2; level++) {
    CHECK(SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
    CHECK(SDL_SetRenderViewport(renderer, NULL));
    CHECK(SDL_SetRenderClipRect(renderer, NULL));
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    CHECK(SDL_RenderClear(renderer));
    CHECK(PerformanceOverlay_Render(device, &sample, level, output));
    CHECK(SDL_RenderPresent(renderer));
    PerformanceOverlayModel model;
    PerformanceOverlay_Build(&sample, level, output, &model);
    int changed = 0, glyph = 0;
    for (int y = 0; y < surface->h; y++)
      for (int x = 0; x < surface->w; x++) {
        Uint8 r, g, b, a;
        CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
        if (r == 32 && g == 24 && b == 16) continue;
        changed++;
        glyph += r > 128 && g > 128 && b > 128;
        CHECK(x >= model.panel.x && x < model.panel.x + model.panel.w);
        CHECK(y >= model.panel.y && y < model.panel.y + model.panel.h);
      }
    CHECK(level ? changed > 100 && glyph > 30 : changed == 0);
  }
  const char *preview = getenv("AR_PERFORMANCE_OVERLAY_PREVIEW");
  if (preview && *preview) CHECK(SDL_SaveBMP(surface, preview));
  PerformanceOverlay_Reset(device);
}

static uint64_t CheckRegionBadges(ArRenderDevice *device, SDL_Renderer *renderer,
                                SDL_Surface *surface, bool capture) {
  if (!surface || surface->w < 448 || surface->h < 88) {
    CHECK(surface && surface->w >= 448 && surface->h >= 88);
    return 0;
  }
  const SettingsOverlayArtwork *art = SettingsOverlayArtwork_Get();
  CHECK(ArRenderTexture_IsValid(art->region_badges));
  CHECK(ArRenderDevice_UseOutputCoordinates(device));
  CHECK(ArRenderDevice_Clear(device, (ArRenderColorF){0.04f, 0.06f, 0.10f, 1}));
  static const char *labels[] = {"US", "JAPAN", "EUROPE", "CUSTOM"};
  for (int badge = 0; badge < kOverlayRegionBadge_Count; ++badge) {
    const ArRenderRectF source = {
      (float)(badge * kRegionBadgeWidth), 0, kRegionBadgeWidth, kRegionBadgeHeight};
    const ArRenderRectF destination = {(float)(8 + badge * 112), 8, 96, 64};
    CHECK(ArRenderDevice_DrawTexture(device, art->region_badges, &source, &destination));
    for (unsigned i = 0; labels[badge][i]; ++i) {
      unsigned ch = (unsigned char)labels[badge][i];
      ArRenderRectF glyph = {(float)((ch % 16) * 8), (float)((ch / 16) * 8), 8, 8};
      ArRenderRectF cell = {(float)(8 + badge * 112 + i * 8), 80, 8, 8};
      CHECK(ArRenderDevice_DrawTexture(device, art->fonts[kText_Normal], &glyph, &cell));
    }
  }
  CHECK(SDL_RenderPresent(renderer));
  /* Include every pixel in the magnified badges; compare before/after device
   * reset below. Check characteristic colors as well, not just nonempty art. */
  uint64_t hash = UINT64_C(14695981039346656037);
  for (int y = 8; y < 72; ++y) {
    const uint32_t *row = (const uint32_t *)((const uint8_t *)surface->pixels + y * surface->pitch);
    for (int x = 8; x < 440; ++x) { hash ^= row[x]; hash *= UINT64_C(1099511628211); }
  }
  const uint32_t *flag_row = (const uint32_t *)((const uint8_t *)surface->pixels +
                                              (8 + 7 * 4) * surface->pitch);
  CHECK(flag_row[8 + 20 * 4] == 0xffff0000u);          /* US red stripe */
  CHECK(flag_row[8 + 112 + 11 * 4] == 0xffff0000u);    /* Japanese disc */
  CHECK(flag_row[8 + 112 + 3 * 4] == 0xffffffffu);     /* Japanese field */
  CHECK(flag_row[8 + 224 + 2 * 4] == 0xff3152a4u);     /* European field */
  if (capture) {
    const char *preview = getenv("AR_OVERLAY_REGION_BADGES_BMP");
    if (preview && *preview) CHECK(SDL_SaveBMP(surface, preview));
  }
  return hash;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--dump-layer-help")) {
    s_dump_catalog = true;
    CheckLayerHelpCatalog();
    return 0;
  }
  char settings_path[160];
  char settings_temporary[164];
  snprintf(settings_path, sizeof(settings_path),
           "/tmp/actraiser-overlay-settings-%ld.ini", (long)getpid());
  snprintf(settings_temporary, sizeof(settings_temporary), "%s.tmp",
           settings_path);
  setenv("AR_OVERLAY_TEST_SETTINGS_PATH", settings_path, 1);
  remove(settings_path);
  remove(settings_temporary);
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);

  DisplayGeometry_SetHorizontal(43, 43);
  Settings_ClearConfigLayer();
  Settings_Init();
  CheckMenuDeviceGateTruthTable();
  Settings_SetActionObserver(ActionObserved);
  /* Most of this test drives every tab and row, including the developer-only
   * ones (the town Light/Weather dials, the inspector). Turn debug settings on
   * so they are all present; a dedicated block below toggles it back off and
   * checks that they collapse. */
  g_settings.show_debug_settings = true;
  SettingsOverlay_Refresh();

  int surface_width = 640;
  int surface_height = 480;
  const char *preview_size = getenv("AR_OVERLAY_TEST_SIZE");
  if (preview_size)
    (void)sscanf(preview_size, "%dx%d", &surface_width, &surface_height);
  if (surface_width <= 0) surface_width = 640;
  if (surface_height <= 0) surface_height = 480;

  CHECK(SDL_Init(SDL_INIT_VIDEO));
  SDL_Surface *surface = SDL_CreateSurface(
      surface_width, surface_height, SDL_PIXELFORMAT_ARGB8888);
  CHECK(surface != NULL);
  SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
  CHECK(renderer != NULL);
  ArRenderDevice render_device = {0};
  ArSdlRenderBackend render_backend = {0};
  CHECK(ArSdlRenderBackend_Bind(
      &render_device, &render_backend, renderer));
  size_t rom_size = 0;
  uint8_t *rom_data = ReadOptionalRom(&rom_size);
  CHECK(SettingsOverlay_Init(&render_device, NULL, rom_data, rom_size));
  const uint64_t region_badges = CheckRegionBadges(&render_device, renderer, surface, false);
  SettingsOverlay_SetManualHooks(&kFakeManualHooks);
  /* Device-reset recovery: rebuild every atlas in place. All rendering below
   * runs against the REBUILT textures, so a broken reload shows up in the
   * preview captures too. */
  CHECK(SettingsOverlay_ReloadTextures(rom_data, rom_size));
  CHECK(CheckRegionBadges(&render_device, renderer, surface, true) == region_badges);
  SettingsOverlay_SetInspectorInfoProvider(InspectorInfo);
  free(rom_data);
  CheckPerformanceOverlay(&render_device, renderer, surface);

  /* Output-space text is shared by the manual and the FPS counter. Keep a
   * real software-renderer regression around the batched glyph path so a bad
   * index/UV layout cannot turn the performance overlay into an invisible
   * one while the ordinary per-glyph menu continues to pass. */
  if (renderer && surface) {
    CHECK(SDL_SetRenderLogicalPresentation(
        renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
    CHECK(SDL_SetRenderViewport(renderer, NULL));
    CHECK(SDL_SetRenderClipRect(renderer, NULL));
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_DrawGameText(8, 8, 2, 255, "FPS 123.4");
    CHECK(SDL_RenderPresent(renderer));
    int changed_pixels = 0;
    const int text_width = SettingsOverlay_GameTextWidth("FPS 123.4", 2);
    for (int y = 8; y < 8 + 2 * kSettingsOverlayGlyphSize; y++)
      for (int x = 8; x < 8 + text_width; x++) {
        Uint8 r = 0, g = 0, b = 0, a = 0;
        CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
        if (r != 32 || g != 24 || b != 16) changed_pixels++;
      }
    CHECK(changed_pixels > 0);

    /* Package names arrive as UTF-8 from whoever authored the pack. The
     * overlay's atlas is ASCII-only, but its measuring must still count
     * characters: a name is as many cells as a reader would count, so right
     * alignment lands correctly and truncation cannot cut a character in
     * half. Byte counting made "Francais" with a cedilla nine cells wide. */
    const int ascii = SettingsOverlay_GameTextWidth("Francais", 2);
    CHECK(SettingsOverlay_GameTextWidth("Fran\u00e7ais", 2) == ascii);
    /* Decomposed and precomposed spellings occupy the same cells. */
    CHECK(SettingsOverlay_GameTextWidth("Franc\u0327ais", 2) == ascii);
    /* Scripts with no glyphs still measure one cell per character. */
    CHECK(SettingsOverlay_GameTextWidth("\u65e5\u672c\u8a9e", 2) ==
          SettingsOverlay_GameTextWidth("abc", 2));
    /* Malformed input must not run past the terminator or hang. */
    /* A bad byte costs one cell and never swallows the rest of the name. */
    const char malformed[] = {'a', (char)0xff, (char)0xfe, 'b', 0};
    CHECK(SettingsOverlay_GameTextWidth(malformed, 2) ==
          SettingsOverlay_GameTextWidth("abcd", 2));
    CHECK(SettingsOverlay_GameTextWidth("", 2) == 0);
    SettingsOverlay_DrawGameText(8, 40, 2, 255, "Fran\u00e7ais \u65e5\u672c");
#ifdef AR_OVERLAY_UI_FONT
    ArTextBackend ui_backend;
    ArSdlTextBackend_Init(&ui_backend);
    const ArFontResourceId ui_fallbacks[] = {
        ArHostFontResources_RegisterFile(&s_font_store, AR_OVERLAY_UI_JP_FONT, NULL, 0),
        ArHostFontResources_RegisterFile(&s_font_store, AR_OVERLAY_UI_AR_FONT, NULL, 0),
        ArHostFontResources_RegisterFile(&s_font_store, AR_OVERLAY_UI_HE_FONT, NULL, 0)};
    const ArTextBackendConfig ui_fonts = {.struct_size = sizeof(ui_fonts),
        .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
        .font_stack_id = "test-interface",
        .resources = ArHostFontResources_Provider(&s_font_store),
        .primary_font = ArHostFontResources_RegisterFile(&s_font_store, AR_OVERLAY_UI_FONT, NULL, 0),
        .fallback_fonts = ui_fallbacks,
        .fallback_font_count = sizeof(ui_fallbacks) / sizeof(ui_fallbacks[0]),
        .font_revision = 2, .cached_size_capacity = 16};
    char ui_error[kArTextRasterErrorCapacity] = {0};
    bool ui_ready = SettingsOverlay_SetTextBackend(&ui_backend, &ui_fonts, ui_error, sizeof(ui_error));
    if (!ui_ready) fprintf(stderr, "interface font: %s\n", ui_error);
    CHECK(ui_ready);
    /* Metadata must be readable before any game pack is activated. Exercise
     * the same trusted host stack, with no selected-pack font resources. */
    ArTextBackendInstance metadata_font = {0};
    CHECK(ArTextBackendInstance_Create(&metadata_font, &ui_backend, &ui_fonts,
                                      ui_error, sizeof(ui_error)));
    const ArTextRasterizer *metadata_rasterizer = ArTextBackendInstance_Get(&metadata_font);
    const char *metadata_names[] = {
        "العَرَبِيَّة (AR 123)", "עִבְרִית (HE 123)", "فارسی", "اردو ٹ ڈ ڑ ں ھ ے"};
    for (size_t name = 0; name < sizeof(metadata_names)/sizeof(metadata_names[0]); ++name) {
      const char *text = metadata_names[name];
      for (size_t i = 0, bytes = strlen(text); i < bytes;) {
        uint32_t scalar = 0;
        if (!ArUnicode_DecodeScalar(text, bytes, i, &scalar, &i)) { CHECK(false); break; }
        bool present = false;
        CHECK(ArTextRasterizer_HasGlyph(metadata_rasterizer, scalar, &present,
                                        ui_error, sizeof(ui_error)) && present);
      }
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      CHECK(SDL_RenderClear(renderer));
      SettingsOverlay_DrawGameText(9, 41, 3, 255, text);
      CHECK(SDL_RenderPresent(renderer));
      size_t ink = 0;
      const int width = SettingsOverlay_GameTextWidth(text, 3);
      CHECK(width > 0);
      for (int y = 0; y < surface->h; ++y) {
        const uint32_t *pixels = (const uint32_t *)((const uint8_t *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < surface->w; ++x) {
          if (pixels[x] == UINT32_C(0xff201810)) continue;
          ++ink;
          CHECK(x >= 9 && x < 9 + width && y >= 41 && y < 65);
        }
      }
      CHECK(ink > 30);
    }
    ArTextBackendInstance_Destroy(&metadata_font);
    /* The real host font must draw more than the old replacement marks. Use
     * non-tile-aligned output coordinates to catch accidental integer division
     * when bridging the overlay's logical and output-pixel drawing APIs. */
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_DrawGameText(9, 41, 3, 255, "Fran?ais ???");
    CHECK(SDL_RenderPresent(renderer));
    size_t reference_size = (size_t)surface->pitch * (size_t)surface->h;
    void *reference = malloc(reference_size);
    CHECK(reference != NULL);
    if (reference) memcpy(reference, surface->pixels, reference_size);
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_DrawGameText(9, 41, 3, 255, "Français 日本語");
    CHECK(SDL_RenderPresent(renderer));
    if (reference) CHECK(memcmp(reference, surface->pixels, reference_size) != 0);
    const int unicode_width = SettingsOverlay_GameTextWidth("Français 日本語", 3);
    CHECK(unicode_width > 0 && unicode_width < SettingsOverlay_GameTextWidth("Fran?ais ???", 3));
    CHECK(SettingsOverlay_GameTextWidth("Français", 3) == SettingsOverlay_GameTextWidth("Franc\u0327ais", 3));
    for (int y = 0; y < surface->h; ++y) {
      const uint32_t *pixels = (const uint32_t *)((const uint8_t *)surface->pixels + y * surface->pitch);
      for (int x = 0; x < surface->w; ++x)
        if (x < 9 || x >= 9 + unicode_width || y < 41 || y >= 65)
          CHECK(pixels[x] == UINT32_C(0xff201810));
    }
    const char *unicode_preview = getenv("AR_OVERLAY_UNICODE_TEST_BMP");
    if (unicode_preview && unicode_preview[0]) CHECK(SDL_SaveBMP(surface, unicode_preview));
    if (reference) memcpy(reference, surface->pixels, reference_size);
    size_t reset_rom_size = 0;
    uint8_t *reset_rom = ReadOptionalRom(&reset_rom_size);
    CHECK(SettingsOverlay_ReloadTextures(reset_rom, reset_rom_size));
    free(reset_rom);
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_DrawGameText(9, 41, 3, 255, "Français 日本語");
    CHECK(SDL_RenderPresent(renderer));
    if (reference) CHECK(memcmp(reference, surface->pixels, reference_size) == 0);
    free(reference);
#endif

    /* PiP reuses the same native frame atlas at output coordinates. Exercise
     * the checked draw seam with exact scaled-tile dimensions; it must render
     * both with a real ROM atlas and with the normal host-frame fallback used
     * by this test when AR_OVERLAY_TEST_ROM is absent. */
    const ArRenderRectI comparison_frame = { 80, 56, 176, 144 };
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    CHECK(SDL_RenderClear(renderer));
    CHECK(SettingsOverlay_DrawGameFrame(comparison_frame, 2));
    CHECK(SDL_RenderPresent(renderer));
    changed_pixels = 0;
    for (int y = comparison_frame.y;
         y < comparison_frame.y + comparison_frame.h; y++)
      for (int x = comparison_frame.x;
           x < comparison_frame.x + comparison_frame.w; x++) {
        Uint8 r = 0, g = 0, b = 0, a = 0;
        CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
        if (r != 32 || g != 24 || b != 16) changed_pixels++;
      }
    CHECK(changed_pixels > 0);
    const char *frame_preview =
        getenv("AR_OVERLAY_GAME_FRAME_TEST_BMP");
    if (frame_preview && frame_preview[0])
      CHECK(SDL_SaveBMP(surface, frame_preview));
  }

  /* Headless SDL reports no refresh rate; let a preview inject one so the
   * "Vsync NHz" row can be eyeballed. */
  const char *refresh_hz = getenv("AR_OVERLAY_TEST_REFRESH_HZ");
  if (refresh_hz && refresh_hz[0]) {
    HostDisplayStatus_SetNominalRefreshHz(atoi(refresh_hz));
    HostDisplayStatus_SetVsyncActive(true);
  }

  SettingsOverlay_Open();
  CHECK(SettingsOverlay_IsOpen());
  CheckManualSectionAvailability();
  CheckInterfaceCatalogs(renderer, surface);
  CheckDecisions(renderer, surface);
  if (renderer) {
    /* Fullscreen 4:3 leaves SDL's game presentation pillarboxed on a wide
     * output. The overlay is terminal host UI: it discards that coordinate
     * space and game-local clip, draws into the bars, and deliberately leaves
     * full-output coordinates active for the host UI that follows it. */
    CHECK(SDL_SetRenderLogicalPresentation(
        renderer, 1024, 768, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    SDL_Rect game_clip = { 0, 0, 1024, 768 };
    CHECK(SDL_SetRenderClipRect(renderer, &game_clip));
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    int logical_width = -1, logical_height = -1;
    SDL_RendererLogicalPresentation logical_mode =
        SDL_LOGICAL_PRESENTATION_LETTERBOX;
    CHECK(SDL_GetRenderLogicalPresentation(
        renderer, &logical_width, &logical_height, &logical_mode));
    CHECK(logical_width == 0 && logical_height == 0);
    CHECK(logical_mode == SDL_LOGICAL_PRESENTATION_DISABLED);
    CHECK(!SDL_RenderViewportSet(renderer));
    SDL_Rect overlay_viewport = { -1, -1, -1, -1 };
    CHECK(SDL_GetRenderViewport(renderer, &overlay_viewport));
    const SDL_Rect full_output = {0, 0, surface_width, surface_height};
    CHECK(SDL_RectsEqual(&overlay_viewport, &full_output));
    CHECK(!SDL_RenderClipEnabled(renderer));
    SDL_RenderPresent(renderer);
    /* On a wide target, surviving pixels in the left pillar prove this was a
     * real full-output draw rather than state bookkeeping alone. */
    const int content_width = surface_height * 4 / 3;
    const int pillar_width = (surface_width - content_width) / 2;
    if (pillar_width > 0 && surface) {
      int changed = 0;
      for (int y = 0; y < surface_height && changed == 0; y++) {
        for (int x = 0; x < pillar_width; x++) {
          Uint8 r = 0, g = 0, b = 0, a = 0;
          CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
          if (r != 32 || g != 24 || b != 16) {
            changed++;
            break;
          }
        }
      }
      CHECK(changed > 0);
    }
    const char *preview = getenv("AR_OVERLAY_TEST_BMP");
    if (preview && preview[0]) CHECK(SDL_SaveBMP(surface, preview));
    /* Scroll to the Refresh rate row and capture it to eyeball the Hz label. */
    const char *rpreview = getenv("AR_OVERLAY_TEST_REFRESH_BMP");
    if (rpreview && rpreview[0]) {
      NavToSection(kSection_Video);
      NavToTab(0);
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      RowToKey("refresh_mode");
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render(
          (ArRenderRectI){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, rpreview));
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
  }

  /* Tab cycling follows the player's OWN SNES L/R keyboard bindings, not
   * hardcoded keys. With the defaults (L=Q, R=W) both directions must work
   * from the nav column — the R half of this regressed once because the
   * overlay guessed Q/E instead of consulting the bindings. */
  NavToSection(kSection_Video);
  {
    int start = -1, count = -1;
    CHECK(SettingsOverlay_GetTabState(&start, &count));
    CHECK(count >= 2);
    CHECK(SettingsOverlay_HandleKey(SDLK_W, true, false));  /* SNES R */
    int after_r = -1;
    CHECK(SettingsOverlay_GetTabState(&after_r, NULL));
    CHECK(after_r == (start + 1) % count);
    CHECK(SettingsOverlay_HandleKey(SDLK_Q, true, false));  /* SNES L */
    int after_l = -1;
    CHECK(SettingsOverlay_GetTabState(&after_l, NULL));
    CHECK(after_l == start);
  }

  /* The overlay opens on primary navigation. B enters the selected section;
   * only then do Up/Down select rows and Left/Right edit values. */
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("hud_scale_percent");
  const SettingDesc *hud_scale = Settings_Find("hud_scale_percent");
  CHECK(hud_scale != NULL);
  if (hud_scale) {
    CHECK(hud_scale->maxval == 400);
    CHECK(Settings_SetLong(hud_scale, hud_scale->maxval) >=
          kSettingChange_Unchanged);
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(g_settings.hud_scale_percent == 0);
    SettingsOverlay_TickAtForTest(SDL_GetTicks() + 5000);
    CHECK(g_settings.hud_scale_percent == 0);  /* held Right stops at wrap */
    char formatted[32];
    CHECK(Settings_FormatValue(hud_scale, formatted, sizeof(formatted)) > 0);
    CHECK(!strcmp(formatted, "Match game"));
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));
  }
  RowToKey("menu_scale_percent");
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "menu_scale_percent"));
  /* menu_scale is an Int row: the press applies the step live, and releasing
   * the key flushes the deferred settings.ini write (numeric rows no longer
   * open a text editor). */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  int auto_x = surface_width * 100 / 464;
  int auto_y = surface_height * 100 / 208;
  int auto_scale = auto_x < auto_y ? auto_x : auto_y;
  auto_scale = auto_scale / 25 * 25;
  if (auto_scale < 25) auto_scale = 25;
  if (auto_scale > 800) auto_scale = 800;
  int expected_scale = auto_scale < 800 ? auto_scale + 25 : 800;
  CHECK(g_settings.menu_scale_percent == expected_scale);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release flushes */
  FILE *saved = fopen(settings_path, "rb");
  CHECK(saved != NULL);
  if (saved) fclose(saved);

  /* Aspect rows share the Video section's General tab. */
  RowToKey("extended_aspect");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_169);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_1610);

  /* Widescreen is now a TAB of Video rather than its own nav entry, and
   * switching tabs must swap the row list without leaving the section.
   * Index 3: Video's tabs are General, Effects, CRT, Widescreen — this index
   * moves whenever a tab is inserted before it. */
  NavToTab(3);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "ws_action"));
  NavToTab(0);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "display_mode"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Drive the Town 3D section through the same key path a user does. This
   * guards both the master toggle and the A/B stage selectors against
   * becoming display-only rows. */
  NavToSection(kSection_Town3D);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_mode"));
  CHECK(!g_settings.sim3d_mode);
  CHECK(!g_settings.sim3d_world_navigation);
  CHECK(g_settings.sim3d_world_navigation_lighting);
  CHECK(g_settings.sim3d_world_navigation_clouds);
  CHECK(g_settings.sim3d_world_navigation_cloud_shadows);
  CHECK(g_settings.sim3d_world_navigation_atmosphere);
  CHECK(g_settings.sim3d_world_navigation_towns);
  CHECK(g_settings.sim3d_world_navigation_relief);
  CHECK(g_settings.sim3d_world_navigation_ground_detail);
  CHECK(g_settings.sim3d_world_navigation_mountains);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_mode);
  RowToKey("sim3d_voxel_preset");
  CHECK(g_settings.sim3d_voxel_preset ==
        kSimBackgroundVoxelPreset_Balanced);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_voxel_preset ==
        kSimBackgroundVoxelPreset_Custom);
#if AR_SIM3D_TERRAIN_ELEVATION
  RowToKey("sim3d_landscape_height_pct");
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct -
            kSimTownTerrainLandscapeHeightStepPct);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct);
#endif
  RowToKey("sim3d_voxel_detail");
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_High);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_Ultra);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_High);
  RowToKey("sim3d_voxel_lod");
  CHECK(g_settings.sim3d_voxel_lod == kSimBackgroundVoxelLod_Adaptive);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_lod == kSimBackgroundVoxelLod_Fixed);
  RowToKey("sim3d_voxel_shading");
  CHECK(g_settings.sim3d_voxel_shading ==
        kSimBackgroundVoxelShading_MaterialAware);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_shading ==
        kSimBackgroundVoxelShading_AmbientOcclusion);
  RowToKey("sim3d_voxel_style");
  CHECK(g_settings.sim3d_voxel_style == kSimBackgroundVoxelStyle_Varied);
  RowToKey("sim3d_voxel_facing");
  CHECK(g_settings.sim3d_voxel_facing ==
        kSimBackgroundVoxelFacing_PerModel);
  RowToKey("sim3d_voxel_render_scale");
  CHECK(g_settings.sim3d_voxel_render_scale ==
        kSimBackgroundVoxelRenderScale_PixelClean);
  /* Walk to a stage toggle by key rather than counting rows: the stage list
   * grows every time a render stage lands. Toggling one from the menu must
   * also change what the renderer is asked for, since the fold is the only
   * thing standing between these rows and the frame payload. */
  RowToKey("sim3d_shadows");
  CHECK(g_settings.sim3d_shadows);
  CHECK(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!g_settings.sim3d_shadows);
  CHECK(!(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_shadows);
  /* The camera/light/weather splits are separate tabs of the same section. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_camera_mode"));
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_shadow_opacity_pct"));
  NavToTab(3);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_underlay_haze_pct"));

  /* Every tab ends with one section-scoped reset button. Town 3D spans four
   * registry categories, including hidden developer dials, and the button
   * must restore all four without touching Audio (or any other section).
   * The first confirm only arms it; the second performs and persists it. */
  const SettingDesc *sim_tilt = Settings_Find("sim3d_tilt_x_mrad");
  const SettingDesc *sim_shadow = Settings_Find("sim3d_shadow_opacity_pct");
  const SettingDesc *sim_corner = Settings_Find("sim3d_cull_corner_px");
  const SettingDesc *volume = Settings_Find("audio_master_volume");
  CHECK(Settings_SetLong(sim_tilt, sim_tilt->defval - sim_tilt->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(sim_shadow, sim_shadow->defval - sim_shadow->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(sim_corner, sim_corner->defval + sim_corner->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(volume, 75) == kSettingChange_Applied);
  CHECK(g_settings.sim3d_mode);  /* made non-default by the Scene test above */
  RowToKey("reset_section_defaults");
  const char *reset_preview = getenv("AR_OVERLAY_RESET_TEST_BMP");
  if (renderer && reset_preview && reset_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, reset_preview));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* arm */
  CHECK(g_settings.sim3d_mode);
  CHECK(g_settings.sim3d_tilt_x_mrad != sim_tilt->defval);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* confirm */
  CHECK(!g_settings.sim3d_mode);
  CHECK(g_settings.sim3d_tilt_x_mrad == sim_tilt->defval);
  CHECK(g_settings.sim3d_shadow_opacity_pct == sim_shadow->defval);
  CHECK(g_settings.sim3d_cull_corner_px == sim_corner->defval);
  CHECK(g_settings.audio_master_volume == 75);
  CHECK(Settings_Reset(volume) == kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Audio frequency is a bounded preset selector, not an arbitrary integer
   * editor. Audio starts with Enable audio, then Audio frequency. The
   * default is Auto (device-native, Hz==0 sentinel); stepping LEFT pins the
   * explicit 48 kHz preset. */
  NavToSection(kSection_Audio);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("audio_frequency");
  CHECK(g_settings.audio_frequency == kAudioFrequency_Auto);
  CHECK(Settings_AudioFrequencyHz() == 0);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Graphics API appears only once the platform offers a choice, and stepping
   * skips backends this build cannot create (Metal on a Windows-like host) in
   * both directions. The value names what Automatic is running on. */
  {
    const SettingDesc *api = Settings_Find("gpu_backend");
    Settings_SetGpuBackendsOffered((1u << kGpuBackend_Direct3D12) |
                                   (1u << kGpuBackend_Vulkan));
    NavToSection(kSection_Video);
    NavToTab(0);
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    RowToKey("gpu_backend");
    CHECK(g_settings.gpu_backend == kGpuBackend_Automatic);
    CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
    CHECK(g_settings.gpu_backend == kGpuBackend_Vulkan);
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(g_settings.gpu_backend == kGpuBackend_Automatic);
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(g_settings.gpu_backend == kGpuBackend_Direct3D12);
    CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
    CHECK(g_settings.gpu_backend == kGpuBackend_Automatic);
    char value[128];
    SettingsOverlay_LocalizedValue(kArUiLocale_English, api, value, sizeof(value));
    CHECK(!strcmp(value, "Automatic"));
    Settings_SetGpuBackendActive(kGpuBackend_Direct3D12);
    SettingsOverlay_LocalizedValue(kArUiLocale_English, api, value, sizeof(value));
    CHECK(!strcmp(value, "Automatic (Direct3D 12)"));
    for (int locale = 0; locale < kArUiLocale_Count; ++locale) {
      SettingsOverlay_LocalizedValue((ArUiLocale)locale, api, value, sizeof(value));
      CHECK(strstr(value, "Direct3D 12") != NULL);
    }
    CHECK(Settings_SetText(api, "Direct3D 12") == kSettingChange_RestartPending);
    SettingsOverlay_LocalizedValue(kArUiLocale_English, api, value, sizeof(value));
    CHECK(!strcmp(value, "Direct3D 12"));
    CHECK(Settings_Reset(api) == kSettingChange_RestartPending);
    Settings_SetGpuBackendActive(kGpuBackend_Automatic);
    Settings_SetGpuBackendsOffered(0);
    CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  }

  /* Hold-to-accelerate lives on Camera sensitivity: a wide 10..400 Int with a
   * base step of 1, so the ramp is actually visible (unlike a 0..100 row whose
   * coarse step would equal its base). A tap steps by one and — proven by the
   * value moving at all — never opens a text editor (BeginEditing would leave
   * the value untouched). Holding then releasing flushes one deferred save. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  const SettingDesc *sens = Settings_Find("input_cam_sensitivity");
  CHECK(sens && sens->type == kSettingType_Int && sens->step == 1);
  int sens_default = g_settings.input_cam_sensitivity;
  CHECK(Settings_SetLong(sens, 150) >= kSettingChange_Applied);  /* != default */
  RowToKey("input_cam_sensitivity");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));   /* tap down */
  CHECK(g_settings.input_cam_sensitivity == 149);  /* stepped, not text-edited */
  CHECK(!SettingsOverlay_IsEditing());            /* numeric never opens a field */
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, false, false));  /* release */
  /* Confirm (B) on a numeric row is a single fine step up, still no editor. */
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.input_cam_sensitivity == 150);
  CHECK(!SettingsOverlay_IsEditing());

  /* The pure ramp: a fresh hold moves one base step, a long hold moves more. */
  CHECK(SettingsOverlay_HoldStepForTest(sens, 0) == 1);
  CHECK(SettingsOverlay_HoldStepForTest(sens, 4000) > 1);

  /* Drive the tick with an injected clock so acceleration is deterministic:
   * press up, let the initial delay pass, then repeats well past the ramp knee
   * move the value by far more than a tap — and it clamps to the range. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* press up */
  int after_press = g_settings.input_cam_sensitivity;
  CHECK(after_press == 151);  /* one base step up from 150 */
  uint64_t base = SDL_GetTicks();
  for (int i = 1; i <= 40; i++)
    SettingsOverlay_TickAtForTest(base + (uint64_t)i * 60);
  CHECK(g_settings.input_cam_sensitivity > after_press + 5);  /* accelerated */
  CHECK(g_settings.input_cam_sensitivity <= 400);  /* normalized to range */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release */
  CHECK(Settings_SetLong(sens, sens_default) >= kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The genuine string holdouts still open a text field: pins are arbitrary
   * PAR codes. Confirm B enters editing, and Escape leaves it without a value
   * change — this is the one path numeric rows deliberately no longer use. */
  NavToSection(kSection_Cheats);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("pins");
  CHECK(Settings_Find("pins")->type == kSettingType_Custom);
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* B opens the field */
  CHECK(SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_IsOpen());
  /* Exercise the actual input wiring, not just the standalone Unicode helper.
   * Delete four user-perceived characters, then commit an ordinary valid pin.
   * Byte-wise deletion leaves debris and prevents that final value parsing. */
  const SettingDesc *pins = Settings_Find("pins");
  CHECK(Settings_SetText(pins, "") != kSettingChange_Rejected);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleText("e\u0301日本👩🏽‍💻"));
  for (int i = 0; i < 4; ++i)
    CHECK(SettingsOverlay_HandleKey(SDLK_BACKSPACE, true, false));
  CHECK(SettingsOverlay_HandleText("bad\xff")); /* Rejected atomically. */
  CHECK(SettingsOverlay_HandleText("7E00210A"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(g_settings.pin_count == 1 && g_settings.pins[0].off == 0x21 &&
        g_settings.pins[0].val == 0x0a);
  CHECK(Settings_Reset(pins) >= kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Controls: the Devices tab holds device/analog rows, and the Keyboard and
   * Gamepad tabs are the two binding pages — the tab now drives
   * input_bind_page, which no longer lists itself as a row. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "input_device"));
  CHECK(!Settings_IsMenuVisible(Settings_Find("input_bind_page")));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_key_up"));

  const char *controls_preview = getenv("AR_OVERLAY_CONTROLS_TEST_BMP");
  if (renderer && controls_preview && controls_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, controls_preview));
  }

  /* Drive one full rebind the way a player would: select the row, arm
   * capture, and feed the raw SDL event — the capture path takes scancodes,
   * not keycodes, so it bypasses SettingsOverlay_HandleKey entirely. */
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  /* A gamepad event must not land in a keyboard row. */
  SDL_Event pad = {0};
  pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  pad.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH;
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(SettingsOverlay_IsCapturing());

  SDL_Event key = {0};
  key.type = SDL_EVENT_KEY_DOWN;
  key.key.scancode = SDL_SCANCODE_I;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));

  /* Escape aborts an armed row and leaves the old binding intact. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  key.key.scancode = SDL_SCANCODE_ESCAPE;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));
  /* Y restores the row default. */
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_UP, false));

  /* Stepping to the Gamepad tab swaps the listed rows to the gamepad set and
   * writes the page setting through, so a reopened menu agrees with it. */
  NavToTab(2);
  CHECK(g_settings.input_bind_page == kInputClass_Gamepad);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_pad_up"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_NORTH,
                        false));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  /* Read-only diagnostics must not re-synchronize a page selector behind the
   * caller's back. The explicit refresh restores the active tab's page. */
  g_settings.input_bind_page = kInputClass_Gamepad;
  const Settings before_queries = g_settings;
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_key_up"));
  CHECK(SettingsOverlay_GetTabState(NULL, NULL));
  CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, NULL));
  CHECK(memcmp(&before_queries, &g_settings, sizeof(g_settings)) == 0);
  SettingsOverlay_Refresh();
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The Save section's tabs are the Actions page plus five editor pages. The
   * backend/arming controls and apply/export commands now live only on the
   * Actions tab instead of repeating on every page. */
  NavToSection(kSection_Save);
  NavToTab(kSaveEditorPage_Actions);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "save_backend"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_backend == 1);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_edit_armed);
  CHECK(Settings_SetText(Settings_Find("save_prog_fillmore"),
                         "act2-cleared") == kSettingChange_Applied);
  const char *save_preview = getenv("AR_OVERLAY_SAVE_TEST_BMP");
  if (renderer && save_preview && save_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, save_preview));
  }

  /* Selecting a page tab writes the page setting through, and the row list
   * follows it. */
  NavToTab(kSaveEditorPage_Items);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Items);
  RowToKey("save_item_slot_1");
  NavToTab(kSaveEditorPage_Progress);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Progress);
  /* The export commands live on the Actions tab now; verify the observer path
   * still dispatches the final conversion action from there. */
  NavToTab(kSaveEditorPage_Actions);
  RowToKey("save_export_ini");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("save_export_ini"));
  s_action_calls = 0;
  s_action_desc = NULL;
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* System > Tools carries the debug-settings switch and the host commands
   * (pause, restart, exit); Restart and Exit are the last two rows of this
   * tab, no longer permanent nav-column slots. */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "show_debug_settings"));
  RowToKey("toggle_pause");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("toggle_pause"));
  RowToKey("restart_game");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 2);
  CHECK(s_action_desc == Settings_Find("restart_game"));
  RowToKey("exit_desktop");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 3);
  CHECK(s_action_desc == Settings_Find("exit_desktop"));

  /* System > Game groups original bug fixes ahead of QoL changes. Heading rows
   * are visible but never take the cursor. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "fix_aitos_event_queue"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.fix_aitos_event_queue);
  RowToKey("fix_bridge_limit");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.fix_bridge_limit);

  /* Inspector is the section's third tab: its first row makes the enabled
   * state explicit, its second dispatches the complete scene-asset dump, and
   * the remainder is supplied by the read-only live-info provider. */
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "scene_inspector"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.scene_inspector);
  if (renderer) {
    int calls_before = s_inspector_info_calls;
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    CHECK(s_inspector_info_calls == calls_before + 1);
  }
  RowToKey("dump_scene_assets");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 4);
  CHECK(s_action_desc == Settings_Find("dump_scene_assets"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Debug-settings gate: with the switch on (set at startup) the town dials,
   * their A/B toggles, and the inspector are all visible. */
  const SettingDesc *debug_row = Settings_Find("show_debug_settings");
  CHECK(debug_row && !Settings_IsDebugOnly(debug_row));  /* never hides itself */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  NavToSection(kSection_Town3D);
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 4);  /* Scene, Camera, Light, Weather */
  }

  /* Turn it off through the menu the way a player would (System > Tools). */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("show_debug_settings");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* on -> off */
  CHECK(!g_settings.show_debug_settings);

  /* The dials, internal A/B toggles, and inspector collapse out... */
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_separated_composite")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("diorama_layer_bg1")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  /* ...while master toggles, major on/off effects, and camera mode stay. */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_mode")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation")));
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_world_navigation_lighting")));
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_world_navigation_clouds")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_cloud_shadows")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_atmosphere")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_towns")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_relief")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_ground_detail")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation_mountains")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_shadows")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_camera_mode")));
#if AR_SIM3D_TERRAIN_ELEVATION
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_landscape_height_pct")));
#else
  CHECK(!Settings_IsMenuVisible(
      Settings_Find("sim3d_landscape_height_pct")));
#endif
  CHECK(Settings_IsMenuVisible(Settings_Find("diorama_skybox")));
  /* System drops the all-debug Inspector tab, leaving Tools and Game. */
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 2);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));  /* back to nav column */
  /* Town 3D collapses to Scene + Camera; cycling never lands on a hidden tab. */
  NavToSection(kSection_Town3D);
  {
    int tabs = -1, active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, &tabs));
    CHECK(tabs == 2);
    for (int i = 0; i < 6; i++) {
      CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
      CHECK(SettingsOverlay_GetTabState(&active, NULL));
      CHECK(active >= 0 && active < 2);
    }
  }
  /* Restore for the section-sweep contact sheet below. */
  g_settings.show_debug_settings = true;
  SettingsOverlay_Refresh();

  /* Every section stays reachable and its nav row stays inside the scroll
   * window, whatever the panel can fit. With AR_OVERLAY_PREVIEW_DIR set this
   * doubles as a contact sheet: one BMP per (section, tab), which is the only
   * practical way to eyeball a layout change across the whole menu. */
  const char *preview_dir = getenv("AR_OVERLAY_PREVIEW_DIR");
  /* Give the contact sheet a POPULATED layer editor. Without the hooks installed
   * every level tab renders its "enter a stage here" notice, which is the one
   * state that needs no review -- the layout worth eyeballing is a room with
   * planes, one of them expanded into its parameters. Fillmore act 2 with the
   * shipped rake on its water is the case the whole feature exists for. */
  if (preview_dir && preview_dir[0]) {
    SettingsOverlay_SetLayerEditorHooks(FakeLayerTable, FakeLayerRoom,
                                        FakeLayerSave);
    memset(&s_fake_layer_table, 0, sizeof(s_fake_layer_table));
    s_fake_room_live = true;
    DioramaRoomOverride *preview_room = DioramaLayerOrder_FindOrAdd(
        &s_fake_layer_table, s_fake_group, s_fake_map);
    if (preview_room) {
      DioramaLayerEditor_SetStrategy(&preview_room->planes[kDioramaPlane_Bg2Hi],
                                     kDioramaDepth_Stack);
      DioramaLayerEditor_SetStrategy(&preview_room->planes[SR_PPU_OVERLAY_BG1],
                                     kDioramaDepth_Rake);
    }
  }
  for (int section = 0; section < kDebugSectionCount; section++) {
    NavToSection(section);
    if (!renderer) continue;
    SettingsOverlay_Render(
        (ArRenderRectI){0, 0, surface_width, surface_height});
    int selected = -1, top = -1, visible = -1, total = -1;
    CHECK(SettingsOverlay_GetNavigationState(
        &selected, &top, &visible, &total));
    CHECK(selected == section);
    CHECK(total == kDebugSectionCount);
    CHECK(selected >= top && selected < top + visible);
    if (!preview_dir || !preview_dir[0]) continue;

    int tabs = 0;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    for (int tab = 0; tab < tabs; tab++) {
      NavToTab(tab);
      /* Park the cursor on the authored water plane so its parameter block is
       * expanded in the shot -- the expansion is the layout decision most worth
       * reviewing, and it is only visible on the selected plane. */
      if (section == kSection_Layers &&
          strcmp(SettingsOverlay_SelectedKey(), "") != 0) {
        for (int guard = 0; guard < 32; guard++) {
          if (!strcmp(SettingsOverlay_SelectedKey(), "bg2hi")) break;
          CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
        }
        /* B expands rather than edits, so the shot shows the parameters without
         * changing the shape the room was seeded with. */
        CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      }
      char path[256];
      snprintf(path, sizeof(path), "%s/section%d-tab%d.bmp",
               preview_dir, section, tab);
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render(
          (ArRenderRectI){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, path));
    }
    CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  }

  /* Second contact sheet with debug settings OFF, so a layout review can see
   * the collapsed menu players actually get (Town 3D without Light/Weather,
   * System without Inspector, the dial rows gone). */
  if (renderer && preview_dir && preview_dir[0]) {
    g_settings.show_debug_settings = false;
    SettingsOverlay_Refresh();
    for (int section = 0; section < kPlayerSectionCount; section++) {
      NavToSection(section);
      int tabs = 0;
      CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      for (int tab = 0; tab < tabs; tab++) {
        NavToTab(tab);
        char path[256];
        snprintf(path, sizeof(path), "%s/section%d-tab%d-dbgoff.bmp",
                 preview_dir, section, tab);
        SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
        SDL_RenderClear(renderer);
        SettingsOverlay_Render(
            (ArRenderRectI){0, 0, surface_width, surface_height});
        SDL_RenderPresent(renderer);
        CHECK(SDL_SaveBMP(surface, path));
      }
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
    g_settings.show_debug_settings = true;
    SettingsOverlay_Refresh();
  }

  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(!SettingsOverlay_IsOpen());
  SettingsOverlay_Open();
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsOpen());

  SettingsOverlay_Open();
  CheckLayerEditorSection();
  SettingsOverlay_Close();
  CheckRegionalControls(renderer, surface);

  /* Debug panels avoid the inspected point and can be moved without a click
   * falling through to the tool beneath them. */
  if (renderer) {
    SDL_SetRenderDrawColor(renderer, 22, 28, 34, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_RenderDebugPanel(
        "SCENE INSPECTOR",
        "CLICK 474,170  WORLD $008E,$00C6\n"
        "GF $016C STATE $00/$00 CAM $0080,$0080 MAP $0000,$0000\n"
        "PPU MODE 7 BRIGHT 15 MAIN $01 SUB $00 MARGIN 0/0\n"
        "BG1 T$03A P1 PAL3 PIX2 CENTER MAP$7104\n"
        "OBJ#12 16X16 BASE$80 SUB$91 PAL4 PRI2 PIX7\n"
        "CANDIDATES; WINDOWS/COLOR MATH MAY MASK A LAYER\n"
        "LEFT CLICK INSPECT  RIGHT CLICK CLEAR  F3 DISABLE",
        (ArRenderPointI){ surface_width / 2, surface_height - 1 });
    SDL_RenderPresent(renderer);
    const char *debug_preview = getenv("AR_OVERLAY_DEBUG_TEST_BMP");
    if (debug_preview && debug_preview[0])
      CHECK(SDL_SaveBMP(surface, debug_preview));
    ArRenderRectI panel_before = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_before));
    CHECK(panel_before.y < surface_height / 2);
    CHECK(panel_before.w < surface_width - 40);
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + panel_before.h - 4));
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_before.x + 4, surface_height / 2);
    SettingsOverlay_EndDebugPanelDrag();
    CHECK(!SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (ArRenderPointI){ surface_width / 2, surface_height - 1 });
    ArRenderRectI panel_after = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_after));
    CHECK(panel_after.y != panel_before.y);
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_after.x + panel_after.w - 4,
        panel_after.y + panel_after.h - 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_after.x + panel_after.w - 4 - panel_after.w / 4,
        panel_after.y + panel_after.h - 4 - panel_after.h / 4);
    SettingsOverlay_EndDebugPanelDrag();
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (ArRenderPointI){ surface_width / 2, surface_height - 1 });
    ArRenderRectI panel_resized = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(panel_resized.w < panel_after.w);
    CHECK(panel_resized.h < panel_after.h);
    SettingsOverlay_HideDebugPanel();
    CHECK(!SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(0, 0));
  }

  SettingsOverlay_SetManualHooks(NULL);
  SettingsOverlay_Destroy();
#ifdef AR_OVERLAY_UI_FONT
  CHECK(ArHostFontResources_Destroy(&s_font_store));
#endif
  ArRenderDevice_Reset(&render_device);
  Settings_SetActionObserver(NULL);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  SDL_Quit();
  remove(settings_path);
  remove(settings_temporary);

  if (s_failures) {
    fprintf(stderr, "settings overlay tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings overlay tests: pass\n");
  return 0;
}
