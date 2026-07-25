#ifndef FORCED_INPUT_H
#define FORCED_INPUT_H

#include <stdint.h>

/* Parse the optional AR_FORCE_INPUT_* diagnostic controls. */
void ForcedInput_Init(void);

/* Add configured held or pulsed buttons for the current host frame. */
uint32_t ForcedInput_Apply(uint32_t live_inputs, int host_frame);

#endif /* FORCED_INPUT_H */
