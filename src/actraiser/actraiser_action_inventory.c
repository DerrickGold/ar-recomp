#include "actraiser_action_inventory.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_02_AF30_M1X0(CpuState *cpu);
static bool Native(CpuState *cpu,unsigned bank) {
  return cpu && cpu->PB==bank && !cpu->DB && !cpu->D && !cpu->x_flag &&
      !cpu->emulation && ActRaiserRegional_InventoryView().enabled && cpu_read8(cpu,0,0x349);
}
static RecompReturn Tail(unsigned target,unsigned owner) {
  if(!cpu_hle_tailcall_request(target,owner))ActRaiserHleFatal("Inventory prefix has no native owner");
  return RECOMP_RETURN_TAILCALL;
}
static void Load8(CpuState *cpu,uint8_t value) {
  cpu->A=(cpu->A&0xff00)|value;ActRaiserCpuHle_SetNegativeZero8(cpu,value);
}
bool ActRaiser_InventoryPickupEntry(CpuState *cpu) {
  if(!Native(cpu,0))return false;
  const unsigned item=(uint8_t)cpu->A;
  return item==1 || ArRegionalSpellInventory_PickupSpell(item)!=0;
}
RecompReturn ActRaiser_InventoryPickup(CpuState *cpu) {
  if(!ActRaiser_InventoryPickupEntry(cpu))ActRaiserHleFatal("Unsupported Action inventory pickup");
  const unsigned item=(uint8_t)cpu->A;
  if(cpu_read8(cpu,0,0x21)!=ActRaiserRegional_InventoryView().count)
    ActRaiserHleFatal("Action inventory count diverged before pickup");
  /* Keep the US helper's PHP/PLP/RTS contract and original return frame.
   * Unchanged sword/heal/score effects still execute the native dispatcher. */
  cpu_mirrors_to_p(cpu);cpu_write8(cpu,0,cpu->S--,cpu->P);
  cpu->P|=CPU_P_M;cpu_p_to_mirrors(cpu);
  const uint8_t spell=ArRegionalSpellInventory_PickupSpell(item);
  if(spell) {
    Load8(cpu,0x0f);if(g_cpu_brk_hook)g_cpu_brk_hook(cpu);
    if(!ActRaiserRegional_PushSpell(spell))ActRaiserHleFatal("Cannot push Action spell");
    cpu_write8(cpu,0,0x21,ActRaiserRegional_InventoryView().count);Load8(cpu,spell);
  } else {
    Load8(cpu,0x8d);if(g_cpu_cop_hook)g_cpu_cop_hook(cpu);
    const uint8_t hp=cpu_read8(cpu,0,0x1d),maximum=cpu_read8(cpu,0,0x1e);
    cpu_write8(cpu,0,0x1e,ArRegionalSpellInventory_GrowHealth(maximum));
    cpu_write8(cpu,0,0x1d,ArRegionalSpellInventory_GrowHealth(hp));Load8(cpu,hp);
  }
  return Tail(0x00884e,0x00879d);
}
bool ActRaiser_InventoryDebitEntry(CpuState *cpu) {
  return Native(cpu,0) && !cpu->m_flag && cpu->X==cpu_read16(cpu,0,0x8a);
}
RecompReturn ActRaiser_InventoryDebit(CpuState *cpu) {
  if(!ActRaiser_InventoryDebitEntry(cpu))ActRaiserHleFatal("Unsupported Action spell completion");
  const ActRaiserInventoryView view=ActRaiserRegional_InventoryView();
  if(view.casting) {
    if(cpu_read8(cpu,0,0x21)!=view.count || !ActRaiserRegional_FinishSpell())
      ActRaiserHleFatal("Action spell completion has no matching collection");
    cpu_write8(cpu,0,0x21,ActRaiserRegional_InventoryView().count);
  }
  /* This is the real post-effect cleanup, after graphics restoration. Earlier
   * room transitions never reach it and therefore retain their pending spell. */
  cpu_write16(cpu,0,0xf9,0);return Tail(0x009efe,0x009efc);
}
bool ActRaiser_InventoryPickupArtEntry(CpuState *cpu) {
  if(!Native(cpu,0) || cpu->m_flag)return false;
  const unsigned item=cpu_read8(cpu,0,cpu->X+0x38);
  return item<8 && cpu->A==0xa000+item*0x80;
}
RecompReturn ActRaiser_InventoryPickupArt(CpuState *cpu) {
  if(!ActRaiser_InventoryPickupArtEntry(cpu))ActRaiserHleFatal("Unsupported Action pickup art selection");
  const unsigned item=cpu_read8(cpu,0,cpu->X+0x38);
  const uint8_t spell=ArRegionalSpellInventory_PickupSpell(item);
  /* Four PAL spell pickups are byte-identical to retained US spell tiles.
   * Health growth uses a full-apple fallback until donor presentation exists;
   * it must never display the misleading native extra-life graphic. */
  cpu->A=spell?0xa400+(spell-1)*0x80:item==1?0xa280:0xa000+item*0x80;
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  cpu_write16(cpu,0,0xd0,cpu->A);
  return Tail(0x0096e5,0x0096e3);
}
bool ActRaiser_InventoryIconEntry(CpuState *cpu) {
  return Native(cpu,2) && cpu->m_flag && ActRaiserRegional_InventoryView().icon_pending &&
      cpu_read8(cpu,0,0x18)>=1 && cpu_read8(cpu,0,0x18)<=7;
}
RecompReturn ActRaiser_InventoryIcon(CpuState *cpu) {
  if(!ActRaiser_InventoryIconEntry(cpu))ActRaiserHleFatal("Unsupported inventory icon upload");
  const unsigned spell=ActRaiserRegional_InventoryView().icon;
  /* Native NMI upload boundary, after pending sprite/font transfers. Use the
   * CPU bus, not a renderer or host VRAM pointer. Four OBJ tiles only, leaving
   * adjacent pickup tiles untouched. No upload on unchanged ordinary frames. */
  cpu_write16(cpu,0,0x2116,0x2d40);
  for(unsigned i=0;i<128;i+=2)
    cpu_write16(cpu,0,0x2118,spell?cpu_read16(cpu,6,0xa400+(spell-1)*128+i):0);
  ActRaiserRegional_InventoryIconUploaded();
  const RecompReturn result=ActRaiserNativeCall(cpu,bank_02_AF30_M1X0,2,0xac22,false);
  return result==RECOMP_RETURN_NORMAL?Tail(0x02ac23,0x02ac20):result;
}
