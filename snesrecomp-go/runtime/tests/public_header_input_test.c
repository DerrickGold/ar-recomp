#include "snesrecomp/runner/input.h"

_Static_assert(offsetof(SrInputDeviceRequest, lifetime_generation) == 8u,
               "input generation ABI offset");
_Static_assert(SR_INPUT_DEVICE_REQUEST_V2_SIZE == 36u,
               "input request ABI extent is architecture-independent");
