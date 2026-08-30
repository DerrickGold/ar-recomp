#include "runner_internal.h"

#include "snes/apu.h"
#include "snes/snes.h"

#include <string.h>

enum { kAudioTraceObserverCapacity = 4 };

_Static_assert(sizeof(((Apu *)0)->ram) == SR_APU_RAM_BYTE_COUNT,
               "public ABI ARAM extent must match the APU");
_Static_assert(sizeof(((Apu *)0)->inPorts) == SR_APU_INPUT_PORT_COUNT &&
                   sizeof(((Apu *)0)->outPorts) == SR_APU_OUTPUT_PORT_COUNT,
               "public ABI port extents must match the APU");

typedef struct AudioTraceObserverSlot {
    Snes *runner;
    uint64_t id;
    SrAudioTraceSubscription subscription;
} AudioTraceObserverSlot;

static AudioTraceObserverSlot
    s_audio_trace_observers[kAudioTraceObserverCapacity];
static uint64_t s_next_audio_trace_observer_id = 1u;
_Atomic(SrAudioTraceMask) g_sr_runner_audio_trace_mask;
#if defined(_MSC_VER)
#define SR_THREAD_LOCAL __declspec(thread)
#else
#define SR_THREAD_LOCAL _Thread_local
#endif
static SR_THREAD_LOCAL unsigned s_audio_production_depth;
static SR_THREAD_LOCAL unsigned s_audio_trace_callback_depth;

static SrAudioTraceMask audio_trace_mask_for_type(
        SrAudioTraceEventType type) {
    switch (type) {
        case SR_AUDIO_TRACE_APU_PORT_APPLY:
            return SR_AUDIO_TRACE_MASK_APU_PORT_APPLY;
        case SR_AUDIO_TRACE_SPC_PORT_READ:
            return SR_AUDIO_TRACE_MASK_SPC_PORT_READ;
        case SR_AUDIO_TRACE_SPC_OPCODE:
            return SR_AUDIO_TRACE_MASK_SPC_OPCODE;
        case SR_AUDIO_TRACE_DSP_WRITE:
            return SR_AUDIO_TRACE_MASK_DSP_WRITE;
        case SR_AUDIO_TRACE_CPU_PORT_WRITE:
            return SR_AUDIO_TRACE_MASK_CPU_PORT_WRITE;
        case SR_AUDIO_TRACE_SPC_UPLOAD:
            return SR_AUDIO_TRACE_MASK_SPC_UPLOAD;
        default:
            return 0u;
    }
}

void sr_runner_audio_production_begin(void) {
    ++s_audio_production_depth;
}

void sr_runner_audio_production_end(void) {
    if (s_audio_production_depth != 0u) --s_audio_production_depth;
}

bool sr_runner_audio_query_forbidden(void) {
    return s_audio_production_depth != 0u ||
           s_audio_trace_callback_depth != 0u;
}

static Snes *runner_from_handle(SrRunnerHandle *runner) {
    return (Snes *)(void *)runner;
}

static void populate_audio_trace_event(Apu *apu,
                                       SrAudioTraceEventType type,
                                       uint64_t cycle_count,
                                       SrAudioTraceEvent *event) {
    memset(event, 0, sizeof(*event));
    event->struct_size = SR_AUDIO_TRACE_EVENT_V3_SIZE;
    event->type = type;
    event->cycle_count = cycle_count;
    if (apu == NULL) return;
    event->apu_ram = apu->ram;
    event->apu_ram_byte_size = SR_APU_RAM_BYTE_COUNT;
    memcpy(event->apu_input_ports, apu->inPorts,
           sizeof(event->apu_input_ports));
    memcpy(event->apu_output_ports, apu->outPorts,
           sizeof(event->apu_output_ports));
    if (apu->spc != NULL) {
        event->spc_pc = apu->spc->pc;
        event->spc_instruction_pc = apu->spc->instructionPc;
        event->spc_a = apu->spc->a;
        event->spc_x = apu->spc->x;
        event->spc_y = apu->spc->y;
        event->spc_sp = apu->spc->sp;
        event->spc_instruction_cycle =
            apu->spc->cyclesUsed > apu->cpuCyclesLeft
                ? (uint8_t)(apu->spc->cyclesUsed - apu->cpuCyclesLeft)
                : 0u;
    }
    event->dsp_slot = apu->dspSlot;
}

void sr_runner_emit_audio_trace(Apu *apu, SrAudioTraceEventType type,
                                uint16_t opcode_pc, uint8_t port,
                                uint8_t dsp_address, uint8_t value,
                                uint64_t cycle_count,
                                uint32_t source_address,
                                uint32_t frame_counter,
                                const char *function_name) {
    SrAudioTraceEvent event;
    const SrAudioTraceMask event_mask = audio_trace_mask_for_type(type);
    unsigned index;
    if (event_mask == 0u || !sr_runner_audio_trace_enabled(event_mask))
        return;
    populate_audio_trace_event(apu, type, cycle_count, &event);
    event.port = port;
    event.dsp_address = dsp_address;
    event.value = value;
    event.source_address = source_address;
    event.frame_counter = frame_counter;
    event.function_name = function_name;
    for (index = 0u; index < kAudioTraceObserverCapacity; ++index) {
        AudioTraceObserverSlot *slot = &s_audio_trace_observers[index];
        Snes *snes = slot->runner;
        if (slot->id == 0u || snes == NULL || snes->apu != apu ||
            (slot->subscription.event_mask & event_mask) == 0u)
            continue;
        if (type == SR_AUDIO_TRACE_SPC_OPCODE) {
            event.spc_pc = opcode_pc;
            event.spc_instruction_pc = opcode_pc;
            event.spc_instruction_cycle = 0u;
        }
        ++s_audio_trace_callback_depth;
        slot->subscription.callback(
            slot->subscription.user_data,
            (SrRunnerHandle *)(void *)snes, &event);
        --s_audio_trace_callback_depth;
    }
}

static void recompute_audio_trace_mask(void) {
    unsigned index;
    SrAudioTraceMask mask = 0u;
    for (index = 0u; index < kAudioTraceObserverCapacity; ++index) {
        if (s_audio_trace_observers[index].id != 0u)
            mask |= s_audio_trace_observers[index].subscription.event_mask;
    }
    atomic_store_explicit(&g_sr_runner_audio_trace_mask, mask,
                          memory_order_relaxed);
}

SrResult sr_runner_subscribe_audio_trace(
        SrRunnerHandle *runner,
        const SrAudioTraceSubscription *subscription,
        uint64_t *out_subscription_id) {
    Snes *snes = runner_from_handle(runner);
    SrAudioTraceMask event_mask;
    unsigned index;
    if (out_subscription_id != NULL) *out_subscription_id = 0u;
    if (snes == NULL || subscription == NULL || out_subscription_id == NULL ||
        subscription->struct_size < SR_AUDIO_TRACE_SUBSCRIPTION_V2_SIZE ||
        subscription->callback == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (subscription->flags != 0u) return SR_RESULT_UNSUPPORTED;
    if (subscription->struct_size >= SR_AUDIO_TRACE_SUBSCRIPTION_V3_SIZE) {
        event_mask = subscription->event_mask;
        if (subscription->reserved != 0u || event_mask == 0u)
            return SR_RESULT_INVALID_ARGUMENT;
        if ((event_mask & ~SR_AUDIO_TRACE_MASK_ALL) != 0u)
            return SR_RESULT_UNSUPPORTED;
    } else {
        event_mask = SR_AUDIO_TRACE_MASK_ALL;
    }
    for (index = 0u; index < kAudioTraceObserverCapacity; ++index) {
        AudioTraceObserverSlot *slot = &s_audio_trace_observers[index];
        if (slot->id != 0u) continue;
        slot->runner = snes;
        memset(&slot->subscription, 0, sizeof(slot->subscription));
        slot->subscription.struct_size = SR_AUDIO_TRACE_SUBSCRIPTION_V3_SIZE;
        slot->subscription.flags = subscription->flags;
        slot->subscription.callback = subscription->callback;
        slot->subscription.user_data = subscription->user_data;
        slot->subscription.event_mask = event_mask;
        slot->id = s_next_audio_trace_observer_id++;
        if (slot->id == 0u) slot->id = s_next_audio_trace_observer_id++;
        *out_subscription_id = slot->id;
        recompute_audio_trace_mask();
        return SR_RESULT_OK;
    }
    return SR_RESULT_UNAVAILABLE;
}

SrResult sr_runner_unsubscribe_audio_trace(SrRunnerHandle *runner,
                                           uint64_t subscription_id) {
    Snes *snes = runner_from_handle(runner);
    unsigned index;
    if (snes == NULL || subscription_id == 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    for (index = 0u; index < kAudioTraceObserverCapacity; ++index) {
        AudioTraceObserverSlot *slot = &s_audio_trace_observers[index];
        if (slot->runner == snes && slot->id == subscription_id) {
            memset(slot, 0, sizeof(*slot));
            recompute_audio_trace_mask();
            return SR_RESULT_OK;
        }
    }
    return SR_RESULT_UNAVAILABLE;
}

void sr_runner_clear_audio_trace_subscriptions(Snes *snes) {
    unsigned index;
    if (snes == NULL) return;
    for (index = 0u; index < kAudioTraceObserverCapacity; ++index) {
        AudioTraceObserverSlot *slot = &s_audio_trace_observers[index];
        if (slot->runner == snes) memset(slot, 0, sizeof(*slot));
    }
    recompute_audio_trace_mask();
}
