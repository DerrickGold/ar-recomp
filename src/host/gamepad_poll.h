#ifndef AR_HOST_GAMEPAD_POLL_H
#define AR_HOST_GAMEPAD_POLL_H

#include <stdbool.h>
#include <stdint.h>

/* Main-thread lifecycle, after SDL gamepad init and before closing devices.
 * Only SDL device polling moves to the worker; event handling stays on main. */
bool HostGamepadPoll_Init(void);
void HostGamepadPoll_Shutdown(void);
bool HostGamepadPoll_Enabled(void);

/* Nonblocking physical-state fallback for keyboard/gamepad arbitration. The
 * worker samples this selection on its next poll. Queued input events remain
 * the authoritative source for gameplay buttons and axes. */
bool HostGamepadPoll_PhysicalActive(uint32_t id, unsigned stick_deadzone,
                                   unsigned trigger_threshold);

#endif
