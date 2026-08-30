#include "snesrecomp/game.h"
#include "snesrecomp/game/apu_sync.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/required_symbols.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/runtime_constants.h"
#include "snesrecomp/game/snes_regs.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/game_audio.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/host/audio_trace.h"
#include "snesrecomp/host/framedump.h"
#include "snesrecomp/host/launcher.h"
#include "snesrecomp/host/widescreen.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/runner/determinism.h"
#include "snesrecomp/runner/ppu_diagnostics.h"
#include "snesrecomp/spc_upload.h"
#include "snesrecomp/support/crc32.h"
#include "snesrecomp/support/file.h"
#include "snesrecomp/runner/replay.h"

int main(void) {
    return SR_RUNNER_ABI_VERSION == 2u &&
            RTL_GAME_MODULE_ABI_VERSION == 2u &&
            kSnesWramSize == 0x20000
        ? 0 : 1;
}
