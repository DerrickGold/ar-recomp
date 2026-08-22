#ifndef HOST_DISPLAY_STATUS_H
#define HOST_DISPLAY_STATUS_H

#include <stdbool.h>

/* Transient presentation properties reported by the active SDL renderer and
 * display. They are deliberately not settings: none is persisted, and game
 * logic must not depend on them. */
void HostDisplayStatus_SetNominalRefreshHz(int hz);
int HostDisplayStatus_NominalRefreshHz(void);

void HostDisplayStatus_SetVsyncActive(bool active);
bool HostDisplayStatus_VsyncActive(void);

#endif /* HOST_DISPLAY_STATUS_H */
