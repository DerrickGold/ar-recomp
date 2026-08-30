#include "snesrecomp/runner/determinism.h"

int snesrecomp_public_header_determinism_probe(void) {
    return SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION == 2u ? 0 : 1;
}
