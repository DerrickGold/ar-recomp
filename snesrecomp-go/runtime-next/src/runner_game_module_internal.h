#ifndef SNESRECOMP_NEXT_RUNNER_GAME_MODULE_INTERNAL_H
#define SNESRECOMP_NEXT_RUNNER_GAME_MODULE_INTERNAL_H

#include "runner_game_module.h"

/* Registration resolves these immutable tables once. They are private runtime
 * caches, not an application integration surface. */
extern const RtlGameIdentity *g_rtl_game_identity;
extern const RtlGameLifecycleApi *g_rtl_game_lifecycle;
extern const RtlGameExecutionApi *g_rtl_game_execution;
extern const RtlGameStateProviderApi *g_rtl_game_state_providers;
extern const RtlGameAudioApi *g_rtl_game_audio;

#endif
