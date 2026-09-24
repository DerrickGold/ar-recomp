#ifndef RANDOMIZER_H
#define RANDOMIZER_H
#include "snesrecomp/game/types.h"
#include "action/action_placements.h"
#include "randomizer_config.h"

/* Seeded content randomizer.
 *
 * The native data passes change the object-type stat records, the
 * bank-$0A object placement streams, and the sim-mode lair table — all mapped
 * in docs/research-symbol-map.md and docs/regional-differences-technical.md.
 * The existing native path transforms the loaded ROM image: register the live
 * cart buffer once, keep a pristine copy, and rewrite data from a seed. Regional
 * placement programs and regional actor stats have value-only entry points
 * using the same transforms. Neither path patches room-control instructions.
 *
 * Every pass in this first slice is a PERMUTATION of data the game already
 * ships (or a scale applied to it). That is a deliberate safety property: a
 * shuffled statue can only land somewhere a statue already stood, so no pass
 * can put an object inside a wall and none of them need the collision oracle.
 * Placing objects at *arbitrary* tiles is a later step and does need it.
 *
 * Re-applying restores the pristine copy first, so passes never compound.
 * Title settings configure the next campaign; a bound campaign always uses
 * its saved recipe. Spawn and level-load consumers read the applied values.
 */

/* What a pass does with the values it owns. */
typedef enum {
  kRandomMode_Off = 0,     /* leave stock data alone */
  kRandomMode_Shuffle,     /* permute the values the game already ships */
  kRandomMode_Random,      /* draw fresh values from the legal range */
  kRandomMode_Count,
} RandomizerMode;

/* Scope a shuffle is allowed to move a value across. Act is the widest SAFE
 * scope for enemy types: every map of an act shares one animation blob
 * ($7E:4000), so a type moved within an act still resolves its frames. */
typedef enum {
  kRandomScope_Map = 0,
  kRandomScope_Act,
  kRandomScope_Count,
} RandomizerScope;

/* Read-only account of the last apply, for the menu to display. */
typedef struct {
  bool applied;              /* false = non-randomized baseline is live */
  uint32 seed;
  int enemy_records;         /* stat records rescaled */
  int statue_drops;          /* type-$80 placements whose item id changed */
  int statue_moves;          /* type-$80 placements whose tile changed */
  int enemy_type_moves;      /* ordinary placements whose type changed */
  int lair_moves;            /* lair records whose cell changed */
  int lair_type_moves;       /* lair records whose monster type changed */
  int maps_touched;
} RandomizerSummary;

/* Register the live cart ROM buffer and snapshot it. Call once, after SnesInit
 * (cart_load COPIES, so the buffer the game reads is g_snes->cart->rom) and
 * before the game coroutine starts. Returns false if the image is not the
 * expected size or the snapshot allocation fails, in which case every other
 * entry point becomes a no-op and the stock ROM stays live. */
bool Randomizer_Init(uint8 *rom, uint32 size);

/* Restore the pristine image, then apply the bound campaign recipe or, at
 * title, the settings draft. Safe to call repeatedly. With the master off,
 * restores the baseline without applying passes. */
void Randomizer_Apply(void);

/* Draw a fresh title-draft seed and re-apply. Does nothing during a bound
 * campaign. Bound to the menu's "New seed" action. */
void Randomizer_Reroll(void);

const RandomizerSummary *Randomizer_LastSummary(void);

/* True once Randomizer_Init has taken a usable snapshot. */
bool Randomizer_IsAvailable(void);

/* Title settings are a draft. A confirmed campaign binds a frozen recipe;
 * all ROM and numerical passes then read it, not mutable global settings.
 * Bind also shows the saved values in the settings UI. Release restores the
 * title draft. Neither operation writes a save or chooses regional rules. */
bool Randomizer_CaptureConfig(RandomizerConfig *out);
bool Randomizer_BindCampaign(const RandomizerConfig *config);
void Randomizer_ReleaseCampaign(void);
bool Randomizer_CampaignBound(void);
RandomizerConfig Randomizer_CurrentConfig(void);

typedef struct RandomizerStatScale {
  int hp_percent, attack_percent;
} RandomizerStatScale;

/* Last applied scale, not pending settings. Identity when disabled/unavailable.
 * Explicit game-owned child initializers also use this scale after selecting
 * their regional base; rewards and other non-combat fields must not use it. */
RandomizerStatScale Randomizer_AppliedStatScale(void);

/* Same rounding/clamping as the ROM pass: zero stays zero, nonzero stays 1..255. */
uint8_t Randomizer_ScaleStat(uint8_t base, int percent);

typedef struct RandomizerSpawnStatBasis {
  uint8_t hp, attack;
  RandomizerStatScale scale;
} RandomizerSpawnStatBasis;

/* Recover the ORIGINAL basis of a freshly copied initializer, never by inverse
 * scaling rounded/clamped bytes. record_offset is a linear ROM offset. Only
 * records owned by the last stat pass recover pristine values and its scale;
 * other records return the copied values with identity scaling. Stale/mutated
 * scaled records or non-byte stats fail without touching out. The game adapter
 * owns descriptor/actor identity and applies regional selection to this basis,
 * then scales once, before its existing difficulty adjustment. No ROM pointers
 * or CPU state cross this boundary. */
bool Randomizer_SpawnStatBasis(uint32_t record_offset, uint16_t copied_hp,
                              uint16_t copied_attack, RandomizerSpawnStatBasis *out);

typedef struct RandomizerPlacementMap {
  uint16_t scene;
  ActionPlacementProgram *program;
} RandomizerPlacementMap;
/* Numerical alternative to the ROM placement adapter. The room owner passes
 * fresh selected programs in area/room order, including every room of an act
 * when using Act scope. Preflight is atomic: invalid inputs change nothing.
 * Only object positions/item IDs/enemy types change, never IDs, reservations,
 * wave gates or native ROM bytes. Reports are separate from the ROM summary.
 * Apply once per fresh room snapshot, never cumulatively to a live program. */
bool Randomizer_ApplyPlacementPrograms(const RandomizerPlacementMap *maps, size_t count,
                                     RandomizerSummary *summary);

#endif /* RANDOMIZER_H */
