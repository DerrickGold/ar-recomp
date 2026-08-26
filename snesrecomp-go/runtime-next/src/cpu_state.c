#include "cpu_state.h"

#include "ar_trace.h"
#include "common_cpu_infra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8 g_ram[kSnesWramSize];
extern uint8 *g_sram;
extern int g_sram_size;
extern uint64_t g_main_cpu_cycles_estimate;
extern uint64_t g_apu_pace_cycles_estimate;
uint8 ReadReg(uint16 reg);
uint16 ReadRegWord(uint16 reg);
void WriteReg(uint16 reg, uint8 value);
void WriteRegWord(uint16 reg, uint16 value);
uint8 *RomPtr(uint32 address);

CpuState g_cpu;
void (*g_cpu_brk_hook)(CpuState *cpu);
void (*g_cpu_cop_hook)(CpuState *cpu);

uint16 ar_cpu_S(void) { return g_cpu.S; }
uint8 ar_cpu_PB(void) { return g_cpu.PB; }

typedef struct DispatchLogEntry {
    uint32 pc24;
    uint32 source_pc24;
    const char *function_name;
    uint8 mx;
    uint8 found;
    uint8 mirrored;
    uint32 frame;
} DispatchLogEntry;

enum { kDispatchLogCapacity = 1024, kDispatchDepthCapacity = 256 };
static DispatchLogEntry g_dispatch_log[kDispatchLogCapacity];
static unsigned g_dispatch_count;
static uint32 g_dispatch_depth[kDispatchDepthCapacity];
static unsigned g_dispatch_depth_count;
extern int snes_frame_counter;

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
        uint8 old_value = cpu->ram[offset];
        cpu->ram[offset] = value;
        if (ar_trace_active()) {
            ar_trace_wram((uint32)offset, old_value, value, 1);
        }
        return;
    }
    if (hardware_register(bank, address)) {
        pace_hardware(address);
        WriteReg(address, value);
        return;
    }
    save_offset = sram_offset(bank, address);
    if (save_offset >= 0) g_sram[save_offset] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
    int offset = ram_offset(bank, address);
    if (offset >= 0 && offset + 1 < kSnesWramSize) {
        uint16 old_value = (uint16)cpu->ram[offset] |
                           ((uint16)cpu->ram[offset + 1] << 8);
        cpu->ram[offset] = (uint8)value;
        cpu->ram[offset + 1] = (uint8)(value >> 8);
        if (ar_trace_active()) {
            ar_trace_wram((uint32)offset, old_value, value, 2);
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

void ar_indirect_suppressed_log(CpuState *cpu, uint32 site_pc24,
                                uint8 bank, uint16 table_base,
                                uint16 x_register) {
    uint16 address;
    uint16 target;
    if (getenv("AR_INDIRLOG") == NULL) return;
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

static void record_dispatch(uint32 pc24, uint32 source_pc24, CpuState *cpu,
                            int found, int mirrored) {
    DispatchLogEntry *event =
        &g_dispatch_log[g_dispatch_count % kDispatchLogCapacity];
    event->pc24 = pc24;
    event->source_pc24 = source_pc24;
    event->function_name = g_last_recomp_func;
    event->mx = (uint8)(((cpu->m_flag & 1u) << 1) | (cpu->x_flag & 1u));
    event->found = found != 0;
    event->mirrored = mirrored != 0;
    event->frame = (uint32)snes_frame_counter;
    ++g_dispatch_count;
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
    record_dispatch(pc24, source_pc24, cpu, function != NULL, mirrored);

    if (function == NULL && g_rtl_game_info != NULL &&
        g_rtl_game_info->recover_dispatch_miss != NULL &&
        g_rtl_game_info->recover_dispatch_miss(source_pc24, pc24)) {
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
        if (ar_trace_active()) ar_trace_dispmiss(source_pc24, pc24);
        cpu->S = miss_restore_stack;
        return RECOMP_RETURN_NORMAL;
    }
    for (index = 0u; index < g_dispatch_depth_count; ++index) {
        if (g_dispatch_depth[index] == pc24) ++live;
    }
    if (live >= 24u) {
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

static void json_string(FILE *file, const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");
    fputc('"', file);
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\\') fputc('\\', file);
        if (*cursor >= 0x20u) fputc(*cursor, file);
        ++cursor;
    }
    fputc('"', file);
}

void CpuDispatchLogWriteFile(const char *path) {
    FILE *file;
    unsigned shown;
    unsigned start;
    unsigned index;
    if (path == NULL || (file = fopen(path, "wb")) == NULL) return;
    shown = g_dispatch_count < kDispatchLogCapacity
                ? g_dispatch_count
                : kDispatchLogCapacity;
    start = g_dispatch_count - shown;
    fprintf(file, "{\n  \"dispatch_log\": {\"total\": %u, \"shown\": %u, \"events\": [",
            g_dispatch_count, shown);
    for (index = 0u; index < shown; ++index) {
        const DispatchLogEntry *event =
            &g_dispatch_log[(start + index) % kDispatchLogCapacity];
        fprintf(file,
                "%s\n    {\"i\":%u,\"pc24\":\"%06X\",\"source_pc24\":\"%06X\",\"func\":",
                index == 0u ? "" : ",", start + index, event->pc24,
                event->source_pc24);
        json_string(file, event->function_name);
        fprintf(file,
                ",\"mx\":%u,\"found\":%u,\"mirror\":%u,\"frame\":%u}",
                event->mx, event->found, event->mirrored, event->frame);
    }
    fputs("\n  ]}\n}\n", file);
    fclose(file);
}
