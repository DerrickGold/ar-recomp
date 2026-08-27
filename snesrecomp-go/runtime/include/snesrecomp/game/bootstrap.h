#ifndef SNESRECOMP_GAME_BOOTSTRAP_H
#define SNESRECOMP_GAME_BOOTSTRAP_H

#include "snesrecomp/game.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Snes Snes;

Snes *SnesInit(const uint8_t *data, int data_size);
void SnesShutdown(void);

/* Registration is atomic and accepted only while no runner is active. It
 * returns UNSUPPORTED for unknown ABI versions/capabilities, BUSY while a
 * runner exists, and INVALID_ARGUMENT for malformed descriptors. */
SrResult RtlRegisterGame(const RtlGameModule *module);
const char *RtlGameIdentifier(void);
bool RtlGameDrawPpuFrame(void);
/* Restart-class configuration. Disabled mode installs no hot audio-extension
 * hooks; enabled callbacks run only at existing DSP/SPC/APU seams. */
void RtlAudioExtensionConfigure(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
