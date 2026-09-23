#include "actraiser/actraiser_town_redevelopment.h"
#include "actraiser/actraiser_cell_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t low[65536], town_ram[65536], rom[65536];
static unsigned writes;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  assert(bank == 0 || bank == 3 || bank == 0x7f);
  return (bank == 0 ? low : bank == 3 ? rom : town_ram)[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | (uint16)cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank == 0x7f); ++writes; town_ram[address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, value); cpu_write8(cpu, bank, address + 1, value >> 8);
}
static void Word(uint8_t *p, unsigned address, unsigned value) { p[address] = value; p[address + 1] = value >> 8; }
static unsigned Base(unsigned town) { return 0x6be7 + town * 512; }
static unsigned At(unsigned town, unsigned x, unsigned y) { return 0x2000 + ActRaiser_CellMarkIndex(town, x, y); }
static CpuState Setup(void) {
  memset(low, 0xa5, sizeof(low)); memset(town_ram, 0x5a, sizeof(town_ram)); memset(rom, 0, sizeof(rom));
  memset(town_ram + 0x6be7, 0, 6 * 512);
  for (unsigned town = 0; town < 6; ++town) {
    Word(rom, 0xdc74 + town * 2, Base(town));
    Word(town_ram, 0x6b18 + town * 2, 2);
    Word(town_ram, 0x9f57 + town * 2, 0);
    Word(town_ram, 0x9efa + town * 2, 0);
    Word(low, 0x22e + town * 2, 1 + town % 3);
  }
  writes = 0;
  return (CpuState){.A=0x1234, .X=0x4567, .Y=0x89ab, .PB=2, .DB=0x7e, .D=0x100, .P=0xff, .S=0x1ffe};
}
static void Record(unsigned town, unsigned slot, unsigned flags, unsigned x, unsigned y) {
  const unsigned at = Base(town) + slot * 4, type = flags & 15;
  town_ram[at] = x; town_ram[at + 1] = y; town_ram[at + 2] = flags; town_ram[at + 3] = 1;
  const unsigned mark = type == 0 ? 0xe0 : type == 1 ? 0xe1 : type == 2 ? flags & 16 ? 0xe5 : 0xe4 :
      type == 3 ? 0xe6 : type == 4 ? 0xe3 : type == 5 ? 0xe7 : 0xe8;
  const unsigned width = type >= 2 && type <= 5 ? 2 : 1;
  for (unsigned dy = 0; dy < width; ++dy) for (unsigned dx = 0; dx < width; ++dx)
    town_ram[At(town, x + dx, y + dy)] = mark;
}
static void VerifyRejected(CpuState *cpu, ActRaiserRedevelopmentStatus expected) {
  ActRaiserTownRedevelopmentPlan plan; memset(&plan, 0xa5, sizeof(plan));
  const ActRaiserTownRedevelopmentPlan old = plan;
  const unsigned before = writes;
  assert(ActRaiserTownRedevelopment_Preview(cpu, 63, false, &plan) == expected);
  assert(!memcmp(&plan, &old, sizeof(plan)) && writes == before);
}
static void Footprints(void) {
  for (unsigned town = 0; town < 6; ++town) for (unsigned flags = 0; flags < 256; ++flags) {
    if ((flags & 15) > 6) continue;
    CpuState cpu = Setup(), before = cpu;
    Record(town, 127, flags, 28, 28);
    town_ram[Base(town) + 127 * 4 + 3] = flags & 16 ? 0xb0 : 1;
    uint8_t expected[sizeof(town_ram)]; memcpy(expected, town_ram, sizeof(expected));
    ActRaiserTownRedevelopmentPlan plan;
    assert(ActRaiserTownRedevelopment_Preview(&cpu, 1u << town, false, &plan) == kActRaiserRedevelopment_Ready);
    assert(!writes && !memcmp(&cpu, &before, sizeof(cpu)));
    const unsigned type = flags & 15;
    const bool removed = (flags & 128) && (type == 0 || (type >= 2 && type <= 4));
    const unsigned cost = removed ? 2 * (1 + town % 3) + 2 : 0;
    assert(plan.removed[town] == removed && plan.growth_credit[town] == cost);
    assert(plan.affected_towns == (removed ? 1u << town : 0));
    if (removed) {
      expected[Base(town) + 127 * 4 + 2] = 0;
      Word(expected, 0x9efa + town * 2, cost);
      const unsigned width = type ? 2 : 1;
      for (unsigned dy = 0; dy < width; ++dy) for (unsigned dx = 0; dx < width; ++dx)
        expected[At(town, 28 + dx, 28 + dy)] = 8;
    }
    assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Ready);
    assert(!memcmp(expected, town_ram, sizeof(expected)) && !memcmp(&cpu, &before, sizeof(cpu)));
    const unsigned applied_writes = writes;
    if (removed) assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Stale);
    assert(applied_writes == writes);
    assert(ActRaiserTownRedevelopment_Preview(&cpu, 1u << town, false, &plan) == kActRaiserRedevelopment_Ready);
    assert(plan.affected_towns == 0);
    assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Ready && writes == applied_writes);
  }
}
static void WholeTowns(void) {
  CpuState cpu = Setup();
  ActRaiserTownRedevelopmentPlan plan;
  for (unsigned town = 0; town < 6; ++town) {
    for (unsigned slot = 0; slot < 128; ++slot) Record(town, slot, 0x80 + 16 * (slot % 3), slot % 32, slot / 32);
    /* Protected bridge replaces one house. */
    Record(town, 64, 0x81, 0, 2);
    Word(town_ram, 0x9efa + town * 2, 27);
  }
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready);
  assert(plan.affected_towns == 63);
  for (unsigned town = 0; town < 6; ++town) {
    assert(plan.removed[town] == 127);
    assert(plan.growth_credit[town] == 127 * (4 + 2 * (town % 3))-27);
  }
  assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Ready);
  for (unsigned town = 0; town < 6; ++town) {
    assert(town_ram[Base(town) + 64 * 4 + 2] == 0x81 && town_ram[At(town, 0, 2)] == 0xe1);
    assert(cpu_read16(&cpu, 0x7f, 0x9efa + town * 2) == 27 + plan.growth_credit[town]);
  }
  /* A partial rebuild/reset uses the existing reserve; it cannot accumulate
   * new credits. Native offscreen houses do not even spend a start cost. */
  Word(town_ram, 0x9efa, cpu_read16(&cpu, 0x7f, 0x9efa) - 4);
  Record(0, 0, 0x80, 0, 0);
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready);
  assert(plan.affected_towns == 1 && plan.removed[0] == 1 && plan.growth_credit[0] == 0);
  assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Ready);
  assert(cpu_read16(&cpu, 0x7f, 0x9efa) == 126 * 4);
}
static void SupportAllowance(void) {
  for(unsigned jp=0;jp<2;++jp)for(unsigned civ=1;civ<=3;++civ)for(unsigned growth=0;growth<20;++growth) {
    CpuState cpu=Setup();Word(low,0x22e,civ);Word(town_ram,0x9efa,growth);
    Record(0,0,0x82,4,4);Record(0,1,0x83,8,8);Record(0,2,0x84,12,12);
    const unsigned price=jp?4:2*civ+2;
    ActRaiserTownRedevelopmentPlan plan;
    assert(ActRaiserTownRedevelopment_Preview(&cpu,1,jp,&plan)==kActRaiserRedevelopment_Ready);
    assert(plan.removed[0]==3 && !plan.houses[0] && plan.growth_credit[0]==(growth<price?price-growth:0));
    assert(ActRaiserTownRedevelopment_Apply(&cpu,&plan)==kActRaiserRedevelopment_Ready);
    const unsigned after=cpu_read16(&cpu,0x7f,0x9efa);
    assert(after==(growth<price?price:growth));
    /* Rebuilt support returned its cost: a second reset grants no credit. */
    Record(0,0,0x82,4,4);
    assert(ActRaiserTownRedevelopment_Preview(&cpu,1,jp,&plan)==kActRaiserRedevelopment_Ready && !plan.growth_credit[0]);
    assert(ActRaiserTownRedevelopment_Apply(&cpu,&plan)==kActRaiserRedevelopment_Ready);
    assert(cpu_read16(&cpu,0x7f,0x9efa)==after);
    Record(0,0,0x80,4,4);Record(0,1,0x82,8,8);
    assert(ActRaiserTownRedevelopment_Preview(&cpu,1,jp,&plan)==kActRaiserRedevelopment_Ready);
    assert(plan.houses[0]==1 && plan.removed[0]==2 && !plan.growth_credit[0]);
    for(unsigned repeat=0;repeat<20;++repeat) {
      assert(ActRaiserTownRedevelopment_Apply(&cpu,&plan)==kActRaiserRedevelopment_Ready);
      Record(0,0,0x80,4,4); /* Native offscreen reconstruction, no debit. */
      assert(ActRaiserTownRedevelopment_Preview(&cpu,1,jp,&plan)==kActRaiserRedevelopment_Ready);
      assert(!plan.growth_credit[0] && cpu_read16(&cpu,0x7f,0x9efa)==after);
    }
  }
}
static void Refusals(void) {
  CpuState cpu = Setup();
  Record(0, 0, 0x80, 4, 4); Record(5, 0, 0x82, 4, 4);
  ActRaiserTownRedevelopmentPlan plan;
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready);
  Word(town_ram, 0x9efa + 10, 1);
  assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_Stale && !writes);
  Word(town_ram, 0x9efa + 10, 0);
  town_ram[Base(5) + 3] = 7; VerifyRejected(&cpu, kActRaiserRedevelopment_StructureBusy);
  assert(ActRaiserTownRedevelopment_Apply(&cpu, &plan) == kActRaiserRedevelopment_StructureBusy && !writes);
  town_ram[Base(5) + 3] = 1;
  town_ram[At(5, 5, 5)] = 0xa2; VerifyRejected(&cpu, kActRaiserRedevelopment_MarkMismatch);
  town_ram[At(5, 5, 5)] = 0xe4;
  Record(5, 1, 0x86, 5, 5); VerifyRejected(&cpu, kActRaiserRedevelopment_MarkMismatch);
  town_ram[At(5, 5, 5)] = 0xe4; VerifyRejected(&cpu, kActRaiserRedevelopment_InvalidFootprint);
  town_ram[Base(5) + 6] = 0;
  town_ram[Base(5)] = 31; VerifyRejected(&cpu, kActRaiserRedevelopment_InvalidFootprint);
  town_ram[Base(5)] = 4;
  town_ram[Base(5) + 2] = 0x87; VerifyRejected(&cpu, kActRaiserRedevelopment_UnknownStructure);
  town_ram[Base(5) + 2] = 0x82;
  Word(town_ram, 0x9f57 + 10, 3); VerifyRejected(&cpu, kActRaiserRedevelopment_PopulationBias);
  Word(town_ram, 0x9f57 + 10, 2);
  VerifyRejected(&cpu, kActRaiserRedevelopment_PopulationBias);
  Word(town_ram, 0x9f57 + 10, 1);
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready);
  Word(low, 0x22e + 10, 0); VerifyRejected(&cpu, kActRaiserRedevelopment_GrowthRange);
  Word(low, 0x22e + 10, 4); VerifyRejected(&cpu, kActRaiserRedevelopment_GrowthRange);
  Word(low, 0x22e + 10, 3);
  /* Existing high reserves stay unchanged, but signed native budgets still
   * reject values beyond 32767. */
  Record(5, 0, 0x80, 4, 4);
  Word(town_ram, 0x9efa + 10, 32768); VerifyRejected(&cpu, kActRaiserRedevelopment_GrowthRange);
  Word(town_ram, 0x9efa + 10, 32767);
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready);
  Word(rom, 0xdc74 + 10, 0x2000); VerifyRejected(&cpu, kActRaiserRedevelopment_Invalid);
  /* Unentered towns may contain native seeded/unfinished records. */
  Word(town_ram, 0x6b18 + 10, 0);
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready && plan.affected_towns == 1);
  assert(ActRaiserTownRedevelopment_Preview(&cpu, 64, false, &plan) == kActRaiserRedevelopment_Invalid);
  assert(ActRaiserTownRedevelopment_Preview(NULL, 1, false, &plan) == kActRaiserRedevelopment_Invalid);
  assert(ActRaiserTownRedevelopment_Apply(&cpu, NULL) == kActRaiserRedevelopment_Invalid && !writes);
}
static bool RecordedTown(const char *wram_path, const char *rom_path) {
  FILE *f = fopen(wram_path, "rb"); assert(f);
  assert(fread(low, 1, sizeof(low), f) == sizeof(low));
  assert(fread(town_ram, 1, sizeof(town_ram), f) == sizeof(town_ram));
  assert(fgetc(f) == EOF); assert(fclose(f) == 0);
  f = fopen(rom_path, "rb"); assert(f);
  assert(fseek(f, 0x18000, SEEK_SET) == 0);
  assert(fread(rom + 0x8000, 1, 0x8000, f) == 0x8000); assert(fclose(f) == 0);
  CpuState cpu = {0}; ActRaiserTownRedevelopmentPlan plan;
  for (unsigned town = 0; town < 6; ++town) {
    const ActRaiserRedevelopmentStatus s = ActRaiserTownRedevelopment_Preview(&cpu, 1u << town, false, &plan);
    printf("recorded town %u: status %u, remove %u, credit %u\n", town, s,
           s == kActRaiserRedevelopment_Ready ? plan.removed[town] : 0,
           s == kActRaiserRedevelopment_Ready ? plan.growth_credit[town] : 0);
  }
  return ActRaiserTownRedevelopment_Preview(&cpu, 63, false, &plan) == kActRaiserRedevelopment_Ready;
}
int main(int argc, char **argv) {
  Footprints(); WholeTowns(); SupportAllowance(); Refusals();
  puts("town redevelopment: bounded mutation, stale/failure guards and allowance tests passed");
  if (argc == 3) return RecordedTown(argv[1], argv[2]) ? 0 : 1;
  assert(argc == 1);
  return 0;
}
