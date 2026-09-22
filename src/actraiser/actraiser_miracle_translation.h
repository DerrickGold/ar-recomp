#ifndef ACTRAISER_MIRACLE_TRANSLATION_H
#define ACTRAISER_MIRACLE_TRANSLATION_H
#include "actraiser/actraiser_dialogue_adapter.h"
#include "regional/regional_costs.h"

/* Older packs may contain fixed US costs. When rules change, use a compatible
 * enhanced fallback or the price-adjusted native text for that invocation.
 * Never rewrite a translator's prose or change the selected pack globally. */
void ActRaiserMiracle_ConstrainText(ArDialogueContentSelection *selection,
    const char *semantic_id, const ArRegionalCostSnapshot *prices);
#endif
