#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_regional_settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536];
static ArRegionalCostSnapshot expected;
static RecompReturn title_return, miracle_return;
static unsigned title_calls, miracle_calls;
static bool edits_allowed = true, edit_during_miracle;
bool InputReplay_PolicyChangesAllowed(void) { return edits_allowed; }
static uint32_t tail_pc, tail_source;
int cpu_hle_tailcall_request(uint32_t pc, uint32_t source) {
  tail_pc=pc; tail_source=source;
  return 1; /* runtime infra tests own inherited-stack/paired-return semantics */
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(bank == 0); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank==0); ram[address]=value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu,bank,address,value); cpu_write8(cpu,bank,address+1,value>>8);
}
RecompReturn bank_02_A622_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalTitleEntry(cpu)); /* delegates exactly once */
  ++title_calls;
  return title_return;
}
bool ActRaiserMiracle_Entry(const CpuState *cpu, unsigned action) {
  return cpu && action >= 5 && action <= 9 && cpu->PB == 1 && cpu->DB == 1 &&
      cpu->m_flag && !cpu->x_flag && !cpu->D && !cpu->emulation;
}
bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule) {
  static const ArRegionalCostRule rules[] = {kArRegionalCost_Lightning,
      kArRegionalCost_Rain, kArRegionalCost_Sunlight, kArRegionalCost_Wind,
      kArRegionalCost_Earthquake};
  if (action < 5 || action > 9 || !rule) return false;
  *rule = rules[action - 5]; return true;
}
RecompReturn ActRaiserMiracle_Run(CpuState *cpu, unsigned action,
                                 const ArRegionalCostSnapshot *quote) {
  assert(action >= 5 && action <= 9);
  assert(!ActRaiserRegional_MiracleEntry(cpu)); /* no nested transaction */
  assert(!memcmp(quote, &expected, sizeof(expected)));
  ArRegionalCostSnapshot displayed;
  assert(ActRaiserRegional_CopyPrices(&displayed));
  assert(!memcmp(&displayed, quote, sizeof(displayed)));
  if (edit_during_miracle) {
    ActRaiserRegionalPricingView view;
    assert(ActRaiserRegional_CopyPricingView(&view) && view.miracle_in_progress);
    assert(ActRaiserRegional_RequestPricing(&view,kArRegionalCostGroup_Miracles,
        kArRegionalCost_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyPrices(&displayed));
    assert(!memcmp(&displayed, quote, sizeof(displayed)));
    assert(ActRaiserRegional_CopyPricingView(&view));
    assert(view.requested.source[kArRegionalCost_Rain]==kArRegionalCost_Japan);
    assert(view.effective.source[kArRegionalCost_Rain]==kArRegionalCost_Europe);
  }
  /* Render consumers own copies, not writable aliases to the accepted quote. */
  memset(&displayed, 0, sizeof(displayed));
  assert(ActRaiserRegional_CopyPrices(&displayed));
  assert(!memcmp(&displayed, quote, sizeof(displayed)));
  ++miracle_calls;
  return miracle_return;
}
static bool Identity(void *unused, uint8_t id[16]) {
  (void)unused; memset(id, 0, 16); id[0] = 42; return true;
}
static void CheckPrices(ArRegionalCostSource source) {
  ArRegionalCostPolicy policy;
  assert(ArRegionalCosts_Init(&policy, source));
  assert(ArRegionalCosts_Resolve(&policy, &expected));
  ArRegionalCostSnapshot actual;
  assert(ActRaiserRegional_CopyPrices(&actual));
  assert(!memcmp(&actual, &expected, sizeof(actual)));
}
static void Install(const char *path, uint8_t *image, ArRegionalCostSource source) {
  ArRegionalCostPolicy policy;
  ArRegionalSession session;
  const uint8_t id[16] = {(uint8_t)(source + 1)};
  SaveError error = {{0}};
  assert(ArRegionalCosts_Init(&policy, source));
  assert(ArRegionalSession_NewGame(&session, 0, id, &policy));
  assert(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm,
                                path, image, image, &error));
  assert(SaveSystem_LoadActive(&error));
}
int main(void) {
  const char *path = "actraiser-regional-runtime-test.srm";
  const char *companion = "actraiser-regional-runtime-test.srm.archeckpoint";
  remove(path); remove(companion);
  uint8_t image[kActRaiserSramSize] = {0};
  SaveError error = {{0}};
  Save_RecomputeChecksum(image);
  assert(Save_WriteFile(kSaveFileFormat_NativeSrm, path, image, &error));
  assert(SaveSystem_Attach(image, sizeof(image), kSaveBackend_NativeSrm,
                          path, "unused-regional-runtime.ini", &error));
  assert(SaveSystem_LoadActive(&error));
  assert(ActRaiserRegional_Initialize(Identity, NULL));
  ActRaiserRegionalPricingView view;
  assert(!ActRaiserRegional_CopyPricingView(&view));
  CheckPrices(kArRegionalCost_US);
  CpuState cpu = {.PB = 2, .DB = 1, .m_flag = 1};
  ram[0x336] = 1;
  /* Different loaded campaigns deliberately share revision 1. */
  for (unsigned source = 0; source < kArRegionalCostSource_Count; ++source) {
    Install(path, image, (ArRegionalCostSource)source);
    CheckPrices(kArRegionalCost_US); /* unloaded campaign never leaks */
    cpu.PB = 2;
    assert(ActRaiser_RegionalTitleEntry(&cpu));
    assert(ActRaiser_RegionalTitle(&cpu) == RECOMP_RETURN_NORMAL);
    CheckPrices((ArRegionalCostSource)source);
    CpuState casting={.S=0x1f0,.X=0xc00,.host_return_valid=1};
    ram[0x2ac]=4; ram[0x21]=5;
    assert(ActRaiser_RegionalScrollEntry(&casting));
    assert(ActRaiser_RegionalScrollCast(&casting)==RECOMP_RETURN_TAILCALL);
    assert(ram[0x21]==5-expected.price[kArRegionalCost_Light]);
    assert(tail_pc==0x9e0e && tail_source==0x9de1);
    ram[0x2ac]=5;
    assert(!ActRaiser_RegionalScrollEntry(&casting)); /* native fallback outside bounded domain */
    cpu.PB = 1;
    for (unsigned action = 5; action <= 9; ++action) {
      cpu.A = action;
      for (unsigned interrupted = 0; interrupted < 2; ++interrupted) {
        miracle_return = interrupted ? RECOMP_RETURN_PARKED_WAIT : RECOMP_RETURN_NORMAL;
        assert(ActRaiserRegional_MiracleEntry(&cpu));
        assert(ActRaiserRegional_RunMiracle(&cpu) == miracle_return);
        assert(ActRaiserRegional_MiracleEntry(&cpu));
        CheckPrices((ArRegionalCostSource)source);
      }
    }
  }
  assert(ActRaiserRegional_CopyPricingView(&view));
  edits_allowed=false;
  assert(ActRaiserRegional_RequestPricing(&view,kArRegionalCostGroup_Miracles,
      kArRegionalCost_Japan)==kActRaiserRegionalEdit_Locked);
  edits_allowed=true;
  ActRaiserRegionalPricingView stale=view;
  stale.campaign[0]^=1;
  assert(ActRaiserRegional_RequestPricing(&stale,kArRegionalCostGroup_Miracles,
      kArRegionalCost_Japan)==kActRaiserRegionalEdit_Stale);
  edit_during_miracle=true; miracle_return=RECOMP_RETURN_NORMAL; cpu.A=5;
  assert(ActRaiserRegional_RunMiracle(&cpu)==RECOMP_RETURN_NORMAL);
  edit_during_miracle=false;
  assert(ActRaiserRegional_RequestPricing(&view,kArRegionalCostGroup_Miracles,
      kArRegionalCost_US)==kActRaiserRegionalEdit_Stale);
  assert(ActRaiserRegional_CopyPricingView(&view));
  assert(!view.miracle_in_progress);
  assert(ActRaiserRegional_RequestPricing(&view,kArRegionalCostGroup_Miracles,
      kArRegionalCost_Japan)==kActRaiserRegionalEdit_Unchanged);
  assert(ActRaiserRegional_CopyPrices(&expected));
  assert(expected.price[kArRegionalCost_Rain]==16);
  assert(expected.price[kArRegionalCost_Light]==1); /* independent group */
  assert(ActRaiserRegional_RunMiracle(&cpu)==RECOMP_RETURN_NORMAL);
  Install(path, image, kArRegionalCost_Japan);
  cpu.PB = 2; ram[0x336] = 0;
  title_return = RECOMP_RETURN_TAILCALL;
  assert(ActRaiser_RegionalTitle(&cpu) == title_return);
  CheckPrices(kArRegionalCost_US); /* escape is not an accepted selection */
  title_return = RECOMP_RETURN_NORMAL;
  assert(ActRaiser_RegionalTitle(&cpu) == RECOMP_RETURN_NORMAL);
  CheckPrices(kArRegionalCost_US); /* New Game doesn't adopt saved JP */
  assert(title_calls == 5 && miracle_calls == 32);
  remove(path); remove(companion);
  return 0;
}
