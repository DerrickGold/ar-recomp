#ifndef AR_REGIONAL_RANDOMIZER_H
#define AR_REGIONAL_RANDOMIZER_H
#include "regional_profiles.h"
#include "randomizer_config.h"

/* New-campaign preparation only. Each selected gameplay leaf draws from its
 * own stable key, independent of inventory order and other enabled groups.
 * Population support/goals stay compatible. Does not touch histories, active
 * rooms, saves, ROM or global settings. Continue restores saved rules instead. */
bool ArRegionalRandomizer_Choose(const ArRegionalRules *base,const RandomizerConfig *config,
    ArRegionalRules *out);
#endif
