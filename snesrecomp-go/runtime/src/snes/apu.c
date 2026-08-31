#include "apu.h"

#include "dsp.h"
#include "saveload.h"
#include "snes.h"
#include "spc.h"
#include "runner_internal.h"
#include "support/audio_audit_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* A small independently assembled bootstrap for recomp projects whose host
 * performs the cartridge-to-ARAM upload. It publishes the conventional AABB
 * ready marker and idles until the host installs the uploaded entry point. */
static const uint8_t k_recomp_boot_rom[0x40] = {
    0xe8, 0xaa,       /* MOV A,#$AA */
    0xc4, 0xf4,       /* MOV $F4,A  */
    0xe8, 0xbb,       /* MOV A,#$BB */
    0xc4, 0xf5,       /* MOV $F5,A  */
    0x2f, 0xfe,       /* BRA $FFC8  */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff,
};

void (*g_apu_spc_port_write_trace_hook)(Apu *, uint8_t, uint8_t);
void (*g_apu_spc_dsp_write_hook)(Apu *, uint8_t, uint8_t);
bool (*g_apu_spc_dsp_write_filter_hook)(Apu *, uint8_t, uint8_t *);
void (*g_apu_extra_saveload_hook)(Apu *, SaveLoadInfo *);

uint64_t g_spc_write_counts[0x100];
uint64_t g_spc_pc_histogram[0x10000];
int g_spc_pc_max_seen;
uint64_t g_spc_outport_value_counts[4 * 256];
typedef struct SpcWriteRec { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[32];
int g_spc_recent_outport_idx;

static Apu *s_active_apu;

_Static_assert((APU_PORT_QUEUE_LEN & (APU_PORT_QUEUE_LEN - 1u)) == 0u,
               "APU port queue length must be a power of two");

Apu *apu_init(void) {
    const char *diagnostics;
    const char *audit_prefix;
    Apu *apu = (Apu *)calloc(1u, sizeof(*apu));
    if (apu == NULL) return NULL;
    diagnostics = getenv("SNESRECOMP_SPC_DIAGNOSTICS");
    apu->diagnosticCountersEnabled = diagnostics != NULL &&
        diagnostics[0] != '\0' && diagnostics[0] != '0';
    audit_prefix = getenv("SNESRECOMP_APU_AUDIT_PREFIX");
    apu->auditWritesEnabled = audit_prefix != NULL &&
        audit_prefix[0] != '\0';
    apu->spc = spc_init(apu);
    apu->dsp = dsp_init(apu->ram);
    if (apu->spc == NULL || apu->dsp == NULL) {
        apu_free(apu);
        return NULL;
    }
    apu->dsp->apu = apu;
    s_active_apu = apu;
    apu_clearPortQueue(apu);
    return apu;
}

void apu_free(Apu *apu) {
    if (apu == NULL) return;
    if (s_active_apu == apu) s_active_apu = NULL;
    spc_free(apu->spc);
    dsp_free(apu->dsp);
    free(apu);
}

void apu_clearPortQueue(Apu *apu) {
    if (apu == NULL) return;
    apu->portQHead = 0u;
    apu->portQTail = 0u;
    memset(apu->portQueue, 0, sizeof(apu->portQueue));
    apu->portClock = 0u;
    apu->portClockNs = 0u;
    memset(apu->portLastTarget, 0, sizeof(apu->portLastTarget));
    memset(apu->portLastVal, 0, sizeof(apu->portLastVal));
    memset(apu->portLastValid, 0, sizeof(apu->portLastValid));
}

void apu_reset(Apu *apu) {
    if (apu == NULL) return;
    apu->romReadable = true;
    spc_reset(apu->spc);
    dsp_reset(apu->dsp);
    memset(apu->ram, 0, sizeof(apu->ram));
    memset(apu->ramWritten, 0, sizeof(apu->ramWritten));
    apu->dspAdr = 0u;
    apu->cycles = 0u;
    apu->sampleClock = 0u;
    apu->cycleClock = 0u;
    apu->timelineTargetCycles = 0u;
    memset(apu->inPorts, 0, sizeof(apu->inPorts));
    memset(apu->outPorts, 0, sizeof(apu->outPorts));
    memset(apu->timer, 0, sizeof(apu->timer));
    apu->cpuCyclesLeft = 7u;
    apu->dspSlot = 0u;
    memset(apu->pad, 0, sizeof(apu->pad));
    apu_clearPortQueue(apu);
}

void apu_markRamWritten(Apu *apu, uint16_t address, size_t count) {
    size_t index;
    if (apu == NULL || !apu->auditWritesEnabled) return;
    for (index = 0u; index < count; ++index) {
        const uint16_t current = (uint16_t)(address + index);
        apu->ramWritten[current >> 3] |=
            (uint8_t)(1u << (current & 7u));
    }
}

static void apply_port_write(Apu *apu, const ApuPortWrite *write) {
    const uint8_t port = (uint8_t)(write->port & 3u);
    apu->inPorts[port] = write->val;
    audio_trace_on_cpu_port_apply(port, write->val);
    if (sr_runner_audio_trace_enabled(SR_AUDIO_TRACE_MASK_APU_PORT_APPLY))
        sr_runner_emit_audio_trace(
            apu, SR_AUDIO_TRACE_APU_PORT_APPLY, 0u, port, 0u,
            write->val, apu->cycleClock, 0u, 0u, NULL);
}

void apu_schedulePortWrite(Apu *apu, uint8_t port, uint8_t value,
                           uint64_t target_sample) {
    if (apu == NULL) return;
    if (apu->portQTail - apu->portQHead >= APU_PORT_QUEUE_LEN) {
        apply_port_write(apu, &apu->portQueue[
            apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)]);
        ++apu->portQHead;
    }
    ApuPortWrite *write = &apu->portQueue[
        apu->portQTail & (APU_PORT_QUEUE_LEN - 1u)];
    write->target_sample = target_sample;
    write->port = (uint8_t)(port & 3u);
    write->val = value;
    ++apu->portQTail;
}

static void drain_port_queue(Apu *apu) {
    while (apu->portQHead != apu->portQTail) {
        ApuPortWrite *write = &apu->portQueue[
            apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
        if (write->target_sample > apu->sampleClock) break;
        apply_port_write(apu, write);
        ++apu->portQHead;
    }
}

void apu_saveload(Apu *apu, SaveLoadInfo *info) {
    if (apu == NULL || info == NULL || info->func == NULL) return;
    if (!info->portable) {
        info->func(info, apu->ram,
                   offsetof(Apu, pad) + sizeof(apu->pad) -
                       offsetof(Apu, ram));
        dsp_saveload(apu->dsp, info);
        spc_saveload(apu->spc, info);
        info->func(info, apu->portQueue, sizeof(apu->portQueue));
        info->func(info, &apu->portQHead, sizeof(apu->portQHead));
        info->func(info, &apu->portQTail, sizeof(apu->portQTail));
        info->func(info, &apu->sampleClock, sizeof(apu->sampleClock));
        info->func(info, &apu->cycleClock, sizeof(apu->cycleClock));
        info->func(info, &apu->timelineTargetCycles,
                   sizeof(apu->timelineTargetCycles));
        info->func(info, &apu->portClock, sizeof(apu->portClock));
        info->func(info, &apu->portClockNs, sizeof(apu->portClockNs));
        info->func(info, apu->portLastTarget, sizeof(apu->portLastTarget));
        info->func(info, apu->portLastVal, sizeof(apu->portLastVal));
        info->func(info, apu->portLastValid, sizeof(apu->portLastValid));
        if (g_apu_extra_saveload_hook != NULL)
            g_apu_extra_saveload_hook(apu, info);
        if (!info->saving && !info->failed && apu->auditWritesEnabled)
            memset(apu->ramWritten, 0xff, sizeof(apu->ramWritten));
        return;
    }
    saveload_bytes(info, apu->ram, sizeof(apu->ram));
    saveload_bool(info, &apu->romReadable);
    saveload_u8(info, &apu->dspAdr);
    saveload_u32(info, &apu->cycles);
    saveload_bytes(info, apu->inPorts, sizeof(apu->inPorts));
    saveload_bytes(info, apu->outPorts, sizeof(apu->outPorts));
    for (unsigned index = 0; index < 3u; ++index) {
        Timer *timer = &apu->timer[index];
        saveload_u8(info, &timer->cycles);
        saveload_u8(info, &timer->divider);
        saveload_u8(info, &timer->target);
        saveload_u8(info, &timer->counter);
        saveload_bool(info, &timer->enabled);
    }
    saveload_u8(info, &apu->cpuCyclesLeft);
    saveload_u8(info, &apu->dspSlot);
    saveload_bytes(info, apu->pad, sizeof(apu->pad));
    dsp_saveload(apu->dsp, info);
    spc_saveload(apu->spc, info);
    for (unsigned index = 0; index < APU_PORT_QUEUE_LEN; ++index) {
        saveload_u64(info, &apu->portQueue[index].target_sample);
        saveload_u8(info, &apu->portQueue[index].port);
        saveload_u8(info, &apu->portQueue[index].val);
    }
    saveload_u32(info, &apu->portQHead);
    saveload_u32(info, &apu->portQTail);
    saveload_u64(info, &apu->sampleClock);
    saveload_u64(info, &apu->cycleClock);
    saveload_u64(info, &apu->timelineTargetCycles);
    saveload_u64(info, &apu->portClock);
    saveload_u64(info, &apu->portClockNs);
    for (unsigned index = 0; index < 4u; ++index)
        saveload_u64(info, &apu->portLastTarget[index]);
    saveload_bytes(info, apu->portLastVal, sizeof(apu->portLastVal));
    saveload_bytes(info, apu->portLastValid, sizeof(apu->portLastValid));
    if (g_apu_extra_saveload_hook != NULL) g_apu_extra_saveload_hook(apu, info);
    if (!info->saving && !info->failed && apu->auditWritesEnabled)
        memset(apu->ramWritten, 0xff, sizeof(apu->ramWritten));
}

uint64_t snes_apu_cycle_count(void) {
    return apu_cycle_count(s_active_apu);
}

uint64_t apu_cycle_count(const Apu *apu) {
    return apu != NULL ? apu->cycleClock : 0u;
}

void apu_cycle(Apu *apu) {
    if (apu == NULL) return;
    ++apu->cycleClock;
    if (apu->cpuCyclesLeft == 0u) {
        unsigned guard = 0u;
        do {
            const uint16_t pc = apu->spc->pc;
            if (apu->diagnosticCountersEnabled) {
                ++g_spc_pc_histogram[pc];
                if ((int)pc > g_spc_pc_max_seen) g_spc_pc_max_seen = pc;
            }
            apu->cpuCyclesLeft = (uint8_t)spc_runOpcode(apu->spc);
        } while (apu->cpuCyclesLeft == 0u && !apu->spc->stopped &&
                 ++guard < 65536u);
        if (apu->cpuCyclesLeft == 0u) apu->cpuCyclesLeft = 1u;
    }
    --apu->cpuCyclesLeft;

    if (apu->dspSlot == 0u)
        drain_port_queue(apu);
    dsp_clock(apu->dsp);
    apu->dspSlot = (uint8_t)((apu->dspSlot + 1u) & 0x1fu);
    if (apu->dspSlot == 0u)
        ++apu->sampleClock;

    for (unsigned index = 0; index < 3u; ++index) {
        Timer *timer = &apu->timer[index];
        if (timer->cycles == 0u) {
            timer->cycles = index == 2u ? 16u : 128u;
            if (timer->enabled) {
                ++timer->divider;
                if (timer->divider == timer->target) {
                    timer->divider = 0u;
                    timer->counter = (uint8_t)((timer->counter + 1u) & 0x0fu);
                    if (index == 0u) ++g_apu_timer0_total_ticks;
                }
            }
        }
        --timer->cycles;
    }
    ++apu->cycles;
}

uint8_t apu_cpuRead(Apu *apu, uint16_t address) {
    if (apu == NULL) return 0u;
    switch (address) {
        case 0xf0u:
        case 0xf1u:
        case 0xfau:
        case 0xfbu:
        case 0xfcu: return 0u;
        case 0xf2u: return apu->dspAdr;
        case 0xf3u: return dsp_read(apu->dsp, (uint8_t)(apu->dspAdr & 0x7fu));
        case 0xf4u:
        case 0xf5u:
        case 0xf6u:
        case 0xf7u: {
            const uint8_t port = (uint8_t)(address - 0xf4u);
            const uint8_t value = apu->inPorts[port];
            audio_trace_on_spc_port_read(port, value);
            if (sr_runner_audio_trace_enabled(
                    SR_AUDIO_TRACE_MASK_SPC_PORT_READ))
                sr_runner_emit_audio_trace(
                    apu, SR_AUDIO_TRACE_SPC_PORT_READ, 0u, port, 0u,
                    value, apu->cycleClock, 0u, 0u, NULL);
            return value;
        }
        case 0xf8u:
        case 0xf9u: return apu->inPorts[address - 0xf4u];
        case 0xfdu:
        case 0xfeu:
        case 0xffu: {
            Timer *timer = &apu->timer[address - 0xfdu];
            const uint8_t value = timer->counter;
            timer->counter = 0u;
            return value;
        }
        default: break;
    }
    if (apu->romReadable && address >= 0xffc0u) {
        return k_recomp_boot_rom[address - 0xffc0u];
    }
    return apu->ram[address];
}

void apu_cpuWrite(Apu *apu, uint16_t address, uint8_t value) {
    if (apu == NULL) return;
    if (apu->diagnosticCountersEnabled && address < 0x100u)
        ++g_spc_write_counts[address];
    if (apu->diagnosticCountersEnabled &&
        address >= 0xf4u && address <= 0xf7u) {
        const unsigned port = address - 0xf4u;
        ++g_spc_outport_value_counts[port * 256u + value];
        SpcWriteRec *record = &g_spc_recent_outport_writes[
            (unsigned)g_spc_recent_outport_idx++ & 31u];
        record->adr = (uint8_t)address;
        record->val = value;
    }
    switch (address) {
        case 0xf0u: break;
        case 0xf1u:
            for (unsigned index = 0; index < 3u; ++index) {
                Timer *timer = &apu->timer[index];
                const bool enable = (value & (uint8_t)(1u << index)) != 0u;
                if (!timer->enabled && enable) {
                    timer->divider = 0u;
                    timer->counter = 0u;
                }
                timer->enabled = enable;
            }
            if ((value & 0x10u) != 0u) {
                apu->inPorts[0] = 0u;
                apu->inPorts[1] = 0u;
            }
            if ((value & 0x20u) != 0u) {
                apu->inPorts[2] = 0u;
                apu->inPorts[3] = 0u;
            }
            apu->romReadable = (value & 0x80u) != 0u;
            break;
        case 0xf2u: apu->dspAdr = value; break;
        case 0xf3u:
            if (apu->dspAdr < 0x80u) {
                const uint8_t original = value;
                if (g_apu_spc_dsp_write_hook != NULL)
                    g_apu_spc_dsp_write_hook(apu, apu->dspAdr, original);
                if (sr_runner_audio_trace_enabled(
                        SR_AUDIO_TRACE_MASK_DSP_WRITE))
                    sr_runner_emit_audio_trace(
                        apu, SR_AUDIO_TRACE_DSP_WRITE, 0u, 0u,
                        apu->dspAdr, original, apu->cycleClock,
                        0u, 0u, NULL);
                if (g_apu_spc_dsp_write_filter_hook == NULL ||
                    g_apu_spc_dsp_write_filter_hook(apu, apu->dspAdr, &value)) {
                    dsp_write(apu->dsp, apu->dspAdr, value);
                }
            }
            break;
        case 0xf4u:
        case 0xf5u:
        case 0xf6u:
        case 0xf7u: {
            const uint8_t port = (uint8_t)(address - 0xf4u);
            apu->outPorts[port] = value;
            if (g_apu_spc_port_write_trace_hook != NULL)
                g_apu_spc_port_write_trace_hook(apu, port, value);
            break;
        }
        case 0xf8u:
        case 0xf9u: apu->inPorts[address - 0xf4u] = value; break;
        case 0xfau:
        case 0xfbu:
        case 0xfcu: apu->timer[address - 0xfau].target = value; break;
        default: break;
    }
    apu->ram[address] = value;
    if (apu->auditWritesEnabled) apu_markRamWritten(apu, address, 1u);
}
