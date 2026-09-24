#ifndef AR_REGIONAL_SESSION_ACTION_H
#define AR_REGIONAL_SESSION_ACTION_H
#include "regional/session/regional_session.h"

/* Room-pinned gameplay and presentation values; no CPU or renderer types.
 * Actor donor art, music, casts and run inventory have different boundaries
 * and deliberately do not activate in this transaction. */
typedef struct ArRegionalActionRoomSnapshot {
  uint8_t hazards;
  uint8_t terrain;
  uint8_t mosaic;
  uint8_t poses;
  uint8_t artwork;
  ArRegionalPlacementPolicy placements;
  ArRegionalDifficulty placement_difficulty;
  ArRegionalActionMotionSnapshot motion;
  ArRegionalEmitterSnapshot emitters;
  bool statue_volley;
  ArRegionalBossSnapshot bosses;
  ArRegionalDifficultySnapshot difficulty;
  bool score_lives;
  ArRegionalCollisionSnapshot collision;
  ArRegionalPlatformSkullSnapshot platform_skull;
  ArRegionalActorStatsSnapshot actor_stats;
  ArRegionalCastHoldSnapshot cast_hold;
  ArRegionalFireSnapshot fire_enemy;
} ArRegionalActionRoomSnapshot;

/* All-or-nothing activation. Preserves individual family revision increments
 * and leaves the session, output time and snapshot unchanged on any failure. */
bool ArRegionalSession_BeginActionRoom(ArRegionalSession *session, uint8_t profile,
                                       uint16_t native_bcd, uint16_t *out_bcd,
                                       ArRegionalActionRoomSnapshot *snapshot);
#endif
