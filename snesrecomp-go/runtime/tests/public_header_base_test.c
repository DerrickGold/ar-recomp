#include "snesrecomp/runner/base.h"

int snesrecomp_public_header_base_probe(void) {
    return SR_RUNNER_ABI_VERSION == 2u ? 0 : 1;
}
