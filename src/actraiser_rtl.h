#ifndef ACTRAISER_RTL_H
#define ACTRAISER_RTL_H

#include "action/action_obj_apron.h"
#include "action/action_bg_plan.h"
#include "snesrecomp/game.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/snes_regs.h"

typedef struct SrRunnerHandle SrRunnerHandle;

/* Lifecycle-owned runner binding. Game code retains only the opaque handle;
 * concrete console/component layouts remain runner-private. */
void ActRaiser_BindRunner(SrRunnerHandle *runner);
void ActRaiserDrawPpuFrame(void);
void ActRaiser_RebindPpuOutputSurfaces(void);

typedef struct ActRaiserRomSetupResult {
  bool visual_patches_applied;
  bool randomizer_initialized;
} ActRaiserRomSetupResult;

bool ActRaiser_InitializeGame(
    const RtlGameInitializeContext *context);
/* Result copied from the most recent lifecycle initialization callback. The
 * mutable ROM view itself is callback-scoped and is never retained. */
ActRaiserRomSetupResult ActRaiser_LastRomSetupResult(void);

/* The apron geometry in force this frame, or {ws_extra, 0} when the apron is
 * not live. `apron == 0` is the disable lever the whole phase rides on: every
 * apron-aware site collapses to its pre-apron expression, so callers test that
 * rather than re-deriving the policy. Live only under the diorama margin policy
 * (host_display.c pins g_ws_extra to the ActRaiser display cap there), because
 * flat mode
 * never samples the columns the apron would fill. */
ActionApronGeometry ActRaiser_ObjApronGeometry(void);

/* The widescreen margin geometry the most recent
 * frame was actually RENDERED with, latched at the end of ActRaiserDrawPpuFrame.
 * A consumer of that frame's captured pixels must use this rather than reading
 * g_ppu->extraLeftCur/extraRightCur, which can be zeroed between the draw and
 * the frame-slot capture (see the latch's comment). Any pointer may be NULL. */
void ActRaiser_LiveMargins(int *left, int *right);
/* The resolved per-layer/per-band background plan and capture-padding fact
 * belonging to those same pixels. Returns false only for an invalid plan. */
bool ActRaiser_LiveActionBgPlan(ActionBgPlan *out,
                                bool *pad_captured_to_budget);
/* Vertical geometry latched with the last rendered frame. Either pointer may
 * be NULL. */
void ActRaiser_LiveVerticalMargins(int *top, int *bottom);
unsigned ActRaiser_TakeVextUnlockedObjects(void);

/* The OAM slots the widescreen HUD-icon promote validated for the frame being
 * drawn, or false when it promoted nothing. This is the ONLY reliable answer to
 * "which sprites are the flat HUD icon": overlayCaptures[Obj] carries an OAM
 * range too, but diorama mode overwrites that capture with its own full-frame
 * 0..127 scene claim, so reading the range back from there silently loses the
 * icon exactly when the diorama is on. Either pointer may be NULL. */
bool ActRaiser_HudObjIconRange(uint8_t *first, uint8_t *count);
/* Publish the host-owned pixel surface containing that promoted icon. The
 * capture owner supplies its format, pitch, dimensions, and lifetime contract;
 * presentation must not reconstruct those details from the backing buffer. */
bool ActRaiser_HudObjSurfaceView(SrPpuSurfaceView *surface);
/* True when the most recently rendered frame moved Death Heim 0701's face
 * band into its focal virtual plane after native capture. The PPU content mask
 * cannot observe host-side postprocessing, so FrameSlot uses this latch to
 * publish that plane. */
bool ActRaiser_DioramaDeathHeimHubFacesPromoted(void);
void ActRaiser_FullSnapshot(const char *prefix);
void RunOneFrameOfGame(void);
void ActRaiser_OnInidispWrite(uint8 value);
void ActRaiser_OnApuPortPace(uint8 port, uint8 value);
/* Release the game coroutine's stack (guard-page mapping) / fiber at shutdown.
 * Safe to call when none was created. */
void ActRaiser_DestroyGameCoroutine(void);
int ActRaiser_ReadRdnmi(const RtlRdnmiReadContext *context);
bool ActRaiser_RecoverDispatchMiss(uint32 source_pc24, uint32 target_pc24);
void ActRaiser_SpcUploaderCompleteTick(void);
void ActRaiser_SpcUploadBindRunner(SrRunnerHandle *runner);
void ActRaiser_WidescreenSpritesBindRunner(SrRunnerHandle *runner);
bool ActRaiser_SpcUploadSource(CpuState *cpu, uint32 *source24);
bool ActRaiser_SpcUploadCustomize(CpuState *cpu,
                                  const SrSpcUploadContext *upload,
                                  uint32 source24);
void ActRaiser_SpcUploadCommit(SrSpcUploadContext *upload);
int ActRaiser_SpcUploadStackPop(const CpuState *cpu);

/* Debug aid: queue one action-stage spell-cycle step. Called from the host
 * input path; the request is consumed at the next frame boundary inside
 * ActRaiser_ApplyCheats, which is where WRAM and VRAM are quiet. Requests do
 * not stack — a burst of presses advances the selection once per frame at
 * most, and the request is dropped entirely if the cheat is disarmed, the
 * player is not in an action stage, or a cast is still resolving. */
void ActRaiser_RequestMagicCycle(void);
/* 0 when no spell is selected, else 1..4 ($02AC). Published into the frame
 * slot so the presentation layer can name the armed cheat's current spell
 * without presentation reading live WRAM. */
uint8 ActRaiser_SelectedMagic(void);

/* BG-only widescreen presentation helpers. The Sky Palace pair temporarily
 * decodes a box-free source map into only BG2's margin columns, then restores
 * game VRAM. Action world margins use the bounded HLE provider. */
void ActRaiser_WidescreenSkyPalacePrepare(SrRunnerHandle *runner);
void ActRaiser_WidescreenSkyPalaceRestore(SrRunnerHandle *runner);
void ActRaiser_WidescreenSpriteActivationProbe(void);

#endif  // ACTRAISER_RTL_H
