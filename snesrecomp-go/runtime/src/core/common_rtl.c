#include "snesrecomp/game/runtime.h"

#include "snesrecomp/host/audio_trace.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/host/framedump.h"
#include "recomp_hw.h"
#include "runner_internal.h"
#include "runner_game_module_internal.h"
#include "runner_state_internal.h"
#include "snesrecomp/spc_upload.h"
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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    RTL_SNAPSHOT_MAGIC = 0x52544c53u,
    /* The accurate DSP changes the raw native APU/DSP layout.  Version 9
     * snapshots cannot be decoded by that layout and must not be accepted as
     * if they were current native snapshots. */
    RTL_SNAPSHOT_LEGACY_VERSION = 11u,
    RTL_SNAPSHOT_VERSION = 12u,
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

uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
uint64_t g_apu_last_sync_cycles;

static _Atomic int s_apu_profile_enabled = -1;
static _Atomic uint64_t s_apuprof_lockwait_ns;
static _Atomic uint64_t s_apuprof_port_sync_ns;
static _Atomic uint64_t s_apuprof_audio_cycles;
static _Atomic uint64_t s_apuprof_port_sync_cycles;
static _Atomic uint64_t s_apuprof_upload_control_cycles;
static _Atomic uint64_t s_apuprof_timeline_cycles;
static _Atomic uint64_t s_apuprof_hook_ns;
static _Atomic uint64_t s_apuprof_upload_ns;
static _Atomic uint64_t s_apuprof_sched_lat_max;
static _Atomic uint64_t s_apuprof_audiowait_max_ns;
static _Atomic uint32_t s_apuprof_port_sync_calls;
static _Atomic uint32_t s_apuprof_port_reads;
static _Atomic uint32_t s_apuprof_port_writes;
static _Atomic(const char *) s_apuprof_last_port_func;
static uint64_t s_apuprof_cycle_baseline;
static uint64_t s_apuprof_port_sync_start_ns;

static void apuprof_catchup_hook(bool begin, uint64_t cycles) {
    if (begin) {
        s_apuprof_port_sync_start_ns = audio_trace_wall_ns();
        return;
    }
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_PORT_SYNC, cycles,
        s_apuprof_port_sync_start_ns != 0u
            ? audio_trace_wall_ns() - s_apuprof_port_sync_start_ns : 0u);
    s_apuprof_port_sync_start_ns = 0u;
}

static int g_audio_output_rate = 44100;
static double g_audio_phase;
static bool g_apu_catchup_suppressed;

bool RtlApuProfileIsEnabled(void) {
    int enabled = atomic_load_explicit(
        &s_apu_profile_enabled, memory_order_relaxed);
    if (enabled < 0) {
        const char *value = getenv("SNESRECOMP_APU_PROFILE");
        int detected =
            value != NULL && value[0] != '\0' && value[0] != '0';
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(
                &s_apu_profile_enabled, &expected, detected,
                memory_order_relaxed, memory_order_relaxed)) {
            enabled = detected;
        } else {
            enabled = expected;
        }
    }
    g_snes_apu_catchup_profile_hook = enabled != 0
        ? apuprof_catchup_hook : NULL;
    return enabled != 0;
}

static void apuprof_max(_Atomic uint64_t *value, uint64_t candidate) {
    uint64_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate > current &&
           !atomic_compare_exchange_weak_explicit(
               value, &current, candidate,
               memory_order_relaxed, memory_order_relaxed)) {}
}

void RtlApuProfileReset(void) {
    (void)RtlApuProfileIsEnabled();
    RtlApuLock();
    s_apuprof_cycle_baseline = snes_apu_cycle_count();
    atomic_store_explicit(&s_apuprof_lockwait_ns, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_port_sync_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_audio_cycles, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_port_sync_cycles, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_upload_control_cycles, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_timeline_cycles, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_hook_ns, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_upload_ns, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_sched_lat_max, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_audiowait_max_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_port_sync_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_port_reads, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_port_writes, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_apuprof_last_port_func, NULL,
                          memory_order_relaxed);
    RtlApuUnlock();
}

void RtlApuProfileRead(RtlApuProfile *out_profile) {
    uint64_t attributed;
    uint64_t current_cycles;
    if (out_profile == NULL || out_profile->struct_size < RTL_APU_PROFILE_V2_SIZE)
        return;
    RtlApuLock();
    current_cycles = snes_apu_cycle_count();
    memset(out_profile, 0, RTL_APU_PROFILE_V2_SIZE);
    out_profile->struct_size = RTL_APU_PROFILE_V2_SIZE;
    out_profile->lock_wait_ns =
        atomic_load_explicit(&s_apuprof_lockwait_ns, memory_order_relaxed);
    out_profile->port_sync_ns =
        atomic_load_explicit(&s_apuprof_port_sync_ns, memory_order_relaxed);
    if (current_cycles >= s_apuprof_cycle_baseline) {
        out_profile->apu_cycles_total =
            current_cycles - s_apuprof_cycle_baseline;
    } else {
        /* Loading an older portable state (or resetting the APU) can move the
         * serialized semantic clock behind this process-local observation
         * baseline.  Report the discontinuity instead of wrapping it into a
         * near-UINT64_MAX cycle count. */
        out_profile->flags |= RTL_APU_PROFILE_INCONSISTENT;
    }
    out_profile->apu_cycles_audio_demand =
        atomic_load_explicit(&s_apuprof_audio_cycles,
                             memory_order_relaxed);
    out_profile->apu_cycles_port_sync =
        atomic_load_explicit(&s_apuprof_port_sync_cycles,
                             memory_order_relaxed);
    out_profile->apu_cycles_upload_control =
        atomic_load_explicit(&s_apuprof_upload_control_cycles,
                             memory_order_relaxed);
    out_profile->apu_cycles_timeline =
        atomic_load_explicit(&s_apuprof_timeline_cycles,
                             memory_order_relaxed);
    attributed = out_profile->apu_cycles_audio_demand +
        out_profile->apu_cycles_port_sync +
        out_profile->apu_cycles_upload_control +
        out_profile->apu_cycles_timeline;
    if (attributed <= out_profile->apu_cycles_total) {
        out_profile->apu_cycles_unattributed =
            out_profile->apu_cycles_total - attributed;
    } else {
        out_profile->flags |= RTL_APU_PROFILE_INCONSISTENT;
    }
    out_profile->hook_ns =
        atomic_load_explicit(&s_apuprof_hook_ns, memory_order_relaxed);
    out_profile->upload_ns =
        atomic_load_explicit(&s_apuprof_upload_ns, memory_order_relaxed);
    out_profile->scheduled_latency_max =
        atomic_load_explicit(&s_apuprof_sched_lat_max, memory_order_relaxed);
    out_profile->audio_wait_max_ns =
        atomic_load_explicit(&s_apuprof_audiowait_max_ns,
                             memory_order_relaxed);
    out_profile->port_sync_calls =
        atomic_load_explicit(&s_apuprof_port_sync_calls,
                             memory_order_relaxed);
    out_profile->port_reads =
        atomic_load_explicit(&s_apuprof_port_reads, memory_order_relaxed);
    out_profile->port_writes =
        atomic_load_explicit(&s_apuprof_port_writes, memory_order_relaxed);
    out_profile->last_port_function =
        atomic_load_explicit(&s_apuprof_last_port_func, memory_order_relaxed);
    RtlApuUnlock();
}

void RtlApuProfileRecordHostWait(uint64_t wait_ns, bool lock_wait) {
    if (!RtlApuProfileIsEnabled()) return;
    if (lock_wait) {
        atomic_fetch_add_explicit(&s_apuprof_lockwait_ns, wait_ns,
                                  memory_order_relaxed);
    } else {
        apuprof_max(&s_apuprof_audiowait_max_ns, wait_ns);
    }
}

void sr_runner_record_apu_profile_cycles(
        SrApuProfileCycleSource source, uint64_t cycles,
        uint64_t elapsed_ns) {
    _Atomic uint64_t *counter = NULL;
    if (!RtlApuProfileIsEnabled()) return;
    switch (source) {
        case SR_APU_PROFILE_CYCLE_AUDIO_DEMAND:
            counter = &s_apuprof_audio_cycles;
            break;
        case SR_APU_PROFILE_CYCLE_PORT_SYNC:
            counter = &s_apuprof_port_sync_cycles;
            atomic_fetch_add_explicit(&s_apuprof_port_sync_ns, elapsed_ns,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&s_apuprof_port_sync_calls, 1u,
                                      memory_order_relaxed);
            break;
        case SR_APU_PROFILE_CYCLE_UPLOAD_CONTROL:
            counter = &s_apuprof_upload_control_cycles;
            break;
        case SR_APU_PROFILE_CYCLE_TIMELINE:
            counter = &s_apuprof_timeline_cycles;
            break;
        default:
            return;
    }
    atomic_fetch_add_explicit(counter, cycles, memory_order_relaxed);
}

uint64_t RtlApuProfileTakeAudioWaitMax(void) {
    return atomic_exchange_explicit(&s_apuprof_audiowait_max_ns, 0u,
                                    memory_order_relaxed);
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

/* Late enough that a legitimate black boot sequence has finished, early
 * enough to be the first thing a developer sees. */
enum { kSrSilentCanvasFrameLimit = 120 };

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
            g_snes, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_HOST_TICK,
            "host-tick-begin");
    }
    WatchdogFrameStart();
    if (g_rtl_game_execution != NULL)
        g_rtl_game_execution->run_frame();
    WatchdogFrameEnd();
    RtlAdvanceApuTimeline();
    if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
        sr_runner_emit_frame_boundary(
            g_snes, SR_EVENT_FRAME_END | SR_EVENT_FRAME_HOST_TICK,
            "host-tick-end");
    }
    if (g_framedump_callback != NULL)
        g_framedump_callback((uint32)snes_frame_counter, g_ram);
    ++snes_frame_counter;
    /*
     * A game can run correctly and still show nothing, because the runner owns
     * per-line rendering but never starts it on its own: the frame is
     * host-resumable, so the game decides when the picture is produced. With
     * Once a host has requested video, a callback that never drives scanout or
     * binds a surface advances forever over a black canvas. Say it once, well
     * after any legitimate black boot sequence. Missing callbacks are reported
     * immediately by RtlGameDrawPpuFrame instead.
     */
    if (snes_frame_counter >= kSrSilentCanvasFrameLimit && g_snes != NULL &&
        g_snes->diagnosticDrawRequested &&
        !g_snes->diagnosticVideoWarningReported &&
        g_rtl_game_execution != NULL &&
        g_rtl_game_execution->draw_ppu_frame != NULL) {
        g_snes->diagnosticVideoWarningReported = true;
        if (!g_snes->diagnosticScanoutObserved) {
            fprintf(stderr,
                    "[runner] %d frames have run but PPU scanout never has, "
                    "so no SNES picture has been rasterized.\n"
                    "[runner] Set RtlGameExecutionApi.draw_ppu_frame and drive "
                    "run_ppu_scanout once per frame.\n",
                    snes_frame_counter);
        }
        if (!g_snes->diagnosticMainSurfaceBound) {
            fprintf(stderr,
                    "[runner] No main output surface has been bound, so the "
                    "PPU has nowhere to draw.\n"
                    "[runner] Call bind_ppu_output_surface with kind "
                    "SR_PPU_OUTPUT_MAIN (scale must be 0).\n");
        }
    }
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
    state.base.semantic = false;
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
    state.base.semantic = false;
    state.base.failed = false;
    state.file = file;
    RtlApuLock();
    snes_saveload(g_snes, &state.base);
    if (!state.base.failed)
        rtl_game_audio_state_loaded(g_snes->apu);
    RtlApuUnlock();
    fclose(file);
    return !state.base.failed;
}

void RtlSaveLoad(int command, int slot) {
    char filename[160];
    const char *title = "game";
    const char *prefix = NULL;
    if (g_rtl_game_identity != NULL) {
        title = g_rtl_game_identity->game_id;
        prefix = g_rtl_game_identity->save_name_prefix;
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
        if (reg == 0x2100u && g_rtl_game_execution != NULL &&
            g_rtl_game_execution->ppu_display_control_write != NULL)
            g_rtl_game_execution->ppu_display_control_write(value);
        observe_register_access(true, reg, value);
    } else if (reg >= 0x2140u && reg < 0x2180u) {
        RtlApuWrite(reg, value);
        observe_register_access(true, reg, value);
    } else if (reg >= 0x2180u && reg < 0x2184u) {
        snes_writeBBus(g_snes, (uint8)reg, value);
    } else if (reg >= 0x4200u && reg < 0x4220u) {
        recomp_write_internal_reg(reg, value);
    } else if (reg >= 0x4300u && reg < 0x4380u) {
        dma_write(g_dma, reg, value);
    } else {
        observe_register_access(true, reg, value);
    }
}

uint8 ReadReg(uint16 reg) {
    if (reg >= 0x2000u && reg < 0x2008u) {
        uint8 value = msu1_enabled() ? msu1_read(reg) : 0u;
        observe_register_access(false, reg, value);
        return value;
    }
    if (reg >= 0x2100u && reg < 0x2140u) {
        if (reg == 0x2137u) snes_latchPpuCounters(g_snes);
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
        if (RtlApuProfileIsEnabled())
            atomic_fetch_add_explicit(&s_apuprof_port_reads, 1u,
                                      memory_order_relaxed);
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
    uint64_t delta;
    if (g_snes == NULL || g_snes->apu == NULL || g_snes->apu->dsp == NULL)
        return;
    delta = g_apu_pace_cycles_estimate - g_apu_last_sync_cycles;
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    if (!g_apu_catchup_suppressed)
        g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;
    audio_trace_on_pace(
        g_snes->apu->cycleClock > g_snes->apu->timelineTargetCycles,
        0u);
}

void RtlAdvanceApuTimeline(void) {
    Apu *apu;
    uint64_t before;
    uint64_t cycles;
    if (g_snes == NULL || g_snes->apu == NULL) return;
    RtlApuLock();
    apu = g_snes->apu;
    apu->timelineTargetCycles += RTL_APU_TIMELINE_CYCLES_PER_TICK;
    before = apu_cycle_count(apu);
    cycles = apu->timelineTargetCycles > before
        ? apu->timelineTargetCycles - before : 0u;
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
    for (uint64_t index = 0u; index < cycles; ++index) apu_cycle(apu);
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_TIMELINE, cycles, 0u);
    RtlApuUnlock();
}

uint64_t RtlApuCycleCount(void) {
    uint64_t cycles;
    RtlApuLock();
    cycles = snes_apu_cycle_count();
    RtlApuUnlock();
    return cycles;
}

static bool write_apu_audit_file(const char *prefix, const char *suffix,
                                 const uint8_t *data, size_t size) {
    char path[1024];
    FILE *file;
    size_t transferred;
    int close_result;
    if (snprintf(path, sizeof(path), "%s%s", prefix, suffix) >=
        (int)sizeof(path)) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    transferred = fwrite(data, 1u, size, file);
    close_result = fclose(file);
    return transferred == size && close_result == 0;
}

int RtlCaptureApuAudit(const char *prefix) {
    enum { kCaptureBytes = SR_APU_RAM_BYTE_COUNT +
                           SR_DSP_REGISTER_BYTE_COUNT +
                           APU_RAM_WRITE_BITMAP_BYTES };
    uint8_t *capture;
    uint8_t *aram;
    uint8_t *dsp;
    uint8_t *written;
    char trace_path[1024];
    bool success;
    if (prefix == NULL || prefix[0] == '\0' || g_snes == NULL ||
        g_snes->apu == NULL || g_snes->apu->dsp == NULL ||
        !g_snes->apu->auditWritesEnabled) return 0;
    capture = (uint8_t *)malloc(kCaptureBytes);
    if (capture == NULL) return 0;
    aram = capture;
    dsp = aram + SR_APU_RAM_BYTE_COUNT;
    written = dsp + SR_DSP_REGISTER_BYTE_COUNT;
    RtlApuLock();
    memcpy(aram, g_snes->apu->ram, SR_APU_RAM_BYTE_COUNT);
    dsp_copyRegisters(g_snes->apu->dsp, dsp);
    memcpy(written, g_snes->apu->ramWritten,
           APU_RAM_WRITE_BITMAP_BYTES);
    RtlApuUnlock();
    success = write_apu_audit_file(
                  prefix, ".aram", aram, SR_APU_RAM_BYTE_COUNT) &&
              write_apu_audit_file(
                  prefix, ".dsp", dsp, SR_DSP_REGISTER_BYTE_COUNT) &&
              write_apu_audit_file(
                  prefix, ".written", written,
                  APU_RAM_WRITE_BITMAP_BYTES);
    if (snprintf(trace_path, sizeof(trace_path), "%s.audio.jsonl", prefix) >=
        (int)sizeof(trace_path) || audio_trace_dump_jsonl(trace_path) != 0)
        success = false;
    free(capture);
    return success ? 1 : 0;
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
    uint64_t produced, target;
    unsigned port;
    if (address < 0x2140u || address > 0x2143u || g_snes == NULL) return;
    port = address & 3u;
    if (g_rtl_game_audio != NULL &&
        g_rtl_game_audio->apu_port_pace != NULL)
        g_rtl_game_audio->apu_port_pace((uint8)port, value);
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    audio_trace_on_cpu_port_write_at(
        (uint8)port, value,
        g_sr_block_index != 0u
            ? g_sr_block_ring[(g_sr_block_index - 1u) &
                              kRuntimeBlockTraceRingMask]
            : 0u,
        g_last_recomp_func);
    if (RtlApuProfileIsEnabled()) {
        atomic_fetch_add_explicit(&s_apuprof_port_writes, 1u,
                                  memory_order_relaxed);
        atomic_store_explicit(&s_apuprof_last_port_func, g_last_recomp_func,
                              memory_order_relaxed);
    }
    if (g_rtl_game_audio != NULL &&
        g_rtl_game_audio->apu_port_write != NULL) {
        uint64_t start = RtlApuProfileIsEnabled()
            ? audio_trace_wall_ns() : 0u;
        g_rtl_game_audio->apu_port_write((uint8)port, value);
        if (start != 0u)
            atomic_fetch_add_explicit(
                &s_apuprof_hook_ns, audio_trace_wall_ns() - start,
                memory_order_relaxed);
    }
    if (sr_runner_audio_trace_enabled(
            SR_AUDIO_TRACE_MASK_CPU_PORT_WRITE))
        sr_runner_emit_audio_trace(
            g_snes->apu, SR_AUDIO_TRACE_CPU_PORT_WRITE, 0u,
            (uint8)port, 0u, value, snes_apu_cycle_count(), 0u,
            snes_frame_counter >= 0 ? (uint32_t)snes_frame_counter : 0u,
            g_last_recomp_func);

    apu = g_snes->apu;
    produced = apu->sampleClock;
    target = apu->portClock > produced ? apu->portClock : produced;
    if (apu->portLastValid[port] && value != apu->portLastVal[port]) {
        uint64_t floor = apu->portLastTarget[port] + APU_PORT_MIN_DWELL;
        uint64_t ceiling = produced +
            8u * RTL_APU_TIMELINE_FRAMES_PER_TICK;
        if (target < floor) target = floor < ceiling ? floor : ceiling;
    }
    apu->portLastTarget[port] = target;
    apu->portLastVal[port] = value;
    apu->portLastValid[port] = 1u;
    apu->portClock = target;
    apu->portClockNs = 0u;
    if (RtlApuProfileIsEnabled()) {
        uint64_t latency = target > produced ? target - produced : 0u;
        apuprof_max(&s_apuprof_sched_lat_max, latency);
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
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_UPLOAD_CONTROL, cycles, 0u);
    return true;
}

static bool upload_spc_image(CpuState *cpu, bool update_result) {
    uint32 source24;
    size_t source_offset;
    SrSpcUploadResult parsed;
    SrSpcUploadContext upload;
    uint64_t profile_start = RtlApuProfileIsEnabled()
        ? audio_trace_wall_ns() : 0u;
    bool initial_upload;
    bool success;
    bool track_writes;
    RtlGameSpcUploadPrepareRawFunc *prepare_raw = NULL;
    uint8_t *customize_before = NULL;
    if (cpu == NULL || g_snes == NULL || g_snes->apu == NULL || g_rom == NULL)
        return false;
    if (g_rtl_game_audio == NULL ||
        g_rtl_game_audio->spc_upload_source == NULL ||
        !g_rtl_game_audio->spc_upload_source(cpu, &source24)) return false;
    source_offset = (size_t)(RomPtr(source24) - g_rom);

    RtlApuLock();
    memset(&upload, 0, sizeof(upload));
    upload.struct_size = SR_SPC_UPLOAD_CONTEXT_V2_SIZE;
    upload.rom_data = g_rom;
    upload.apu_ram = g_snes->apu->ram;
    upload.rom_byte_size = rom_size();
    upload.apu_ram_byte_size = SR_APU_RAM_BYTE_COUNT;
    upload.script_offset = source_offset;
    if (g_rtl_game_audio->struct_size >= RTL_GAME_AUDIO_API_V3_SIZE)
        prepare_raw = g_rtl_game_audio->spc_upload_prepare_raw;
    track_writes = g_snes->apu->auditWritesEnabled;
    if (track_writes)
        sr_spc_upload_begin_write_tracking(
            g_snes->apu->ramWritten, sizeof(g_snes->apu->ramWritten));
    if (track_writes &&
        (prepare_raw != NULL ||
         g_rtl_game_audio->spc_upload_customize != NULL)) {
        customize_before = (uint8_t *)malloc(SR_APU_RAM_BYTE_COUNT);
        if (customize_before != NULL)
            memcpy(customize_before, g_snes->apu->ram,
                   SR_APU_RAM_BYTE_COUNT);
    }
    if (prepare_raw != NULL) {
        success = prepare_raw(cpu, &upload, source24);
    } else {
        success = sr_spc_upload_image(g_rom, rom_size(), source_offset,
                                      g_snes->apu->ram, &parsed);
        if (success) {
            upload.script_offset = parsed.script_offset;
            upload.entry_point = parsed.entry_point;
            upload.block_count = parsed.block_count;
        }
    }
    if (!success) {
        free(customize_before);
        if (track_writes) sr_spc_upload_end_write_tracking();
        RtlApuUnlock();
        return false;
    }
    if (g_rtl_game_audio->spc_upload_customize != NULL)
        success = g_rtl_game_audio->spc_upload_customize(
            cpu, &upload, source24);
    if (customize_before != NULL) {
        uint32_t address;
        for (address = 0u; address < SR_APU_RAM_BYTE_COUNT; ++address) {
            if (customize_before[address] != g_snes->apu->ram[address])
                apu_markRamWritten(g_snes->apu, (uint16_t)address, 1u);
        }
        free(customize_before);
    }
    if (track_writes) sr_spc_upload_end_write_tracking();
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
    if (g_rtl_game_audio->spc_upload_commit != NULL)
        g_rtl_game_audio->spc_upload_commit(&upload);
    success = apply_spc_upload_control(g_snes->apu, &upload);
    if (!success) {
        RtlApuUnlock();
        return false;
    }
    g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
    RtlAudioExtensionNotifyUploadLocked(source24);
    if (sr_runner_audio_trace_enabled(SR_AUDIO_TRACE_MASK_SPC_UPLOAD))
        sr_runner_emit_audio_trace(
            g_snes->apu, SR_AUDIO_TRACE_SPC_UPLOAD, 0u, 0u, 0u, 0u,
            snes_apu_cycle_count(), source24,
            snes_frame_counter >= 0 ? (uint32_t)snes_frame_counter : 0u,
            g_last_recomp_func);
    RtlApuUnlock();

    if (g_rtl_game_audio != NULL &&
        g_rtl_game_audio->spc_upload_completed != NULL)
        g_rtl_game_audio->spc_upload_completed(source24);
    if (profile_start != 0u)
        atomic_fetch_add_explicit(
            &s_apuprof_upload_ns, audio_trace_wall_ns() - profile_start,
            memory_order_relaxed);
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
    int pop = g_rtl_game_audio != NULL &&
                      g_rtl_game_audio->spc_upload_stack_pop != NULL ?
                  g_rtl_game_audio->spc_upload_stack_pop(cpu) : 0;
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
    sr_runner_audio_production_begin();
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
                const uint64_t cycle_start = snes_apu_cycle_count();
                audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
                while (cycle_budget-- > 0 &&
                       dsp->sampleWrite - dsp->sampleRead < needed)
                    apu_cycle(g_snes->apu);
                audio_trace_set_producer(AUDIO_TRACE_PRODUCER_UNKNOWN);
                sr_runner_record_apu_profile_cycles(
                    SR_APU_PROFILE_CYCLE_AUDIO_DEMAND,
                    snes_apu_cycle_count() - cycle_start, 0u);
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
    if (g_rtl_game_audio != NULL && g_rtl_game_audio->mix_output != NULL)
        g_rtl_game_audio->mix_output(audio_buffer, samples);
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
    sr_runner_audio_production_end();
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
    if (g_rtl_game_identity != NULL)
        RtlMigrateLegacySram(g_rtl_game_identity->game_id);
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
