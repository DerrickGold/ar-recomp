#ifndef ACTRAISER_LOCALIZATION_SCHEDULE_H
#define ACTRAISER_LOCALIZATION_SCHEDULE_H

#include "actraiser/actraiser_localization_text.h"

/* Game-thread execution adapter, separate from immutable renderer snapshots.
 * These callbacks preserve the native per-frame menu/Mode7 work. They may yield
 * to the host; callers must reacquire session-owned pointers afterwards. */
typedef struct ActRaiserLocalizationDialogueHost {
  void *context;
  bool (*wait_frame)(void *context);
  bool (*confirm_page)(void *context);
} ActRaiserLocalizationDialogueHost;

bool ActRaiserLocalizationRuntime_BeginDialogue(
    const ActRaiserLocalizationTextObservation *observation);
/* Game-thread gate: consumes failure feedback for the current window before
 * permitting more authored work. Called again after every yielding callback. */
bool ActRaiserLocalizationRuntime_DialogueScheduled(void);
/* Rechecked after each native input-poll frame. A source contraction or switch
 * to retail can retire an authored-only wait without synthesizing a button. */
bool ActRaiserLocalizationRuntime_PageConfirmationPending(void);
void ActRaiserLocalizationRuntime_ScheduleByte(
    const ActRaiserLocalizationTextObservation *observation, uint8_t code,
    uint8_t text_speed, const ActRaiserLocalizationDialogueHost *host);
bool ActRaiserLocalizationRuntime_ContinueDialogue(
    const ActRaiserLocalizationDialogueHost *host, bool retain_rows);
void ActRaiserLocalizationRuntime_ReturnDialogue(void);

/* Conditional HLE seams: interpreter/reader wrappers always execute the
 * original bodies. Only enhanced continuation waits are adapted. */
bool ActRaiser_LocalizationScheduleEntry(CpuState *cpu);
RecompReturn ActRaiser_LocalizationRunDialogue(CpuState *cpu);
bool ActRaiser_LocalizationScheduleByte(CpuState *cpu);
RecompReturn ActRaiser_LocalizationReadTextByte(CpuState *cpu);
bool ActRaiser_LocalizationScheduleContinuation(CpuState *cpu);
RecompReturn ActRaiser_LocalizationContinueDialogue(CpuState *cpu);

#endif
