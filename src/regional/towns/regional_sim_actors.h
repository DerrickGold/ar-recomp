#ifndef AR_REGIONAL_SIM_ACTORS_H
#define AR_REGIONAL_SIM_ACTORS_H
#include "regional/towns/regional_sim_combat.h"
#include "regional/towns/regional_sim_ai.h"
#include <stddef.h>

/* Separate live and cached generations. Neither family knows how the native
 * game stores actors, and the native adapter never packs codec bit fields. */
typedef struct ArRegionalSimActorRules {
  ArRegionalSimCombatSnapshot combat;
  ArRegionalSimAiSnapshot ai;
} ArRegionalSimActorRules;
enum { kArRegionalSimActorSlots=4, kArRegionalSimActorTowns=6,
       kArRegionalSimActorsV1EncodedBytes=12+28*2,
       kArRegionalSimActorsEncodedBytes=12+28*4 };
typedef struct ArRegionalSimActors {
  ArRegionalSimActorRules cached[24],active[4];
  uint8_t active_town_tag; /* 0 unavailable, otherwise town + 1 */
} ArRegionalSimActors;
bool ArRegionalSimActors_Valid(const ArRegionalSimActors *actors);
bool ArRegionalSimActors_LoadTown(ArRegionalSimActors *actors,unsigned town);
bool ArRegionalSimActors_SaveTown(ArRegionalSimActors *actors,unsigned town);
bool ArRegionalSimActors_Birth(ArRegionalSimActors *actors,unsigned town,unsigned slot,ArRegionalSimActorRules snapshot);
/* O(1); never reads requested/effective choices or other towns' caches. */
bool ArRegionalSimActors_Read(const ArRegionalSimActors *actors,unsigned town,unsigned slot,ArRegionalSimActorRules *snapshot);
/* Version 1 is needed to preserve existing combat-only replay identities.
 * It cannot encode non-US AI. Version 2 stores both families independently. */
bool ArRegionalSimActors_EncodeVersion(const ArRegionalSimActors *actors,uint8_t *out,size_t capacity,unsigned version);
bool ArRegionalSimActors_Encode(const ArRegionalSimActors *actors,uint8_t *out,size_t capacity);
bool ArRegionalSimActors_Decode(const uint8_t *bytes,size_t size,ArRegionalSimActors *actors);
#endif
