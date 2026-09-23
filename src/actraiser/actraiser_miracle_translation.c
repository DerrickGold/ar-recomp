#include "actraiser/actraiser_miracle_translation.h"
#include <string.h>

static bool HasPrice(const ArLanguagePack *pack, const char *id, const char *token) {
  const ArLanguageMessage *m = ArLanguagePack_FindMessage(pack,id);
  for (uint32_t depth=0; m && m->is_alias && depth<ArLanguagePack_MessageCount(pack); ++depth)
    m=ArLanguagePack_FindMessage(pack,ArLanguagePack_GetString(pack,m->alias));
  if (!m || m->is_alias) return false;
  for (uint32_t i=0;i<m->operation_count;++i) {
    const ArLanguageOperation *op=ArLanguagePack_GetOperation(pack,m,i);
    if (op && op->kind==kArLanguageOperation_Placeholder) {
      const char *name=ArLanguagePack_GetString(pack,op->value.placeholder);
      if (name && !strcmp(name,token)) return true;
    }
  }
  return false;
}

void ActRaiserMiracle_ConstrainText(ArDialogueContentSelection *selection,
    const char *id, const ArRegionalCostSnapshot *prices) {
  if (!selection || !id || !prices || selection->presentation!=kArDialoguePresentation_Enhanced) return;
  static const struct { const char *id, *token; ArRegionalCostRule rule; } fields[]={
    {"sim.miracle.lightning.insufficient_sp","miracle_lightning_sp",kArRegionalCost_Lightning},
    {"sim.miracle.rain.insufficient_sp","miracle_rain_sp",kArRegionalCost_Rain},
    {"sim.miracle.sun.insufficient_sp","miracle_sunlight_sp",kArRegionalCost_Sunlight},
    {"sim.miracle.wind.insufficient_sp","miracle_wind_sp",kArRegionalCost_Wind},
    {"sim.miracle.earthquake.insufficient_sp","miracle_earthquake_sp",kArRegionalCost_Earthquake},
  };
  for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
    if (strcmp(id,fields[i].id)) continue;
    if (prices->price[fields[i].rule]==ArRegionalCosts_Descriptor(fields[i].rule)->price[kArRegionalSource_US]) return;
    if (HasPrice(selection->selected_pack,id,fields[i].token)) return;
    selection->selected_pack=NULL;
    if (HasPrice(selection->native_us_enhanced_pack,id,fields[i].token)) return;
    selection->native_us_enhanced_pack=NULL;
    selection->presentation=kArDialoguePresentation_NativeRetail;
    return;
  }
}
