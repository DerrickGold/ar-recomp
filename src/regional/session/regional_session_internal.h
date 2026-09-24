#ifndef AR_REGIONAL_SESSION_INTERNAL_H
#define AR_REGIONAL_SESSION_INTERNAL_H
#include "regional/session/regional_session.h"

/* Shared by activation and persistence, not a substitute for edit authority.
 * Full validation remains mandatory at external/session entry boundaries. */
bool ArRegionalSession_Valid(const ArRegionalSession *session);
#endif
