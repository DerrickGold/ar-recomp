/* Optional bridge-limit enhancement. Completed bridges migrate from each
 * town's 128-record array into checksummed SRAM without losing support,
 * marks, rendering, or crossing state. The four HLE entry points below must
 * remain coordinated. ROM mapping and persistence details live in
 * docs/SEAMS.md and docs/save-format.md. */

#include "actraiser_cell_map.h"
#include "actraiser_town_metatile.h"
#include "actraiser_town_structure_steps.h"
#include "snesrecomp/game/cpu.h"
#include "save_system.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>

enum {
  /* Town-bank ($7F) simulation variables (DB-relative in the ROM). */
  kVar_TownIdWord     = 0x7BF9,  /* current town id, 16-bit */
  kVar_TownIndexWord  = 0x7BFB,  /* town id * 2, used as the DC74 index */
  kVar_AllocSlot      = 0x7C05,  /* allocator: chosen slot index */
  kVar_AllocRemaining = 0x7C1D,  /* allocator: slots left counter */
  kVar_PendingCellX   = 0x7C9D,  /* allocation request: cell X */
  kVar_PendingCellY   = 0x7C9F,  /* allocation request: cell Y */
  kVar_PendingType    = 0x7CA1,  /* allocation request: type byte */
  kVar_MarkCellX      = 0x7C4B,
  kVar_MarkCellY      = 0x7C4D,
  kVar_StructureStepRecordIndex = 0x7BE7,

  kTownRomBank = 0x03,
  kRom_StructListPtrs = 0xDC74,  /* bank $03 data: per-town array bases */

  kStructRecordCount = 128,
  kStructRecordSize  = 4,
  kStructFlag_Active   = 0x80,
  kStructFlag_Building = 0x40,
  kStructFlag_Subtype  = 0x10,
  kStructType_ClassMask = 0x0F,
  kStructType_Bridge    = 0x01,
  kStructRecordCellXOffset = 0,
  kStructRecordCellYOffset = 1,
  kStructRecordActionOffset = 3,
  kBridgeStructureStepClassOffset = 2,
  kStructureStepVariantSourceShift = 3,
  kStructureStepVariantOffsetMask = 0x0E,
  kStructureStepDrawListPointerOffset = sizeof(uint16),
  kStructureStepTerminatorMinimum = 0x00FD,
  kWordHighByteMask = 0xFF00,
  kMaximumBridgeDrawCommands = 16,
};

/* AR_BRIDGEFIX_DEBUG play-test log levels ([bridgefix] on stderr, captured in
 * runs/<ts>/console.log): 1 = bridge migration/validation and table-full
 * failures; 2 = also every structure allocation. */
enum {
  kSramBank            = 0x70,
  kExt_MagicAddr       = 0x1D70,   /* 'A','X','B','1' */
  kExt_RecordsAddr     = 0x1D74,
  kExt_PerTown         = 16,
  kExt_TownCount       = 6,
  kExt_RecordsEnd      = kExt_RecordsAddr + kExt_TownCount * kExt_PerTown * 4,
  kSram_ChecksumRange  = 0x1FEC,   /* c1/c2 cover [0, this) */
  kSram_ChecksumAddr   = 0x1FEC,

  kVar_TownActWords    = 0x6B18,   /* census activity gate, word per town */
  kVar_SupportCapacity = 0x6B26,   /* census output: support, word per town */
  kVar_HousePopBias    = 0x9F57,   /* census output subtraction, per town */
  kWram_PopulationBase = 0x021C,   /* long $00:021C,X population output */
  kVar_RoadMap         = 0x6800,   /* $7F: 6 towns x $80 road-map bytes */
  kVar_TownMapActive   = 0x919E,   /* per-town byte gating bridge marks */
};

enum {
  kStructureMark_House = 0xE0,
  kStructureMark_BridgeHorizontal = 0xE1,
  kStructureMark_BridgeVertical = 0xE2,
  kStructureMark_FactoryTier4 = 0xE3,
  kStructureMark_CornField = 0xE4,
  kStructureMark_WheatField = 0xE5,
  kStructureMark_FactoryTier3 = 0xE6,
  kStructureMark_Class5 = 0xE7,
  kStructureMark_Class6 = 0xE8,
};

static const uint8 kExtMagic[4] = { 'A', 'X', 'B', '1' };

static int bridgefix_debug_level(void) {
  static int level = -1;
  if (level < 0) {
    const char *env = getenv("AR_BRIDGEFIX_DEBUG");
    level = env && *env ? atoi(env) : 0;
    if (level < 0) level = 0;
  }
  return level;
}

static const char *struct_class_name(uint8 flags) {
  static const char *const names[] = {
    "house", "bridge", "field", "factory3", "factory4",
    "class5", "class6", "class7",
  };
  return names[flags & kStructType_ClassMask & 0x07];
}

static uint16 struct_list_base(CpuState *cpu, uint16 town_index_word) {
  return cpu_read16(cpu, kTownRomBank,
                    (uint16)(kRom_StructListPtrs + town_index_word));
}

/* ---- extension area (SRAM) ------------------------------------------- */

static uint16 ext_record_addr(unsigned town, int slot) {
  return (uint16)(kExt_RecordsAddr + (town * kExt_PerTown + slot) * 4);
}

static int ext_area_valid(CpuState *cpu) {
  for (int i = 0; i < 4; i++)
    if (cpu_read8(cpu, kSramBank, (uint16)(kExt_MagicAddr + i)) !=
        kExtMagic[i])
      return 0;
  return 1;
}

static void ext_area_init(CpuState *cpu) {
  for (unsigned t = 0; t < kExt_TownCount; t++)
    for (int i = 0; i < kExt_PerTown; i++)
      for (int b = 0; b < 4; b++)
        cpu_write8(cpu, kSramBank, (uint16)(ext_record_addr(t, i) + b), 0);
  for (int i = 0; i < 4; i++)
    cpu_write8(cpu, kSramBank, (uint16)(kExt_MagicAddr + i), kExtMagic[i]);
}

/* Refresh the live SRAM checksum after sidecar writes. The sidecar and these
 * checksum bytes are then copied into the persistence shadow: they remain
 * session-only until the ROM's $03:A656 save transaction copies the changed
 * native town block into SRAM. That native change makes auto-persist commit
 * the complete live image, including the already-checksummed sidecar. */
static void ext_fix_checksum(CpuState *cpu) {
  uint16 c1 = 0, c2 = 0;
  for (uint16 a = 0; a < kSram_ChecksumRange; a = (uint16)(a + 2)) {
    const uint16 w = cpu_read16(cpu, kSramBank, a);
    c1 ^= w;
    c2 = (uint16)(c2 + w);
  }
  cpu_write16(cpu, kSramBank, kSram_ChecksumAddr, c2);
  cpu_write16(cpu, kSramBank, (uint16)(kSram_ChecksumAddr + 2), c1);
}

static void ext_make_session_only(void) {
  SaveSystem_ResyncShadowRange(kExt_MagicAddr,
                               kExt_RecordsEnd - kExt_MagicAddr);
  SaveSystem_ResyncShadowRange(kSram_ChecksumAddr, 4);
}

static void ext_read_record(CpuState *cpu, unsigned town, int slot,
                            uint8 out[4]) {
  const uint16 ext = ext_record_addr(town, slot);
  for (int b = 0; b < 4; b++)
    out[b] = cpu_read8(cpu, kSramBank, (uint16)(ext + b));
}

static void ext_clear_record(CpuState *cpu, unsigned town, int slot) {
  const uint16 ext = ext_record_addr(town, slot);
  for (int b = 0; b < 4; b++)
    cpu_write8(cpu, kSramBank, (uint16)(ext + b), 0);
}

static int bridge_records_match(const uint8 a[4], const uint8 b[4]) {
  return a[0] == b[0] && a[1] == b[1] &&
      ((a[2] ^ b[2]) & 0x10) == 0;
}

/* Bridge bits live in the road word for the containing 4x4-cell square.
 * Type $01 uses bit $0080; orientation $11 uses $0100. A sidecar record is
 * never authoritative when that native crossing bit is absent. */
static int bridge_has_road_bit(CpuState *cpu, unsigned town,
                               const uint8 rec[4]) {
  if (town >= kExt_TownCount || rec[0] >= 0x20 || rec[1] >= 0x20)
    return 0;
  const uint16 road_addr = (uint16)(kVar_RoadMap + town * 0x80 +
                                    (rec[1] >> 2) * 0x10 +
                                    (rec[0] >> 2) * 2);
  const uint16 road = cpu_read16(cpu, 0x7F, road_addr);
  return (road & ((rec[2] & kStructFlag_Subtype) ? 0x0100 : 0x0080)) != 0;
}

static int bridge_record_valid(CpuState *cpu, unsigned town,
                               const uint8 rec[4]) {
  const uint8 flags = rec[2];
  return (flags & kStructFlag_Active) &&
      !(flags & kStructFlag_Building) &&
      (flags & kStructType_ClassMask) == kStructType_Bridge &&
      bridge_has_road_bit(cpu, town, rec);
}

/* Return only the first valid copy of a bridge. Old v2 builds could append a
 * duplicate every time the same native save was loaded and migrated again. */
static int ext_bridge_get(CpuState *cpu, unsigned town, int slot,
                          uint8 out[4]) {
  if (!ext_area_valid(cpu) || town >= kExt_TownCount ||
      slot < 0 || slot >= kExt_PerTown)
    return 0;
  uint8 rec[4];
  ext_read_record(cpu, town, slot, rec);
  if (!bridge_record_valid(cpu, town, rec)) return 0;
  for (int i = 0; i < slot; i++) {
    uint8 prior[4];
    ext_read_record(cpu, town, i, prior);
    if (bridge_record_valid(cpu, town, prior) &&
        bridge_records_match(rec, prior))
      return 0;
  }
  if (out)
    for (int b = 0; b < 4; b++) out[b] = rec[b];
  return 1;
}

static int main_has_bridge(CpuState *cpu, uint8 db, uint16 base,
                           const uint8 bridge[4]) {
  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 addr = (uint16)(base + i * kStructRecordSize);
    uint8 rec[4];
    for (int b = 0; b < 4; b++)
      rec[b] = cpu_read8(cpu, db, (uint16)(addr + b));
    if ((rec[2] & kStructFlag_Active) &&
        (rec[2] & kStructType_ClassMask) == kStructType_Bridge &&
        bridge_records_match(rec, bridge))
      return 1;
  }
  return 0;
}

/* Move every completed bridge record of the current town into the extension
 * area. The operation is idempotent: invalid/stale/duplicate sidecar records
 * are reclaimed, and a native bridge already represented in the sidecar is
 * simply removed from the native table. Bridges mid-construction stay put;
 * a full extension area degrades gracefully to authentic behavior. */
static int ext_migrate_bridges(CpuState *cpu, uint8 db, unsigned town,
                               uint16 base) {
  if (town >= kExt_TownCount) return 0;
  int migrated = 0;
  int sram_changed = 0;

  int has_bridge = 0;
  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 rec = (uint16)(base + i * kStructRecordSize);
    const uint8 flags = cpu_read8(cpu, db, (uint16)(rec + 2));
    if ((flags & kStructFlag_Active) &&
        !(flags & kStructFlag_Building) &&
        (flags & kStructType_ClassMask) == kStructType_Bridge) {
      has_bridge = 1;
      break;
    }
  }
  if (!ext_area_valid(cpu)) {
    if (!has_bridge) return 0;
    ext_area_init(cpu);
    sram_changed = 1;
  }

  /* Reclaim records that cannot describe a completed native bridge in the
   * current road map, along with later copies of an earlier valid record. */
  for (int i = 0; i < kExt_PerTown; i++) {
    uint8 rec[4];
    ext_read_record(cpu, town, i, rec);
    if (rec[2] == 0) continue;
    int keep = bridge_record_valid(cpu, town, rec);
    for (int j = 0; keep && j < i; j++) {
      uint8 prior[4];
      ext_read_record(cpu, town, j, prior);
      if (bridge_record_valid(cpu, town, prior) &&
          bridge_records_match(rec, prior))
        keep = 0;
    }
    if (!keep) {
      if (bridgefix_debug_level() >= 1)
        fprintf(stderr,
                "[bridgefix] town %u: reclaimed invalid/duplicate "
                "extension slot %d (cell %u,%u, flags $%02X)\n",
                town, i, (unsigned)rec[0], (unsigned)rec[1],
                (unsigned)rec[2]);
      ext_clear_record(cpu, town, i);
      sram_changed = 1;
    }
  }

  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 rec = (uint16)(base + i * kStructRecordSize);
    const uint8 flags = cpu_read8(cpu, db, (uint16)(rec + 2));
    if (!(flags & kStructFlag_Active) || (flags & kStructFlag_Building) ||
        (flags & kStructType_ClassMask) != kStructType_Bridge)
      continue;
    uint8 native[4];
    for (int b = 0; b < 4; b++)
      native[b] = cpu_read8(cpu, db, (uint16)(rec + b));
    /* A malformed native record is safer left in the table: without the
     * crossing bit, the sidecar would have no independent existence proof. */
    if (!bridge_record_valid(cpu, town, native)) continue;

    int ext_slot = -1;
    int matching_slot = -1;
    for (int j = 0; j < kExt_PerTown; j++) {
      uint8 ext_rec[4];
      ext_read_record(cpu, town, j, ext_rec);
      if (ext_rec[2] == 0 && ext_slot < 0) ext_slot = j;
      if (bridge_record_valid(cpu, town, ext_rec) &&
          bridge_records_match(native, ext_rec)) {
        matching_slot = j;
        break;
      }
    }
    if (matching_slot >= 0) {
      ext_slot = matching_slot;
    } else if (ext_slot >= 0) {
      const uint16 ext = ext_record_addr(town, ext_slot);
      for (int b = 0; b < 4; b++)
        cpu_write8(cpu, kSramBank, (uint16)(ext + b), native[b]);
      sram_changed = 1;
    } else {
      continue;
    }

    const uint16 ext = ext_record_addr(town, ext_slot);
    for (int b = 0; b < 4; b++)
      cpu_write8(cpu, db, (uint16)(rec + b), 0);
    if (bridgefix_debug_level() >= 1) {
      fprintf(stderr,
              "[bridgefix] town %u: %s bridge slot %d (cell %u,%u, "
              "flags $%02X) in extension slot %d — record slot freed\n",
              town, matching_slot >= 0 ? "deduplicated" : "migrated", i,
              (unsigned)cpu_read8(cpu, kSramBank, ext),
              (unsigned)cpu_read8(cpu, kSramBank, (uint16)(ext + 1)),
              (unsigned)cpu_read8(cpu, kSramBank, (uint16)(ext + 2)),
              ext_slot);
    }
    migrated++;
  }
  if (sram_changed) {
    ext_fix_checksum(cpu);
    ext_make_session_only();
  }
  return migrated;
}

static int ext_bridge_count(CpuState *cpu, uint8 db, unsigned town,
                            uint16 base) {
  if (!ext_area_valid(cpu)) return 0;
  int n = 0;
  for (int i = 0; i < kExt_PerTown; i++) {
    uint8 rec[4];
    if (ext_bridge_get(cpu, town, i, rec) &&
        !main_has_bridge(cpu, db, base, rec))
      n++;
  }
  return n;
}

/* Render the initial draw command of the native bridge rebuild program.
 * $9F8D derives the variant from record byte 3; $A4A8 selects the bridge
 * program table (type index 2); and $A591 executes the count + {dx,dy,tile}
 * command list. Completed bridge programs begin with one ordinary timed draw
 * followed by $FD, so a direct restamp is visually identical without
 * allocating one of $9D4D's 128 animation-step entries. */
static int draw_extension_bridge(CpuState *cpu, uint8 db,
                                 const uint8 rec[4]) {
  const uint16 variant_offset = (uint16)(
      (rec[kStructRecordActionOffset] >>
       kStructureStepVariantSourceShift) &
      kStructureStepVariantOffsetMask);
  const uint16 program = ActRaiser_ResolveTownStructureStepProgram(
      cpu, kBridgeStructureStepClassOffset, variant_offset,
      kActRaiserTownStructureStepProgramFamily_Rebuild);
  const uint16 first_command = cpu_read16(
      cpu, kTownRomBank, program);
  if (first_command >= kStructureStepTerminatorMinimum) return 0;

  const uint16 draw_list = cpu_read16(
      cpu, kTownRomBank,
      (uint16)(program + kStructureStepDrawListPointerOffset));
  /* Native bridge lists contain one command. A small ceiling makes a damaged
   * sidecar fail closed instead of interpreting arbitrary ROM as a list. */
  return ActRaiser_ExecuteTownDrawList(
      cpu, db, rec[kStructRecordCellXOffset],
      rec[kStructRecordCellYOffset], draw_list,
      kMaximumBridgeDrawCommands);
}

/* Set N/Z/C exactly; every exit of both replaced routines ends on an
 * operation that produced Z=1, N=0 (LDA #$00 or the loop counter reaching
 * zero), with carry carrying the result. */
static void bugfix_exit_flags(CpuState *cpu, int carry) {
  cpu->_flag_C = carry ? 1 : 0;
  /* The ROM exits through REP #$20 after an 8-bit zero-producing operation:
   * N=0, M=0, Z=1, C=result. X width and all unrelated flags are preserved. */
  cpu->P = (uint8)((cpu->P & ~0xA3) | 0x02 | (carry ? 0x01 : 0));
  cpu_p_to_mirrors(cpu);
}

/* Faithful HLE of $03:9D9F — allocate a structure record in the current
 * town's 128-slot array. Inputs: DB-relative $7C9D/$7C9F/$7CA1 (cell X/Y and
 * type byte), $7BFB (town*2). Success: record written, $7C05 = slot index,
 * X = record pointer, carry clear. Failure: carry set.
 *
 * fix_bridge_limit extension: before the authentic scan, migrate completed
 * bridges to the bridge-specific sidecar. The scan then naturally finds the
 * vacated records without changing its capacity or allocation rules. */
RecompReturn ActRaiser_AllocStructureRecord(CpuState *cpu) {
  const uint8 db = cpu->DB;
  const uint16 town_index = cpu_read16(cpu, db, kVar_TownIndexWord);
  const uint16 base = struct_list_base(cpu, town_index);

  /* v2 fix_bridge_limit: lazily migrate completed bridges into the SRAM
   * extension area so the authentic scan below finds their vacated slots.
   * The bridge stays fully alive — road-map bit, census support, and cell
   * marks are all carried by the other v2 hooks. */
  if (g_settings.fix_bridge_limit)
    ext_migrate_bridges(cpu, db, (unsigned)(town_index >> 1), base);

  int slot = -1;
  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 rec = (uint16)(base + i * kStructRecordSize);
    if (!(cpu_read8(cpu, db, (uint16)(rec + 2)) & kStructFlag_Active)) {
      slot = i;
      break;
    }
  }

  const int debug = bridgefix_debug_level();
  const uint8 new_type = cpu_read8(cpu, db, kVar_PendingType);
  const int new_is_bridge =
      (new_type & kStructType_ClassMask) == kStructType_Bridge;

  if (slot >= 0) {
    const uint16 rec = (uint16)(base + slot * kStructRecordSize);
    if (debug >= 2 || (debug >= 1 && new_is_bridge)) {
      fprintf(stderr,
              "[bridgefix] town %u: alloc slot %3d = %s $%02X at cell "
              "%u,%u (%d slots left)\n",
              (unsigned)(town_index >> 1), slot,
              struct_class_name(new_type), (unsigned)new_type,
              (unsigned)cpu_read8(cpu, db, kVar_PendingCellX),
              (unsigned)cpu_read8(cpu, db, kVar_PendingCellY),
              kStructRecordCount - slot - 1);
    }
    cpu_write8(cpu, db, rec, cpu_read8(cpu, db, kVar_PendingCellX));
    cpu_write8(cpu, db, (uint16)(rec + 1),
               cpu_read8(cpu, db, kVar_PendingCellY));
    cpu_write8(cpu, db, (uint16)(rec + 2),
               (uint8)(new_type | kStructFlag_Active));
    cpu_write8(cpu, db, (uint16)(rec + 3), 0);
    cpu_write16(cpu, db, kVar_AllocSlot, (uint16)slot);
    cpu_write16(cpu, db, kVar_AllocRemaining,
                (uint16)(kStructRecordCount - slot));
    cpu->X = rec;
    cpu->A = (uint16)(base & 0xFF00);
    bugfix_exit_flags(cpu, 0);
  } else {
    if (debug >= 1) {
      fprintf(stderr,
              "[bridgefix] town %u: TABLE FULL — %s $%02X at cell %u,%u "
              "NOT built (authentic 128-structure cap)\n",
              (unsigned)(town_index >> 1),
              struct_class_name(new_type), (unsigned)new_type,
              (unsigned)cpu_read8(cpu, db, kVar_PendingCellX),
              (unsigned)cpu_read8(cpu, db, kVar_PendingCellY));
    }
    cpu_write16(cpu, db, kVar_AllocSlot, kStructRecordCount);
    cpu_write16(cpu, db, kVar_AllocRemaining, 0);
    cpu->X = (uint16)(base + kStructRecordCount * kStructRecordSize);
    cpu->A = (uint16)((base & 0xFF00) |
                      cpu_read8(cpu, db, (uint16)(cpu->X - 2)));
    bugfix_exit_flags(cpu, 1);
  }

  cpu->S = (uint16)(cpu->S + 2);  /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}

/* Faithful HLE of $03:C07E — the town population/support census. Iterates
 * the record table and stores: population (per-house people by civ subtype:
 * $20 -> 8, $10 -> 6, else 4; +2; minus $9F57,X) into long $00:021C+town*2,
 * and support capacity (completed class 2 = 32/48 by wheat bit, class 3 = 72
 * unless building, class 4 = 72, all other classes = 32) into $6B26+town*2.
 * Towns with no act completions ($6B18,X == 0) skip the store entirely.
 *
 * v2 extension: +32 support per extension bridge, counted regardless of the
 * toggle so disabling fix_bridge_limit never collapses a town's support. */
RecompReturn ActRaiser_TownCensus(CpuState *cpu) {
  const uint8 db = cpu->DB;
  const uint8 saved_p = cpu->P;      /* PHP */
  const uint16 saved_x = cpu->X;     /* PHX */
  uint16 native_y = cpu->Y;          /* Y is not saved by the ROM */

  uint16 population = 0, support = 0;
  cpu_write16(cpu, db, kVar_AllocSlot, 0);       /* STZ $7C05 */
  cpu_write16(cpu, db, 0x7C07, 0);               /* STZ $7C07 */
  cpu_write16(cpu, db, kVar_AllocRemaining, kStructRecordCount);

  const uint16 town_index = cpu_read16(cpu, db, kVar_TownIndexWord);
  const uint16 active =
      cpu_read16(cpu, db, (uint16)(kVar_TownActWords + town_index));
  uint16 exit_a;
  if (active != 0) {
    const uint16 base = struct_list_base(cpu, town_index);
    for (int i = 0; i < kStructRecordCount; i++) {
      const uint16 rec = (uint16)(base + i * kStructRecordSize);
      const uint8 f2 = cpu_read8(cpu, db, (uint16)(rec + 2));
      if (!(f2 & kStructFlag_Active)) continue;
      const uint8 cls = f2 & kStructType_ClassMask;
      if (cls == 0) {
        const uint8 sub = f2 & 0x30;
        population = (uint16)(population +
                              (sub == 0x20 ? 8 : sub == 0x10 ? 6 : 4));
      } else {
        uint16 add = 0x20;
        if (cls == 2)
          add = (f2 & kStructFlag_Building) ? 0
                : (f2 & kStructFlag_Subtype) ? 0x30 : 0x20;
        else if (cls == 3)
          add = (f2 & kStructFlag_Building) ? 0 : 0x48;
        else if (cls == 4)
          add = 0x48;
        native_y = add;
        support = (uint16)(support + add);
      }
    }
    support = (uint16)(support +
                       0x20 * (uint16)ext_bridge_count(
                                  cpu, db, (unsigned)(town_index >> 1), base));
    cpu_write16(cpu, db, kVar_AllocSlot, population);
    cpu_write16(cpu, db, 0x7C07, support);
    cpu_write16(cpu, db, kVar_AllocRemaining, 0);  /* loop counter spent */
    const uint16 bias =
        cpu_read16(cpu, db, (uint16)(kVar_HousePopBias + town_index));
    cpu_write16(cpu, 0x00, (uint16)(kWram_PopulationBase + town_index),
                (uint16)(population + 2 - bias));
    cpu_write16(cpu, db, (uint16)(kVar_SupportCapacity + town_index),
                support);
    exit_a = support;                /* last LDA $7C07 before the store */
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

/* Faithful HLE of $03:9CFB — the construction-scene MARKS pass. Rewrites
 * the current town's per-cell structure marks in the $7F:2000 map from the
 * record table: one byte via the $9FCD shape (houses $E0, bridges $E1/$E2
 * by orientation — gated on the $919E,Y town-map-active byte — class 6
 * $E8) or a 2x2 block via the $9FE4 shape (+0/+1/+$10/+$11: fields
 * $E4/$E5 by the wheat bit, class 3 $E6, class 4 $E3, class 5 $E7).
 *
 * v2 extension: after the 128 records, extension bridges are marked with
 * the same gate and codes, so migrated bridges keep their tiles across
 * every construction-event redraw. */
RecompReturn ActRaiser_SceneMarkStructures(CpuState *cpu) {
  const uint8 db = cpu->DB;
  const uint16 town_index = cpu_read16(cpu, db, kVar_TownIndexWord);
  const unsigned town = (unsigned)(town_index >> 1);
  const uint16 base = struct_list_base(cpu, town_index);
  /* $9F73's gate: LDY $7BF9 (town id, NOT doubled); LDA $919E,Y. */
  const uint8 map_active =
      cpu_read8(cpu, db, (uint16)(kVar_TownMapActive + town));

  uint16 native_y = cpu->Y;
  uint16 a_high = (uint16)(base & 0xFF00);
  for (int i = 0; i < kStructRecordCount; i++) {
    const uint16 rec = (uint16)(base + i * kStructRecordSize);
    const uint8 f2 = cpu_read8(cpu, db, (uint16)(rec + 2));
    if (!(f2 & kStructFlag_Active)) continue;
    /* The scanner pushes $9D3B as its handler continuation for every active
     * record. Only the bridge handler replaces Y with the current town. */
    native_y = 0x9D3B;
    const uint8 x = cpu_read8(cpu, db, rec);
    const uint8 y = cpu_read8(cpu, db, (uint16)(rec + 1));
    uint8 code = 0;
    ActRaiserTownStructureMarkShape mark_shape =
        kActRaiserTownStructureMarkShape_Cell;
    switch (f2 & kStructType_ClassMask) {
      case 0: code = kStructureMark_House; break;          /* $9F1A */
      case 1:                                              /* $9F73 */
        native_y = (uint16)town;
        if (!map_active) continue;
        code = (f2 & kStructFlag_Subtype)
            ? kStructureMark_BridgeHorizontal
            : kStructureMark_BridgeVertical;
        break;
      case 2:
        code = (f2 & kStructFlag_Subtype)
            ? kStructureMark_WheatField
            : kStructureMark_CornField;
        mark_shape = kActRaiserTownStructureMarkShape_Block2x2;
        break;
      case 3:
        code = kStructureMark_FactoryTier3;
        mark_shape = kActRaiserTownStructureMarkShape_Block2x2;
        break;                                             /* $9F3F */
      case 4:
        code = kStructureMark_FactoryTier4;
        mark_shape = kActRaiserTownStructureMarkShape_Block2x2;
        break;                                             /* $9F4C */
      case 5:
        code = kStructureMark_Class5;
        mark_shape = kActRaiserTownStructureMarkShape_Block2x2;
        break;                                             /* $9F59 */
      case 6: code = kStructureMark_Class6; break;         /* $9F66 */
      default: continue;
    }
    /* Exact $9FCD/$9FE4 -> $9710 scratch and A-high side effects. The
     * quadrant contributes to X/the full mark index, but not to $7C05. */
    cpu_write8(cpu, db, kVar_MarkCellX, x);
    cpu_write8(cpu, db, kVar_MarkCellY, y);
    cpu_write16(cpu, db, kVar_AllocSlot,
                ActRaiser_CellMarkPreQuadrantIndex(town, x, y));
    const uint16 idx = ActRaiser_WriteTownStructureMark(
        cpu, db, town, x, y, code, mark_shape);
    a_high = (uint16)(idx & kWordHighByteMask);
  }

  int ext_marked = 0;
  if (map_active && ext_area_valid(cpu)) {
    for (int i = 0; i < kExt_PerTown; i++) {
      uint8 rec[4];
      if (!ext_bridge_get(cpu, town, i, rec) ||
          main_has_bridge(cpu, db, base, rec))
        continue;
      const uint8 code = (rec[2] & kStructFlag_Subtype)
          ? kStructureMark_BridgeHorizontal
          : kStructureMark_BridgeVertical;
      ActRaiser_WriteTownStructureMark(
          cpu, db, town, rec[0], rec[1], code,
          kActRaiserTownStructureMarkShape_Cell);
      ext_marked++;
    }
  }
  if (ext_marked && bridgefix_debug_level() >= 1)
    fprintf(stderr,
            "[bridgefix] town %u: construction redraw restamped %d "
            "extension bridge(s)\n", town, ext_marked);

  /* Exit state of the original loop. Extension marking above deliberately
   * has no architectural side effects: it behaves as a post-pass. */
  cpu_write8(cpu, db, kVar_StructureStepRecordIndex, kStructRecordCount);
  cpu->X = (uint16)(base + kStructRecordCount * kStructRecordSize);
  cpu->Y = native_y;
  cpu->A = (uint16)(a_high | kStructRecordCount);
  cpu->_flag_C = 1;
  /* CMP #$80 produced N=0,Z=1,C=1; REP #$20 clears M only. Preserve X. */
  cpu->P = (uint8)((cpu->P & ~0xA3) | 0x03);
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16)(cpu->S + 2);   /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}

/* Faithful HLE of the tiny $03:89F0 scene-finish leaf (LDA #$FFFF;
 * STA $7C97; RTS), with a sidecar-only rendering post-pass inserted before
 * its native state change. $8053 calls this immediately after $9D4D stages
 * and draws the main table's reconstruction entries and $BB94 ticks them.
 * Replaying each validated sidecar bridge's native initial draw command here
 * gives recordless bridges the same artwork without borrowing a structure or
 * animation-step slot. */
RecompReturn ActRaiser_FinishSceneBridgeRendering(CpuState *cpu) {
  const uint8 db = cpu->DB;
  const uint16 town_index = cpu_read16(cpu, db, kVar_TownIndexWord);
  const unsigned town = (unsigned)(town_index >> 1);
  const uint16 base = struct_list_base(cpu, town_index);
  int rendered = 0;

  if (ext_area_valid(cpu) && town < kExt_TownCount) {
    for (int i = 0; i < kExt_PerTown; i++) {
      uint8 rec[4];
      if (!ext_bridge_get(cpu, town, i, rec) ||
          main_has_bridge(cpu, db, base, rec))
        continue;
      rendered += draw_extension_bridge(cpu, db, rec);
    }
  }
  if (rendered && bridgefix_debug_level() >= 1)
    fprintf(stderr,
            "[bridgefix] town %u: reconstruction rendered %d extension "
            "bridge(s)\n", town, rendered);

  cpu_write16(cpu, db, 0x7C97, 0xFFFF);  /* native STA $7C97 */
  cpu->A = 0xFFFF;
  /* LDA #$FFFF sets N and clears Z. Preserve C, M/X, and other flags. */
  cpu->P = (uint8)((cpu->P & ~0x82) | 0x80);
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16)(cpu->S + 2);         /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}
