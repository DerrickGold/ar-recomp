#ifndef ACTRAISER_REGIONAL_EDITOR_H
#define ACTRAISER_REGIONAL_EDITOR_H

#include "actraiser/regional/actraiser_regional_settings.h"
#include "regional/session/regional_session.h"

/* Private game-side editing boundary, not an overlay API. The runtime owns
 * session lifetime and supplies fresh replay/title authority. This module
 * owns edit validation and deferred intent, never CPU, save files or prompts.
 * Native Palace confirmation remains in actraiser_regional_runtime.c. */
typedef struct ActRaiserRegionalPopulationIntent {
  bool pending, profile;
  ArRegionalProfileGroup group;
  uint8_t campaign[16];
  ArRegionalRules base_rules;
  ArRegionalSource source;
} ActRaiserRegionalPopulationIntent;

typedef struct ActRaiserRegionalEditContext {
  ArRegionalSession *session;
  ActRaiserRegionalPopulationIntent *population;
  bool new_game, editable;
} ActRaiserRegionalEditContext;

bool ActRaiserRegionalEditor_PopulationPending(const ActRaiserRegionalPopulationIntent *intent,
                                               const ArRegionalSession *session);
void ActRaiserRegionalEditor_InvalidatePopulation(ActRaiserRegionalPopulationIntent *intent,
                                                  const ArRegionalSession *session);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_PreviewProfile(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalProfileGroup group, ArRegionalSource source, ActRaiserRegionalEditImpact *out);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestProfile(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalProfileGroup group, ArRegionalSource source);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestRules(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, ArRegionalSource source);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_PreviewRules(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, ArRegionalSource source, ActRaiserRegionalEditImpact *out);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestDifficultyChoice(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalDifficultyChoice choice);
ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestDifficulty(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalDifficulty level);
#endif
