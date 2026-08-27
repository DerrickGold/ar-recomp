#include "runner_next.h"

#include "runner_next_internal.h"
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
_Static_assert(sizeof(SrPpuObjPart) == 8u,
               "public ABI OBJ part must have a fixed layout");
_Static_assert(offsetof(SrPpuObjPart, x) == 0u &&
                   offsetof(SrPpuObjPart, y) == 2u &&
                   offsetof(SrPpuObjPart, tile_attr) == 4u &&
                   offsetof(SrPpuObjPart, size) == 6u &&
                   offsetof(SrPpuObjPart, reserved) == 7u,
               "public ABI OBJ part field offsets changed");

static SrRunnerCpuStateProvider *s_cpu_state_provider;
static const void *s_cpu_component;
static Snes *s_cpu_state_runner;
static SrRunnerExecutionStateProvider *s_execution_state_provider;
static Snes *s_execution_state_runner;
static SrRunnerPpuObjRasterProvider *s_ppu_obj_raster_provider;
static Snes *s_ppu_obj_raster_runner;
static SrRunnerPpuObjResolveProvider *s_ppu_obj_resolve_provider;
static Snes *s_ppu_obj_resolve_runner;
static SrRunnerPpuObjPartsRasterProvider *s_ppu_obj_parts_raster_provider;
static Snes *s_ppu_obj_parts_raster_runner;
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

static SrResult get_component(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component) {
    Snes *snes = runner_from_handle(runner);
    const void *resolved = NULL;
    if (out_component == NULL) return SR_RESULT_INVALID_ARGUMENT;
    *out_component = NULL;
    if (snes == NULL) return SR_RESULT_INVALID_ARGUMENT;
    switch (component) {
        case SR_COMPONENT_RUNNER: resolved = snes; break;
        case SR_COMPONENT_CPU:
            resolved = s_cpu_state_runner == snes && s_cpu_component != NULL
                ? s_cpu_component : snes->cpu;
            break;
        case SR_COMPONENT_PPU: resolved = snes->ppu; break;
        case SR_COMPONENT_APU: resolved = snes->apu; break;
        case SR_COMPONENT_DSP:
            resolved = snes->apu != NULL ? snes->apu->dsp : NULL;
            break;
        case SR_COMPONENT_SPC:
            resolved = snes->apu != NULL ? snes->apu->spc : NULL;
            break;
        case SR_COMPONENT_DMA: resolved = snes->dma; break;
        case SR_COMPONENT_CARTRIDGE: resolved = snes->cart; break;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
    *out_component = component_handle(resolved);
    return resolved != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult query_generations(
        SrRunnerHandle *runner, SrGenerationSnapshot *out_generations) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_generations == NULL ||
        out_generations->struct_size < SR_GENERATION_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    out_generations->struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE;
    out_generations->reserved = 0u;
    out_generations->lifetime_generation = snes->abiLifetimeGeneration;
    out_generations->tick_generation = snes->abiTickGeneration;
    out_generations->reset_generation = snes->abiResetGeneration;
    out_generations->load_generation = snes->abiLoadGeneration;
    out_generations->mutation_generation = snes->abiMutationGeneration;
    return SR_RESULT_OK;
}

static SrResult resolve_memory(Snes *snes, SrMemoryRegion region,
                               const uint8_t **data, uint64_t *byte_size) {
    if (snes == NULL || data == NULL || byte_size == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    switch (region) {
        case SR_MEMORY_WRAM:
            *data = snes->ram;
            *byte_size = kSnesWramSize;
            break;
        case SR_MEMORY_SRAM:
            if (snes->cart == NULL || snes->cart->ram == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->ram;
            *byte_size = snes->cart->ramSize;
            break;
        case SR_MEMORY_ROM:
            if (snes->cart == NULL || snes->cart->rom == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->rom;
            *byte_size = snes->cart->romSize;
            break;
        case SR_MEMORY_HIGH_OAM:
            if (snes->ppu == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->ppu->highOam;
            *byte_size = SR_PPU_HIGH_OAM_BYTE_COUNT;
            break;
        case SR_MEMORY_APU_RAM:
        case SR_MEMORY_DSP_REGISTERS:
            /* The audio thread can advance these components between main
             * runner ticks. They need an APU-lock-aware pinned snapshot, not
             * a borrowed pointer with a misleading main-thread generation. */
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
        default:
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return *data != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult borrow_memory(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_SPAN_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_memory(snes, region, &data, &byte_size);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_SPAN_V2_SIZE);
        out_span->struct_size = SR_BORROWED_SPAN_V2_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_SPAN_V2_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->byte_size = byte_size;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_is_valid(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_SPAN_V2_SIZE || span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_memory(snes, span->region, &data, &byte_size) != SR_RESULT_OK)
        return 0u;
    return span->data == data && span->byte_size == byte_size ? 1u : 0u;
}

static SrResult resolve_u16_memory(Snes *snes, SrMemoryRegion region,
                                   const uint16_t **data,
                                   uint64_t *element_count) {
    if (snes == NULL || data == NULL || element_count == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (snes->ppu == NULL) {
        *data = NULL;
        *element_count = 0u;
        return SR_RESULT_UNAVAILABLE;
    }
    switch (region) {
        case SR_MEMORY_VRAM:
            *data = snes->ppu->vram;
            *element_count = SR_PPU_VRAM_WORD_COUNT;
            break;
        case SR_MEMORY_CGRAM:
            *data = snes->ppu->cgram;
            *element_count = SR_PPU_CGRAM_WORD_COUNT;
            break;
        case SR_MEMORY_OAM:
            *data = snes->ppu->oam;
            *element_count = SR_PPU_OAM_WORD_COUNT;
            break;
        default:
            *data = NULL;
            *element_count = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return SR_RESULT_OK;
}

static SrResult borrow_u16_memory(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_U16_SPAN_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_u16_memory(snes, region, &data, &element_count);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_U16_SPAN_V2_SIZE);
        out_span->struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->element_count = element_count;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_u16_is_valid(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_U16_SPAN_V2_SIZE ||
        span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_u16_memory(snes, span->region, &data, &element_count) !=
        SR_RESULT_OK)
        return 0u;
    return span->data == data && span->element_count == element_count ? 1u : 0u;
}

static SrResult query_cpu_state(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_CPU_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_CPU_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_CPU_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_cpu_state_runner != snes || s_cpu_state_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_cpu_state_provider(snes, out_state);
}

static SrResult query_execution_state(
        SrRunnerHandle *runner, SrExecutionSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_EXECUTION_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_EXECUTION_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_EXECUTION_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_execution_state_runner != snes ||
        s_execution_state_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_execution_state_provider(snes, out_state);
}

static void recompute_event_mask(void) {
    SrEventMask mask = 0u;
    unsigned index;
    for (index = 0u; index < kEventObserverCapacity; ++index) {
        if (s_event_observers[index].id != 0u)
            mask |= s_event_observers[index].subscription.event_mask;
    }
    g_sr_runner_event_mask = mask;
}

static SrResult subscribe_events(
        SrRunnerHandle *runner, const SrEventSubscription *subscription,
        uint64_t *out_subscription_id) {
    Snes *snes = runner_from_handle(runner);
    unsigned index;
    if (out_subscription_id != NULL) *out_subscription_id = 0u;
    if (snes == NULL || subscription == NULL || out_subscription_id == NULL ||
        subscription->struct_size < SR_EVENT_SUBSCRIPTION_V2_SIZE ||
        subscription->callback == NULL || subscription->event_mask == 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if ((subscription->event_mask & ~SR_EVENT_MASK_V2_SUPPORTED) != 0u)
        return SR_RESULT_UNSUPPORTED;
    if ((subscription->flags &
         ~(SR_EVENT_FILTER_PC_RANGE | SR_EVENT_FILTER_ADDRESS_RANGE |
           SR_EVENT_FILTER_MEMORY_REGION)) != 0u)
        return SR_RESULT_UNSUPPORTED;
    if ((subscription->flags & SR_EVENT_FILTER_ADDRESS_RANGE) != 0u &&
        (subscription->event_mask &
         ~(SR_EVENT_MASK_MEMORY_WRITE | SR_EVENT_MASK_REGISTER_ACCESS |
           SR_EVENT_MASK_DMA)) != 0u)
        return SR_RESULT_UNSUPPORTED;
    if ((subscription->flags & SR_EVENT_FILTER_MEMORY_REGION) != 0u &&
        (subscription->event_mask & ~SR_EVENT_MASK_MEMORY_WRITE) != 0u)
        return SR_RESULT_UNSUPPORTED;
    if ((subscription->flags & SR_EVENT_FILTER_PC_RANGE) != 0u &&
        ((subscription->event_mask &
          ~(SR_EVENT_MASK_EXECUTION_BLOCK |
            SR_EVENT_MASK_DYNAMIC_DISPATCH |
            SR_EVENT_MASK_INTERRUPT |
            SR_EVENT_MASK_ERROR)) != 0u ||
         subscription->pc_first > subscription->pc_last ||
         subscription->pc_last > UINT32_C(0x00ffffff)))
        return (subscription->event_mask &
                ~(SR_EVENT_MASK_EXECUTION_BLOCK |
                  SR_EVENT_MASK_DYNAMIC_DISPATCH |
                  SR_EVENT_MASK_INTERRUPT |
                  SR_EVENT_MASK_ERROR)) != 0u
            ? SR_RESULT_UNSUPPORTED : SR_RESULT_INVALID_ARGUMENT;
    if ((subscription->flags & SR_EVENT_FILTER_ADDRESS_RANGE) != 0u &&
        subscription->address_first > subscription->address_last)
        return SR_RESULT_INVALID_ARGUMENT;
    if ((subscription->flags & SR_EVENT_FILTER_MEMORY_REGION) != 0u &&
        subscription->memory_region > SR_MEMORY_HIGH_OAM)
        return SR_RESULT_INVALID_ARGUMENT;
    for (index = 0u; index < kEventObserverCapacity; ++index) {
        EventObserverSlot *slot = &s_event_observers[index];
        if (slot->id != 0u) continue;
        slot->runner = snes;
        slot->subscription = *subscription;
        slot->subscription.struct_size = SR_EVENT_SUBSCRIPTION_V2_SIZE;
        slot->id = s_next_event_observer_id++;
        if (slot->id == 0u) slot->id = s_next_event_observer_id++;
        *out_subscription_id = slot->id;
        recompute_event_mask();
        return SR_RESULT_OK;
    }
    return SR_RESULT_UNAVAILABLE;
}

static SrResult unsubscribe_events(SrRunnerHandle *runner,
                                   uint64_t subscription_id) {
    Snes *snes = runner_from_handle(runner);
    unsigned index;
    if (snes == NULL || subscription_id == 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    for (index = 0u; index < kEventObserverCapacity; ++index) {
        EventObserverSlot *slot = &s_event_observers[index];
        if (slot->runner == snes && slot->id == subscription_id) {
            memset(slot, 0, sizeof(*slot));
            recompute_event_mask();
            return SR_RESULT_OK;
        }
    }
    return SR_RESULT_UNAVAILABLE;
}

void sr_runner_emit_event(Snes *snes, SrEventMask event_mask,
                          SrRunnerEvent *event) {
    unsigned index;
    if (snes == NULL || event == NULL || event_mask == 0u ||
        (g_sr_runner_event_mask & event_mask) == 0u)
        return;
    lock_event_dispatch();
    event->struct_size = SR_RUNNER_EVENT_V2_SIZE;
    event->serial = ++s_event_serial;
    for (index = 0u; index < kEventObserverCapacity; ++index) {
        EventObserverSlot *slot = &s_event_observers[index];
        const SrEventSubscription *filter = &slot->subscription;
        SrRunnerEventCallback callback;
        void *user_data;
        if (slot->runner != snes || slot->id == 0u ||
            (filter->event_mask & event_mask) == 0u)
            continue;
        if ((filter->flags & SR_EVENT_FILTER_PC_RANGE) != 0u &&
            (event->pc24 < filter->pc_first ||
             event->pc24 > filter->pc_last))
            continue;
        if ((filter->flags & SR_EVENT_FILTER_ADDRESS_RANGE) != 0u &&
            (event->address < filter->address_first ||
             event->address > filter->address_last))
            continue;
        if ((filter->flags & SR_EVENT_FILTER_MEMORY_REGION) != 0u &&
            event->memory_region != filter->memory_region)
            continue;
        callback = filter->callback;
        user_data = filter->user_data;
        callback(user_data, (SrRunnerHandle *)(void *)snes, event);
    }
    unlock_event_dispatch();
}

void sr_runner_emit_memory_write(Snes *snes, SrMemoryRegion region,
                                 uint32_t address, uint32_t previous_value,
                                 uint32_t value, uint32_t width_bytes) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_MEMORY_WRITE;
    event.frame_counter = snes != NULL ? snes->abiFrameCounter : 0u;
    event.memory_region = region;
    event.address = address;
    event.previous_value = previous_value;
    event.value = value;
    event.width_bytes = width_bytes;
    sr_runner_emit_event(snes, SR_EVENT_MASK_MEMORY_WRITE, &event);
}

void sr_runner_emit_ppu_memory_write(Ppu *ppu, SrMemoryRegion region,
                                     uint32_t address,
                                     uint32_t previous_value,
                                     uint32_t value,
                                     uint32_t width_bytes) {
    if (ppu != s_owned_ppu) return;
    sr_runner_emit_memory_write(s_ppu_owner_runner, region, address,
                                previous_value, value, width_bytes);
}

void sr_runner_emit_register_access(Snes *snes, bool write,
                                    uint32_t address, uint32_t value,
                                    uint32_t width_bytes) {
    SrRunnerEvent event = {0};
    event.type = write ? SR_EVENT_REGISTER_WRITE : SR_EVENT_REGISTER_READ;
    event.frame_counter = snes != NULL ? snes->abiFrameCounter : 0u;
    event.address = address;
    event.value = value;
    event.width_bytes = width_bytes;
    sr_runner_emit_event(snes, SR_EVENT_MASK_REGISTER_ACCESS, &event);
}

void sr_runner_emit_frame_boundary(Snes *snes, uint32_t flags,
                                   const char *label) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_FRAME_BOUNDARY;
    event.frame_counter = snes != NULL ? snes->abiFrameCounter : 0u;
    event.flags = flags;
    event.label = label;
    sr_runner_emit_event(snes, SR_EVENT_MASK_FRAME, &event);
}

void sr_runner_emit_audio_produced(Snes *snes, const int16_t *samples,
                                   uint64_t frame_offset,
                                   uint32_t frame_count,
                                   uint32_t sample_rate,
                                   uint16_t channel_count) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_AUDIO_PRODUCED;
    event.flags = SR_EVENT_AUDIO_FINAL_MIX |
                  SR_EVENT_AUDIO_TRANSIENT_SAMPLES;
    event.label = "final-mix";
    event.audio_frame_offset = frame_offset;
    event.audio_samples = samples;
    event.audio_frame_count = frame_count;
    event.audio_sample_rate = sample_rate;
    event.audio_channel_count = channel_count;
    event.audio_sample_format = SR_AUDIO_SAMPLE_FORMAT_S16_NATIVE;
    sr_runner_emit_event(snes, SR_EVENT_MASK_AUDIO, &event);
}

void sr_runner_emit_interrupt(Snes *snes, SrInterruptKind kind,
                              uint32_t flags, uint32_t pc24,
                              uint16_t vector, int32_t scanline,
                              const char *label) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_INTERRUPT;
    event.frame_counter = snes != NULL ? snes->abiFrameCounter : 0u;
    event.flags = flags;
    event.pc24 = pc24 & UINT32_C(0x00ffffff);
    event.interrupt_kind = kind;
    event.interrupt_vector = vector;
    event.interrupt_scanline = scanline;
    event.label = label;
    sr_runner_emit_event(snes, SR_EVENT_MASK_INTERRUPT, &event);
}

void sr_runner_emit_error(Snes *snes, SrRunnerErrorCode code,
                          uint32_t flags, uint32_t pc24,
                          uint32_t source_pc24, const char *label) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_ERROR;
    event.frame_counter = snes != NULL ? snes->abiFrameCounter : 0u;
    event.flags = flags;
    event.pc24 = pc24 & UINT32_C(0x00ffffff);
    event.source_pc24 = source_pc24 & UINT32_C(0x00ffffff);
    event.address = event.pc24;
    event.error_code = code;
    event.label = label;
    sr_runner_emit_event(snes, SR_EVENT_MASK_ERROR, &event);
}

void sr_runner_clear_event_subscriptions(Snes *snes) {
    unsigned index;
    if (snes == NULL) return;
    for (index = 0u; index < kEventObserverCapacity; ++index) {
        if (s_event_observers[index].runner == snes)
            memset(&s_event_observers[index], 0,
                   sizeof(s_event_observers[index]));
    }
    recompute_event_mask();
}

static SrResult mutation_region_size(Snes *snes, SrMemoryRegion region,
                                     uint64_t *out_size) {
    if (snes == NULL || out_size == NULL) return SR_RESULT_INVALID_ARGUMENT;
    switch (region) {
        case SR_MEMORY_WRAM:
            *out_size = kSnesWramSize;
            return snes->ram != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
        case SR_MEMORY_SRAM:
            if (snes->cart == NULL || snes->cart->ram == NULL)
                return SR_RESULT_UNAVAILABLE;
            *out_size = snes->cart->ramSize;
            return SR_RESULT_OK;
        case SR_MEMORY_VRAM:
            *out_size = SR_PPU_VRAM_WORD_COUNT * 2u;
            return snes->ppu != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
        case SR_MEMORY_CGRAM:
            *out_size = SR_PPU_CGRAM_WORD_COUNT * 2u;
            return snes->ppu != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
        case SR_MEMORY_OAM:
            *out_size = SR_PPU_OAM_WORD_COUNT * 2u;
            return snes->ppu != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
        case SR_MEMORY_HIGH_OAM:
            *out_size = SR_PPU_HIGH_OAM_BYTE_COUNT;
            return snes->ppu != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
        case SR_MEMORY_ROM:
        case SR_MEMORY_APU_RAM:
        case SR_MEMORY_DSP_REGISTERS:
            return SR_RESULT_UNSUPPORTED;
        default:
            return SR_RESULT_INVALID_ARGUMENT;
    }
}

static SrResult validate_mutation(Snes *snes,
                                  const SrMutationCommand *command) {
    uint64_t region_size;
    SrResult result;
    if (snes == NULL || command == NULL ||
        command->struct_size < SR_MUTATION_COMMAND_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    if (command->flags != 0u) return SR_RESULT_UNSUPPORTED;
    switch (command->type) {
        case SR_MUTATION_WRITE_MEMORY:
            if (command->byte_count == 0u ||
                command->byte_count > SR_MUTATION_INLINE_BYTE_CAPACITY)
                return SR_RESULT_INVALID_ARGUMENT;
            result = mutation_region_size(snes, command->memory_region,
                                          &region_size);
            if (result != SR_RESULT_OK) return result;
            if (command->address > region_size ||
                command->byte_count > region_size - command->address)
                return SR_RESULT_INVALID_ARGUMENT;
            return SR_RESULT_OK;
        case SR_MUTATION_SET_INPUT:
            if ((command->input_value & UINT32_C(0xff000000)) != 0u ||
                (command->input_mask & UINT32_C(0xff000000)) != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            return SR_RESULT_OK;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
}

static SrResult queue_mutation(SrRunnerHandle *runner,
                               const SrMutationCommand *command,
                               uint64_t *out_command_id) {
    Snes *snes = runner_from_handle(runner);
    SrResult result;
    unsigned index;
    if (out_command_id != NULL) *out_command_id = 0u;
    if (out_command_id == NULL) return SR_RESULT_INVALID_ARGUMENT;
    result = validate_mutation(snes, command);
    if (result != SR_RESULT_OK) return result;
    lock_mutations();
    for (index = 0u; index < kMutationCapacity; ++index) {
        MutationSlot *slot = &s_mutations[index];
        if (slot->id != 0u) continue;
        slot->runner = snes;
        slot->command = *command;
        slot->command.struct_size = SR_MUTATION_COMMAND_V2_SIZE;
        slot->id = s_next_mutation_id++;
        if (slot->id == 0u) slot->id = s_next_mutation_id++;
        slot->state = SR_MUTATION_STATE_QUEUED;
        slot->result = SR_RESULT_PENDING;
        *out_command_id = slot->id;
        atomic_fetch_add_explicit(&s_pending_mutation_count, 1u,
                                  memory_order_release);
        unlock_mutations();
        return SR_RESULT_OK;
    }
    unlock_mutations();
    return SR_RESULT_UNAVAILABLE;
}

static SrResult query_mutation(SrRunnerHandle *runner, uint64_t command_id,
                               uint32_t flags,
                               SrMutationStatus *out_status) {
    Snes *snes = runner_from_handle(runner);
    unsigned index;
    if (snes == NULL || command_id == 0u || out_status == NULL ||
        out_status->struct_size < SR_MUTATION_STATUS_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    if ((flags & ~SR_MUTATION_QUERY_CONSUME) != 0u)
        return SR_RESULT_UNSUPPORTED;
    lock_mutations();
    for (index = 0u; index < kMutationCapacity; ++index) {
        MutationSlot *slot = &s_mutations[index];
        if (slot->runner != snes || slot->id != command_id) continue;
        memset(out_status, 0, SR_MUTATION_STATUS_V2_SIZE);
        out_status->struct_size = SR_MUTATION_STATUS_V2_SIZE;
        out_status->state = slot->state;
        out_status->result = slot->result;
        out_status->command_id = slot->id;
        out_status->applied_frame_counter = slot->applied_frame_counter;
        if ((flags & SR_MUTATION_QUERY_CONSUME) != 0u &&
            (slot->state == SR_MUTATION_STATE_APPLIED ||
             slot->state == SR_MUTATION_STATE_FAILED))
            memset(slot, 0, sizeof(*slot));
        unlock_mutations();
        return SR_RESULT_OK;
    }
    unlock_mutations();
    return SR_RESULT_UNAVAILABLE;
}

static uint8_t read_word_byte(const uint16_t *values, uint64_t address) {
    uint16_t word = values[address >> 1u];
    return (uint8_t)((word >> ((address & 1u) * 8u)) & 0xffu);
}

static void write_word_byte(uint16_t *values, uint64_t address,
                            uint8_t value) {
    unsigned shift = (unsigned)(address & 1u) * 8u;
    uint16_t mask = (uint16_t)(UINT16_C(0x00ff) << shift);
    uint16_t word = values[address >> 1u];
    values[address >> 1u] = (uint16_t)((word & ~mask) |
                                      ((uint16_t)value << shift));
}

static SrResult apply_memory_mutation(Snes *snes,
                                      const SrMutationCommand *command) {
    uint64_t index;
    SrResult result = validate_mutation(snes, command);
    if (result != SR_RESULT_OK) return result;
    /* Expire borrowed views before the first externally observable write. */
    sr_runner_note_mutation(snes);
    for (index = 0u; index < command->byte_count; ++index) {
        uint64_t address = command->address + index;
        uint8_t previous = 0u;
        uint8_t value = command->bytes[index];
        switch (command->memory_region) {
            case SR_MEMORY_WRAM:
                previous = snes->ram[address];
                snes->ram[address] = value;
                break;
            case SR_MEMORY_SRAM:
                previous = snes->cart->ram[address];
                snes->cart->ram[address] = value;
                break;
            case SR_MEMORY_VRAM:
                previous = read_word_byte(snes->ppu->vram, address);
                write_word_byte(snes->ppu->vram, address, value);
                break;
            case SR_MEMORY_CGRAM:
                previous = read_word_byte(snes->ppu->cgram, address);
                write_word_byte(snes->ppu->cgram, address, value);
                break;
            case SR_MEMORY_OAM:
                previous = read_word_byte(snes->ppu->oam, address);
                write_word_byte(snes->ppu->oam, address, value);
                break;
            case SR_MEMORY_HIGH_OAM:
                previous = snes->ppu->highOam[address];
                snes->ppu->highOam[address] = value;
                break;
            default:
                return SR_RESULT_UNSUPPORTED;
        }
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            sr_runner_emit_memory_write(
                snes, command->memory_region, (uint32_t)address,
                previous, value, 1u);
        }
    }
    return SR_RESULT_OK;
}

static SrResult apply_mutation(Snes *snes,
                               const SrMutationCommand *command,
                               uint32_t *inputs) {
    SrResult result;
    switch (command->type) {
        case SR_MUTATION_WRITE_MEMORY:
            result = apply_memory_mutation(snes, command);
            break;
        case SR_MUTATION_SET_INPUT:
            if (inputs == NULL) return SR_RESULT_UNAVAILABLE;
            sr_runner_note_mutation(snes);
            *inputs = (*inputs & ~command->input_mask) |
                      (command->input_value & command->input_mask);
            result = SR_RESULT_OK;
            break;
        default:
            result = SR_RESULT_UNSUPPORTED;
            break;
    }
    return result;
}

void sr_runner_apply_pending_mutations(Snes *snes, uint32_t *inputs,
                                       uint64_t frame_counter) {
    uint64_t cutoff;
    if (snes == NULL ||
        atomic_load_explicit(&s_pending_mutation_count,
                             memory_order_acquire) == 0u)
        return;
    lock_mutations();
    cutoff = s_next_mutation_id - 1u;
    unlock_mutations();
    for (;;) {
        SrMutationCommand command;
        MutationSlot *selected = NULL;
        uint64_t selected_id = 0u;
        SrResult result;
        unsigned index;
        lock_mutations();
        for (index = 0u; index < kMutationCapacity; ++index) {
            MutationSlot *slot = &s_mutations[index];
            if (slot->runner != snes ||
                slot->state != SR_MUTATION_STATE_QUEUED ||
                slot->id > cutoff ||
                (selected != NULL && slot->id >= selected->id))
                continue;
            selected = slot;
        }
        if (selected == NULL) {
            unlock_mutations();
            break;
        }
        selected->state = SR_MUTATION_STATE_APPLYING;
        selected_id = selected->id;
        command = selected->command;
        atomic_fetch_sub_explicit(&s_pending_mutation_count, 1u,
                                  memory_order_release);
        unlock_mutations();

        result = apply_mutation(snes, &command, inputs);

        lock_mutations();
        for (index = 0u; index < kMutationCapacity; ++index) {
            MutationSlot *slot = &s_mutations[index];
            if (slot->runner != snes || slot->id != selected_id) continue;
            slot->state = result == SR_RESULT_OK
                ? SR_MUTATION_STATE_APPLIED : SR_MUTATION_STATE_FAILED;
            slot->result = result;
            slot->applied_frame_counter = frame_counter;
            break;
        }
        unlock_mutations();
    }
}

void sr_runner_clear_mutations(Snes *snes) {
    unsigned removed_pending = 0u;
    unsigned index;
    if (snes == NULL) return;
    lock_mutations();
    for (index = 0u; index < kMutationCapacity; ++index) {
        MutationSlot *slot = &s_mutations[index];
        if (slot->runner != snes) continue;
        if (slot->state == SR_MUTATION_STATE_QUEUED) ++removed_pending;
        memset(slot, 0, sizeof(*slot));
    }
    if (removed_pending != 0u)
        atomic_fetch_sub_explicit(&s_pending_mutation_count, removed_pending,
                                  memory_order_release);
    unlock_mutations();
}

void sr_runner_bind_ppu_owner(Snes *snes, Ppu *ppu, bool enabled) {
    if (!enabled && s_ppu_owner_runner != snes) return;
    s_ppu_owner_runner = enabled ? snes : NULL;
    s_owned_ppu = enabled ? ppu : NULL;
}

static uint8_t ppu_background_bpp(uint8_t mode, unsigned layer,
                                  uint32_t flags) {
    static const uint8_t bpp[7][4] = {
        {2u, 2u, 2u, 2u}, {4u, 4u, 2u, 0u}, {4u, 4u, 0u, 0u},
        {8u, 4u, 0u, 0u}, {8u, 2u, 0u, 0u}, {4u, 2u, 0u, 0u},
        {4u, 0u, 0u, 0u},
    };
    if (layer >= 4u) return 0u;
    if (mode < 7u) return bpp[mode][layer];
    if (mode == 7u && layer == 0u) return 8u;
    if (mode == 7u && layer == 1u &&
        (flags & SR_PPU_STATE_MODE7_EXT_BG) != 0u) return 8u;
    return 0u;
}

static SrResult query_ppu_state(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned layer;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_state->flags =
        (PPU_forcedBlank(ppu) ? SR_PPU_STATE_FORCED_BLANK : 0u) |
        (PPU_bg3priority(ppu) ? SR_PPU_STATE_BG3_PRIORITY : 0u) |
        (PPU_interlace(ppu) ? SR_PPU_STATE_INTERLACE : 0u) |
        (PPU_objInterlace(ppu) ? SR_PPU_STATE_OBJ_INTERLACE : 0u) |
        (PPU_overscan(ppu) ? SR_PPU_STATE_OVERSCAN : 0u) |
        (PPU_pseudoHires(ppu) ? SR_PPU_STATE_PSEUDO_HIRES : 0u) |
        (PPU_m7extBg(ppu) ? SR_PPU_STATE_MODE7_EXT_BG : 0u);
    out_state->display_control = ppu->inidisp;
    out_state->object_select = ppu->obsel;
    out_state->bg_mode_control = ppu->bgmode;
    out_state->mosaic_control = ppu->mosaic;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->brightness = (uint8_t)PPU_brightness(ppu);
    out_state->main_screen = ppu->screenEnabled[0];
    out_state->sub_screen = ppu->screenEnabled[1];
    out_state->main_windowed = ppu->screenWindowed[0];
    out_state->sub_windowed = ppu->screenWindowed[1];
    out_state->object_size_select = (uint8_t)PPU_objSize(ppu);
    out_state->margin_left = ppu->extraLeftCur;
    out_state->margin_right = ppu->extraRightCur;
    out_state->margin_top = ppu->extraTopCur;
    out_state->margin_bottom = ppu->extraBottomCur;
    out_state->object_tile_base_1_word = (uint32_t)PPU_objTileAdr1(ppu);
    out_state->object_tile_base_2_word = (uint32_t)PPU_objTileAdr2(ppu);
    for (layer = 0u; layer < 4u; ++layer) {
        SrPpuBackgroundState *background = &out_state->backgrounds[layer];
        background->h_scroll = ppu->hScroll[layer];
        background->v_scroll = ppu->vScroll[layer];
        background->tilemap_base_word =
            (uint16_t)PPU_bgTilemapAdr(ppu, layer);
        background->tile_base_word = (uint16_t)PPU_bgTileAdr(ppu, layer);
        background->tilemap_width_tiles =
            PPU_bgTilemapWider(ppu, layer) ? 64u : 32u;
        background->tilemap_height_tiles =
            PPU_bgTilemapHigher(ppu, layer) ? 64u : 32u;
        background->tile_size_pixels = PPU_bigTiles(ppu, layer) ? 16u : 8u;
        background->bits_per_pixel = ppu_background_bpp(
            out_state->bg_mode, layer, out_state->flags);
    }
    out_state->window_select = ppu->windowsel;
    out_state->window_logic = ppu->wbgobjlog;
    out_state->color_math_control = ppu->cgwsel;
    out_state->color_math_designation = ppu->cgadsub;
    for (layer = 0u; layer < 4u; ++layer)
        out_state->background_tilemap_control[layer] = ppu->bgXsc[layer];
    out_state->background_tile_base_control = ppu->bgTileAdr;
    out_state->mode7_select = ppu->m7sel;
    memcpy(out_state->mode7_matrix, ppu->m7matrix,
           sizeof(out_state->mode7_matrix));
    return SR_RESULT_OK;
}

static SrResult resolve_ppu_background_coordinate(
        SrRunnerHandle *runner,
        const SrPpuBackgroundCoordinateRequest *request,
        SrPpuBackgroundCoordinateResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    PpuWidescreenLayerPolicy policy;
    int source_x = 0;
    int sample_y = 0;
    bool mapped;
    bool mosaic = false;
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_BACKGROUND_COORDINATE_REQUEST_V2_SIZE ||
        out_result->struct_size <
            SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE ||
        request->flags != 0u || request->layer >= 4u ||
        request->screen_x < -(int32_t)SR_PPU_HORIZONTAL_MARGIN_MAX ||
        request->screen_x >= (int32_t)(SR_PPU_NATIVE_WIDTH +
                                       SR_PPU_HORIZONTAL_MARGIN_MAX) ||
        request->screen_y < -(int32_t)SR_PPU_VERTICAL_MARGIN_MAX ||
        request->screen_y >= (int32_t)(SR_PPU_NATIVE_HEIGHT +
                                       SR_PPU_VERTICAL_MARGIN_MAX))
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE);
    out_result->struct_size =
        SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (snes->ppu == NULL) return SR_RESULT_UNAVAILABLE;
    mapped = PpuResolveBackgroundCoordinate(
        snes->ppu, (uint8_t)request->layer, request->screen_x,
        request->screen_y, &source_x, &sample_y, &policy, &mosaic);
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
                   "ABI background fill values must match the PPU");
    _Static_assert(SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE ==
                       kPpuWidescreenMotion_FillRelative &&
                   SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL ==
                       kPpuWidescreenMotion_NormalScroll,
                   "ABI background motion values must match the PPU");
    out_result->flags =
        (mapped ? SR_PPU_BACKGROUND_COORDINATE_MAPPED : 0u) |
        (policy.band_override
             ? SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE : 0u) |
        (mosaic ? SR_PPU_BACKGROUND_COORDINATE_MOSAIC : 0u);
    out_result->source_x = source_x;
    out_result->sample_y = sample_y;
    out_result->fill = (SrPpuBackgroundFill)policy.fill;
    out_result->motion = (SrPpuBackgroundMotion)policy.motion;
    return SR_RESULT_OK;
}

static uint8_t ppu_frame_color_component(uint16_t value,
                                         uint8_t brightness) {
    uint32_t expanded = ((uint32_t)value << 3) | ((uint32_t)value >> 2);
    return (uint8_t)((expanded * brightness) / 15u);
}

static uint32_t ppu_frame_color_argb(const Ppu *ppu, uint16_t color) {
    uint8_t brightness = (uint8_t)PPU_brightness(ppu);
    return UINT32_C(0xff000000) |
        ((uint32_t)ppu_frame_color_component(color & 0x1fu, brightness) << 16) |
        ((uint32_t)ppu_frame_color_component((color >> 5) & 0x1fu,
                                             brightness) << 8) |
        ppu_frame_color_component((color >> 10) & 0x1fu, brightness);
}

static uint32_t ppu_overlay_content_mask(const Ppu *ppu, unsigned source) {
    uint32_t mask = 0u;
    unsigned band;
    if (ppu->overlayRenderBuffer[source] != NULL &&
        (ppu->overlayRenderContentMask[source] & 1u) != 0u)
        mask |= 1u;
    for (band = 1u; band <= 3u; ++band) {
        if (ppu->overlayRenderBands[source][band - 1u] != NULL &&
            (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u)
            mask |= 1u << band;
    }
    return mask;
}

static uint32_t ppu_overlay_fill_argb(
        const Ppu *ppu, const PpuOverlayCapture *capture) {
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Black)
        return UINT32_C(0xff000000);
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Cgram)
        return ppu_frame_color_argb(
            ppu, ppu->cgram[capture->transparentFillCgram]);
    return 0u;
}

static SrResult query_ppu_frame_state(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned source;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_FRAME_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_FRAME_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_PPU_FRAME_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    _Static_assert(SR_PPU_OVERLAY_SOURCE_COUNT == kPpuOverlaySource_Count,
                   "ABI overlay count must match the PPU");
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
                   "ABI overlay flags must match the PPU");
    out_state->display_control = ppu->inidisp;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->hud_split_height = ppu->wsHudSplitHeight;
    out_state->hud_left_end = ppu->wsHudLeftEnd;
    out_state->hud_right_start = ppu->wsHudRightStart;
    out_state->hud_player_row_y = ppu->wsHudPlayerRowY;
    out_state->hud_left_only_y = ppu->wsHudLeftOnlyY;
    out_state->margin_budget = ppu->extraLeftRight;
    out_state->mode7_override_active = ppu->m7Override.rgba != NULL ? 1u : 0u;
    out_state->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        SrPpuOverlayState *overlay = &out_state->overlays[source];
        overlay->x0 = capture->x0;
        overlay->x1 = capture->x1;
        overlay->y0 = capture->y0;
        overlay->y1 = capture->y1;
        overlay->flags = capture->flags;
        overlay->content_band_mask = ppu_overlay_content_mask(ppu, source);
        overlay->transparent_fill_argb =
            ppu_overlay_fill_argb(ppu, capture);
        overlay->transparent_fill_configured =
            capture->transparentFillConfigured != 0u ? 1u : 0u;
        overlay->oam_first = capture->oamFirst;
        overlay->oam_count = capture->oamCount;
    }
    return SR_RESULT_OK;
}

static void ppu_surface_view_init(SrPpuSurfaceView *view,
                                  const uint8_t *data, uint32_t pitch,
                                  uint32_t height, int32_t origin_x,
                                  int32_t origin_y, uint32_t scale,
                                  bool has_content) {
    if (view == NULL || data == NULL || pitch == 0u ||
        pitch % sizeof(uint32_t) != 0u || height == 0u) return;
    view->flags = SR_PPU_SURFACE_BOUND |
        (has_content ? SR_PPU_SURFACE_HAS_CONTENT : 0u);
    view->pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32;
    view->data = data;
    view->pitch_bytes = pitch;
    view->byte_size = (uint64_t)pitch * height;
    view->width_pixels = pitch / (uint32_t)sizeof(uint32_t);
    view->height_pixels = height;
    view->origin_x = origin_x;
    view->origin_y = origin_y;
    view->scale = scale;
}

static uint32_t ppu_overlay_surface_height(
        const PpuOverlayCapture *capture) {
    int height;
    if (capture == NULL || capture->x1 <= capture->x0 ||
        capture->y1 <= capture->y0) return 0u;
    height = capture->y0 < 0 ? capture->y1 - capture->y0 : capture->y1;
    if (height <= 0) return 0u;
    if (height > kPpuBufHeight) height = kPpuBufHeight;
    return (uint32_t)height;
}

static SrResult query_ppu_surfaces(
        SrRunnerHandle *runner, SrPpuSurfaceSnapshot *out_surfaces) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint32_t rendered_height;
    unsigned source, band;
    if (snes == NULL || out_surfaces == NULL ||
        out_surfaces->struct_size < SR_PPU_SURFACE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_surfaces, 0, SR_PPU_SURFACE_SNAPSHOT_V2_SIZE);
    out_surfaces->struct_size = SR_PPU_SURFACE_SNAPSHOT_V2_SIZE;
    out_surfaces->lifetime_generation = snes->abiLifetimeGeneration;
    out_surfaces->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    out_surfaces->band_count = SR_PPU_SURFACE_BAND_COUNT;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_surfaces->binding_generation = ppu->surfaceBindingGeneration;
    rendered_height = (uint32_t)PpuRenderedHeight(ppu);
    ppu_surface_view_init(
        &out_surfaces->main, ppu->renderBuffer, ppu->renderPitch,
        rendered_height,
        PpuSurfaceApron(ppu, ppu->renderPitch) + ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    ppu_surface_view_init(
        &out_surfaces->authentic, ppu->authenticRenderBuffer,
        ppu->authenticRenderPitch, rendered_height,
        PpuSurfaceApron(ppu, ppu->authenticRenderPitch) +
            ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        uint32_t height = ppu->overlayRenderHeight[source] != 0u
            ? ppu->overlayRenderHeight[source]
            : ppu_overlay_surface_height(capture);
        int32_t origin_y = capture->y0 < 0 ? -capture->y0 : 0;
        ppu_surface_view_init(
            &out_surfaces->overlays[source][0],
            ppu->overlayRenderBuffer[source],
            ppu->overlayRenderPitch[source], height,
            PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                ppu->extraLeftRight,
            origin_y, 1u,
            (ppu->overlayRenderContentMask[source] & 1u) != 0u);
        for (band = 1u; band < SR_PPU_SURFACE_BAND_COUNT; ++band)
            ppu_surface_view_init(
                &out_surfaces->overlays[source][band],
                ppu->overlayRenderBands[source][band - 1u],
                ppu->overlayRenderPitch[source], height,
                PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                    ppu->extraLeftRight,
                origin_y, 1u,
                (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u);
    }
    if (ppu->m7OverlayBuffer != NULL && ppu->m7OverlayScale != 0u) {
        uint32_t scale = ppu->m7OverlayScale;
        int32_t width = (int32_t)(ppu->m7OverlayPitch / sizeof(uint32_t));
        int32_t span = (kPpuXPixels + 2 * ppu->extraLeftRight) *
            (int32_t)scale;
        int32_t apron = (width - span) / 2;
        if (apron < 0) apron = 0;
        ppu_surface_view_init(
            &out_surfaces->mode7, ppu->m7OverlayBuffer,
            ppu->m7OverlayPitch, rendered_height * scale,
            apron + ppu->extraLeftRight * (int32_t)scale,
            PpuVerticalOrigin(ppu) * (int32_t)scale, scale,
            ppu->m7Override.rgba != NULL);
    }
    return SR_RESULT_OK;
}

static uint32_t ppu_surface_snapshot_is_valid(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces) {
    Snes *snes = runner_from_handle(runner);
    return snes != NULL && snes->ppu != NULL && surfaces != NULL &&
        surfaces->struct_size >= SR_PPU_SURFACE_SNAPSHOT_V2_SIZE &&
        surfaces->lifetime_generation == snes->abiLifetimeGeneration &&
        surfaces->binding_generation ==
            snes->ppu->surfaceBindingGeneration;
}

static Ppu *ppu_output_control_target(Snes *snes) {
    if (snes == NULL || snes->ppu == NULL || s_ppu_owner_runner != snes ||
        s_owned_ppu != snes->ppu) return NULL;
    return snes->ppu;
}

static SrResult validate_ppu_output_capacity(
        const SrPpuOutputBindingRequest *request, uint64_t minimum_width,
        uint64_t maximum_width, uint64_t maximum_height) {
    uint64_t width;
    if (request->pixels == NULL) {
        return request->pixel_byte_size == 0u && request->pitch_bytes == 0u &&
                       request->height_pixels == 0u
            ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
    }
    if (((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u ||
        request->pitch_bytes == 0u ||
        request->pitch_bytes % sizeof(uint32_t) != 0u ||
        request->pitch_bytes > (uint64_t)SIZE_MAX ||
        request->height_pixels == 0u ||
        request->height_pixels > maximum_height)
        return SR_RESULT_INVALID_ARGUMENT;
    width = request->pitch_bytes / sizeof(uint32_t);
    if (width < minimum_width || width > maximum_width ||
        request->pitch_bytes > UINT64_MAX / request->height_pixels ||
        request->pixel_byte_size <
            request->pitch_bytes * request->height_pixels)
        return SR_RESULT_INVALID_ARGUMENT;
    return SR_RESULT_OK;
}

static SrResult bind_ppu_output_surface(
        SrRunnerHandle *runner, const SrPpuOutputBindingRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    SrResult capacity_result;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE ||
        request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    switch (request->kind) {
        case SR_PPU_OUTPUT_MAIN: {
            uint32_t render_flags = 0u;
            if ((request->flags &
                 ~SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER) != 0u ||
                request->source != 0u || request->band != 0u ||
                request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            if ((request->flags &
                 SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER) != 0u)
                render_flags |= kPpuRenderFlags_ReferencePixelRenderer;
            PpuBeginDrawing(ppu, request->pixels,
                            (size_t)request->pitch_bytes, render_flags);
            return SR_RESULT_OK;
        }
        case SR_PPU_OUTPUT_AUTHENTIC:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u || request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindAuthenticSurface(
                       ppu, request->pixels, (size_t)request->pitch_bytes)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_OVERLAY:
            if (request->flags != 0u ||
                request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
                request->band != 0u || request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindOverlaySurfaceSized(
                       ppu, (PpuOverlaySource)request->source,
                       request->pixels, (size_t)request->pitch_bytes,
                       request->height_pixels)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_OVERLAY_PRIORITY:
            if (request->flags != 0u ||
                request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
                request->band == 0u ||
                request->band >= SR_PPU_SURFACE_BAND_COUNT ||
                request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            if (request->pixels != NULL &&
                request->pitch_bytes !=
                    ppu->overlayRenderPitch[request->source])
                return SR_RESULT_INVALID_ARGUMENT;
            return PpuBindOverlayPrioSurface(
                       ppu, (PpuOverlaySource)request->source,
                       (int)request->band, request->pixels)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_MODE7:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u ||
                (request->pixels == NULL ? request->scale != 0u
                                         : (request->scale < 1u ||
                                            request->scale > 4u)))
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, (uint64_t)SR_PPU_NATIVE_WIDTH * request->scale,
                (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * request->scale,
                (uint64_t)SR_PPU_SURFACE_MAX_HEIGHT * request->scale);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindMode7OverlaySurface(
                       ppu, request->pixels, (size_t)request->pitch_bytes,
                       (uint8_t)request->scale)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u || request->scale != 0u ||
                request->pixels != NULL || request->pixel_byte_size != 0u ||
                request->pitch_bytes != 0u || request->height_pixels != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            PpuClearOverlayBindings(ppu);
            return SR_RESULT_OK;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
}

static SrResult configure_ppu_horizontal_margin(
        SrRunnerHandle *runner,
        const SrPpuHorizontalMarginRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint8_t old_budget, old_left, old_right;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_HORIZONTAL_MARGIN_REQUEST_V2_SIZE ||
        request->flags != 0u || request->budget_pixels >
            SR_PPU_HORIZONTAL_MARGIN_MAX ||
        request->reserved[0] != 0u || request->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    old_budget = ppu->extraLeftRight;
    old_left = ppu->extraLeftCur;
    old_right = ppu->extraRightCur;
    switch (request->mode) {
        case SR_PPU_HORIZONTAL_MARGIN_AVAILABLE:
            PpuSetExtraSpace(ppu, (uint8_t)request->budget_pixels);
            break;
        case SR_PPU_HORIZONTAL_MARGIN_CENTERED:
            PpuSetExtraSpaceCentered(ppu, (uint8_t)request->budget_pixels);
            break;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
    if (ppu->extraLeftRight != old_budget ||
        ppu->extraLeftCur != old_left || ppu->extraRightCur != old_right)
        PpuNoteSurfaceViewChange(ppu);
    return SR_RESULT_OK;
}

static SrResult claim_ppu_overlay_capture(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    int64_t x1, y1;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OVERLAY_CAPTURE_REQUEST_V2_SIZE ||
        (request->flags & ~SR_PPU_OVERLAY_FLAGS_SUPPORTED) != 0u ||
        request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
        request->width <= 0 || request->height <= 0 ||
        request->reserved[0] != 0u || request->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    x1 = (int64_t)request->x + request->width;
    y1 = (int64_t)request->y + request->height;
    if (x1 < INT32_MIN || x1 > INT32_MAX ||
        y1 < INT32_MIN || y1 > INT32_MAX)
        return SR_RESULT_INVALID_ARGUMENT;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (ppu->overlayCaptures[request->source].x1 >
        ppu->overlayCaptures[request->source].x0)
        return SR_RESULT_BUSY;
    return PpuSetOverlayCapture(
               ppu, (PpuOverlaySource)request->source,
               request->x, request->y, request->width, request->height,
               (uint8_t)request->flags)
        ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
}

static SrResult claim_ppu_mode7_override(
        SrRunnerHandle *runner,
        const SrPpuMode7OverrideRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint64_t pixel_count;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_MODE7_OVERRIDE_REQUEST_V2_SIZE ||
        request->flags != 0u || request->pixels == NULL ||
        ((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u ||
        request->width_pixels == 0u || request->height_pixels == 0u ||
        request->width_pixels > INT32_MAX ||
        request->height_pixels > INT32_MAX ||
        request->canvas_x0 < 0 || request->canvas_y0 < 0 ||
        request->canvas_x1 <= request->canvas_x0 ||
        request->canvas_y1 <= request->canvas_y0 ||
        request->canvas_x1 > (int32_t)SR_PPU_MODE7_CANVAS_EXTENT ||
        request->canvas_y1 > (int32_t)SR_PPU_MODE7_CANVAS_EXTENT ||
        request->wrap > 1u || request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (request->width_pixels > UINT64_MAX / request->height_pixels)
        return SR_RESULT_INVALID_ARGUMENT;
    pixel_count = (uint64_t)request->width_pixels * request->height_pixels;
    if (pixel_count > UINT64_MAX / sizeof(uint32_t) ||
        request->pixel_byte_size < pixel_count * sizeof(uint32_t))
        return SR_RESULT_INVALID_ARGUMENT;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (ppu->m7Override.rgba != NULL) return SR_RESULT_BUSY;
    return PpuSetMode7Override(
               ppu, request->pixels, (int)request->width_pixels,
               (int)request->height_pixels, request->canvas_x0,
               request->canvas_y0, request->canvas_x1, request->canvas_y1,
               (uint8_t)request->wrap)
        ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult rasterize_ppu_obj_range(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_RASTER_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RASTER_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RASTER_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RASTER_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_raster_runner != snes ||
        s_ppu_obj_raster_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_raster_provider(snes, request, out_result);
}

static SrResult resolve_ppu_obj_range(
        SrRunnerHandle *runner, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_RESOLVE_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_resolve_runner != snes ||
        s_ppu_obj_resolve_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_resolve_provider(snes, request, out_result);
}

static SrResult rasterize_ppu_obj_parts(
        SrRunnerHandle *runner,
        const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_PARTS_RASTER_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RASTER_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RASTER_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RASTER_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_parts_raster_runner != snes ||
        s_ppu_obj_parts_raster_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_parts_raster_provider(snes, request, out_result);
}

static SrResult query_cpu_math_state(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state);
static SrResult restore_cpu_math_state(SrRunnerHandle *runner,
                                       const SrCpuMathState *state);

static const SnesRunnerApi k_runner_api = {
    SR_RUNNER_ABI_VERSION,
    (uint32_t)sizeof(SnesRunnerApi),
    SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
        SR_RUNNER_CAP_EXECUTION_STATE |
        SR_RUNNER_CAP_EVENT_OBSERVERS |
        SR_RUNNER_CAP_SAFE_POINT_MUTATIONS |
        SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
        SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
        SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
        SR_RUNNER_CAP_CPU_MATH_STATE |
        SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS |
        SR_RUNNER_CAP_SPC_CONTROL |
        SR_RUNNER_CAP_AUDIO_MIX_CONTROL,
    get_component,
    query_generations,
    borrow_memory,
    borrow_is_valid,
    query_cpu_state,
    query_ppu_state,
    borrow_u16_memory,
    borrow_u16_is_valid,
    query_ppu_frame_state,
    rasterize_ppu_obj_range,
    resolve_ppu_obj_range,
    rasterize_ppu_obj_parts,
    query_ppu_surfaces,
    ppu_surface_snapshot_is_valid,
    query_execution_state,
    subscribe_events,
    unsubscribe_events,
    queue_mutation,
    query_mutation,
    resolve_ppu_background_coordinate,
    bind_ppu_output_surface,
    configure_ppu_horizontal_margin,
    claim_ppu_overlay_capture,
    claim_ppu_mode7_override,
    query_cpu_math_state,
    restore_cpu_math_state,
    sr_runner_subscribe_audio_trace,
    sr_runner_unsubscribe_audio_trace,
    sr_runner_compare_exchange_spc_pc,
    sr_runner_configure_audio_mix,
};

/* Keep synchronized with the source boundary in runner.cmake. */
static const SrRunnerDescriptor k_runner = {
    SR_RUNNER_ABI_VERSION,
    "next",
    0u,
    (uint32_t)sizeof(SrRunnerDescriptor),
    SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
        SR_RUNNER_CAP_EXECUTION_STATE |
        SR_RUNNER_CAP_EVENT_OBSERVERS |
        SR_RUNNER_CAP_SAFE_POINT_MUTATIONS |
        SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
        SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
        SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
        SR_RUNNER_CAP_CPU_MATH_STATE |
        SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS |
        SR_RUNNER_CAP_SPC_CONTROL |
        SR_RUNNER_CAP_AUDIO_MIX_CONTROL,
};

const SrRunnerDescriptor *sr_runner_descriptor(void) {
    return &k_runner;
}

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
    if (requested_abi_version != SR_RUNNER_ABI_VERSION) return NULL;
    return &k_runner_api;
}

SrRunnerHandle *sr_runner_handle(Snes *snes) {
    return (SrRunnerHandle *)(void *)snes;
}

void sr_runner_set_cpu_state_provider(
        Snes *snes, SrRunnerCpuStateProvider *provider,
        const void *component_handle) {
    if (provider == NULL && s_cpu_state_runner != snes) return;
    s_cpu_state_runner = provider != NULL ? snes : NULL;
    s_cpu_state_provider = provider;
    s_cpu_component = provider != NULL ? component_handle : NULL;
}

void sr_runner_set_execution_state_provider(
        Snes *snes, SrRunnerExecutionStateProvider *provider) {
    if (provider == NULL && s_execution_state_runner != snes) return;
    s_execution_state_runner = provider != NULL ? snes : NULL;
    s_execution_state_provider = provider;
}

void sr_runner_set_ppu_obj_raster_provider(
        Snes *snes, SrRunnerPpuObjRasterProvider *provider) {
    if (provider == NULL && s_ppu_obj_raster_runner != snes) return;
    s_ppu_obj_raster_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_raster_provider = provider;
}

void sr_runner_set_ppu_obj_resolve_provider(
        Snes *snes, SrRunnerPpuObjResolveProvider *provider) {
    if (provider == NULL && s_ppu_obj_resolve_runner != snes) return;
    s_ppu_obj_resolve_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_resolve_provider = provider;
}

void sr_runner_set_ppu_obj_parts_raster_provider(
        Snes *snes, SrRunnerPpuObjPartsRasterProvider *provider) {
    if (provider == NULL && s_ppu_obj_parts_raster_runner != snes) return;
    s_ppu_obj_parts_raster_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_parts_raster_provider = provider;
}

static void invalidate_lifetime(Snes *snes) {
    if (snes != NULL) ++snes->abiLifetimeGeneration;
}

void sr_runner_note_tick(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiTickGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_reset(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiResetGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_load(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiLoadGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_mutation(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiMutationGeneration;
    invalidate_lifetime(snes);
}

static SrResult query_cpu_math_state(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_CPU_MATH_STATE_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_CPU_MATH_STATE_V2_SIZE);
    out_state->struct_size = SR_CPU_MATH_STATE_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    out_state->multiply_operand = snes->multiplyA;
    out_state->multiply_or_remainder_result = snes->multiplyResult;
    out_state->divide_dividend = snes->divideA;
    out_state->divide_quotient = snes->divideResult;
    return SR_RESULT_OK;
}

static SrResult restore_cpu_math_state(SrRunnerHandle *runner,
                                       const SrCpuMathState *state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || state == NULL ||
        state->struct_size < SR_CPU_MATH_STATE_V2_SIZE ||
        state->flags != 0u || state->reserved8 != 0u ||
        state->reserved32 != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (state->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    sr_runner_note_mutation(snes);
    snes->multiplyA = state->multiply_operand;
    snes->multiplyResult = state->multiply_or_remainder_result;
    snes->divideA = state->divide_dividend;
    snes->divideResult = state->divide_quotient;
    return SR_RESULT_OK;
}
