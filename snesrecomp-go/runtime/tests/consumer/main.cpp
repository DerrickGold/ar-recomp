#include <snesrecomp/runner/base.h>
#include <snesrecomp/runner/ppu.h>
#include <snesrecomp/runner/events.h>
#include <snesrecomp/runner/audio.h>
#include <snesrecomp/runner/mutation.h>
#include <snesrecomp/runner/api.h>
#include <snesrecomp/support/crc32.h>

static_assert(SR_RUNNER_ABI_VERSION == 2u, "unexpected runner ABI");

int main() {
    static const uint8_t probe[] = {'s', 'd', 'k'};
    return crc32_compute(probe, sizeof(probe)) == UINT32_C(0x72071968)
        ? 0 : 1;
}
