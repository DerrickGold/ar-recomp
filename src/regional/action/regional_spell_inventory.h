#ifndef AR_REGIONAL_SPELL_INVENTORY_H
#define AR_REGIONAL_SPELL_INVENTORY_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalInventoryDescriptor {
  const char *key;
  uint16_t enabled[kArRegionalSource_Count];
} ArRegionalInventoryDescriptor;
const ArRegionalInventoryDescriptor *ArRegionalInventory_Descriptor(void);
bool ArRegionalInventory_Resolve(ArRegionalSource source, bool *enabled);

/* Action-run state, not cartridge SRAM or a settings snapshot. A room/retry
 * preserves the collection and cancels only an interrupted cast. The native
 * byte-count overflow at 256 pushes is intentional. Dead entries need not be
 * erased; only [0,count) belongs to the live collection. */
typedef struct ArRegionalSpellInventory {
  uint8_t spells[256];
  uint8_t count, icon, casting;
  bool enabled;
} ArRegionalSpellInventory;

void ArRegionalSpellInventory_Reset(ArRegionalSpellInventory *inventory, bool enabled);
bool ArRegionalSpellInventory_Valid(const ArRegionalSpellInventory *inventory);
bool ArRegionalSpellInventory_Push(ArRegionalSpellInventory *inventory, unsigned spell);
bool ArRegionalSpellInventory_BeginCast(ArRegionalSpellInventory *inventory, uint8_t *spell);
bool ArRegionalSpellInventory_FinishCast(ArRegionalSpellInventory *inventory);
void ArRegionalSpellInventory_Interrupt(ArRegionalSpellInventory *inventory);
/* PAL Action pickup interpretation; zero means use the shared native effect.
 * The health pickup is deliberately not encoded as an inventory spell. */
uint8_t ArRegionalSpellInventory_PickupSpell(unsigned item);
uint8_t ArRegionalSpellInventory_GrowHealth(uint8_t health);

#endif
