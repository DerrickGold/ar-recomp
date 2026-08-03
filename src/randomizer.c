#include "randomizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

/* ---------------------------------------------------------------- ROM layout
 *
 * All addresses below are documented in docs/SEAMS.md "Content / randomizer
 * seams" and dumped by tools/act_content.py, which is the reference decoder
 * for everything this file rewrites. Offsets are LINEAR file offsets, matching
 * RomFixedPtr's LoROM mapping: file = (bank << 15) | (addr & 0x7fff).
 */
#define ROM_SIZE_EXPECTED 0x100000u

static uint32 LoRom(uint8 bank, uint16 addr) {
  return ((uint32)bank << 15) | (addr & 0x7fffu);
}

/* $00:95DD — eight per-region object-type table pointers, $18 = 0..7. */
static const uint16 kTypeTables[8] = {
  0x96AF, 0xA8F6, 0xB449, 0xC11E, 0xCD9B, 0xD928, 0xE722, 0xF39A,
};

/* Spawn-record byte offsets ($00:95F0 copies these into the object). */
enum {
  kRecFlags = 4,      /* word: $0001 attacker, $0200 pickup, $4000 boss */
  kRecAtk   = 7,      /* -> obj +$2A */
  kRecHp    = 8,      /* -> obj +$2C */
};

/* Bank $0A level index and placement-stream opcodes. */
#define LEVEL_INDEX_ADDR 0xB100
enum {
  kOpEnd      = 0xFF,
  kOpWaveGate = 0xFE,
  kOpReserve  = 0xFD,
  kOpGoto     = 0xFC,
  kTypeStatue = 0x80,   /* the item statue; $38 carries the item id */
  kItemCount  = 8,      /* $00:879D handles ids 0..7 */
};

/* $03:B825 — 24 lair records x 9 bytes, 4 per town. */
#define LAIR_TABLE_ADDR 0xB825
enum { kLairCount = 24, kLairStride = 9, kLairPerTown = 4 };
enum { kLairCellX = 0, kLairCellY = 1, kLairType = 3 };

/* Act-2 entry sub-map per region $01..$06 (ram-map $7E:0019; the same split the
 * professional-mode order table $02:9013 enumerates). Index by $18. */
static const uint8 kAct2Entry[8] = { 0, 2, 2, 3, 4, 4, 5, 0 };

/* --------------------------------------------------------------------- state */

static uint8 *g_rom_live;
static uint8 *g_rom_pristine;
static uint32 g_rom_size;
static RandomizerSummary g_summary;

bool Randomizer_IsAvailable(void) { return g_rom_pristine != NULL; }
const RandomizerSummary *Randomizer_LastSummary(void) { return &g_summary; }

/* ----------------------------------------------------------------------- rng
 *
 * splitmix64, streamed per pass. Each pass derives its own stream from
 * (seed, pass id) so enabling one pass never shifts the numbers another pass
 * draws — a seed's meaning for "statue drops" stays put when you toggle
 * "enemy types" on. That is what makes seeds shareable.
 */
typedef struct { uint64 s; } Rng;

static void RngSeed(Rng *r, uint32 seed, uint32 pass) {
  r->s = ((uint64)seed << 32) ^ (uint64)pass ^ 0x9E3779B97F4A7C15ull;
}

static uint64 RngNext(Rng *r) {
  uint64 z = (r->s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/* Unbiased below 2^32; the modulo skew is negligible for our tiny ranges but
 * rejection is cheap and makes the seed->result mapping exact. */
static uint32 RngBelow(Rng *r, uint32 n) {
  if (n <= 1) return 0;
  uint32 limit = 0xFFFFFFFFu - (0xFFFFFFFFu % n) - 1;
  for (;;) {
    uint32 v = (uint32)(RngNext(r) >> 32);
    if (v <= limit) return v % n;
  }
}

/* Fisher-Yates over an index array. */
static void RngShuffle(Rng *r, int *idx, int count) {
  for (int i = count - 1; i > 0; i--) {
    int j = (int)RngBelow(r, (uint32)(i + 1));
    int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
}

/* ------------------------------------------------------------- rom accessors */

static uint8 RomU8(uint32 off) {
  return (off < g_rom_size) ? g_rom_live[off] : 0;
}
static uint16 RomU16(uint32 off) {
  return (uint16)(RomU8(off) | ((uint16)RomU8(off + 1) << 8));
}
static void RomWrite8(uint32 off, uint8 v) {
  if (off < g_rom_size) g_rom_live[off] = v;
}

/* --------------------------------------------------------- pass 1: enemy stats
 *
 * Scale ATK ($2A) and HP ($2C) in every record of every region table. Records
 * with the pickup flag ($0200) are skipped: their "HP 1" is the one sword hit
 * that breaks a statue, not durability, and their ATK 0 is not damage.
 *
 * CAVEAT (SEAMS §1): about a dozen handlers overwrite $2C at runtime for phase
 * changes and sub-object spawns. Those bosses will ignore this pass. The
 * summary counts records rewritten, not enemies actually affected.
 */
static void ScaleStat(uint32 off, int percent) {
  if (percent == 100) return;
  int v = RomU8(off);
  if (v == 0) return;                       /* 0 means "deals/has nothing" */
  int scaled = (v * percent + 50) / 100;
  if (scaled < 1) scaled = 1;               /* never silently delete a stat */
  if (scaled > 255) scaled = 255;
  RomWrite8(off, (uint8)scaled);
}

/* Walk one region's object-type table. The tables carry no count: the nearest
 * forward pointer target bounds them, and a zero word is an unused type slot
 * rather than a terminator (SEAMS "Object & spawn-handler model"). */
static void ForEachRecord(int region, void (*fn)(uint32 rec_off, void *ctx),
                          void *ctx) {
  uint16 base = kTypeTables[region];
  uint16 a = base;
  uint32 end = 0x10000u;
  uint32 hard = (uint32)base + 0x100u;
  while (a < end && a < hard) {
    uint16 ptr = RomU16(LoRom(0x00, a));
    a = (uint16)(a + 2);
    if (ptr == 0) continue;
    if (!(ptr > base)) break;
    if (ptr < end) end = ptr;
    fn(LoRom(0x00, ptr), ctx);
  }
}

typedef struct { int hp_percent, atk_percent, count; } StatCtx;

static void ApplyStatsToRecord(uint32 rec, void *vctx) {
  StatCtx *c = (StatCtx *)vctx;
  uint16 flags = RomU16(rec + kRecFlags);
  if (flags & 0x0200) return;               /* pickup/statue, not an enemy */
  ScaleStat(rec + kRecAtk, c->atk_percent);
  ScaleStat(rec + kRecHp, c->hp_percent);
  c->count++;
}

static void PassEnemyStats(int hp_percent, int atk_percent) {
  if (hp_percent == 100 && atk_percent == 100) return;
  StatCtx ctx = { hp_percent, atk_percent, 0 };
  for (int r = 0; r < 8; r++) ForEachRecord(r, ApplyStatsToRecord, &ctx);
  g_summary.enemy_records = ctx.count;
}

/* ------------------------------------------------------- placement stream walk
 *
 * One collected placement entry. `off` is the linear offset of its 4-byte
 * record, so a pass can rewrite bytes in place.
 */
typedef struct {
  uint32 off;
  uint8 tx, ty, param, type;
  uint8 wave;      /* which $FE-gated batch this entry belongs to */
} Placement;

enum { kMaxPlacements = 256 };

typedef struct {
  Placement items[kMaxPlacements];
  int count;
} PlacementList;

/* Collect every placement of one map, following the stream opcodes. A wave gate
 * ($FE) does not end the map: the rest of the list is spawned later by $00:A813
 * from the stashed cursor, so the entries after it are still this map's. */
static void CollectPlacements(uint16 stream_addr, PlacementList *out) {
  uint16 y = stream_addr;
  uint16 seen[64];
  int seen_n = 0;
  uint8 wave = 0;
  for (int guard = 0; guard < 256; guard++) {
    for (int i = 0; i < seen_n; i++) if (seen[i] == y) return;   /* cycle guard */
    if (seen_n < 64) seen[seen_n++] = y;
    for (int inner = 0; inner < kMaxPlacements * 4; inner++) {
      uint8 op = RomU8(LoRom(0x0A, y));
      if (op == kOpEnd) return;
      if (op == kOpWaveGate) { y = (uint16)(y + 5); wave++; break; }
      if (op == kOpReserve)  { y = (uint16)(y + 2); continue; }
      if (op == kOpGoto)     { y = RomU16(LoRom(0x0A, (uint16)(y + 1))); break; }
      if (out->count < kMaxPlacements) {
        Placement *p = &out->items[out->count++];
        p->off   = LoRom(0x0A, y);
        p->tx    = RomU8(p->off);
        p->ty    = RomU8(p->off + 1);
        p->param = RomU8(p->off + 2);
        p->type  = RomU8(p->off + 3);
        p->wave  = wave;
      }
      y = (uint16)(y + 4);
    }
  }
}

/* Iterate the $0A:B100 index. Returns the number of maps visited. */
typedef void (*MapFn)(uint8 mode, uint8 sub, uint16 stream_addr, void *ctx);

static int ForEachMap(MapFn fn, void *ctx) {
  int maps = 0;
  /* The stock index is 50 entries and $FFFF-terminated. The cap is not for the
   * stock image: it stops a degenerate one (all zeroes, a truncated dump) from
   * spinning here forever, since `a` is 16-bit and would wrap rather than run
   * off the end. Every walk in this file is bounded for the same reason. */
  for (uint16 a = LEVEL_INDEX_ADDR, guard = 0; guard < 256;
       a = (uint16)(a + 4), guard++) {
    uint16 key = RomU16(LoRom(0x0A, a));
    if (key == 0xFFFF) break;
    uint16 base = (uint16)(LEVEL_INDEX_ADDR + RomU16(LoRom(0x0A, (uint16)(a + 2))));
    uint8 mode = (uint8)(key & 0xFF), sub = (uint8)(key >> 8);
    if (mode == 0) continue;                       /* not an action map */
    /* Skip the header: player start (3B) then $FF-terminated 5-byte terrain
     * damage boxes. What follows is the placement list. */
    uint16 y = (uint16)(base + 3);
    int boxes = 0;
    while (RomU8(LoRom(0x0A, y)) != kOpEnd) {
      if (++boxes > 256) break;            /* degenerate image; see above */
      y = (uint16)(y + 5);
    }
    if (boxes > 256) continue;
    y = (uint16)(y + 1);
    fn(mode, sub, y, ctx);
    maps++;
  }
  return maps;
}

/* ------------------------------------------------- pass 2/3: statue drops+spots
 *
 * Both live in the same 4-byte placement record: byte 2 is the item id that
 * $00:879D will apply, bytes 0/1 are the tile. Shuffling permutes the values
 * a map already has; Random draws fresh ones. Position is shuffle-only —
 * "somewhere a statue already stood" is what keeps it off a collision oracle.
 */
typedef struct {
  RandomizerMode drops;
  RandomizerMode spots;
  uint32 seed;
} StatueCtx;

static void StatuePass(uint8 mode, uint8 sub, uint16 stream, void *vctx) {
  StatueCtx *c = (StatueCtx *)vctx;
  PlacementList list;
  list.count = 0;
  CollectPlacements(stream, &list);

  int idx[kMaxPlacements], n = 0;
  for (int i = 0; i < list.count; i++)
    if (list.items[i].type == kTypeStatue && n < kMaxPlacements) idx[n++] = i;
  if (n == 0) return;

  /* Per-map stream so one map's result never depends on another's count. */
  uint32 tag = ((uint32)mode << 8) | sub;
  Rng rng;

  if (c->drops != kRandomMode_Off) {
    RngSeed(&rng, c->seed, 0x0D200000u ^ tag);
    if (c->drops == kRandomMode_Shuffle) {
      int order[kMaxPlacements];
      for (int i = 0; i < n; i++) order[i] = i;
      RngShuffle(&rng, order, n);
      uint8 vals[kMaxPlacements];
      for (int i = 0; i < n; i++) vals[i] = list.items[idx[i]].param;
      for (int i = 0; i < n; i++) {
        uint8 v = vals[order[i]];
        if (v != list.items[idx[i]].param) g_summary.statue_drops++;
        RomWrite8(list.items[idx[i]].off + 2, v);
      }
    } else {
      for (int i = 0; i < n; i++) {
        uint8 v = (uint8)RngBelow(&rng, kItemCount);
        if (v != list.items[idx[i]].param) g_summary.statue_drops++;
        RomWrite8(list.items[idx[i]].off + 2, v);
      }
    }
  }

  /* Positions permute WITHIN A WAVE, not across the whole map. A $FE gate
   * despawns everything before it ($00:A813 -> $00:874E) and moves the
   * checkpoint forward, so a statue relocated from a later wave to an earlier
   * wave's position would spawn behind a checkpoint the player has already
   * passed. Same-wave swaps are always still reachable. */
  if (c->spots != kRandomMode_Off && n > 1) {
    RngSeed(&rng, c->seed, 0x5D070000u ^ tag);
    for (int w = 0; w <= 255; w++) {
      int wid[kMaxPlacements], wn = 0;
      for (int i = 0; i < n; i++)
        if (list.items[idx[i]].wave == (uint8)w) wid[wn++] = idx[i];
      if (wn == 0) { if (w > list.items[idx[n - 1]].wave) break; continue; }
      if (wn < 2) continue;
      int order[kMaxPlacements];
      for (int i = 0; i < wn; i++) order[i] = i;
      RngShuffle(&rng, order, wn);
      uint8 xs[kMaxPlacements], ys[kMaxPlacements];
      for (int i = 0; i < wn; i++) {
        xs[i] = list.items[wid[i]].tx;
        ys[i] = list.items[wid[i]].ty;
      }
      for (int i = 0; i < wn; i++) {
        uint8 nx = xs[order[i]], ny = ys[order[i]];
        if (nx != list.items[wid[i]].tx || ny != list.items[wid[i]].ty)
          g_summary.statue_moves++;
        RomWrite8(list.items[wid[i]].off + 0, nx);
        RomWrite8(list.items[wid[i]].off + 1, ny);
      }
    }
  }
}

/* ------------------------------------------------------ pass 4: enemy types
 *
 * Permute the type byte among ordinary placements. Scope matters: a type index
 * means something different in each region's table, and its frames resolve
 * against the per-ACT animation blob at $7E:4000. Map scope is trivially safe;
 * Act scope is the widest safe scope, because all maps of one act share a blob
 * (SEAMS §5 — only 13 maps load one, at the act entries).
 *
 * Excluded from the permutation: statues ($80 and every other bit-7 "common"
 * type, which resolve against a different table entirely), and bosses//special
 * records, identified by the $4000 boss flag on their type record. Moving a
 * boss would need its boss blob moved too.
 */
typedef struct {
  RandomizerScope scope;
  uint32 seed;
  /* Act-scope accumulation: entries collected across the act's maps. */
  uint32 offs[kMaxPlacements * 4];
  uint8 types[kMaxPlacements * 4];
  int n;
  uint8 cur_mode, cur_act;
} TypeCtx;

/* A type is movable when it is region-scoped (bit 7 clear) and its record does
 * not carry the boss flag. Unused/zero table slots are not movable either. */
static bool TypeIsMovable(uint8 region, uint8 type) {
  if (type & 0x80) return false;
  uint16 base = kTypeTables[region & 7];
  uint16 slot = (uint16)(base + (uint16)(type & 0x7F) * 2);
  if (slot >= (uint16)(base + 0x100)) return false;
  uint16 ptr = RomU16(LoRom(0x00, slot));
  if (ptr == 0 || ptr <= base) return false;
  return (RomU16(LoRom(0x00, ptr) + kRecFlags) & 0x4000) == 0;
}

static uint8 ActOf(uint8 mode, uint8 sub) {
  if (mode >= 1 && mode <= 6) return (uint8)(sub >= kAct2Entry[mode] ? 2 : 1);
  return 1;                                   /* Death Heim: one group */
}

static void TypeFlush(TypeCtx *c) {
  if (c->n > 1) {
    Rng rng;
    RngSeed(&rng, c->seed,
            0x7E900000u ^ ((uint32)c->cur_mode << 8) ^ c->cur_act);
    int order[kMaxPlacements * 4];
    for (int i = 0; i < c->n; i++) order[i] = i;
    RngShuffle(&rng, order, c->n);
    for (int i = 0; i < c->n; i++) {
      uint8 v = c->types[order[i]];
      if (v != c->types[i]) g_summary.enemy_type_moves++;
      RomWrite8(c->offs[i] + 3, v);
    }
  }
  c->n = 0;
}

static void TypePass(uint8 mode, uint8 sub, uint16 stream, void *vctx) {
  TypeCtx *c = (TypeCtx *)vctx;
  uint8 act = ActOf(mode, sub);
  bool per_map = (c->scope == kRandomScope_Map);
  if (per_map || mode != c->cur_mode || act != c->cur_act) TypeFlush(c);
  c->cur_mode = mode;
  c->cur_act = act;

  PlacementList list;
  list.count = 0;
  CollectPlacements(stream, &list);
  for (int i = 0; i < list.count; i++) {
    if (!TypeIsMovable(mode, list.items[i].type)) continue;
    if (c->n >= (int)(sizeof(c->types) / sizeof(c->types[0]))) break;
    c->offs[c->n] = list.items[i].off;
    c->types[c->n] = list.items[i].type;
    c->n++;
  }
  if (per_map) TypeFlush(c);
}

/* --------------------------------------------------------- pass 5: sim lairs
 *
 * 24 records of 9 bytes, 4 per town, installed by $03:B7C6. Positions are
 * permuted WITHIN a town: the cells a town's four lairs occupy are known-good
 * squares on that town's map, and nothing in $03:B7C6 validates a position
 * against terrain, so keeping them in-town is what stops a lair landing in
 * water. Monster types can be permuted across all 24 freely — a type is just
 * a world-record class and every town can host every class.
 */
static void PassLairs(RandomizerMode spots, RandomizerMode types, uint32 seed) {
  uint32 base = LoRom(0x03, LAIR_TABLE_ADDR);
  Rng rng;

  if (spots != kRandomMode_Off) {
    for (int town = 0; town < kLairCount / kLairPerTown; town++) {
      RngSeed(&rng, seed, 0x1A180000u ^ (uint32)town);
      int order[kLairPerTown];
      for (int i = 0; i < kLairPerTown; i++) order[i] = i;
      RngShuffle(&rng, order, kLairPerTown);
      uint8 xs[kLairPerTown], ys[kLairPerTown];
      for (int i = 0; i < kLairPerTown; i++) {
        uint32 rec = base + (uint32)(town * kLairPerTown + i) * kLairStride;
        xs[i] = RomU8(rec + kLairCellX);
        ys[i] = RomU8(rec + kLairCellY);
      }
      for (int i = 0; i < kLairPerTown; i++) {
        uint32 rec = base + (uint32)(town * kLairPerTown + i) * kLairStride;
        if (xs[order[i]] != xs[i] || ys[order[i]] != ys[i])
          g_summary.lair_moves++;
        RomWrite8(rec + kLairCellX, xs[order[i]]);
        RomWrite8(rec + kLairCellY, ys[order[i]]);
      }
    }
  }

  if (types != kRandomMode_Off) {
    RngSeed(&rng, seed, 0x1A170000u);
    uint8 vals[kLairCount];
    for (int i = 0; i < kLairCount; i++)
      vals[i] = RomU8(base + (uint32)i * kLairStride + kLairType);
    if (types == kRandomMode_Shuffle) {
      int order[kLairCount];
      for (int i = 0; i < kLairCount; i++) order[i] = i;
      RngShuffle(&rng, order, kLairCount);
      for (int i = 0; i < kLairCount; i++) {
        uint8 v = vals[order[i]];
        if (v != vals[i]) g_summary.lair_type_moves++;
        RomWrite8(base + (uint32)i * kLairStride + kLairType, v);
      }
    } else {
      /* The four lair monster classes, $12..$15 (ram-map "Monster Types"). */
      for (int i = 0; i < kLairCount; i++) {
        uint8 v = (uint8)(0x12 + RngBelow(&rng, 4));
        if (v != vals[i]) g_summary.lair_type_moves++;
        RomWrite8(base + (uint32)i * kLairStride + kLairType, v);
      }
    }
  }
}

/* ------------------------------------------------------------------- driver */

bool Randomizer_Init(uint8 *rom, uint32 size) {
  g_rom_live = NULL;
  g_rom_pristine = NULL;
  g_rom_size = 0;
  memset(&g_summary, 0, sizeof g_summary);
  if (!rom || size != ROM_SIZE_EXPECTED) {
    fprintf(stderr, "[randomizer] disabled: unexpected ROM image (%u bytes)\n",
            (unsigned)size);
    return false;
  }
  g_rom_pristine = (uint8 *)malloc(size);
  if (!g_rom_pristine) {
    fprintf(stderr, "[randomizer] disabled: snapshot allocation failed\n");
    return false;
  }
  memcpy(g_rom_pristine, rom, size);
  g_rom_live = rom;
  g_rom_size = size;
  return true;
}

void Randomizer_Apply(void) {
  if (!Randomizer_IsAvailable()) return;

  /* Always restore first: passes must never compound across re-applies, and
   * turning the master off has to give back the stock image exactly. */
  memcpy(g_rom_live, g_rom_pristine, g_rom_size);
  memset(&g_summary, 0, sizeof g_summary);

  if (!g_settings.rando_enable) return;

  uint32 seed = (uint32)g_settings.rando_seed;
  g_summary.applied = true;
  g_summary.seed = seed;

  PassEnemyStats(g_settings.rando_enemy_hp, g_settings.rando_enemy_atk);

  if (g_settings.rando_statue_drops != kRandomMode_Off ||
      g_settings.rando_statue_spots != kRandomMode_Off) {
    StatueCtx ctx;
    ctx.drops = (RandomizerMode)g_settings.rando_statue_drops;
    ctx.spots = (RandomizerMode)g_settings.rando_statue_spots;
    ctx.seed = seed;
    g_summary.maps_touched = ForEachMap(StatuePass, &ctx);
  }

  if (g_settings.rando_enemy_types != kRandomMode_Off) {
    TypeCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.scope = (RandomizerScope)g_settings.rando_enemy_scope;
    ctx.seed = seed;
    ctx.cur_mode = 0xFF;
    int maps = ForEachMap(TypePass, &ctx);
    TypeFlush(&ctx);                       /* drain the final act */
    if (maps > g_summary.maps_touched) g_summary.maps_touched = maps;
  }

  PassLairs((RandomizerMode)g_settings.rando_lair_spots,
            (RandomizerMode)g_settings.rando_lair_types, seed);

  fprintf(stderr,
          "[randomizer] seed %u applied: %d stat records, %d drops, %d statue "
          "moves, %d enemy types, %d lair moves, %d lair types\n",
          (unsigned)seed, g_summary.enemy_records, g_summary.statue_drops,
          g_summary.statue_moves, g_summary.enemy_type_moves,
          g_summary.lair_moves, g_summary.lair_type_moves);
}

void Randomizer_Reroll(void) {
  /* Cheap, and it only has to be unpredictable to a human choosing a run. */
  static uint64 counter;
  Rng r;
  counter += 0x9E3779B9u;
  const uint32 counter_seed = (uint32)counter;
  RngSeed(&r, (uint32)(uintptr_t)&counter ^ counter_seed, counter_seed);
  long seed = (long)(RngNext(&r) % 1000000000u);
  const SettingDesc *d = Settings_Find("rando_seed");
  if (d) Settings_SetLong(d, seed);
  else g_settings.rando_seed = seed;
  Randomizer_Apply();
}
