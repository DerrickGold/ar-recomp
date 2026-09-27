#ifndef AR_REGIONAL_SESSION_TEST_H
#define AR_REGIONAL_SESSION_TEST_H
/* Regional-session test suites and their shared, stateless fixture helpers.
 * These declarations are private to the regional-session test executable. */
#include "regional/session/regional_session.h"

int RegionalSessionTest_RunActivation(void);
int RegionalSessionTest_RunPersistence(void);
int RegionalSessionTest_RunCodec(void);
int RegionalSessionTest_RunProfiles(void);
int RegionalSessionTest_RunRandomizer(void);

void RegionalSessionTest_MakeImage(uint8_t *image, unsigned marker);
/* Compare session state independently of the production serializer. */
bool RegionalSessionTest_Equal(const ArRegionalSession *a, const ArRegionalSession *b);
SaveCheckpointStatus RegionalSessionTest_AcceptOpaque(const uint8_t *bytes, size_t size,
                                                      void *context);
#endif
