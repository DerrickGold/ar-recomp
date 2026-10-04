#ifndef AR_ACTRAISER_DEATH_HEIM_HUB_H
#define AR_ACTRAISER_DEATH_HEIM_HUB_H
/* Death Heim room 1 A's native face/eye capture. The game frame owner prepares
 * OAM winner capture before scanout and joins eyes to faces afterward. The
 * published far band serves both the authored plane and the enlarged skybox. */
#include <stdbool.h>

void ActRaiser_DioramaDeathHeimEyesPrepare(void);
void ActRaiser_DioramaDeathHeimHubStatuesFinish(int width);
bool ActRaiser_DioramaDeathHeimHubFacesPromoted(void);

#endif
