#include "snesrecomp/runner.h"
#include "snesrecomp/runner/ppu_diagnostics.h"
#include "snesrecomp/runner/determinism.h"
#include "snesrecomp/runner/replay.h"
#include "snesrecomp/support/digest.h"

static_assert(SR_RUNNER_ABI_VERSION == 2u, "unexpected runner ABI");

int main() {
    uint8_t digest[32];
    return sr_support_sha256(nullptr, 0, digest) ? 0 : 1;
}
