#ifndef AR_ACTRAISER_ACTION_INVENTORY_H
#define AR_ACTRAISER_ACTION_INVENTORY_H
/* ActRaiser inventory hooks: item pickups, spell-inventory debits, pickup art,
 * the HUD icon and the health DMA, under the regional spell-inventory rules
 * and media.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_action_inventory_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_InventoryPickupEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryPickup(CpuState *cpu);
bool ActRaiser_InventoryDebitEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryDebit(CpuState *cpu);
bool ActRaiser_InventoryPickupArtEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryPickupArt(CpuState *cpu);
bool ActRaiser_InventoryIconEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryIcon(CpuState *cpu);
bool ActRaiser_InventoryHealthDmaEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryHealthDma(CpuState *cpu);
bool ActRaiser_InventoryInitialIconEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryInitialIcon(CpuState *cpu);
#endif
