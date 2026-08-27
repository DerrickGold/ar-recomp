#include "runner_game_module_internal.h"

/* Registration-time module caches live in their own translation unit so
 * consumers of the frame/runtime helpers do not accidentally pull the much
 * larger CPU-infrastructure object from a static runner library. */
const RtlGameIdentity *g_rtl_game_identity;
const RtlGameLifecycleApi *g_rtl_game_lifecycle;
const RtlGameExecutionApi *g_rtl_game_execution;
const RtlGameStateProviderApi *g_rtl_game_state_providers;
const RtlGameAudioApi *g_rtl_game_audio;
