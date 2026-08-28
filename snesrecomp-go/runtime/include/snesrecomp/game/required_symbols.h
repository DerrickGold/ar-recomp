/**
 * @file required_symbols.h
 * @brief Symbols the GAME PROJECT must define. The runner references these and
 *        deliberately does not implement them.
 * @ingroup sr_game
 *
 * Everything declared here is part of the runner's link surface but not its
 * implementation: the runner calls these, and a project that omits one gets a
 * linker error naming a symbol it has never heard of. This header is the
 * machine-readable source used by `snesbuild doctor` and the human-readable
 * place that says "these are yours".
 *
 * Include it from one game translation unit and define every symbol below.
 * `snesbuild doctor` checks the same list.
 *
 * This header intentionally declares nothing the runner defines. If you are
 * looking for what the runner provides, start at `snesrecomp/runner.h` and
 * `snesrecomp/game/bootstrap.h`.
 */
#ifndef SNESRECOMP_GAME_REQUIRED_SYMBOLS_H
#define SNESRECOMP_GAME_REQUIRED_SYMBOLS_H

#include "snesrecomp/game/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Marks a declaration the game project owns. Purely documentary -- it expands
 * to nothing -- but it makes the contract greppable from both the headers and
 * the tooling.
 */
#define SR_GAME_PROVIDES /* implemented by the game project */

/**
 * @name APU synchronization
 * The runner touches APU state from its SNES, MSU1, audio-trace, and
 * audio-control paths, but whether more than one thread ever reaches the APU
 * is the frontend's decision, so the frontend owns the lock.
 *
 * A frontend that renders audio synchronously may implement these as no-ops,
 * but they must still exist. Both are also called before the frontend has
 * finished initializing (during `SnesInit`) and in headless runs where no
 * audio device is ever created, so both must tolerate being called before any
 * lock object exists.
 * @{
 */
SR_GAME_PROVIDES void RtlApuLock(void);
SR_GAME_PROVIDES void RtlApuUnlock(void);
/**
 * Only the lock pair is owed. `RtlApuWrite` and `rtl_accumulate_apu_catchup`
 * are declared next to them in `apu_sync.h` but are implemented by the runner
 * in `core/common_rtl.c`; defining them in a game project is a duplicate
 * symbol. (`tests/apu_sync_stub.c` does define all four, because it stands in
 * for the runner in tests that never link `common_rtl.c`.)
 */
/** @} */

#ifdef __cplusplus
}
#endif

#endif
