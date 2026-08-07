#ifndef HOST_INPUT_H
#define HOST_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Host-owned controls sit above the physical input mapper. They own pause,
 * turbo, the held joypad word, and the one-shot redraw request used while
 * emulation is frozen. */
void HostInput_HandleKeyboard(int scancode, bool pressed);
void HostInput_ClearHeld(void);
uint32_t HostInput_ComputeGameInputs(bool *keep_running);

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

/* Installs the gamepad edge-action bridge after InputMap_Init. */
void HostInput_InstallActionHandler(void);

#endif /* HOST_INPUT_H */
