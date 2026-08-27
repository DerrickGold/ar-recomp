/* Unity-build domain fragment. Included only by runner.c. */
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

