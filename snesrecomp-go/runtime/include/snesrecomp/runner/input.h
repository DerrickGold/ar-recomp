/** Host-independent serial controller input. Submit on the emulation thread
 * between ticks, never from an event/audio callback. Deltas are signed mouse
 * counts (positive right/down), not screen coordinates. */
#pragma once
#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t SrInputDevice;
enum {
    SR_INPUT_DEVICE_GAMEPAD = 0u,
    SR_INPUT_DEVICE_NONE = 1u,
    SR_INPUT_DEVICE_MOUSE = 2u
};
#define SR_MOUSE_BUTTON_LEFT  UINT32_C(1)
#define SR_MOUSE_BUTTON_RIGHT UINT32_C(2)

/** Changing device resets that port's shift state. For an unchanged mouse,
 * deltas accumulate until a latch; button state replaces the previous sample.
 * Gamepad buttons continue to come from RtlRunFrame's existing packed input.
 * Ports are zero-based. Reserved fields and non-mouse fields must be zero. */
typedef struct SrInputDeviceRequest {
    uint32_t struct_size;
    uint32_t port;
    uint64_t lifetime_generation;
    SrInputDevice device;
    uint32_t buttons;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t reserved;
} SrInputDeviceRequest;
#define SR_INPUT_DEVICE_REQUEST_V2_SIZE \
    ((uint32_t)(offsetof(SrInputDeviceRequest, reserved) + sizeof(uint32_t)))

#ifdef __cplusplus
}
#endif
