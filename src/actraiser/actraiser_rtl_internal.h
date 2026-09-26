#ifndef AR_ACTRAISER_RTL_INTERNAL_H
#define AR_ACTRAISER_RTL_INTERNAL_H
/* ActRaiser runtime internals: what actraiser_rtl.c offers the files split from
 * it (actraiser_cheats.c and the passes in actraiser/enhancements/): the runner
 * accessors, register and PPU queries, developer flags, and ApplyCheats. What
 * the passes share among themselves is in enhancements/actraiser_enhancements_internal.h.
 * Not a public API.
 * Phase: game (main thread). */
#include "snesrecomp/support/utf8_fs.h"

#include "actraiser/actraiser_rtl.h"
#include "actraiser_game.h"
#include "actraiser_action_bg.h"
#include "actraiser_hle_fatal.h"
#include "actraiser/actraiser_event_bugfixes.h"
#include "actraiser/actraiser_credits.h"
#include "actraiser/actraiser_hud.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/actraiser_bg3_upload.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser/actraiser_localization_routes.h"
#include "action/action_bg_tuner.h"
#include "action/action_effects.h"
#include "action/action_load_pacing.h"
#include "actraiser/enhancements/actraiser_ws_gap.h"
#include "actraiser/cpu_65816_math.h"
#include "diorama/diorama_capture_blend.h"
#include "diorama/diorama.h"
#include "diorama/diorama_layer_order.h"
#include "diorama/diorama_performance.h"
#include "app/performance_metrics.h"
#include "diorama/diorama_planes.h"
#include "deterministic_hash.h"
#include "present/display_geometry.h"
#include "host/host_display.h"   /* kHostDisplayFramebufferHeight */
#include "app/settings.h"
#include "app/session_fatal.h"
#include "audio/audio_presentation_policy.h"
#include "replacements/hd_replacement_host.h"
#include "replacements/hd_replacements.h"
#include "replacements/music_replacements.h"
#include "audio/native_audio_extension.h"
#include "randomizer/randomizer.h"
#include "render/bg3_composite_policy.h"
#include "dev/native_audio_trace.h"
#include "dev/hd_tile_census.h"
#include "dev/sfx_census.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim3d/sim3d.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "sim/sim_visual_patches.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "snesrecomp/game/trace.h"
#include <stdio.h>
#include "actraiser/actraiser_sim_menu.h"
#include "sim/menu/sim_menu_art.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>


/* Developer-only environment controls are immutable for a game process. Keep
 * their historical presence-based syntax, but snapshot them before the first
 * emulated frame so diagnostics do not repeatedly traverse the host
 * environment from vblank, object, or presentation paths. */
typedef enum ActRaiserDeveloperFlag {
  kActRaiserDeveloperFlag_DisableActionLoadPacing,
  kActRaiserDeveloperFlag_LoadPacingLog,
  kActRaiserDeveloperFlag_VblankLog,
  kActRaiserDeveloperFlag_CopLog,
  kActRaiserDeveloperFlag_YieldLog,
  kActRaiserDeveloperFlag_FrameLog,
  kActRaiserDeveloperFlag_ObjectLog,
  kActRaiserDeveloperFlag_PpuLog,
  kActRaiserDeveloperFlag_WidescreenLayerLog,
  kActRaiserDeveloperFlag_VerticalExtensionTileLog,
  kActRaiserDeveloperFlag_VerticalExtensionLog,
  kActRaiserDeveloperFlag_HudIconLog,
  kActRaiserDeveloperFlag_ApronLog,
  kActRaiserDeveloperFlag_TitleLog,
  kActRaiserDeveloperFlag_Count,
} ActRaiserDeveloperFlag;

typedef struct ActRaiserDeveloperEnvironment {
  bool flags[kActRaiserDeveloperFlag_Count];
  bool widescreen_only_bg_present;
  int widescreen_only_bg_layer;
  bool widescreen_clamp_present;
  uint8_t widescreen_clamp_mask;
} ActRaiserDeveloperEnvironment;

static const char *const kActRaiserDeveloperFlagNames[] = {
  [kActRaiserDeveloperFlag_DisableActionLoadPacing] =
      "AR_NO_ACTION_LOAD_PACING",
  [kActRaiserDeveloperFlag_LoadPacingLog] = "AR_LOADPACELOG",
  [kActRaiserDeveloperFlag_VblankLog] = "AR_VBLOG",
  [kActRaiserDeveloperFlag_CopLog] = "AR_COPLOG",
  [kActRaiserDeveloperFlag_YieldLog] = "AR_YIELDLOG",
  [kActRaiserDeveloperFlag_FrameLog] = "AR_FRAMELOG",
  [kActRaiserDeveloperFlag_ObjectLog] = "AR_OBJLOG",
  [kActRaiserDeveloperFlag_PpuLog] = "AR_PPULOG",
  [kActRaiserDeveloperFlag_WidescreenLayerLog] = "AR_WS_LAYERS",
  [kActRaiserDeveloperFlag_VerticalExtensionTileLog] = "AR_VEXT_TILES",
  [kActRaiserDeveloperFlag_VerticalExtensionLog] = "AR_VEXT_LOG",
  [kActRaiserDeveloperFlag_HudIconLog] = "AR_HUDICON",
  [kActRaiserDeveloperFlag_ApronLog] = "AR_APRONLOG",
  [kActRaiserDeveloperFlag_TitleLog] = "AR_TITLELOG",
};

_Static_assert(
    sizeof(kActRaiserDeveloperFlagNames) /
        sizeof(kActRaiserDeveloperFlagNames[0]) ==
        kActRaiserDeveloperFlag_Count,
    "developer environment flag table is incomplete");

typedef struct ActRaiserPpuShapeRegisters {
  uint8_t inidisp, bgmode, mosaic, m7sel, setini;
  uint8_t bg_xsc[4];
  uint16_t bg_tile_adr;
  uint16_t hscroll[4], vscroll[4];
  int16_t m7matrix[8];
  uint8_t screen_enabled[2], screen_windowed[2];
  uint32_t windowsel;
  uint16_t wbgobjlog, fixed_color;
  uint8_t cgwsel, cgadsub;
} ActRaiserPpuShapeRegisters;
extern volatile int g_sr_in_interrupt;

/* A 65816 hardware interrupt is register-transparent to the interrupted
 * code: RTI restores P/PC/PB and a well-behaved handler save/restores
 * A/X/Y/D/DB. We invoke the recompiled NMI/IRQ handlers as plain host-C
 * calls on the shared g_cpu; if a handler body has an internal stack
 * imbalance (e.g. an x-width mismatch) its terminal RTI can pop the
 * wrong byte as P and corrupt the interrupted code's M/X width flags.
 * Snapshot the CPU register frame before the handler and restore it
 * after — the handler's RAM/PPU side effects (the point of the IRQ)
 * persist in g_ram/g_ppu, only the CPU registers are made transparent. */
typedef struct { uint16 A, X, Y, S, D; uint8 DB, PB, P, m_flag, x_flag,
  emulation, host_return_valid, fN, fV, fZ, fC, fI, fD; } CpuRegSnapshot;

/* ---- defined in actraiser_rtl.c ---- */
SrRunnerHandle *ActRaiser_Runner(void);
const SnesRunnerApi *ActRaiser_RunnerApi(void);
bool ActRaiser_QueryPpuState(SrPpuStateSnapshot *state);
uint8_t ActRaiser_QueryHdmaActiveMask(void);
bool ActRaiser_ResetPpuFrameCaptures(void);
bool ActRaiser_ClearPpuObjMetadata(void);
const ActRaiserDeveloperEnvironment *
ActRaiser_GetDeveloperEnvironment(void);
bool ActRaiser_DeveloperFlagEnabled(ActRaiserDeveloperFlag flag);
bool ActRaiser_PpuShapeTraceActive(unsigned gf);
void ActRaiser_PpuShapeCaptureRegisters(
    const SrPpuStateSnapshot *ppu, ActRaiserPpuShapeRegisters *output);
void ActRaiser_PpuShapeTraceLine(
    unsigned gf, int line, const ActRaiserPpuShapeRegisters *before,
    const SrPpuScanoutLineContext *context);
void ActRaiser_SaveRegs(CpuState *c, CpuRegSnapshot *s);
void ActRaiser_RestoreRegs(CpuState *c, const CpuRegSnapshot *s);
void ActRaiser_EmitInterrupt(SrInterruptKind kind, uint32 flags,
                                    uint32 pc24, uint16 vector,
                                    int32 scanline, const char *label);
uint32 ActRaiser_LastBlockPc(void);
bool ActRaiser_QueryInputState(SrInputStateSnapshot *state);

/* ---- defined in actraiser_cheats.c ---- */
void ActRaiser_ApplyCheats(void);

#endif  /* AR_ACTRAISER_RTL_INTERNAL_H */
