#ifndef SNESRECOMP_NEXT_SPC_PLAYER_H
#define SNESRECOMP_NEXT_SPC_PLAYER_H

#include "types.h"

typedef struct Dsp Dsp;
typedef struct SpcPlayer SpcPlayer;

typedef void SpcPlayerInitialize(SpcPlayer *player);
typedef void SpcPlayerUpload(SpcPlayer *player, const uint8 *data);

/* Optional game-side high-level audio adapter. The hardware APU/DSP remains
 * authoritative; this interface only preserves the established game ABI. */
struct SpcPlayer {
    Dsp *dsp;
    uint8 input_ports[4];
    uint8 port_to_snes[4];
    SpcPlayerInitialize *initialize;
    SpcPlayerUpload *upload;
};

extern SpcPlayer *g_spc_player;

#endif
