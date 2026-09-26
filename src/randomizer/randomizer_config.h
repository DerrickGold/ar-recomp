#ifndef AR_RANDOMIZER_CONFIG_H
#define AR_RANDOMIZER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Value-only campaign recipe. No settings, ROM, CPU or filesystem ownership.
 * Generator 0 means an older save did not record a recipe, not seed zero.
 * Generator 1 freezes the existing placement passes and keyed regional rolls. */
typedef struct RandomizerConfig {
  uint32_t seed;
  uint16_t hp_percent, attack_percent;
  uint8_t generator;
  bool enabled;
  uint8_t enemy_types, enemy_scope, statue_drops, statue_spots, lair_spots, lair_types;
  bool regional_action, regional_towns;
} RandomizerConfig;

enum { kRandomizerConfigBytes = 28, kRandomizerGenerator = 1 };
RandomizerConfig RandomizerConfig_Default(void);
bool RandomizerConfig_Valid(const RandomizerConfig *config);
/* Canonical, fixed-width little-endian recipe; never serialize struct padding.
 * A zero recipe remains explicitly unknown. Invalid inputs leave out alone. */
bool RandomizerConfig_Encode(const RandomizerConfig *config, uint8_t out[kRandomizerConfigBytes]);
bool RandomizerConfig_Decode(const uint8_t *bytes, size_t size, RandomizerConfig *out);

#endif
