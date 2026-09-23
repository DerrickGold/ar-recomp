#include "actraiser_construction_runtime.h"

#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_town_status_runtime.h"

extern RecompReturn bank_03_82DB_M0X0(CpuState *cpu);
extern RecompReturn bank_03_84B9_M0X0(CpuState *cpu);
static bool s_active, s_japanese;

void ActRaiserConstructionRuntime_Reset(void) { s_active = s_japanese = false; }
bool ActRaiser_RegionalConstructionEntry(CpuState *cpu) {
  bool unused;
  return !s_active && ActRaiser_RegionalTownStatusCycleEntry(cpu) &&
      ActRaiserRegional_ConstructionSnapshot(false, &unused);
}
static RecompReturn Run(CpuState *cpu, RecompReturn (*native)(CpuState *)) {
  if (s_active || !ActRaiserRegional_ConstructionSnapshot(true, &s_japanese))
    ActRaiserHleFatal("Cannot capture construction price transaction");
  s_active = true;
  /* One outer owner composes the independent price and reporting families.
   * Nested plot/off-screen work inherits the complete transaction snapshot. */
  const RecompReturn result = ActRaiserTownStatusRuntime_Run(cpu, native);
  s_active = false;
  return result;
}
RecompReturn ActRaiser_RegionalConstruction(CpuState *cpu) { return Run(cpu, bank_03_82DB_M0X0); }
RecompReturn ActRaiser_RegionalOffscreenConstruction(CpuState *cpu) { return Run(cpu, bank_03_84B9_M0X0); }
bool ActRaiser_RegionalConstructionPriceEntry(CpuState *cpu) {
  return s_active && s_japanese && ActRaiserTownStatus_Entry(cpu, true);
}
static RecompReturn Price(CpuState *cpu, uint32_t target, uint32_t origin) {
  if (!ActRaiser_RegionalConstructionPriceEntry(cpu))
    ActRaiserHleFatal("Construction price prefix outside its transaction");
  /* JP uses LDA #4: N/Z only. In particular, the native support-building
   * growth return inherits carry. Do not turn its ADC into a host += price. */
  cpu->A = 4;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->A);
  if (!cpu_hle_tailcall_request(target, origin))
    ActRaiserHleFatal("Construction price prefix has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalConstructionBudget(CpuState *cpu) { return Price(cpu, 0x038544, 0x03853b); }
RecompReturn ActRaiser_RegionalConstructionPayment(CpuState *cpu) { return Price(cpu, 0x03842e, 0x038425); }
RecompReturn ActRaiser_RegionalConstructionReturn(CpuState *cpu) { return Price(cpu, 0x038497, 0x03848e); }
