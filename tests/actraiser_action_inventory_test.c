#include "actraiser/actraiser_action_inventory.h"
#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_native_call.h"
#include "actraiser/actraiser_scroll_cast.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536],art[2048];
static ArRegionalSpellInventory inventory;
static bool dirty;
static unsigned target,owner,brk,cop,ppu_writes,native_calls;
static uint16_t pixels[64];
static RecompReturn native_result;
void (*g_cpu_brk_hook)(CpuState *cpu);
void (*g_cpu_cop_hook)(CpuState *cpu);
static void Brk(CpuState *cpu) {brk=(uint8_t)cpu->A;}
static void Cop(CpuState *cpu) {cop=(uint8_t)cpu->A;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {
  (void)cpu;if(bank==6) {assert(at>=0xa400 && at<0xac00);return art[at-0xa400];}
  assert(!bank);return ram[at];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank);ram[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {
  if(at==0x2118) {assert(ppu_writes<64);pixels[ppu_writes++]=value;}
  cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);
}
ActRaiserInventoryView ActRaiserRegional_InventoryView(void) {
  return (ActRaiserInventoryView){inventory.count,inventory.icon,inventory.casting,inventory.enabled,dirty};
}
bool ActRaiserRegional_PushSpell(unsigned spell) {dirty=true;return ArRegionalSpellInventory_Push(&inventory,spell);}
bool ActRaiserRegional_BeginSpell(uint8_t *spell) {return ArRegionalSpellInventory_BeginCast(&inventory,spell);}
bool ActRaiserRegional_FinishSpell(void) {dirty=true;return ArRegionalSpellInventory_FinishCast(&inventory);}
void ActRaiserRegional_InventoryIconUploaded(void) {dirty=false;}
int cpu_hle_tailcall_request(uint32 pc,uint32 site) {target=pc;owner=site;return true;}
RecompReturn bank_02_AF30_M1X0(CpuState *cpu) {(void)cpu;return native_result;}
RecompReturn ActRaiserNativeCall(CpuState *cpu,ActRaiserNativeLeaf leaf,uint8_t bank,uint16_t caller,bool long_call) {
  assert(bank==2 && caller==0xac22 && !long_call && leaf==bank_02_AF30_M1X0);
  ++native_calls;return leaf(cpu);
}
static CpuState Setup(unsigned p) {
  memset(ram,0xa5,sizeof(ram));ram[0x349]=1;ram[0x18]=1;
  ByteOrder_WriteLe16(ram+0x8a,0x8a0);ram[0x21]=0;ram[0xf8]=0;
  ByteOrder_WriteLe16(ram+0x8d0,0);
  ArRegionalSpellInventory_Reset(&inventory,true);dirty=true;
  target=owner=brk=cop=ppu_writes=native_calls=0;native_result=RECOMP_RETURN_NORMAL;
  CpuState cpu={.S=0x1e00,.X=0x8a0,.Y=0x9876,.P=(uint8_t)p};cpu_p_to_mirrors(&cpu);return cpu;
}
static void Pickups(void) {
  for(unsigned p=0;p<256;++p) {
    if(p&CPU_P_X)continue;
    for(unsigned id=0;id<8;++id)for(unsigned hp=0;hp<26;++hp) {
      CpuState cpu=Setup(p);cpu.A=0xab00|id;ram[0x1d]=hp;ram[0x1e]=24-hp%25;
      const uint8_t max=ram[0x1e],spell=ArRegionalSpellInventory_PickupSpell(id);
      const bool special=id==1 || spell;
      assert(ActRaiser_InventoryPickupEntry(&cpu)==special);
      if(!special)continue;
      assert(ActRaiser_InventoryPickup(&cpu)==RECOMP_RETURN_TAILCALL && target==0x884e && owner==0x879d);
      assert(cpu.S==0x1dff && ram[0x1e00]==p && cpu.m_flag && !cpu.x_flag && cpu.X==0x8a0 && cpu.Y==0x9876);
      if(spell)assert(inventory.count==1 && inventory.icon==spell && ram[0x21]==1 && brk==15 && !cop && ram[0x1d]==hp && ram[0x1e]==max);
      else assert(!inventory.count && !brk && cop==0x8d && ram[0x1d]==ArRegionalSpellInventory_GrowHealth(hp) && ram[0x1e]==ArRegionalSpellInventory_GrowHealth(max));
    }
  }
}
static void Casts(void) {
  for(unsigned spell=1;spell<=4;++spell)for(unsigned gate=0;gate<5;++gate) {
    CpuState cpu=Setup(0);assert(ActRaiserRegional_PushSpell(spell));ram[0x21]=1;
    if(gate==1)ram[0xf8]=1;
    if(gate==2)ByteOrder_WriteLe16(ram+0x8d0,8);
    if(gate==3)ByteOrder_WriteLe16(ram+0x8d0,0x2000);
    if(gate==4) {inventory.casting=spell;}
    ArRegionalCostSnapshot unused={0};
    assert(ActRaiserScrollCast_Gate(&cpu,&unused)==(gate?0x984e:0x9e0e));
    assert(inventory.count==1 && ram[0x21]==1);
    if(gate)continue;
    assert(inventory.casting==spell && ByteOrder_ReadLe16(ram+0x2ac)==spell);
    assert(ActRaiser_InventoryDebitEntry(&cpu));
    assert(ActRaiser_InventoryDebit(&cpu)==RECOMP_RETURN_TAILCALL && target==0x9efe && owner==0x9efc);
    assert(!inventory.count && !inventory.casting && !inventory.icon && !ram[0x21] && !ByteOrder_ReadLe16(ram+0xf9));
    assert(ActRaiser_InventoryDebit(&cpu)==RECOMP_RETURN_TAILCALL && !inventory.count);
  }
}
static void Art(void) {
  for(unsigned id=0;id<8;++id) {
    CpuState cpu=Setup(0);cpu.A=0xa000+id*128;ram[cpu.X+0x38]=id;
    assert(ActRaiser_InventoryPickupArtEntry(&cpu));
    assert(ActRaiser_InventoryPickupArt(&cpu)==RECOMP_RETURN_TAILCALL && target==0x96e5 && owner==0x96e3);
    const unsigned spell=ArRegionalSpellInventory_PickupSpell(id);
    assert(cpu.A==(spell?0xa400+(spell-1)*128:id==1?0xa280:0xa000+id*128));
    assert(ByteOrder_ReadLe16(ram+0xd0)==cpu.A);
  }
  for(unsigned spell=0;spell<=4;++spell) {
    CpuState cpu=Setup(CPU_P_M);cpu.PB=2;inventory.icon=spell;
    for(unsigned i=0;i<sizeof(art);++i)art[i]=(i*37)^(i>>7);
    const CpuState before=cpu;
    assert(ActRaiser_InventoryIconEntry(&cpu));
    assert(ActRaiser_InventoryIcon(&cpu)==RECOMP_RETURN_TAILCALL && target==0x02ac23 && owner==0x02ac20);
    assert(ppu_writes==64 && ByteOrder_ReadLe16(ram+0x2116)==0x2d40 && !dirty && native_calls==1);
    assert(!memcmp(&cpu,&before,sizeof(cpu)) && !ActRaiser_InventoryIconEntry(&cpu));
    for(unsigned i=0;i<64;++i)assert(pixels[i]==(spell?ByteOrder_ReadLe16(art+(spell-1)*128+i*2):0));
  }
  const RecompReturn escapes[]={RECOMP_RETURN_SKIP_1,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned i=0;i<4;++i) {
    CpuState cpu=Setup(CPU_P_M);cpu.PB=2;native_result=escapes[i];
    assert(ActRaiser_InventoryIcon(&cpu)==escapes[i] && !target && !dirty);
  }
}
static void Guards(void) {
  for(unsigned which=0;which<4;++which)for(unsigned bad=0;bad<8;++bad) {
    CpuState cpu=Setup(which==3?CPU_P_M:0);cpu.PB=which==3?2:0;
    cpu.A=which==2?0xa000:0;ram[cpu.X+0x38]=0;
    switch(bad) {
      case 0:cpu.PB=3;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.x_flag=1;break;case 4:cpu.emulation=1;break;
      case 5:inventory.enabled=false;break;case 6:ram[0x349]=0;break;
      case 7:
        if(which==0)cpu.A=3; /* shared native effect */
        else if(which==1)cpu.X+=64;
        else if(which==2)cpu.A++;
        else dirty=false;
        break;
    }
    const bool accepted=which==0?ActRaiser_InventoryPickupEntry(&cpu):which==1?ActRaiser_InventoryDebitEntry(&cpu):
      which==2?ActRaiser_InventoryPickupArtEntry(&cpu):ActRaiser_InventoryIconEntry(&cpu);
    assert(!accepted && !target && !ppu_writes);
  }
  assert(!ActRaiser_InventoryPickupEntry(NULL) && !ActRaiser_InventoryDebitEntry(NULL) &&
      !ActRaiser_InventoryPickupArtEntry(NULL) && !ActRaiser_InventoryIconEntry(NULL));
}
static void Roms(char **paths) {
  uint8_t us[1048576],rom[1048576];
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    if(!region)memcpy(us,rom,sizeof(us));
    if(region<2) {
      const unsigned base=region?0x78c:0x79d;
      assert(!memcmp(rom+base,(uint8_t[]){8,0xe2,0x20,0xc9,0,0xf0,0x15},7));
      assert(!memcmp(rom+base+0xb1,(uint8_t[]){0x28,0x60},2));
      continue;
    }
    assert(!memcmp(rom+0x87f,(uint8_t[]){0x5a,0xa4,0x21,0x99,0,0x1c,0xe6,0x21,0x7a,0x60},10));
    assert(!memcmp(rom+0x806,(uint8_t[]){0xa5,0x1e,0xc9,0x18,0xb0,2,0xe6,0x1e,0xa5,0x1d,0xc9,0x18,0xb0,2,0xe6,0x1d},16));
    assert(!memcmp(rom+0x1a8e,(uint8_t[]){0xc6,0x21,0xa4,0x21,0xd0,5,0xa9,0,0,0x80,3,0xb9,0xff,0x1b},14));
    const unsigned items[]={0,2,4,7};
    for(unsigned spell=0;spell<4;++spell)
      assert(!memcmp(rom+0x32800+items[spell]*128,us+0x32400+spell*128,128));
  }
  puts("five-ROM pickup contracts; PAL stack/health/debit instructions and US-resident spell graphics verified");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);
  g_cpu_brk_hook=Brk;g_cpu_cop_hook=Cop;Pickups();Casts();Art();Guards();if(argc==6)Roms(argv+1);
  puts("Action inventory adapters: pickup ABI, independent HP caps, cast gates/debit and bounded NMI icon upload passed");
  return 0;
}
