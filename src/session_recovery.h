#ifndef AR_SESSION_RECOVERY_H
#define AR_SESSION_RECOVERY_H

#include "session_fatal.h"
#include "localization/ui_catalog.h"

enum { kSessionRecoveryCapacity = 4096 };

/* Pure application-level presentation: no SDL, settings, saves or I/O.
 * Technical details are literal, bounded UTF-8; recovery actions use the host
 * interface locale, independently of the game pack or font renderer. */
const char *SessionRecovery_Title(ArUiLocale locale, SessionFailureKind kind);
bool SessionRecovery_Format(char *output, size_t capacity, ArUiLocale locale,
    SessionFailureKind kind, const char *detail, bool settings_failed, bool save_failed);

#endif
