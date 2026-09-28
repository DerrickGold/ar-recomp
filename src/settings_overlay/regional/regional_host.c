#include "settings_overlay/regional/regional_host.h"

#include <stdio.h>
#include <string.h>
#include "actraiser/actraiser_rtl.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/settings.h"
#include "settings_overlay/settings_overlay.h"

static bool s_headless;

static bool WaitForDecision(void) {
  SettingsOverlayDecisionResult result;
  do {
    ActRaiser_YieldToHost();
    result = SettingsOverlay_TakeDecisionResult();
  } while (result == kOverlayDecision_Pending);
  return result == kOverlayDecision_Accepted;
}

static bool ContinuePrompt(void *context, ActRaiserRegionalContinueNotice notice) {
  const bool *headless = context;
  if (!headless || *headless) return false;
  const char *body = notice == kActRaiserRegionalContinue_Estimate
      ? "overlay.region.legacy_estimate"
      : notice == kActRaiserRegionalContinue_LoadFailed ? "overlay.region.continue_failed"
                                                        : "overlay.region.adoption_failed";
  const char *accept = notice == kActRaiserRegionalContinue_Estimate
      ? "overlay.region.acknowledge" : "overlay.decision.retry";
  if (!SettingsOverlay_BeginDecision("overlay.region.continue_title", body, accept)) return false;
  return WaitForDecision();
}

static bool PopulationPrompt(void *context, ActRaiserRegionalPopulationNotice notice,
                             ArRegionalSource source, bool gameplay_profile,
                             const uint16_t removed[6]) {
  const bool *headless = context;
  if (!headless || *headless) return false;
  bool opened;
  if (notice == kActRaiserRegionalPopulation_Confirm) {
    char body[2048];
    const ArUiLocale locale = (ArUiLocale)g_settings.interface_language;
    if (!SettingsOverlayRegions_PopulationConfirmation(locale, source, removed, body, sizeof(body)))
      return false;
    if (gameplay_profile) {
      const size_t used = strlen(body);
      const char *scope = ArUiCatalog_Text(locale, "overlay.region.menu.confirm_gameplay", NULL);
      const int written = snprintf(body + used, sizeof(body) - used, "\n\n%s", scope);
      if (written < 0 || (size_t)written >= sizeof(body) - used) return false;
    }
    opened = SettingsOverlay_BeginDecisionText("overlay.region.population_label", body,
                                              "overlay.region.population_accept");
  } else {
    const char *key = notice == kActRaiserRegionalPopulation_Complete
        ? "overlay.region.population_complete"
        : "overlay.region.population_failed";
    opened = SettingsOverlay_BeginNotice("overlay.region.population_label", key,
                                         "overlay.region.acknowledge");
  }
  return opened && WaitForDecision();
}

void SettingsOverlayRegionalHost_InstallHooks(void) {
  static const SettingsOverlayRegionalHooks hooks = {
    .copy = ActRaiserRegional_CopyRulesView,
    .request = ActRaiserRegional_RequestProfile,
    .difficulty = ActRaiserRegional_RequestDifficultyChoice,
    .setting = ActRaiserRegional_RequestRules,
    .preview_setting = ActRaiserRegional_PreviewRules,
    .preview = ActRaiserRegional_PreviewProfile,
  };
  SettingsOverlay_SetRegionalHooks(&hooks);
}

void SettingsOverlayRegionalHost_InstallPrompts(bool headless) {
  s_headless = headless;
  ActRaiserRegional_SetContinuePrompt(ContinuePrompt, &s_headless);
  ActRaiserRegional_SetPopulationPrompt(PopulationPrompt, &s_headless);
}
