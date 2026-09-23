#include "regional/regional_spell_inventory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  ArRegionalSpellInventory inv;
  for(unsigned source=0;source<3;++source) {
    bool enabled;assert(ArRegionalInventory_Resolve(source,&enabled) && enabled==(source==2));
    ArRegionalSpellInventory_Reset(&inv,enabled);assert(ArRegionalSpellInventory_Valid(&inv));
    if(!enabled)assert(!ArRegionalSpellInventory_Push(&inv,1));
  }
  for(unsigned depth=0;depth<256;++depth)for(unsigned spell=1;spell<=4;++spell) {
    ArRegionalSpellInventory_Reset(&inv,true);
    for(unsigned i=0;i<depth;++i)assert(ArRegionalSpellInventory_Push(&inv,(i%4)+1));
    ArRegionalSpellInventory before=inv;
    assert(ArRegionalSpellInventory_Push(&inv,spell));
    assert(inv.count==(uint8_t)(depth+1) && inv.icon==spell && inv.spells[depth]==spell);
    assert(!memcmp(inv.spells,before.spells,depth));
    assert(ArRegionalSpellInventory_Valid(&inv));
    uint8_t selected=0;
    if(depth==255) {assert(!ArRegionalSpellInventory_BeginCast(&inv,&selected));continue;}
    assert(ArRegionalSpellInventory_BeginCast(&inv,&selected) && selected==spell);
    assert(inv.count==depth+1 && !ArRegionalSpellInventory_BeginCast(&inv,&selected));
    ArRegionalSpellInventory_Interrupt(&inv);
    assert(inv.count==depth+1 && !inv.casting && inv.icon==spell);
    assert(!ArRegionalSpellInventory_FinishCast(&inv));
    assert(ArRegionalSpellInventory_BeginCast(&inv,&selected));
    assert(ArRegionalSpellInventory_FinishCast(&inv));
    assert(inv.count==depth && inv.icon==(depth?((depth-1)%4)+1:0) && !inv.casting);
  }
  ArRegionalSpellInventory_Reset(&inv,true);
  assert(ArRegionalSpellInventory_Push(&inv,1) && ArRegionalSpellInventory_Push(&inv,4));
  uint8_t selected;assert(ArRegionalSpellInventory_BeginCast(&inv,&selected) && selected==4);
  assert(ArRegionalSpellInventory_Push(&inv,2) && inv.casting==4 && inv.icon==2);
  assert(ArRegionalSpellInventory_FinishCast(&inv) && inv.count==2 && inv.icon==4);
  ArRegionalSpellInventory_Reset(&inv,true);
  for(unsigned i=0;i<255;++i)assert(ArRegionalSpellInventory_Push(&inv,1));
  assert(ArRegionalSpellInventory_BeginCast(&inv,&selected) && selected==1);
  assert(ArRegionalSpellInventory_Push(&inv,4) && !inv.count && inv.casting==1);
  assert(ArRegionalSpellInventory_FinishCast(&inv));
  assert(!inv.count && !inv.icon && !inv.casting && ArRegionalSpellInventory_Valid(&inv));
  for(unsigned i=0;i<256;++i)assert(ArRegionalSpellInventory_GrowHealth(i)==(i<24?i+1:i));
  const uint8_t map[]={1,0,2,0,3,0,0,4};
  for(unsigned i=0;i<256;++i)assert(ArRegionalSpellInventory_PickupSpell(i)==(i<8?map[i]:0));
  assert(!ArRegionalSpellInventory_Push(&inv,0) && !ArRegionalSpellInventory_Push(&inv,5));
  assert(!ArRegionalSpellInventory_Valid(NULL));
  assert(ArRegionalSpellInventory_Push(&inv,1));
  inv.spells[0]=5;assert(!ArRegionalSpellInventory_Valid(&inv));
  puts("spell inventory: LIFO, delayed debit, interruption, pickup map and byte-count wrap passed");
  return 0;
}
