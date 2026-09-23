#ifndef ACTRAISER_ACTION_INVENTORY_H
#define ACTRAISER_ACTION_INVENTORY_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_InventoryPickupEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryPickup(CpuState *cpu);
bool ActRaiser_InventoryDebitEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryDebit(CpuState *cpu);
bool ActRaiser_InventoryPickupArtEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryPickupArt(CpuState *cpu);
bool ActRaiser_InventoryIconEntry(CpuState *cpu);
RecompReturn ActRaiser_InventoryIcon(CpuState *cpu);
#endif
