/* Unity-build domain fragment. Included only by runner.c. */
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

