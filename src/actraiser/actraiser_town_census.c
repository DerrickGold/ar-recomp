#include "actraiser_town_census.h"
#include "actraiser_bridge_extension.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_regional_runtime.h"

enum {
  kVar_TownIndexWord=0x7bfb, kVar_AllocSlot=0x7c05, kVar_AllocRemaining=0x7c1d,
  kVar_TownActWords=0x6b18, kVar_SupportCapacity=0x6b26, kVar_HousePopBias=0x9f57,
  kWram_PopulationBase=0x021c, kRom_StructListPtrs=0xdc74,
  kStructRecordCount=128, kStructRecordSize=4, kStructFlag_Active=0x80,
  kStructType_ClassMask=15, kStructFlag_DisabledSupport=0x40, kStructFlag_Subtype=0x10
};
static uint16 struct_list_base(CpuState *cpu,uint16 town_index) {
  return cpu_read16(cpu,3,(uint16)(kRom_StructListPtrs+town_index));
}

typedef struct TownCensus {
  uint16 population, support, native_y;
} TownCensus;

static TownCensus Count(CpuState *cpu, uint8 db, uint16 town_index,
                       const ArRegionalSupportSnapshot *snapshot, uint16 native_y) {
  TownCensus result = {.native_y = native_y};
  const uint16 base = struct_list_base(cpu, town_index);
  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 rec = (uint16)(base + i * kStructRecordSize);
    const uint8 f2 = cpu_read8(cpu, db, (uint16)(rec + 2));
    if (!(f2 & kStructFlag_Active)) continue;
    const uint8 cls = f2 & kStructType_ClassMask;
    if (cls == 0) {
      const uint8 sub = f2 & 0x30;
      result.population = (uint16)(result.population + (sub == 0x20 ? 8 : sub == 0x10 ? 6 : 4));
    } else {
      uint16 add = snapshot->amount[kArRegionalSupport_Other];
      if (cls == 2)
        add = (f2 & kStructFlag_DisabledSupport) ? 0
              : (f2 & kStructFlag_Subtype) ? snapshot->amount[kArRegionalSupport_UpgradedField]
              : snapshot->amount[kArRegionalSupport_RegularField];
      else if (cls == 3)
        add = (f2 & kStructFlag_DisabledSupport) ? 0 : snapshot->amount[kArRegionalSupport_Factory3];
      else if (cls == 4)
        add = snapshot->amount[kArRegionalSupport_Factory4];
      result.native_y = add;
      result.support = (uint16)(result.support + add);
    }
  }
  result.support = (uint16)(result.support + snapshot->amount[kArRegionalSupport_Other] *
                            ActRaiserBridgeExtension_Count(cpu, db, town_index >> 1));
  return result;
}

static void Publish(CpuState *cpu, uint8 db, uint16 town_index, TownCensus count) {
  const uint16 bias = cpu_read16(cpu, db, (uint16)(kVar_HousePopBias + town_index));
  cpu_write16(cpu, 0, (uint16)(kWram_PopulationBase + town_index), (uint16)(count.population + 2 - bias));
  cpu_write16(cpu, db, (uint16)(kVar_SupportCapacity + town_index), count.support);
}

bool ActRaiserTownCensus_Refresh(CpuState *cpu, unsigned town,
                               const ArRegionalSupportSnapshot *snapshot) {
  if (!cpu || town >= 6 || !ArRegionalSupport_Valid(snapshot)) return false;
  const uint16 index = (uint16)(town * 2);
  if (cpu_read16(cpu, 0x7f, kVar_TownActWords + index))
    Publish(cpu, 0x7f, index, Count(cpu, 0x7f, index, snapshot, 0));
  return true;
}

/* Support affects admission, not the population of existing houses. Native
 * modular bias arithmetic is deliberate; redevelopment must separately prove
 * its post-demolition population valid before committing any conversion. */
RecompReturn ActRaiserTownCensus_Run(CpuState *cpu,const ArRegionalSupportSnapshot *snapshot) {
  if (!cpu || !ArRegionalSupport_Valid(snapshot)) ActRaiserHleFatal("Invalid town census support profile");
  const uint8 db = cpu->DB;
  const uint8 saved_p = cpu->P;      /* PHP */
  const uint16 saved_x = cpu->X;     /* PHX */
  uint16 native_y = cpu->Y;          /* Y is not saved by the ROM */

  cpu_write16(cpu, db, kVar_AllocSlot, 0);       /* STZ $7C05 */
  cpu_write16(cpu, db, 0x7C07, 0);               /* STZ $7C07 */
  cpu_write16(cpu, db, kVar_AllocRemaining, kStructRecordCount);

  const uint16 town_index = cpu_read16(cpu, db, kVar_TownIndexWord);
  const uint16 active =
      cpu_read16(cpu, db, (uint16)(kVar_TownActWords + town_index));
  uint16 exit_a;
  if (active != 0) {
    const TownCensus count = Count(cpu, db, town_index, snapshot, native_y);
    native_y = count.native_y;
    cpu_write16(cpu, db, kVar_AllocSlot, count.population);
    cpu_write16(cpu, db, 0x7C07, count.support);
    cpu_write16(cpu, db, kVar_AllocRemaining, 0);  /* loop counter spent */
    Publish(cpu, db, town_index, count);
    exit_a = count.support;                /* last LDA $7C07 before the store */
  } else {
    exit_a = active;                 /* fell through the $6B18 gate */
  }

  cpu->A = exit_a;
  cpu->X = saved_x;                  /* PLX */
  cpu->Y = native_y;
  cpu->P = saved_p;                  /* PLP restores every flag */
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16)(cpu->S + 2);     /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_TownCensus(CpuState *cpu) {
  ArRegionalSupportSnapshot snapshot;
  if(!ActRaiserRegional_CopySupport(&snapshot))ActRaiserHleFatal("Invalid active population support rules");
  return ActRaiserTownCensus_Run(cpu,&snapshot);
}
