#ifndef SNESRECOMP_RUNNER_GAME_MODULE_INTERNAL_H
#define SNESRECOMP_RUNNER_GAME_MODULE_INTERNAL_H

#include "snesrecomp/game.h"

typedef struct Apu Apu;

/* Registration resolves these immutable tables once. They are private runtime
 * caches, not an application integration surface. */
extern const RtlGameIdentity *g_rtl_game_identity;
extern const RtlGameLifecycleApi *g_rtl_game_lifecycle;
extern const RtlGameExecutionApi *g_rtl_game_execution;
extern const RtlGameStateProviderApi *g_rtl_game_state_providers;
extern const RtlGameAudioApi *g_rtl_game_audio;
void rtl_game_audio_state_loaded(Apu *apu);
/* Called only from the runner's SPC-upload safe point while it owns the APU
 * lock. The callback may therefore mutate its borrowed ARAM view directly. */
void RtlAudioExtensionNotifyUploadLocked(uint32_t source24);

#endif
