#ifndef AR_ACTRAISER_WORLD_TRAVEL_H
#define AR_ACTRAISER_WORLD_TRAVEL_H
/* Borrow the suspended AR1 world coroutine for imported level travel. The
 * original ROM owns Advent, Mode-7 motion, music uploads and the return fade.
 * Calls are serialized with the game owner; no worker may access this state. */
#include <stdbool.h>

bool ActRaiserWorldTravel_Begin(void);
bool ActRaiserWorldTravel_Ready(void);
void ActRaiserWorldTravel_Return(void);
bool ActRaiserWorldTravel_Complete(void);

/* Runtime seams: service only after resuming a coroutine yield. Parked black
 * and restored checkpoints advance audio time without issuing native NMIs. */
void ActRaiserWorldTravel_ServiceYield(void);
bool ActRaiserWorldTravel_HoldsNativeTicks(void);
void ActRaiserWorldTravel_Reset(void);
#endif
