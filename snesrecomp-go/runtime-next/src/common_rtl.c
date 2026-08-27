#include "common_rtl.h"

#include "audio_trace.h"
#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "framedump.h"
#include "recomp_hw.h"
#include "runner_next_internal.h"
#include "spc_upload.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/cart_map.h"
#include "snes/dsp.h"
#include "snes/msu1.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    RTL_SNAPSHOT_MAGIC = 0x52544c53u,
    RTL_SNAPSHOT_LEGACY_VERSION = 9u,
    RTL_SNAPSHOT_VERSION = 10u,
    RTL_SNAPSHOT_EXTENDED_AUDIO = 0x00010000u,
    RTL_AUDIO_NATIVE_RATE = 32040,
    RTL_AUDIO_CHUNK = 1024
};

#define RTL_SRAM_FILE "saves/save.srm"
#define RTL_SRAM_BACKUP_FILE "saves/save.srm.bak"

uint8 g_ram[kSnesWramSize];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;
uint8 g_snesrecomp_last_hdmaen;

uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
uint64_t g_apu_last_sync_cycles;

void (*g_rtl_inidisp_hook)(uint8 value);
void (*g_rtl_apu_port_hook)(uint8 port, uint8 value);
void (*g_rtl_apu_port_pace_hook)(uint8 port, uint8 value);
void (*g_rtl_apu_port_trace_hook)(uint8 port, uint8 value);
void (*g_rtl_spc_upload_hook)(uint32 source);
void (*g_rtl_spc_upload_trace_hook)(uint32 source);
void (*g_rtl_apu_state_loaded_hook)(Apu *apu);
void (*g_rtl_music_mix_hook)(int16 *buffer, int frames);

static int g_apuprof = -1;
uint64_t g_apuprof_lockwait_ns;
uint64_t g_apuprof_catchup_ns;
uint64_t g_apuprof_catchup_cyc;
uint64_t g_apuprof_hook_ns;
uint64_t g_apuprof_upload_ns;
uint64_t g_apuprof_sched_lat_max;
uint64_t g_apuprof_audiowait_max_ns;
uint32_t g_apuprof_catchup_calls;
uint32_t g_apuprof_port_reads;
uint32_t g_apuprof_port_writes;
const char *g_apuprof_last_port_func;

static int g_audio_output_rate = 44100;
static double g_audio_phase;
static bool g_apu_catchup_suppressed;

int ApuProfEnabled(void) {
    if (g_apuprof < 0) {
        const char *value = getenv("AR_APUPROF");
        g_apuprof = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return g_apuprof;
}

void ApuProfFrameReset(void) {
    g_apuprof_lockwait_ns = 0u;
    g_apuprof_catchup_ns = 0u;
    g_apuprof_catchup_cyc = 0u;
    g_apuprof_hook_ns = 0u;
    g_apuprof_upload_ns = 0u;
    g_apuprof_sched_lat_max = 0u;
    g_apuprof_catchup_calls = 0u;
    g_apuprof_port_reads = 0u;
    g_apuprof_port_writes = 0u;
    g_apuprof_last_port_func = NULL;
}

static uint32 snapshot_version(void) {
    return RTL_SNAPSHOT_VERSION |
           (dsp_extendedVoicesEnabled() ? RTL_SNAPSHOT_EXTENDED_AUDIO : 0u);
}

typedef struct FileSaveLoad {
    SaveLoadInfo base;
    FILE *file;
} FileSaveLoad;

static void file_saveload(SaveLoadInfo *base, void *data, size_t size) {
    FileSaveLoad *state = (FileSaveLoad *)base;
    size_t transferred;
    if (base->failed) return;
    transferred = base->saving ? fwrite(data, 1u, size, state->file)
                               : fread(data, 1u, size, state->file);
    base->failed = transferred != size;
}

void RtlReset(int mode) {
    snes_frame_counter = 0;
    g_main_cpu_cycles_estimate = 0u;
    g_apu_pace_cycles_estimate = 0u;
    g_apu_last_sync_cycles = 0u;
    g_apu_catchup_suppressed = false;
    if (g_snes != NULL) snes_reset(g_snes, true);
    SnesEnterNativeMode();
    if (g_ppu != NULL) ppu_reset(g_ppu);
    if ((mode & 1) == 0 && g_sram != NULL && g_sram_size > 0)
        memset(g_sram, 0, (size_t)g_sram_size);
    RtlApuLock();
    g_audio_phase = 0.0;
    RtlApuUnlock();
}

void RtlSetAudioOutputRate(int hz) {
    if (hz < 8000) hz = 8000;
    if (hz > 384000) hz = 384000;
    g_audio_output_rate = hz;
    g_audio_phase = 0.0;
}

int RtlGetAudioOutputRate(void) { return g_audio_output_rate; }

bool RtlRunFrame(uint32 inputs) {
    if (g_snes != NULL) {
        g_snes->abiFrameCounter = snes_frame_counter >= 0
            ? (uint64_t)snes_frame_counter : 0u;
    }
    sr_runner_apply_pending_mutations(
        g_snes, &inputs, g_snes != NULL ? g_snes->abiFrameCounter : 0u);
    if ((inputs & 0x30u) == 0x30u) inputs ^= 0x30u;
    if ((inputs & 0xc0u) == 0xc0u) inputs ^= 0xc0u;
    if ((inputs & 0x30000u) == 0x30000u) inputs ^= 0x30000u;
    if ((inputs & 0xc0000u) == 0xc0000u) inputs ^= 0xc0000u;
    /* Expire thread-confined borrowed views before this tick mutates any
     * runner-owned input, memory, or component state. */
    sr_runner_note_tick(g_snes);
    if (g_snes != NULL) {
        g_snes->input1_currentState = (uint16)(inputs & 0xfffu);
        g_snes->input2_currentState = (uint16)((inputs >> 12) & 0xfffu);
    }
    if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
        sr_runner_emit_frame_boundary(
            g_snes, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_VBLANK, "vblank");
    }
    WatchdogFrameStart();
    if (g_rtl_game_info != NULL && g_rtl_game_info->run_frame != NULL)
        g_rtl_game_info->run_frame();
    WatchdogFrameEnd();
    if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
        sr_runner_emit_frame_boundary(g_snes, SR_EVENT_FRAME_END,
                                      "complete");
    }
    if (g_framedump_callback != NULL)
        g_framedump_callback((uint32)snes_frame_counter, g_ram);
    debug_server_record_frame(snes_frame_counter);
    ++snes_frame_counter;
    return false;
}

void RtlSaveSnapshot(const char *filename) {
    uint32 magic = RTL_SNAPSHOT_MAGIC;
    uint32 version = snapshot_version();
    FILE *file;
    FileSaveLoad state;
    if (filename == NULL || g_snes == NULL) return;
    file = fopen(filename, "wb");
    if (file == NULL) return;
    state.base.func = file_saveload;
    state.base.saving = true;
    state.base.portable = true;
    state.base.failed = false;
    state.file = file;
    saveload_u32(&state.base, &magic);
    saveload_u32(&state.base, &version);
    RtlApuLock();
    snes_saveload(g_snes, &state.base);
    RtlApuUnlock();
    fclose(file);
    if (state.base.failed)
        fprintf(stderr, "Unable to write snapshot %s\n", filename);
}

bool RtlLoadSnapshot(const char *filename) {
    uint8 header[8];
    bool portable;
    uint32 legacy_version;
    FILE *file;
    FileSaveLoad state;
    if (filename == NULL || g_snes == NULL) return false;
    file = fopen(filename, "rb");
    if (file == NULL) return false;
    if (fread(header, sizeof(header), 1u, file) != 1u) {
        fclose(file);
        return false;
    }
    legacy_version = RTL_SNAPSHOT_LEGACY_VERSION |
        (dsp_extendedVoicesEnabled() ? RTL_SNAPSHOT_EXTENDED_AUDIO : 0u);
    if (!saveload_decode_snapshot_header(
            header, RTL_SNAPSHOT_MAGIC, snapshot_version(), legacy_version,
            &portable)) {
        fclose(file);
        return false;
    }
    state.base.func = file_saveload;
    state.base.saving = false;
    state.base.portable = portable;
    state.base.failed = false;
    state.file = file;
    RtlApuLock();
    snes_saveload(g_snes, &state.base);
    if (!state.base.failed && g_rtl_apu_state_loaded_hook != NULL)
        g_rtl_apu_state_loaded_hook(g_snes->apu);
    RtlApuUnlock();
    fclose(file);
    return !state.base.failed;
}

void RtlSaveLoad(int command, int slot) {
    char filename[160];
    const char *title = "game";
    const char *prefix = NULL;
    if (g_rtl_game_info != NULL) {
        if (g_rtl_game_info->title != NULL) title = g_rtl_game_info->title;
        prefix = g_rtl_game_info->save_name_prefix;
    }
    if (prefix != NULL)
        snprintf(filename, sizeof(filename), "saves/%s%d.sav", prefix, slot);
    else
        snprintf(filename, sizeof(filename), "saves/%s_save%d.sav", title, slot);
    if (command == kSaveLoad_Save) RtlSaveSnapshot(filename);
    else if (command == kSaveLoad_Load) (void)RtlLoadSnapshot(filename);
}

void MemCpy(void *destination, const void *source, int size) {
    if (size > 0) memcpy(destination, source, (size_t)size);
}

bool Unreachable(void) {
    g_fail = true;
    if (sr_runner_event_enabled(SR_EVENT_MASK_ERROR)) {
        sr_runner_emit_error(g_snes, SR_RUNNER_ERROR_UNREACHABLE, 0u,
                             0u, 0u, "unreachable");
    }
    return false;
}

static uint32 rom_size(void) {
    if (g_snes != NULL && g_snes->cart != NULL && g_snes->cart->romSize != 0u)
        return g_snes->cart->romSize;
    return 0x80000u;
}

uint8 *RomPtr(uint32 address) {
    uint32 size = rom_size();
    SrCartMapping mapping = SR_CART_MAPPING_LOROM;
    SrCartAddress decoded;
    if (g_snes != NULL && g_snes->cart != NULL)
        mapping = (SrCartMapping)g_snes->cart->type;
    decoded = sr_cart_map_read(mapping, (uint8)(address >> 16),
                               (uint16)address, size, 0u);
    if (decoded.region != SR_CART_REGION_ROM) {
        g_fail = true;
        if (sr_runner_event_enabled(SR_EVENT_MASK_ERROR)) {
            sr_runner_emit_error(g_snes, SR_RUNNER_ERROR_UNMAPPED_ROM, 0u,
                                 address, 0u, "unmapped-rom");
        }
        cpu_trace_offrails("RomPtr-unmapped", address & 0xffffffu);
        decoded = sr_cart_map_read(SR_CART_MAPPING_LOROM,
                                   (uint8)(address >> 16),
                                   (uint16)(address | 0x8000u), size, 0u);
    }
    return (uint8 *)(g_rom + decoded.offset % size);
}

uint8 *MvnPtr(uint8 bank, uint16 address) {
    if (bank == 0x7eu) return g_ram + address;
    if (bank == 0x7fu) return g_ram + 0x10000u + address;
    if ((bank < 0x40u || (bank >= 0x80u && bank < 0xc0u)) &&
        address < 0x2000u) return g_ram + address;
    return RomPtr(((uint32)bank << 16) | address);
}

uint8 *IndirPtr_Slow(LongPtr pointer, uint16 offset) {
    return IndirPtr(pointer, offset);
}

static void observe_register_access(bool write, uint16 reg, uint8 value) {
    if (sr_runner_event_enabled(SR_EVENT_MASK_REGISTER_ACCESS)) {
        sr_runner_emit_register_access(g_snes, write, reg, value, 1u);
    }
}

void WriteReg(uint16 reg, uint8 value) {
    if (reg >= 0x2000u && reg < 0x2008u) {
        if (msu1_enabled()) msu1_write(reg, value);
        observe_register_access(true, reg, value);
    } else if (reg >= 0x2100u && reg < 0x2140u) {
        ppu_write(g_ppu, (uint8)reg, value);
        if (reg == 0x2100u && g_rtl_inidisp_hook != NULL)
            g_rtl_inidisp_hook(value);
        observe_register_access(true, reg, value);
    } else if (reg >= 0x2140u && reg < 0x2180u) {
        RtlApuWrite(reg, value);
        observe_register_access(true, reg, value);
    } else if (reg >= 0x2180u && reg < 0x2184u) {
        snes_writeBBus(g_snes, (uint8)reg, value);
    } else if (reg >= 0x4200u && reg < 0x4220u) {
        if (reg == 0x420cu) g_snesrecomp_last_hdmaen = value;
        recomp_write_internal_reg(reg, value);
    } else if (reg >= 0x4300u && reg < 0x4380u) {
        dma_write(g_dma, reg, value);
    } else {
        observe_register_access(true, reg, value);
    }
    debug_server_on_reg_write(reg, value);
}

uint8 ReadReg(uint16 reg) {
    if (reg >= 0x2000u && reg < 0x2008u) {
        uint8 value = msu1_enabled() ? msu1_read(reg) : 0u;
        observe_register_access(false, reg, value);
        return value;
    }
    if (reg >= 0x2100u && reg < 0x2140u) {
        uint8 value = ppu_read(g_ppu, (uint8)reg);
        observe_register_access(false, reg, value);
        return value;
    }
    if (reg >= 0x2140u && reg < 0x2180u)
        return snes_read(g_snes, reg);
    if (reg == 0x2180u) return snes_readBBus(g_snes, (uint8)reg);
    if (reg == 0x4016u || reg == 0x4017u) return snes_readReg(g_snes, reg);
    if (reg >= 0x4200u && reg < 0x4220u)
        return recomp_read_internal_reg(reg);
    if (reg >= 0x4300u && reg < 0x4380u) return dma_read(g_dma, reg);
    observe_register_access(false, reg, 0u);
    return 0u;
}

uint16 ReadRegWord(uint16 reg) {
    if (reg >= 0x2140u && reg <= 0x217fu && g_snes != NULL) {
        uint8 low, high;
        RtlApuLock();
        rtl_accumulate_apu_catchup();
        snes_catchupApu(g_snes);
        if (ApuProfEnabled()) ++g_apuprof_port_reads;
        low = g_snes->apu->outPorts[reg & 3u];
        high = g_snes->apu->outPorts[(reg + 1u) & 3u];
        RtlApuUnlock();
        observe_register_access(false, reg, low);
        observe_register_access(false, (uint16)(reg + 1u), high);
        return (uint16)low | ((uint16)high << 8);
    }
    return (uint16)ReadReg(reg) | ((uint16)ReadReg((uint16)(reg + 1u)) << 8);
}

void WriteRegWord(uint16 reg, uint16 value) {
    if (reg == 0x2118u) {
        /* A low/high pair is one SNES word transfer; VMAIN chooses which byte
         * increments the address, so the ordinary PPU ports are sufficient. */
        ppu_write(g_ppu, 0x18u, (uint8)value);
        ppu_write(g_ppu, 0x19u, (uint8)(value >> 8));
        observe_register_access(true, 0x2118u, (uint8)value);
        observe_register_access(true, 0x2119u, (uint8)(value >> 8));
        debug_server_on_reg_write(0x2118u, (uint8)value);
        debug_server_on_reg_write(0x2119u, (uint8)(value >> 8));
        return;
    }
    if (reg >= 0x2140u && reg <= 0x217fu) {
        WriteReg((uint16)(reg + 1u), (uint8)(value >> 8));
        WriteReg(reg, (uint8)value);
        return;
    }
    WriteReg(reg, (uint8)value);
    WriteReg((uint16)(reg + 1u), (uint8)(value >> 8));
}

void rtl_accumulate_apu_catchup(void) {
    static uint32_t last_read;
    static uint64_t consumer_seen_ms;
    static uint64_t last_wall_ms;
    uint64_t delta;
    uint64_t now;
    uint32_t current_read;
    bool consumer_active;
    if (g_snes == NULL || g_snes->apu == NULL || g_snes->apu->dsp == NULL)
        return;
    delta = g_apu_pace_cycles_estimate - g_apu_last_sync_cycles;
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    now = audio_trace_wall_ms();
    current_read = g_snes->apu->dsp->sampleRead;
    if (current_read != last_read) {
        last_read = current_read;
        consumer_seen_ms = now;
    }
    consumer_active = consumer_seen_ms != 0u && now - consumer_seen_ms < 250u;
    if (!g_apu_catchup_suppressed || !consumer_active)
        g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;
    if (!consumer_active && last_wall_ms != 0u) {
        uint64_t elapsed = now - last_wall_ms;
        uint32_t baseline;
        if (elapsed > 32u) elapsed = 32u;
        baseline = (uint32_t)(elapsed * 1024u);
        g_snes->apuCatchupCycles += baseline;
        audio_trace_on_pace(0, baseline);
    } else {
        audio_trace_on_pace(consumer_active ? 1 : 0, 0u);
    }
    last_wall_ms = now;
}

void RtlSetApuCatchupSuppressed(bool suppressed) {
    RtlApuLock();
    g_apu_catchup_suppressed = suppressed;
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    if (g_snes != NULL) g_snes->apuCatchupCycles = 0.0;
    RtlApuUnlock();
}

void RtlApuWrite(uint16 address, uint8 value) {
    Apu *apu;
    uint64_t quantum, now, produced, delta, target;
    unsigned port;
    if (address < 0x2140u || address > 0x2143u || g_snes == NULL) return;
    port = address & 3u;
    if (g_rtl_apu_port_pace_hook != NULL)
        g_rtl_apu_port_pace_hook((uint8)port, value);
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    audio_trace_on_cpu_port_write((uint8)port, value);
    if (ApuProfEnabled()) {
        ++g_apuprof_port_writes;
        g_apuprof_last_port_func = g_last_recomp_func;
    }
    if (g_rtl_apu_port_hook != NULL) {
        uint64_t start = ApuProfEnabled() ? audio_trace_wall_ns() : 0u;
        g_rtl_apu_port_hook((uint8)port, value);
        if (start != 0u) g_apuprof_hook_ns += audio_trace_wall_ns() - start;
    }
    if (g_rtl_apu_port_trace_hook != NULL)
        g_rtl_apu_port_trace_hook((uint8)port, value);

    apu = g_snes->apu;
    quantum = audio_trace_consume_quantum();
    if (quantum == 0u) quantum = 534u;
    now = audio_trace_wall_ns();
    produced = apu->sampleClock;
    delta = apu->portClockNs == 0u ? 0u
        : (now - apu->portClockNs) * RTL_AUDIO_NATIVE_RATE / 1000000000u;
    if (delta > 4u * quantum) delta = 4u * quantum;
    target = apu->portClock + delta;
    if (target < produced) target = produced;
    if (target > produced + 3u * quantum) target = produced + 3u * quantum;
    if (apu->portLastValid[port] && value != apu->portLastVal[port]) {
        uint64_t floor = apu->portLastTarget[port] + APU_PORT_MIN_DWELL;
        uint64_t ceiling = produced + 8u * quantum;
        if (target < floor) target = floor < ceiling ? floor : ceiling;
    }
    apu->portLastTarget[port] = target;
    apu->portLastVal[port] = value;
    apu->portLastValid[port] = 1u;
    apu->portClock = target;
    apu->portClockNs = now;
    if (ApuProfEnabled()) {
        uint64_t latency = target > produced ? target - produced : 0u;
        if (latency > g_apuprof_sched_lat_max)
            g_apuprof_sched_lat_max = latency;
    }
    apu_schedulePortWrite(apu, (uint8)port, value, target);
    RtlApuUnlock();
}

static bool apply_spc_upload_control(
        Apu *apu, const SrSpcUploadContext *upload) {
    uint32_t cycles;
    if (apu == NULL || apu->spc == NULL || upload == NULL ||
        upload->struct_size < SR_SPC_UPLOAD_CONTEXT_V2_SIZE ||
        (upload->control_flags &
         ~(SR_SPC_UPLOAD_CONTROL_SET_PC |
           SR_SPC_UPLOAD_CONTROL_RUN_UNTIL_PC)) != 0u ||
        upload->reserved8[0] != 0u || upload->reserved8[1] != 0u ||
        upload->reserved8[2] != 0u)
        return false;
    if ((upload->control_flags & SR_SPC_UPLOAD_CONTROL_SET_PC) != 0u)
        apu->spc->pc = upload->requested_pc;
    if ((upload->control_flags & SR_SPC_UPLOAD_CONTROL_RUN_UNTIL_PC) == 0u)
        return true;
    if (upload->stop_pc_count == 0u ||
        upload->stop_pc_count > SR_SPC_UPLOAD_STOP_PC_MAX ||
        upload->max_cycles == 0u ||
        upload->max_cycles > SR_SPC_UPLOAD_MAX_CONTROL_CYCLES)
        return false;
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
    for (cycles = 0u; cycles < upload->max_cycles; ++cycles) {
        uint32_t index;
        bool reached = false;
        for (index = 0u; index < upload->stop_pc_count; ++index) {
            if (apu->spc->pc == upload->stop_pc[index]) {
                reached = true;
                break;
            }
        }
        if (reached || apu->spc->stopped) break;
        apu_cycle(apu);
    }
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
    return true;
}

static bool upload_spc_image(CpuState *cpu, bool update_result) {
    uint32 source24;
    size_t source_offset;
    SrSpcUploadResult parsed;
    SrSpcUploadContext upload;
    uint64_t profile_start = ApuProfEnabled() ? audio_trace_wall_ns() : 0u;
    bool initial_upload;
    bool success;
    if (cpu == NULL || g_snes == NULL || g_snes->apu == NULL || g_rom == NULL)
        return false;
    if (g_rtl_game_info == NULL ||
        g_rtl_game_info->spc_upload_source == NULL ||
        !g_rtl_game_info->spc_upload_source(cpu, &source24)) return false;
    source_offset = (size_t)(RomPtr(source24) - g_rom);

    RtlApuLock();
    memset(&upload, 0, sizeof(upload));
    upload.struct_size = SR_SPC_UPLOAD_CONTEXT_V2_SIZE;
    upload.rom_data = g_rom;
    upload.apu_ram = g_snes->apu->ram;
    upload.rom_byte_size = rom_size();
    upload.apu_ram_byte_size = SR_APU_RAM_BYTE_COUNT;
    success = sr_spc_upload_image(g_rom, rom_size(), source_offset,
                                  g_snes->apu->ram, &parsed);
    if (!success) {
        RtlApuUnlock();
        return false;
    }
    upload.script_offset = parsed.script_offset;
    upload.entry_point = parsed.entry_point;
    upload.block_count = parsed.block_count;
    if (g_rtl_game_info->spc_upload_customize != NULL)
        success = g_rtl_game_info->spc_upload_customize(
            cpu, &upload, source24);
    if (!success) {
        RtlApuUnlock();
        return false;
    }
    apu_clearPortQueue(g_snes->apu);
    memset(g_snes->apu->inPorts, 0, sizeof(g_snes->apu->inPorts));
    memset(g_snes->apu->outPorts, 0, sizeof(g_snes->apu->outPorts));
    initial_upload = g_snes->apu->romReadable;
    if (initial_upload) {
        g_snes->apu->romReadable = false;
        g_snes->apuCatchupCycles = 0.0;
        g_snes->apu->cpuCyclesLeft = 0u;
        if (upload.entry_point != 0u) {
            Spc *spc = g_snes->apu->spc;
            spc->a = spc->x = spc->y = 0u;
            if (spc->sp == 0u) spc->sp = 0xefu;
            spc->pc = upload.entry_point;
        }
    }
    upload.state_flags = initial_upload ? SR_SPC_UPLOAD_STATE_INITIAL : 0u;
    if (g_snes->apu->spc->stopped)
        upload.state_flags |= SR_SPC_UPLOAD_STATE_SPC_STOPPED;
    upload.spc_pc = g_snes->apu->spc->pc;
    if (g_rtl_game_info->spc_upload_commit != NULL)
        g_rtl_game_info->spc_upload_commit(&upload);
    success = apply_spc_upload_control(g_snes->apu, &upload);
    if (!success) {
        RtlApuUnlock();
        return false;
    }
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    RtlAudioExtensionNotifyUploadLocked(source24);
    RtlApuUnlock();

    if (g_rtl_spc_upload_hook != NULL) g_rtl_spc_upload_hook(source24);
    if (g_rtl_spc_upload_trace_hook != NULL)
        g_rtl_spc_upload_trace_hook(source24);
    if (profile_start != 0u)
        g_apuprof_upload_ns += audio_trace_wall_ns() - profile_start;
    if (update_result) {
        cpu->A &= 0xff00u;
        cpu->X = cpu->Y = 0u;
        cpu->_flag_Z = 1u;
        cpu->_flag_N = 0u;
        cpu->P = (uint8)((cpu->P & ~0x82u) | 0x02u);
    }
    return true;
}

bool RtlUploadSpcImageFromDp(CpuState *cpu) {
    bool success = upload_spc_image(cpu, false);
    int pop = g_rtl_game_info != NULL &&
                      g_rtl_game_info->spc_upload_stack_pop != NULL ?
                  g_rtl_game_info->spc_upload_stack_pop(cpu) : 0;
    if (cpu != NULL) cpu->S = (uint16)(cpu->S + pop);
    return success;
}

bool RtlHandleSpcUpload(CpuState *cpu) {
    return upload_spc_image(cpu, true);
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
    int rendered = 0;
    double step;
    if (audio_buffer == NULL || samples <= 0 || channels != 2 ||
        g_snes == NULL || g_snes->apu == NULL) return;
    step = (double)RTL_AUDIO_NATIVE_RATE / (double)g_audio_output_rate;
    while (rendered < samples) {
        int chunk = samples - rendered;
        uint32_t needed;
        if (chunk > RTL_AUDIO_CHUNK) chunk = RTL_AUDIO_CHUNK;
        needed = (uint32_t)(g_audio_phase + (double)chunk * step) + 2u;
        for (;;) {
            Dsp *dsp;
            uint32_t available;
            RtlApuLock();
            dsp = g_snes->apu->dsp;
            available = dsp->sampleWrite - dsp->sampleRead;
            if (available < needed) {
                int cycle_budget = 256;
                audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
                while (cycle_budget-- > 0 &&
                       dsp->sampleWrite - dsp->sampleRead < needed)
                    apu_cycle(g_snes->apu);
                audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
                available = dsp->sampleWrite - dsp->sampleRead;
            }
            if (available >= needed) {
                dsp_getSamplesResampled(dsp, audio_buffer + rendered * 2,
                                        chunk, step, &g_audio_phase);
                RtlApuUnlock();
                break;
            }
            RtlApuUnlock();
        }
        rendered += chunk;
    }
    RtlApuLock();
    msu1_mix(audio_buffer, samples, g_audio_output_rate);
    if (g_rtl_music_mix_hook != NULL)
        g_rtl_music_mix_hook(audio_buffer, samples);
    RtlApuUnlock();
    {
        uint64_t frame_offset = g_snes->abiAudioFrameCounter;
        g_snes->abiAudioFrameCounter += (uint64_t)samples;
        if (sr_runner_event_enabled(SR_EVENT_MASK_AUDIO)) {
            sr_runner_emit_audio_produced(
                g_snes, audio_buffer, frame_offset, (uint32_t)samples,
                (uint32_t)g_audio_output_rate, (uint16_t)channels);
        }
    }
}

void RtlMigrateLegacySram(const char *legacy_title) {
    char legacy[128];
    char buffer[1024];
    size_t count;
    FILE *source;
    FILE *destination;
    if (legacy_title == NULL || legacy_title[0] == '\0') return;
    destination = fopen(RTL_SRAM_FILE, "rb");
    if (destination != NULL) { fclose(destination); return; }
    snprintf(legacy, sizeof(legacy), "saves/%s.srm", legacy_title);
    if (strcmp(legacy, RTL_SRAM_FILE) == 0) return;
    source = fopen(legacy, "rb");
    if (source == NULL) return;
    destination = fopen(RTL_SRAM_FILE, "wb");
    if (destination == NULL) { fclose(source); return; }
    while ((count = fread(buffer, 1u, sizeof(buffer), source)) != 0u)
        (void)fwrite(buffer, 1u, count, destination);
    fclose(destination);
    fclose(source);
}

void RtlReadSram(void) {
    FILE *file;
    if (g_sram == NULL || g_sram_size <= 0) return;
    if (g_rtl_game_info != NULL)
        RtlMigrateLegacySram(g_rtl_game_info->title);
    file = fopen(RTL_SRAM_FILE, "rb");
    if (file == NULL) return;
    if (fread(g_sram, 1u, (size_t)g_sram_size, file) != (size_t)g_sram_size)
        fprintf(stderr, "Unable to read complete SRAM file\n");
    fclose(file);
}

void RtlWriteSram(void) {
    FILE *file;
    if (g_sram == NULL || g_sram_size <= 0) return;
#ifdef _WIN32
    (void)MoveFileExA(RTL_SRAM_FILE, RTL_SRAM_BACKUP_FILE,
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    (void)rename(RTL_SRAM_FILE, RTL_SRAM_BACKUP_FILE);
#endif
    file = fopen(RTL_SRAM_FILE, "wb");
    if (file == NULL) return;
    if (fwrite(g_sram, 1u, (size_t)g_sram_size, file) != (size_t)g_sram_size)
        fprintf(stderr, "Unable to write complete SRAM file\n");
    fclose(file);
}
