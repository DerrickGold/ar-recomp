#include "actraiser_action_inventory.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_regional_media.h"

extern RecompReturn bank_02_AF30_M1X0(CpuState *cpu);
extern RecompReturn bank_02_AF3D_M1X0(CpuState *cpu);
static bool s_health_delegate;
static bool Context(CpuState *cpu,unsigned bank) {
  return cpu && cpu->PB==bank && !cpu->DB && !cpu->D && !cpu->x_flag &&
      !cpu->emulation;
}
static bool Native(CpuState *cpu,unsigned bank) {
  return Context(cpu,bank) && ActRaiserRegional_InventoryView().enabled && cpu_read8(cpu,0,0x349);
}
static bool EuropeanArt(void) {
  return (ActRaiserRegional_ArtworkSnapshot()&(1u<<kArRegionalArtwork_ActionItems))!=0;
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
  return item==1 || item==5 || item==6 || ArRegionalSpellInventory_PickupSpell(item)!=0;
}
RecompReturn ActRaiser_InventoryPickup(CpuState *cpu) {
  if(!ActRaiser_InventoryPickupEntry(cpu))ActRaiserHleFatal("Unsupported Action inventory pickup");
  const unsigned item=(uint8_t)cpu->A;
  if(cpu_read8(cpu,0,0x21)!=ActRaiserRegional_InventoryView().count)
    ActRaiserHleFatal("Action inventory count diverged before pickup");
  /* Keep the US helper's PHP/PLP/RTS contract and original return frame.
   * Unchanged sword/heal/score effects remain native. PAL's shared heal and
   * score pickups use sound0F rather than US0D before those same effects. */
  cpu_mirrors_to_p(cpu);cpu_write8(cpu,0,cpu->S--,cpu->P);
  cpu->P|=CPU_P_M;cpu_p_to_mirrors(cpu);
  const uint8_t spell=ArRegionalSpellInventory_PickupSpell(item);
  if(item==5 || item==6) {
    Load8(cpu,0x0f);if(g_cpu_brk_hook)g_cpu_brk_hook(cpu);
    return Tail(item==5?0x008825:0x008832,0x00879d);
  }
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
   * A080 identifies the genuine item1 request at the scoped NMI DMA seam.
   * Without donor art, a full apple avoids an incorrect extra-life graphic. */
  cpu->A=spell?0xa400+(spell-1)*0x80:
      item==1?(ActRaiserRegionalMedia_ActionHealth(EuropeanArt()).data?0xa080:0xa280):0xa000+item*0x80;
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
  const ArRegionalMediaBytes donor=ActRaiserRegionalMedia_ActionHud(EuropeanArt(),spell);
  /* Native NMI upload boundary, after pending sprite/font transfers. Use the
   * CPU bus, not a renderer or host VRAM pointer. Four OBJ tiles only, leaving
   * adjacent pickup tiles untouched. No upload on unchanged ordinary frames. */
  cpu_write16(cpu,0,0x2116,0x2d40);
  for(unsigned i=0;i<128;i+=2)
    cpu_write16(cpu,0,0x2118,donor.data?(uint16_t)(donor.data[i]|(uint16_t)donor.data[i+1]<<8):
        spell?cpu_read16(cpu,6,0xa400+(spell-1)*128+i):0);
  ActRaiserRegional_InventoryIconUploaded();
  const RecompReturn result=ActRaiserNativeCall(cpu,bank_02_AF30_M1X0,2,0xac22,false);
  return result==RECOMP_RETURN_NORMAL?Tail(0x02ac23,0x02ac20):result;
}

bool ActRaiser_InventoryHealthDmaEntry(CpuState *cpu) {
  if(s_health_delegate) {s_health_delegate=false;return false;}
  /* The native queue is the identity: no pending host token, borrowed WRAM
   * or fictitious ROM address is needed. Only the first DMA slot is an item. */
  return Native(cpu,2) && cpu->m_flag && !cpu->X &&
      cpu_read16(cpu,0,0xd0)==0xa080 && cpu_read8(cpu,0,0xd2)==6 &&
      cpu_read16(cpu,0,0xd3)==0x2d80 && cpu_read16(cpu,0,0xd5)==128 &&
      ActRaiserRegionalMedia_ActionHealth(EuropeanArt()).data;
}
RecompReturn ActRaiser_InventoryHealthDma(CpuState *cpu) {
  if(!ActRaiser_InventoryHealthDmaEntry(cpu))ActRaiserHleFatal("Unsupported Action health art DMA");
  const ArRegionalMediaBytes donor=ActRaiserRegionalMedia_ActionHealth(EuropeanArt());
  /* This synchronous leaf retains the original JSR frame and owns DMA
   * source/count residue, CPU flags and RTS. Never postprocess an escape. */
  s_health_delegate=true;
  const RecompReturn result=bank_02_AF3D_M1X0(cpu);
  s_health_delegate=false;
  if(result!=RECOMP_RETURN_NORMAL)return result;
  /* Replace only its128 pixel bytes before NMI presents them. The final
   * VRAM address remains2DC0; no native registers or stack bytes are changed. */
  cpu_write16(cpu,0,0x2116,0x2d80);
  for(unsigned i=0;i<128;i+=2)
    cpu_write16(cpu,0,0x2118,(uint16_t)(donor.data[i]|(uint16_t)donor.data[i+1]<<8));
  return result;
}
bool ActRaiser_InventoryInitialIconEntry(CpuState *cpu) {
  if(!Native(cpu,2) || cpu->m_flag || cpu_read8(cpu,0,0x18)<1 ||
      cpu_read8(cpu,0,0x18)>7 || cpu_read16(cpu,0,0x0c)!=128)return false;
  const unsigned selected=cpu_read8(cpu,0,0x2ac);
  if(selected>4 || cpu->X!=(selected?selected-1:5)*128)return false;
  return true;
}
RecompReturn ActRaiser_InventoryInitialIcon(CpuState *cpu) {
  if(!ActRaiser_InventoryInitialIconEntry(cpu))ActRaiserHleFatal("Unsupported initial Action icon upload");
  const unsigned spell=ActRaiserRegional_InventoryView().icon;
  const ArRegionalMediaBytes donor=ActRaiserRegionalMedia_ActionHud(EuropeanArt(),spell);
  for(unsigned i=0;i<256;i+=2) {
    /* Source selection belongs to the host collection, but the native
     * accumulator, index and loop-counter residue still belong to BCED. */
    cpu->A=cpu_read16(cpu,6,(uint16_t)(0xa400+cpu->X));
    const uint16_t pixels=donor.data?(uint16_t)(donor.data[i]|(uint16_t)donor.data[i+1]<<8):
        spell?cpu_read16(cpu,6,(uint16_t)(0xa400+(spell-1)*128+i)):0;
    cpu_write16(cpu,0,0x2118,pixels);
    cpu->X=(uint16_t)(cpu->X+2);
    cpu_write16(cpu,0,0x0c,(uint16_t)(127-i/2));
  }
  ActRaiserCpuHle_SetNegativeZero16(cpu,0);
  return Tail(0x02bcfa,0x02bced);
}
