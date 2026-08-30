#include "snesrecomp/support/input_replay.h"

int snesrecomp_public_header_input_replay_probe(void) {
    return SR_INPUT_REPLAY_FORMAT_VERSION == 1u ? 0 : 1;
}
