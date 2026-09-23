#include "actraiser_town_redevelopment.h"

#include "actraiser_cell_map.h"
#include "deterministic_hash.h"
#include "regional/regional_construction.h"

#include <string.h>

enum {
  kTownBank = 0x7f, kRecordPointers = 0xdc74, kRecords = 128,
  kRecordBytes = 4, kCellMarks = 0x2000, kActComplete = 0x6b18,
  kGrowth = 0x9efa, kPopulationBias = 0x9f57, kCivilization = 0x022e,
  kActive = 0x80, kEmptyCell = 0x08,
};

static bool Removable(unsigned type) { return type == 0 || (type >= 2 && type <= 4); }
static unsigned Width(unsigned type) { return type >= 2 && type <= 5 ? 2 : 1; }
static unsigned Mark(unsigned flags) {
  switch (flags & 15) {
    case 0: return 0xe0;
    case 2: return flags & 0x10 ? 0xe5 : 0xe4;
    case 3: return 0xe6;
    case 4: return 0xe3;
    default: return 0;
  }
}
static uint16_t RecordBase(CpuState *cpu, unsigned town) {
  return cpu_read16(cpu, 3, kRecordPointers + town * 2);
}
static uint8_t ReadByte(CpuState *cpu, uint8_t bank, uint16_t address, uint64_t *hash) {
  const uint8_t value = cpu_read8(cpu, bank, address);
  *hash = DeterministicHash_Fnv1a64Byte(*hash, value);
  return value;
}
static uint16_t ReadWord(CpuState *cpu, uint8_t bank, uint16_t address, uint64_t *hash) {
  const uint8_t lo = ReadByte(cpu, bank, address, hash);
  return lo | (uint16_t)ReadByte(cpu, bank, address + 1, hash) << 8;
}

ActRaiserRedevelopmentStatus ActRaiserTownRedevelopment_Preview(
    CpuState *cpu, uint8_t towns, bool japanese_construction, ActRaiserTownRedevelopmentPlan *plan) {
  if (!cpu || !plan || (towns & ~0x3fu)) return kActRaiserRedevelopment_Invalid;
  ActRaiserTownRedevelopmentPlan next = {
      .fingerprint = DETERMINISTIC_HASH_FNV1A64_OFFSET, .requested_towns = towns,
      .japanese_construction = japanese_construction};
  for (unsigned town = 0; town < kActRaiserRedevelopmentTowns; ++town) {
    if (!(towns & (1u << town))) continue;
    if (!ReadWord(cpu, kTownBank, kActComplete + town * 2, &next.fingerprint)) continue;
    const uint16_t base = ReadWord(cpu, 3, kRecordPointers + town * 2, &next.fingerprint);
    /* Fixed US layout; corrupt/donor pointer tables must not redirect writes. */
    if (base != 0x6be7 + town * kRecords * kRecordBytes) return kActRaiserRedevelopment_Invalid;
    const unsigned civilization = ReadWord(cpu, 0, kCivilization + town * 2, &next.fingerprint);
    const unsigned growth = ReadWord(cpu, kTownBank, kGrowth + town * 2, &next.fingerprint);
    const unsigned bias = ReadWord(cpu, kTownBank, kPopulationBias + town * 2, &next.fingerprint);
    uint8_t occupied[128] = {0}; /* Detect overlaps, including protected records. */
    for (unsigned slot = 0; slot < kRecords; ++slot) {
      const uint16_t record = base + slot * kRecordBytes;
      const uint8_t x = ReadByte(cpu, kTownBank, record, &next.fingerprint);
      const uint8_t y = ReadByte(cpu, kTownBank, record + 1, &next.fingerprint);
      const uint8_t flags = ReadByte(cpu, kTownBank, record + 2, &next.fingerprint);
      const uint8_t action = ReadByte(cpu, kTownBank, record + 3, &next.fingerprint);
      if (!(flags & kActive)) continue;
      const unsigned type = flags & 15, width = Width(type);
      if (type > 6) return kActRaiserRedevelopment_UnknownStructure;
      if (x + width > 32 || y + width > 32 ||
          (Removable(type) && width == 2 && ((x & 3) || (y & 3))))
        return kActRaiserRedevelopment_InvalidFootprint;
      /* $A004 latches bit7 after action0's one-time initialization. Many
       * completed/off-town records stay in that initialized action0, rather
       * than moving to action1. Neither state proves visual quiescence; the
       * owning transaction must separately retire visual/construction work. */
      if (Removable(type) && (action & 15) != 1 && (action & 0x8f) != 0x80)
        return kActRaiserRedevelopment_StructureBusy;
      for (unsigned dy = 0; dy < width; ++dy) for (unsigned dx = 0; dx < width; ++dx) {
        const unsigned cell = (y + dy) * 32 + x + dx;
        if (occupied[cell / 8] & (1u << (cell & 7))) return kActRaiserRedevelopment_InvalidFootprint;
        occupied[cell / 8] |= 1u << (cell & 7);
        if (Removable(type)) {
          const uint16_t index = ActRaiser_CellMarkIndex(town, x + dx, y + dy);
          if (ReadByte(cpu, kTownBank, kCellMarks + index, &next.fingerprint) != Mark(flags))
            return kActRaiserRedevelopment_MarkMismatch;
        }
      }
      if (Removable(type)) ++next.removed[town];
      if (type == 0) ++next.houses[town];
    }
    /* Census after clearing houses is 2-bias. Zero is also unsafe: native
     * town entry $03:807F treats it as a new town and resets civilization.
     * Preserve the adjustment; incompatible saves need a separate migration. */
    if (bias >= 2) return kActRaiserRedevelopment_PopulationBias;
    if (!next.removed[town]) continue;
    uint16_t price;
    if (!ArRegionalConstruction_Price(japanese_construction, civilization, &price))
      return kActRaiserRedevelopment_GrowthRange;
    /* A bounded reserve floor, not a refund: offscreen construction is free
     * in the native game. Additive house credits could therefore be farmed by
     * repeated offscreen rebuild/reset cycles. Preserve any larger reserve;
     * otherwise cover the removed houses, or one support start. Support
     * normally returns its price on completion. No per-frame replenishment. */
    const unsigned required=(next.houses[town]?next.houses[town]:1)*price;
    const unsigned credit=growth<required?required-growth:0;
    /* $8529 uses signed subtraction to size a normal construction batch. */
    if (growth + credit > 0x7fff)
      return kActRaiserRedevelopment_GrowthRange;
    next.growth_credit[town] = credit;
    next.affected_towns |= 1u << town;
  }
  *plan = next;
  return kActRaiserRedevelopment_Ready;
}

ActRaiserRedevelopmentStatus ActRaiserTownRedevelopment_Apply(
    CpuState *cpu, const ActRaiserTownRedevelopmentPlan *plan) {
  if (!plan) return kActRaiserRedevelopment_Invalid;
  ActRaiserTownRedevelopmentPlan current;
  ActRaiserRedevelopmentStatus status = ActRaiserTownRedevelopment_Preview(
      cpu, plan->requested_towns, plan->japanese_construction, &current);
  if (status != kActRaiserRedevelopment_Ready) return status;
  if (current.fingerprint != plan->fingerprint || current.affected_towns != plan->affected_towns ||
      memcmp(current.removed, plan->removed, sizeof(current.removed)) ||
      memcmp(current.houses, plan->houses, sizeof(current.houses)) ||
      memcmp(current.growth_credit, plan->growth_credit, sizeof(current.growth_credit)))
    return kActRaiserRedevelopment_Stale;
  for (unsigned town = 0; town < kActRaiserRedevelopmentTowns; ++town) {
    if (!(current.affected_towns & (1u << town))) continue;
    const uint16_t base = RecordBase(cpu, town);
    for (unsigned slot = 0; slot < kRecords; ++slot) {
      const uint16_t record = base + slot * kRecordBytes;
      const uint8_t flags = cpu_read8(cpu, kTownBank, record + 2);
      if (!(flags & kActive) || !Removable(flags & 15)) continue;
      const uint8_t x = cpu_read8(cpu, kTownBank, record), y = cpu_read8(cpu, kTownBank, record + 1);
      const unsigned width = Width(flags & 15);
      for (unsigned dy = 0; dy < width; ++dy) for (unsigned dx = 0; dx < width; ++dx)
        cpu_write8(cpu, kTownBank, kCellMarks + ActRaiser_CellMarkIndex(town, x + dx, y + dy), kEmptyCell);
      cpu_write8(cpu, kTownBank, record + 2, 0); /* Native action-7 retirement. */
    }
    const uint16_t address = kGrowth + town * 2;
    cpu_write16(cpu, kTownBank, address, cpu_read16(cpu, kTownBank, address) + current.growth_credit[town]);
  }
  return kActRaiserRedevelopment_Ready;
}
