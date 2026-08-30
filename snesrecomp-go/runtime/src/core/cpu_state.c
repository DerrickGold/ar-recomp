#include "snesrecomp/game/cpu.h"

#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "runner_internal.h"
#include "runner_game_module_internal.h"
#include "runner_state_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CpuState g_cpu;
void (*g_cpu_brk_hook)(CpuState *cpu);
void (*g_cpu_cop_hook)(CpuState *cpu);

uint16 sr_cpu_stack_pointer(void) { return g_cpu.S; }
uint8 sr_cpu_program_bank(void) { return g_cpu.PB; }

enum {
    kDispatchDepthCapacity = 256,
    kMissingDispatchCapacity = 256,
};
typedef struct MissingDispatchWarning {
    uint32 site_pc24;
    uint32 target_pc24;
    uint32 hits;
} MissingDispatchWarning;
static uint32 g_dispatch_depth[kDispatchDepthCapacity];
static unsigned g_dispatch_depth_count;
static MissingDispatchWarning
    g_missing_dispatch_warning[kMissingDispatchCapacity];
static unsigned g_missing_dispatch_warning_count;

static int ram_offset(uint8 bank, uint16 address) {
    if (bank == 0x7eu) return address;
    if (bank == 0x7fu) return 0x10000 + address;
    if (address < 0x2000u && (bank <= 0x3fu ||
                              (bank >= 0x80u && bank <= 0xbfu))) {
        return address;
    }
    return -1;
}

static int hardware_register(uint8 bank, uint16 address) {
    return address >= 0x2000u && address < 0x6000u &&
           (bank <= 0x3fu || (bank >= 0x80u && bank <= 0xbfu));
}

static int sram_offset(uint8 bank, uint16 address) {
    uint32 offset;
    if (g_sram == NULL || g_sram_size <= 0) return -1;
    if (((bank >= 0x70u && bank <= 0x7du) ||
         (bank >= 0xf0u && bank <= 0xfdu)) && address < 0x8000u) {
        offset = ((uint32)(bank & 0x0fu) << 15) | address;
    } else if ((bank <= 0x3fu || (bank >= 0x80u && bank <= 0xbfu)) &&
               address >= 0x6000u && address < 0x8000u) {
        offset = ((uint32)(bank & 0x1fu) << 13) | (address - 0x6000u);
    } else {
        return -1;
    }
    return (int)(offset % (uint32)g_sram_size);
}

static void pace_hardware(uint16 address) {
    g_main_cpu_cycles_estimate += 256u;
    if (address >= 0x2140u && address <= 0x217fu) {
        g_apu_pace_cycles_estimate += 256u;
    }
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
    int offset = ram_offset(bank, address);
    int save_offset;
    if (offset >= 0) return cpu->ram[offset];
    if (hardware_register(bank, address)) {
        pace_hardware(address);
        return ReadReg(address);
    }
    save_offset = sram_offset(bank, address);
    if (save_offset >= 0) return g_sram[save_offset];
    return *RomPtr(((uint32)bank << 16) | address);
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
    int offset = ram_offset(bank, address);
    if (offset >= 0 && offset + 1 < kSnesWramSize) {
        return (uint16)cpu->ram[offset] |
               ((uint16)cpu->ram[offset + 1] << 8);
    }
    if (hardware_register(bank, address) && address != 0xffffu) {
        pace_hardware(address);
        return ReadRegWord(address);
    }
    return (uint16)cpu_read8(cpu, bank, address) |
           ((uint16)cpu_read8(cpu, (uint8)(bank + (address == 0xffffu)),
                              (uint16)(address + 1u)) << 8);
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
    int offset = ram_offset(bank, address);
    int save_offset;
    if (offset >= 0) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            uint8 old_value = cpu->ram[offset];
            cpu->ram[offset] = value;
            sr_runner_emit_memory_write(
                g_snes, SR_MEMORY_WRAM, (uint32)offset,
                old_value, value, 1u);
        } else {
            cpu->ram[offset] = value;
        }
        return;
    }
    if (hardware_register(bank, address)) {
        pace_hardware(address);
        WriteReg(address, value);
        return;
    }
    save_offset = sram_offset(bank, address);
    if (save_offset >= 0) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            uint8 old_value = g_sram[save_offset];
            g_sram[save_offset] = value;
            sr_runner_emit_memory_write(
                g_snes, SR_MEMORY_SRAM, (uint32)save_offset,
                old_value, value, 1u);
        } else {
            g_sram[save_offset] = value;
        }
    }
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
    int offset = ram_offset(bank, address);
    if (offset >= 0 && offset + 1 < kSnesWramSize) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            uint16 old_value = (uint16)cpu->ram[offset] |
                               ((uint16)cpu->ram[offset + 1] << 8);
            cpu->ram[offset] = (uint8)value;
            cpu->ram[offset + 1] = (uint8)(value >> 8);
            sr_runner_emit_memory_write(
                g_snes, SR_MEMORY_WRAM, (uint32)offset,
                old_value, value, 2u);
        } else {
            cpu->ram[offset] = (uint8)value;
            cpu->ram[offset + 1] = (uint8)(value >> 8);
        }
        return;
    }
    if (hardware_register(bank, address) && address != 0xffffu) {
        pace_hardware(address);
        WriteRegWord(address, value);
        return;
    }
    cpu_write8(cpu, bank, address, (uint8)value);
    cpu_write8(cpu, (uint8)(bank + (address == 0xffffu)),
               (uint16)(address + 1u), (uint8)(value >> 8));
}

void cpu_state_init(CpuState *cpu, uint8 *ram) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->S = 0x01ffu;
    cpu->P = CPU_P_M | CPU_P_X | CPU_P_I;
    cpu->m_flag = 1u;
    cpu->x_flag = 1u;
    cpu->emulation = 1u;
    cpu->_flag_I = 1u;
    cpu->ram = ram;
}

void cpu_dbg_funcname(const char *name) { (void)name; }

void sr_indirect_suppressed_log(CpuState *cpu, uint32 site_pc24,
                                uint8 bank, uint16 table_base,
                                uint16 x_register) {
    uint16 address;
    uint16 target;
    if (getenv("SNESRECOMP_INDIRECT_LOG") == NULL) return;
    address = (uint16)(table_base + x_register);
    target = cpu_read16(cpu, bank, address);
    fprintf(stderr,
            "[indirlog] site=$%06X table=$%02X:%04X X=$%04X target=$%04X m=%u x=%u\n",
            site_pc24 & 0xffffffu, bank, table_base, x_register, target,
            cpu->m_flag & 1u, cpu->x_flag & 1u);
}

static RecompReturn (*dispatch_lookup(CpuState *cpu, uint32 pc24))(CpuState *) {
    unsigned low = 0u;
    unsigned high = g_dispatch_table_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2u;
        if (g_dispatch_table[middle].pc24 < pc24) low = middle + 1u;
        else high = middle;
    }
    if (low < g_dispatch_table_count && g_dispatch_table[low].pc24 == pc24) {
        unsigned mx = ((cpu->m_flag & 1u) << 1) | (cpu->x_flag & 1u);
        return g_dispatch_table[low].variant[mx];
    }
    return NULL;
}

static RecompReturn (*dispatch_lookup_mirrored(CpuState *cpu, uint32 pc24,
                                               int *mirrored))(CpuState *) {
    RecompReturn (*function)(CpuState *) = dispatch_lookup(cpu, pc24);
    uint8 bank = (uint8)(pc24 >> 16);
    *mirrored = 0;
    if (function == NULL && (bank < 0x40u ||
                             (bank >= 0x80u && bank < 0xc0u))) {
        function = dispatch_lookup(cpu, pc24 ^ 0x800000u);
        *mirrored = function != NULL;
    }
    return function;
}

static int dispatch_source_is_continuation(CpuState *cpu,
                                           uint32 source_pc24) {
    uint8 source_opcode;
    if ((source_pc24 & 0x00ffffffu) == 0x00ffffffu) return 0;
    source_opcode = cpu_read8(cpu, (uint8)(source_pc24 >> 16),
                              (uint16)source_pc24);
    return source_opcode == 0x60u || source_opcode == 0x6bu;
}

void cpu_dispatch_diagnostic_reset(void) {
    memset(g_missing_dispatch_warning, 0, sizeof(g_missing_dispatch_warning));
    g_missing_dispatch_warning_count = 0u;
}

unsigned cpu_dispatch_missing_warning_count(void) {
    return g_missing_dispatch_warning_count;
}

uint32 cpu_dispatch_missing_warning_hits(uint32 site_pc24,
                                         uint32 target_pc24) {
    unsigned index;
    site_pc24 &= 0xffffffu;
    target_pc24 &= 0xffffffu;
    for (index = 0u; index < g_missing_dispatch_warning_count; ++index) {
        const MissingDispatchWarning *entry =
            &g_missing_dispatch_warning[index];
        if (entry->site_pc24 == site_pc24 &&
            entry->target_pc24 == target_pc24) return entry->hits;
    }
    return 0u;
}

static int missing_dispatch_milestone(uint32 hits) {
    if (hits == 0u) return 0;
    while (hits > 1u && hits % 16u == 0u) hits /= 16u;
    return hits == 1u;
}

static void dispatch_missing_body_warn(CpuState *cpu, uint32 site_pc24,
                                       uint32 target_pc24) {
    MissingDispatchWarning *entry = NULL;
    unsigned index;
    site_pc24 &= 0xffffffu;
    target_pc24 &= 0xffffffu;
    if (getenv("SNESRECOMP_NO_DISPATCH_MISSING_WARNING") != NULL) return;
    for (index = 0u; index < g_missing_dispatch_warning_count; ++index) {
        MissingDispatchWarning *candidate =
            &g_missing_dispatch_warning[index];
        if (candidate->site_pc24 == site_pc24 &&
            candidate->target_pc24 == target_pc24) {
            entry = candidate;
            break;
        }
    }
    if (entry == NULL) {
        if (g_missing_dispatch_warning_count >= kMissingDispatchCapacity)
            return;
        entry = &g_missing_dispatch_warning[g_missing_dispatch_warning_count++];
        entry->site_pc24 = site_pc24;
        entry->target_pc24 = target_pc24;
    }
    if (entry->hits != UINT32_MAX) ++entry->hits;
    if (!missing_dispatch_milestone(entry->hits)) return;
    fprintf(stderr,
            "[dispatch-missing] %s site=$%06X target=$%06X M%uX%u "
            "S=$%04X frame=%d has no generated body (hits=%u).\n"
            "[dispatch-missing] A repeated hit usually means a data-driven "
            "script handler is being skipped; run dispatch-census and "
            "regenerate after adding or learning the target.\n",
            g_last_recomp_func != NULL ? g_last_recomp_func : "?",
            site_pc24, target_pc24, cpu->m_flag & 1u, cpu->x_flag & 1u,
            cpu->S, snes_frame_counter, entry->hits);
}

static void record_dispatch(uint32 pc24, uint32 source_pc24, CpuState *cpu,
                            int found, int mirrored, int trapped,
                            const char *label) {
    if (sr_runner_event_enabled(SR_EVENT_MASK_DYNAMIC_DISPATCH)) {
        SrRunnerEvent runner_event = {0};
        int continuation = dispatch_source_is_continuation(cpu, source_pc24);
        runner_event.type = SR_EVENT_DYNAMIC_DISPATCH;
        runner_event.frame_counter = snes_frame_counter >= 0
            ? (uint64)snes_frame_counter : 0u;
        runner_event.flags =
            (found ? SR_EVENT_DISPATCH_FOUND : 0u) |
            (mirrored ? SR_EVENT_DISPATCH_MIRRORED : 0u) |
            (continuation ? SR_EVENT_DISPATCH_CONTINUATION : 0u) |
            (trapped ? SR_EVENT_DISPATCH_TRAPPED : 0u);
        runner_event.cpu_flags =
            (cpu->m_flag ? SR_CPU_STATE_M_FLAG : 0u) |
            (cpu->x_flag ? SR_CPU_STATE_X_FLAG : 0u) |
            (cpu->emulation ? SR_CPU_STATE_EMULATION : 0u) |
            (cpu->host_return_valid ? SR_CPU_STATE_HOST_RETURN_VALID : 0u);
        runner_event.pc24 = pc24 & 0x00ffffffu;
        runner_event.source_pc24 = source_pc24 & 0x00ffffffu;
        runner_event.register_x = cpu->X;
        runner_event.stack_pointer = cpu->S;
        runner_event.label = label;
        sr_runner_emit_event(g_snes, SR_EVENT_MASK_DYNAMIC_DISPATCH,
                             &runner_event);
    }
}

void cpu_trace_resolved_dispatch(CpuState *cpu, uint32 pc24,
                                 uint32 source_pc24) {
    record_dispatch(pc24 & 0xffffffu, source_pc24 & 0xffffffu, cpu, 1, 0,
                    0, NULL);
}

void cpu_trace_trapped_dispatch(CpuState *cpu, uint32 pc24,
                                uint32 source_pc24) {
    RecompReturn (*function)(CpuState *);
    int mirrored;
    if (!sr_runner_event_enabled(SR_EVENT_MASK_DYNAMIC_DISPATCH)) return;
    pc24 &= 0xffffffu;
    function = dispatch_lookup_mirrored(cpu, pc24, &mirrored);
    record_dispatch(pc24, source_pc24 & 0xffffffu, cpu,
                    function != NULL, mirrored, 1, NULL);
}

static RecompReturn dispatch_once(CpuState *cpu, uint32 pc24,
                                  uint16 miss_restore_stack,
                                  uint32 source_pc24) {
    RecompReturn (*function)(CpuState *);
    unsigned index;
    unsigned live = 0u;
    int mirrored;
    pc24 &= 0xffffffu;
    source_pc24 &= 0xffffffu;
    function = dispatch_lookup_mirrored(cpu, pc24, &mirrored);
#if !SNESRECOMP_SEMANTIC_DISPATCH_TRACE
    record_dispatch(pc24, source_pc24, cpu, function != NULL, mirrored,
                    0, g_last_recomp_func);
#endif
    if (function == NULL &&
        !dispatch_source_is_continuation(cpu, source_pc24)) {
        uint8 opcode = cpu_read8(cpu, (uint8)(pc24 >> 16), (uint16)pc24);
        if (opcode != 0x60u && opcode != 0x6bu) {
            dispatch_missing_body_warn(cpu, source_pc24, pc24);
        }
    }

    if (function == NULL && g_rtl_game_execution != NULL &&
        g_rtl_game_execution->recover_dispatch_miss != NULL &&
        g_rtl_game_execution->recover_dispatch_miss(source_pc24, pc24)) {
        uint8 bank = (uint8)(pc24 >> 16);
        uint16 address = (uint16)pc24;
        uint8 opcode = cpu_read8(cpu, bank, address);
        uint32 followed = 0xffffffffu;
        if (opcode == 0x80u) {
            int8 delta = (int8)cpu_read8(cpu, bank, (uint16)(address + 1u));
            followed = ((uint32)bank << 16) |
                       (uint16)(address + 2u + delta);
        } else if (opcode == 0x82u) {
            int16 delta = (int16)cpu_read16(cpu, bank, (uint16)(address + 1u));
            followed = ((uint32)bank << 16) |
                       (uint16)(address + 3u + delta);
        }
        if (followed != 0xffffffffu) {
            function = dispatch_lookup_mirrored(cpu, followed, &mirrored);
            if (function != NULL) pc24 = followed;
        }
        if (function == NULL) {
            /* The game-approved target is a data-driven handler that has no
             * emitted body. Model its return instead of treating the miss as
             * a return from the surrounding host function: the next word on
             * the 65816 stack is the handler's RTS continuation, and that
             * continuation may contain width-restoring REP/SEP instructions. */
            uint16 return_address =
                (uint16)cpu_read8(cpu, 0u, (uint16)(cpu->S + 1u));
            return_address |=
                (uint16)cpu_read8(cpu, 0u, (uint16)(cpu->S + 2u)) << 8;
            cpu->S = (uint16)(cpu->S + 2u);
            pc24 = ((uint32)bank << 16) |
                   (uint16)(return_address + 1u);
            function = dispatch_lookup_mirrored(cpu, pc24, &mirrored);
        }
    }

    for (index = 0u; function == NULL && index < 8u; ++index) {
        uint8 bank = (uint8)(pc24 >> 16);
        uint8 opcode = cpu_read8(cpu, bank, (uint16)pc24);
        uint16 return_address;
        if (opcode != 0x60u && opcode != 0x6bu) break;
        return_address = (uint16)cpu_read8(cpu, 0u, (uint16)(cpu->S + 1u));
        return_address |= (uint16)cpu_read8(cpu, 0u,
                                            (uint16)(cpu->S + 2u)) << 8;
        cpu->S = (uint16)(cpu->S + 2u);
        if (opcode == 0x6bu) {
            bank = cpu_read8(cpu, 0u, (uint16)(cpu->S + 1u));
            ++cpu->S;
        }
        pc24 = ((uint32)bank << 16) | (uint16)(return_address + 1u);
        function = dispatch_lookup_mirrored(cpu, pc24, &mirrored);
    }

    if (function == NULL) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_ERROR)) {
            sr_runner_emit_error(
                g_snes, SR_RUNNER_ERROR_DISPATCH_MISS,
                SR_EVENT_ERROR_RECOVERABLE, pc24, source_pc24,
                "dispatch-miss");
        }
        cpu->S = miss_restore_stack;
        return RECOMP_RETURN_NORMAL;
    }
    for (index = 0u; index < g_dispatch_depth_count; ++index) {
        if (g_dispatch_depth[index] == pc24) ++live;
    }
    if (live >= 24u) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_ERROR)) {
            sr_runner_emit_error(
                g_snes, SR_RUNNER_ERROR_DISPATCH_RECURSION_LIMIT,
                SR_EVENT_ERROR_RECOVERABLE, pc24, source_pc24,
                "dispatch-recursion-limit");
        }
        cpu->S = miss_restore_stack;
        return RECOMP_RETURN_NORMAL;
    }
    cpu->host_return_valid = 0u;
    if (g_dispatch_depth_count < kDispatchDepthCapacity) {
        g_dispatch_depth[g_dispatch_depth_count++] = pc24;
        {
            RecompReturn result = function(cpu);
            --g_dispatch_depth_count;
            return result;
        }
    }
    return function(cpu);
}

RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 miss_restore_stack,
                                  uint32 source_pc24) {
    for (;;) {
        RecompReturn result = dispatch_once(cpu, pc24, miss_restore_stack,
                                            source_pc24);
        if (result != RECOMP_RETURN_TAILCALL) return result;
        pc24 = g_tailcall_pc24;
        miss_restore_stack = g_tailcall_miss_s;
        source_pc24 = g_tailcall_src24;
    }
}

RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                             uint16 miss_restore_stack) {
    return cpu_dispatch_pc_from(cpu, pc24, miss_restore_stack, 0xffffffu);
}

int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    int mirrored;
    return dispatch_lookup_mirrored(cpu, pc24 & 0xffffffu, &mirrored) != NULL;
}
