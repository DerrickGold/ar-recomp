#ifndef RANDOMIZER_H
#define RANDOMIZER_H
#include "types.h"

/* Seeded content randomizer.
 *
 * Everything it changes is ROM *data* — the object-type stat records, the
 * bank-$0A object placement streams, and the sim-mode lair table — all mapped
 * in docs/SEAMS.md "Content / randomizer seams". So the whole feature is one
 * transform of the loaded ROM image rather than a set of code hooks: register
 * the live cart buffer once, keep a pristine copy, and rewrite it from a seed.
 *
 * Every pass in this first slice is a PERMUTATION of data the game already
 * ships (or a scale applied to it). That is a deliberate safety property: a
 * shuffled statue can only land somewhere a statue already stood, so no pass
 * can put an object inside a wall and none of them need the collision oracle.
 * Placing objects at *arbitrary* tiles is a later step and does need it.
 *
 * Re-applying is safe at any time: the pristine copy is restored first, so
 * passes never compound. Stat changes take effect at the next spawn, placement
 * changes at the next level load.
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
  bool applied;              /* false = stock ROM image is live */
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

/* Restore the pristine image, then apply the passes enabled in g_settings.
 * Safe to call repeatedly. No-op (but still restores) when the master is off. */
void Randomizer_Apply(void);

/* Draw a fresh seed and re-apply. Bound to the menu's "New seed" action. */
void Randomizer_Reroll(void);

const RandomizerSummary *Randomizer_LastSummary(void);

/* True once Randomizer_Init has taken a usable snapshot. */
bool Randomizer_IsAvailable(void);

#endif /* RANDOMIZER_H */
