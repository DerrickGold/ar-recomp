#include "snesrecomp/runner.h"

#include "runner_internal.h"
#include "snesrecomp/game/runtime_constants.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#include <stdatomic.h>
#include <string.h>

_Static_assert(SR_PPU_NATIVE_WIDTH == kPpuXPixels,
               "public ABI native width must match the PPU");
_Static_assert(SR_PPU_NATIVE_HEIGHT == kPpuYPixels,
               "public ABI native height must match the PPU");
_Static_assert(SR_PPU_HORIZONTAL_MARGIN_MAX == kPpuExtraLeftRight &&
                   SR_PPU_VERTICAL_MARGIN_MAX == kPpuExtraTopBottom,
               "public ABI margin limits must match the PPU");
_Static_assert(SR_PPU_SURFACE_MAX_WIDTH == kPpuSurfaceWidth &&
                   SR_PPU_SURFACE_MAX_HEIGHT == kPpuBufHeight,
               "public ABI surface limits must match the PPU");
_Static_assert(SR_PPU_OBJ_APRON == kPpuObjApron,
               "public ABI OBJ apron must match the PPU");
_Static_assert(SR_PPU_OBJ_X_WRAP == kPpuObjXWrap,
               "public ABI OBJ X wrap must match the PPU");
_Static_assert(SR_PPU_OBJ_Y_WRAP == kPpuObjYWrap,
               "public ABI OBJ Y wrap must match the PPU");
_Static_assert(SR_PPU_OBJ_Y_NEGATIVE_FROM == kPpuObjYNegativeFrom,
               "public ABI OBJ negative-Y band must match the PPU");
_Static_assert(SR_PPU_MODE7_CANVAS_EXTENT == kPpuMode7CanvasExtent,
               "public ABI Mode-7 canvas extent must match the PPU");
_Static_assert(SR_PPU_CGRAM_WORD_COUNT == kPpuCgramEntries,
               "public ABI CGRAM extent must match the PPU");
_Static_assert(SR_PPU_TILE_ID_COUNT == kPpuObjTileIds,
               "public ABI tile-id extent must match the PPU");
_Static_assert(SR_PPU_SURFACE_BAND_COUNT == 4u,
               "public ABI surface bands must cover primary plus 3 splits");
_Static_assert(SR_PPU_OVERLAY_BG1 == kPpuOverlaySource_Bg1 &&
                   SR_PPU_OVERLAY_BG2 == kPpuOverlaySource_Bg2 &&
                   SR_PPU_OVERLAY_BG3 == kPpuOverlaySource_Bg3 &&
                   SR_PPU_OVERLAY_BG4 == kPpuOverlaySource_Bg4 &&
                   SR_PPU_OVERLAY_OBJ == kPpuOverlaySource_Obj &&
                   SR_PPU_OVERLAY_SOURCE_COUNT == kPpuOverlaySource_Count,
               "public ABI overlay IDs must match the PPU");
_Static_assert(SR_PPU_OVERLAY_REMOVE_FROM_GAME ==
                       kPpuOverlayFlag_RemoveFromGame &&
                   SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH ==
                       kPpuOverlayFlag_MarkObjColorMath &&
                   SR_PPU_OVERLAY_MARK_BG_HALF_ADD ==
                       kPpuOverlayFlag_MarkBgHalfAdd &&
                   SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT ==
                       kPpuOverlayFlag_ApplyBgFixedColorSubtract &&
                   SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN ==
                       kPpuOverlayFlag_MarkFullAddSubscreen &&
                   SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkMainScreenWinner &&
                   SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkOwningScreenWinner,
               "public ABI overlay flags must match the PPU");
_Static_assert(SR_PPU_BACKGROUND_FILL_INHERIT ==
                       kPpuWidescreenBandFill_Inherit &&
                   SR_PPU_BACKGROUND_FILL_TRANSPARENT ==
                       kPpuWidescreenBandFill_Transparent &&
                   SR_PPU_BACKGROUND_FILL_LIVE_WORLD ==
                       kPpuWidescreenBandFill_LiveWorld &&
                   SR_PPU_BACKGROUND_FILL_CLAMP ==
                       kPpuWidescreenBandFill_Clamp &&
                   SR_PPU_BACKGROUND_FILL_MIRROR ==
                       kPpuWidescreenBandFill_Mirror &&
                   SR_PPU_BACKGROUND_FILL_REPEAT ==
                       kPpuWidescreenBandFill_Repeat &&
                   SR_PPU_BACKGROUND_FILL_RAW_WRAP ==
                       kPpuWidescreenBandFill_RawWrap,
               "public ABI background fills must match the PPU");
_Static_assert(SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE ==
                       kPpuWidescreenMotion_FillRelative &&
                   SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL ==
                       kPpuWidescreenMotion_NormalScroll,
               "public ABI background motion must match the PPU");
_Static_assert(sizeof(SrPpuObjPart) == 8u,
               "public ABI OBJ part must have a fixed layout");
_Static_assert(offsetof(SrPpuObjPart, x) == 0u &&
                   offsetof(SrPpuObjPart, y) == 2u &&
                   offsetof(SrPpuObjPart, tile_attr) == 4u &&
                   offsetof(SrPpuObjPart, size) == 6u &&
                   offsetof(SrPpuObjPart, reserved) == 7u,
               "public ABI OBJ part field offsets changed");

static RtlGameCpuStateQueryFunc *s_cpu_state_provider;
static void *s_cpu_state_user_data;
static const void *s_cpu_component;
static Snes *s_cpu_state_runner;
static RtlGameExecutionStateQueryFunc *s_execution_state_provider;
static void *s_execution_state_user_data;
static Snes *s_execution_state_runner;
static SrRunnerPpuObjRasterProvider *s_ppu_obj_raster_provider;
static Snes *s_ppu_obj_raster_runner;
static SrRunnerPpuObjResolveProvider *s_ppu_obj_resolve_provider;
static Snes *s_ppu_obj_resolve_runner;
static SrRunnerPpuObjPartsRasterProvider *s_ppu_obj_parts_raster_provider;
static Snes *s_ppu_obj_parts_raster_runner;
static SrRunnerPpuScanoutProvider *s_ppu_scanout_provider;
static Snes *s_ppu_scanout_runner;
static Snes *s_ppu_owner_runner;
static Ppu *s_owned_ppu;

enum { kEventObserverCapacity = 8 };
enum { kMutationCapacity = 32 };

typedef struct EventObserverSlot {
    Snes *runner;
    uint64_t id;
    SrEventSubscription subscription;
} EventObserverSlot;

typedef struct MutationSlot {
    Snes *runner;
    uint64_t id;
    SrMutationCommand command;
    SrMutationState state;
    SrResult result;
    uint64_t applied_frame_counter;
} MutationSlot;

static EventObserverSlot s_event_observers[kEventObserverCapacity];
static uint64_t s_next_event_observer_id = 1u;
static uint64_t s_event_serial;
static atomic_flag s_event_dispatch_lock = ATOMIC_FLAG_INIT;
static MutationSlot s_mutations[kMutationCapacity];
static uint64_t s_next_mutation_id = 1u;
static atomic_uint s_pending_mutation_count;
static atomic_flag s_mutation_lock = ATOMIC_FLAG_INIT;
SrEventMask g_sr_runner_event_mask;

static void lock_event_dispatch(void) {
    while (atomic_flag_test_and_set_explicit(
               &s_event_dispatch_lock, memory_order_acquire)) {}
}

static void unlock_event_dispatch(void) {
    atomic_flag_clear_explicit(&s_event_dispatch_lock, memory_order_release);
}

static void lock_mutations(void) {
    while (atomic_flag_test_and_set_explicit(
               &s_mutation_lock, memory_order_acquire)) {}
}

static void unlock_mutations(void) {
    atomic_flag_clear_explicit(&s_mutation_lock, memory_order_release);
}

static Snes *runner_from_handle(SrRunnerHandle *runner) {
    return (Snes *)(void *)runner;
}

static const SrComponentHandle *component_handle(const void *component) {
    return (const SrComponentHandle *)component;
}

/* Keep these domains in one translation unit: the public ABI is cold, while
 * several PPU helpers share hot private state and benefit from whole-unit
 * optimization. The fragments provide maintainable ownership without changing
 * object layout or forcing internal symbols into headers. */
#include "runner_core.inc"
#include "runner_events.inc"
#include "runner_mutation.inc"
#include "runner_ppu.inc"
#include "runner_state.inc"

