#ifndef AR_IMPORTED_WORLD_TRAVEL_H
#define AR_IMPORTED_WORLD_TRAVEL_H
/* Optional travel runs at the main loop's acknowledged producer boundary.
 * The AR1 session, including its suspended native stack, remains alive. */
#include <stdbool.h>
#include <SDL3/SDL.h>
bool ImportedWorldTravel_HandleEvent(const SDL_Event *event);
bool ImportedWorldTravel_Pending(void);
void ImportedWorldTravel_Begin(void);
bool ImportedWorldTravel_Active(void);
bool ImportedWorldTravel_DrawWorld(void);
/* The shared snapshot action queues capture after AR2's next composed frame. */
void ImportedWorldTravel_RequestSnapshot(void);
/* System-menu actions; rejected outside an active level. */
bool ImportedWorldTravel_RequestRetry(void);
bool ImportedWorldTravel_RequestReturn(void);
/* One main-loop iteration; false requests application shutdown. Events,
 * lifecycle and graphics resets remain owned by the application's loop. */
bool ImportedWorldTravel_Update(bool hidden);
void ImportedWorldTravel_EndSession(void);
#endif
