#ifndef AR_SESSION_FATAL_H
#define AR_SESSION_FATAL_H

#include <stdbool.h>

/*
 * Latched request for an orderly, user-visible runtime shutdown.
 *
 * Callers must already be on the main/game thread and must return to the host
 * loop after requesting shutdown. The first failure wins so a secondary
 * teardown or presentation error cannot hide the condition the user can act
 * on. Boot failures that occur before a session exists still use Die().
 */
void SessionFatal_Request(const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
bool SessionFatal_Requested(void);
const char *SessionFatal_Message(void);

#endif /* AR_SESSION_FATAL_H */
