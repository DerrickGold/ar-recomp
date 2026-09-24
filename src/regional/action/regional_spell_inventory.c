#include "regional/action/regional_spell_inventory.h"

static const ArRegionalInventoryDescriptor kRule={"action_spell_inventory",{0,0,1}};
const ArRegionalInventoryDescriptor *ArRegionalInventory_Descriptor(void) {return &kRule;}
bool ArRegionalInventory_Resolve(ArRegionalSource source,bool *enabled) {
  if(!enabled || (unsigned)source>=kArRegionalSource_Count)return false;
  *enabled=kRule.enabled[source]!=0;return true;
}
void ArRegionalSpellInventory_Reset(ArRegionalSpellInventory *inventory,bool enabled) {
  if(inventory) {*inventory=(ArRegionalSpellInventory){0};inventory->enabled=enabled;}
}
bool ArRegionalSpellInventory_Valid(const ArRegionalSpellInventory *inventory) {
  if(!inventory || inventory->icon>4 || inventory->casting>4)return false;
  if(!inventory->enabled)return !inventory->count && !inventory->icon && !inventory->casting;
  for(unsigned i=0;i<inventory->count;++i)
    if(!inventory->spells[i] || inventory->spells[i]>4)return false;
  return true;
}
bool ArRegionalSpellInventory_Push(ArRegionalSpellInventory *inventory,unsigned spell) {
  if(!inventory || !inventory->enabled || !spell || spell>4)return false;
  inventory->spells[inventory->count++]=(uint8_t)spell;
  inventory->icon=(uint8_t)spell;
  return true;
}
bool ArRegionalSpellInventory_BeginCast(ArRegionalSpellInventory *inventory,uint8_t *spell) {
  if(!inventory || !spell || !inventory->enabled || !inventory->count || inventory->casting)return false;
  const uint8_t top=inventory->spells[inventory->count-1];
  if(!top || top>4)return false;
  inventory->casting=top;*spell=top;return true;
}
bool ArRegionalSpellInventory_FinishCast(ArRegionalSpellInventory *inventory) {
  if(!inventory || !inventory->enabled || !inventory->casting)return false;
  /* Payment follows the native effect and removes the then-current top.
   * A pickup during a cast changes the collection, not the selected effect. */
  /* A 256th pickup can wrap the byte count while an effect is pending. The
   * PAL word debit would underflow and read outside the collection. Keep that
   * exceptional empty result bounded rather than fabricate another spell. */
  if(inventory->count)--inventory->count;
  inventory->icon=inventory->count?inventory->spells[inventory->count-1]:0;
  inventory->casting=0;
  return true;
}
void ArRegionalSpellInventory_Interrupt(ArRegionalSpellInventory *inventory) {
  if(inventory)inventory->casting=0;
}
uint8_t ArRegionalSpellInventory_PickupSpell(unsigned item) {
  static const uint8_t spells[8]={1,0,2,0,3,0,0,4};
  return item<8?spells[item]:0;
}
uint8_t ArRegionalSpellInventory_GrowHealth(uint8_t health) {
  return health<24?health+1:health;
}
