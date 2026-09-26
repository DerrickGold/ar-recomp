#include "settings_overlay/regional/regional_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "actraiser/actraiser_rtl.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/settings.h"
#include "settings_overlay/settings_overlay.h"

Settings g_settings;
static ActRaiserRegionalContinuePrompt continue_prompt;
static ActRaiserRegionalPopulationPrompt population_prompt;
static void *continue_context, *population_context;
static SettingsOverlayRegionalHooks hooks;
static bool open_ok, format_ok;
static int opened, yielded, taken, pending;
static size_t body_length;
static SettingsOverlayDecisionResult terminal;
static const char *title_key, *body_key, *accept_key;
static char decision_body[2048];
static const uint16_t removed[6] = {1, 2, 3, 4, 5, 6};

void ActRaiserRegional_SetContinuePrompt(ActRaiserRegionalContinuePrompt fn, void *context) {
  continue_prompt = fn;
  continue_context = context;
}
void ActRaiserRegional_SetPopulationPrompt(ActRaiserRegionalPopulationPrompt fn, void *context) {
  population_prompt = fn;
  population_context = context;
}
void SettingsOverlay_SetRegionalHooks(const SettingsOverlayRegionalHooks *value) { hooks = *value; }
void ActRaiser_YieldToHost(void) {
  assert(opened == 1 && open_ok && yielded == taken);
  yielded++;
}
SettingsOverlayDecisionResult SettingsOverlay_TakeDecisionResult(void) {
  assert(yielded == taken + 1);
  taken++;
  return pending-- > 0 ? kOverlayDecision_Pending : terminal;
}
bool SettingsOverlay_BeginDecision(const char *title, const char *body, const char *accept) {
  opened++;
  title_key = title;
  body_key = body;
  accept_key = accept;
  return open_ok;
}
bool SettingsOverlay_BeginDecisionText(const char *title, const char *body, const char *accept) {
  snprintf(decision_body, sizeof(decision_body), "%s", body);
  return SettingsOverlay_BeginDecision(title, NULL, accept);
}
bool SettingsOverlay_BeginNotice(const char *title, const char *body, const char *accept) {
  return SettingsOverlay_BeginDecision(title, body, accept);
}
bool SettingsOverlayRegions_PopulationConfirmation(ArUiLocale locale, ArRegionalSource source,
                                                   const uint16_t counts[6], char *out,
                                                   size_t capacity) {
  assert(locale == (ArUiLocale)g_settings.interface_language);
  assert(source == kArRegionalSource_Japan && counts == removed && body_length < capacity);
  memset(out, 'x', body_length);
  out[body_length] = '\0';
  return format_ok;
}
const char *ArUiCatalog_Text(ArUiLocale locale, const char *key, const char *fallback) {
  assert(locale == (ArUiLocale)g_settings.interface_language && !fallback);
  assert(!strcmp(key, "overlay.region.menu.confirm_gameplay"));
  return "Gameplay scope";
}

/* The adapter only installs these: rule validation remains the campaign's job. */
bool ActRaiserRegional_CopyRulesView(ActRaiserRegionalRulesView *out) { (void)out; return false; }
ActRaiserRegionalEditResult ActRaiserRegional_RequestProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group, ArRegionalSource source) {
  (void)view;
  (void)group;
  (void)source;
  return 0;
}
ActRaiserRegionalEditResult ActRaiserRegional_RequestRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source) {
  (void)view;
  (void)group;
  (void)source;
  return 0;
}
ActRaiserRegionalEditResult ActRaiserRegional_RequestDifficultyChoice(
    const ActRaiserRegionalRulesView *view, ArRegionalDifficultyChoice choice) {
  (void)view;
  (void)choice;
  return 0;
}
ActRaiserRegionalEditResult ActRaiserRegional_PreviewProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group, ArRegionalSource source,
    ActRaiserRegionalEditImpact *out) {
  (void)out;
  return ActRaiserRegional_RequestProfile(view, group, source);
}
ActRaiserRegionalEditResult ActRaiserRegional_PreviewRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source, ActRaiserRegionalEditImpact *out) {
  (void)out;
  return ActRaiserRegional_RequestRules(view, group, source);
}

static void Reset(void) {
  open_ok = format_ok = true;
  opened = yielded = taken = pending = 0;
  body_length = 3;
  terminal = kOverlayDecision_Accepted;
  decision_body[0] = '\0';
  g_settings.interface_language = kArUiLocale_Japanese;
}
static bool Population(ActRaiserRegionalPopulationNotice notice, bool gameplay) {
  return population_prompt(population_context, notice, kArRegionalSource_Japan, gameplay, removed);
}

int main(void) {
  SettingsOverlayRegionalHost_InstallHooks();
  assert(hooks.copy == ActRaiserRegional_CopyRulesView);
  assert(hooks.request == ActRaiserRegional_RequestProfile);
  assert(hooks.setting == ActRaiserRegional_RequestRules);
  assert(hooks.difficulty == ActRaiserRegional_RequestDifficultyChoice);
  assert(hooks.preview == ActRaiserRegional_PreviewProfile);
  assert(hooks.preview_setting == ActRaiserRegional_PreviewRules);

  Reset();
  SettingsOverlayRegionalHost_InstallPrompts(true);
  assert(!continue_prompt(continue_context, kActRaiserRegionalContinue_Estimate));
  assert(!Population(kActRaiserRegionalPopulation_Confirm, true));
  assert(!opened && !yielded);
  SettingsOverlayRegionalHost_InstallPrompts(false);
  assert(!continue_prompt(NULL, kActRaiserRegionalContinue_Estimate));
  assert(!population_prompt(NULL, kActRaiserRegionalPopulation_Confirm,
                           kArRegionalSource_Japan, false, removed));
  assert(!opened && !yielded);

  const char *continue_keys[] = {
    "overlay.region.legacy_estimate", "overlay.region.continue_failed",
    "overlay.region.adoption_failed",
  };
  const ActRaiserRegionalContinueNotice notices[] = {
    kActRaiserRegionalContinue_Estimate, kActRaiserRegionalContinue_LoadFailed,
    kActRaiserRegionalContinue_SaveFailed,
  };
  for (size_t i = 0; i < 3; i++) {
    for (int outcome = kOverlayDecision_None; outcome <= kOverlayDecision_Cancelled; outcome++) {
      if (outcome == kOverlayDecision_Pending) continue;
      Reset();
      pending = 2;
      terminal = (SettingsOverlayDecisionResult)outcome;
      assert(continue_prompt(continue_context, notices[i]) ==
             (terminal == kOverlayDecision_Accepted));
      assert(yielded == 3 && taken == 3);
      assert(!strcmp(title_key, "overlay.region.continue_title"));
      assert(!strcmp(body_key, continue_keys[i]));
      assert(!strcmp(accept_key, i ? "overlay.decision.retry" : "overlay.region.acknowledge"));
    }
  }
  Reset();
  open_ok = false;
  assert(!continue_prompt(continue_context, notices[0]) && !yielded);
  Reset();
  pending = 1;
  assert(Population(kActRaiserRegionalPopulation_Confirm, true) && yielded == 2);
  assert(!strcmp(decision_body, "xxx\n\nGameplay scope"));
  assert(!strcmp(title_key, "overlay.region.population_label"));
  assert(!strcmp(accept_key, "overlay.region.population_accept"));
  Reset();
  terminal = kOverlayDecision_Cancelled;
  assert(!Population(kActRaiserRegionalPopulation_Confirm, false));
  assert(!strcmp(decision_body, "xxx") && yielded == 1);
  Reset();
  format_ok = false;
  assert(!Population(kActRaiserRegionalPopulation_Confirm, true) && !opened && !yielded);
  Reset();
  body_length = sizeof(decision_body) - 1;
  assert(!Population(kActRaiserRegionalPopulation_Confirm, true) && !opened && !yielded);
  Reset();
  open_ok = false;
  assert(!Population(kActRaiserRegionalPopulation_Confirm, false) && !yielded);
  const char *population_keys[] = {
    "overlay.region.population_failed", "overlay.region.population_complete",
    "overlay.region.population_name_pending",
  };
  for (int i = 0; i < 3; i++) {
    Reset();
    assert(Population((ActRaiserRegionalPopulationNotice)(i + 1), false));
    assert(!strcmp(body_key, population_keys[i]));
    assert(!strcmp(accept_key, "overlay.region.acknowledge") && yielded == 1);
  }
  puts("Regional dialogs: headless refusal, localization, cancellation and host yielding passed");
  return 0;
}
