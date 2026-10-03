#ifndef AR_HOST_INPUT_H
#define AR_HOST_INPUT_H
/* Host-owned controls sit above the physical input mapper. They own event
 * routing, pause, turbo, and redraw requests while emulation is frozen. */

#include <stdbool.h>
#include <stdint.h>

union SDL_Event;

/* Keyboard, mouse, text and gamepad routing, including modal menu/capture
 * precedence. Returns false for non-input events (window, graphics, quit),
 * which remain the application loop's responsibility. Device changes and
 * releases retain their existing routes even when presses are suppressed. */
bool HostInput_HandleEvent(const union SDL_Event *event);
/* Main-thread routing during action-diorama production: gameplay inputs and
 * host camera pose only. False leaves the event queued until runner pause. */
bool HostInput_TryHandleStreamEvent(const union SDL_Event *event);

void HostInput_HandleKeyboard(int scancode, bool pressed, bool repeated);
void HostInput_ClearHeld(void);
/* Bounded startup/focus diagnostics; no typed text or key values are logged. */
void HostInput_LogStatus(const char *reason);
/* Sample frontend input and apply host-frame scripted overrides. Replay
 * resolution belongs beside each RtlRunFrame call so turbo/catch-up ticks are
 * represented individually in canonical recordings. */
uint32_t HostInput_SampleLiveInputs(void);
uint32_t HostInput_ResolveActionInputs(uint32_t input);

bool HostInput_MenuGamepadIsActive(void);
bool HostInput_MenuKeyboardIsActive(void);
/* Auto-mode event arbitration: true when a simultaneous live pad input owns
 * this host iteration and a synthesized keyboard twin must be ignored. */
bool HostInput_KeyboardIsSuppressed(void);

bool HostInput_IsPaused(void);
bool HostInput_IsTurbo(void);
void HostInput_TogglePause(void);
void HostInput_ToggleTurbo(void);

void HostInput_RequestPausedRedraw(void);
bool HostInput_IsPausedRedrawPending(void);
bool HostInput_RedrawPausedFrameIfNeeded(void);
void HostInput_MarkFrameDrawn(void);

/* Inspector selection temporarily owns pause only when it introduced the
 * pause. This lets closing the selection restore the user's prior state. */
bool HostInput_InspectorOwnsPause(void);
void HostInput_OnInspectorSelection(bool had_selection);
void HostInput_CloseInspectorSelection(void);

void HostInput_AdjustSim3DCamera(float yaw_delta, float pitch_delta,
                                 float zoom_delta);
void HostInput_ResetSim3DCamera(void);
void HostInput_ApplyAnalogCamera(void);
/* Main-thread update for a captured action/diorama frame, including while
 * the game producer runs. Uses no live WRAM or SIM camera state. Shares the
 * elapsed-time clock above so stream handoffs cannot double-step the camera. */
void HostInput_ApplyDioramaPresentationCamera(void);

/* Advances the session-only click/hold comparison control. A pending fresh
 * native-frame upload and the visible transition both count as host pauses;
 * callers include them in the same freeze coordinator as the settings
 * overlay. */
void HostInput_UpdateRenderComparison(void);
bool HostInput_RenderComparisonOwnsPause(void);
bool HostInput_RenderComparisonCaptureRequired(void);

/* Owns InputMap's lifetime, clears pause/turbo/held controls on each begin,
 * and installs the gamepad edge-action bridge. Pair once per game session. */
void HostInput_BeginSession(void);
void HostInput_EndSession(void);

/* Apply live settings here, beside the subsystem they configure. */
struct SettingDesc;
void HostInput_ApplySetting(const struct SettingDesc *desc);

#endif /* AR_HOST_INPUT_H */
