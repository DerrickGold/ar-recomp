#ifndef ACTRAISER_EVENT_BUGFIXES_H
#define ACTRAISER_EVENT_BUGFIXES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Repair the one verified retail-ROM town-event queue collision. The caller
 * owns policy: invoke this only while the corresponding enhancement is on.
 * Returns true only when the exact stale Aitos latch was cleared. */
bool ActRaiser_RepairAitosEventQueueCollision(uint8_t *wram,
                                               size_t wram_size);

#endif
