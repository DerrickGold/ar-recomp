#ifndef SNESRECOMP_RECOMPILED_GAME_RUNTIME_H
#define SNESRECOMP_RECOMPILED_GAME_RUNTIME_H

#include "snesrecomp/game/apu_sync.h"
#include "snesrecomp/game/types.h"

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CpuState CpuState;

extern uint8 g_ram[kSnesWramSize];
extern uint8 *g_sram;
extern int g_sram_size;
extern const uint8 *g_rom;
extern int snes_frame_counter;

extern uint64_t g_main_cpu_cycles_estimate;
extern uint64_t g_apu_pace_cycles_estimate;
extern uint64_t g_apu_last_sync_cycles;

void MemCpy(void *destination, const void *source, int size);
bool Unreachable(void);
uint8 *RomPtr(uint32 address);
uint8 *MvnPtr(uint8 bank, uint16 address);
uint8 *IndirPtr_Slow(LongPtr pointer, uint16 offset);

static inline uint8 *RomFixedPtr(uint32 address) { return RomPtr(address); }
static inline LongPtr MAKE_LONG(uint16 address, uint8 bank) {
    LongPtr result = {address, bank};
    return result;
}
static inline uint16 GET_WORD(const uint8 *pointer) {
    uint16 value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}
static inline void SET_WORD(uint8 *pointer, uint16 value) {
    memcpy(pointer, &value, sizeof(value));
}
#define GET_BYTE(pointer) (*(uint8 *)(pointer))

static inline uint8 *IndirPtr(LongPtr pointer, uint16 offset) {
    uint32 address = (((uint32)pointer.bank << 16) | pointer.addr) + offset;
    uint8 bank = (uint8)(address >> 16);
    if (bank == 0x7eu || bank == 0x7fu)
        return &g_ram[address & kSnesWramMask];
    if ((uint16)address < 0x2000u &&
        (bank < 0x40u || (bank >= 0x80u && bank < 0xc0u)))
        return &g_ram[(uint16)address];
    return RomPtr(address & 0xffffffu);
}
static inline void IndirWriteByte(LongPtr pointer, uint16 offset, uint8 value) {
    *IndirPtr(pointer, offset) = value;
}
static inline void IndirWriteWord(LongPtr pointer, uint16 offset, uint16 value) {
    uint8 *destination = IndirPtr(pointer, offset);
    destination[0] = (uint8)value;
    destination[1] = (uint8)(value >> 8);
}

void WriteReg(uint16 reg, uint8 value);
void WriteRegWord(uint16 reg, uint16 value);
uint8 ReadReg(uint16 reg);
uint16 ReadRegWord(uint16 reg);

void RtlReset(int mode);
bool RtlRunFrame(uint32 inputs);
void RtlSetAudioOutputRate(int hz);
int RtlGetAudioOutputRate(void);
void RtlRenderAudio(int16 *audio_buffer, int samples, int channels);
#define RTL_APU_TIMELINE_FRAMES_PER_TICK UINT32_C(534)
#define RTL_APU_TIMELINE_CYCLES_PER_TICK UINT32_C(17088)
/* Advance the serialized 60 Hz APU target by one game tick. One APU cycle is
 * one S-DSP slot, so 17,088 cycles complete 534 native stereo frames. The
 * operation executes only cycles not already produced by an audio consumer. */
void RtlAdvanceApuTimeline(void);
/* Read the monotonic semantic APU clock under the runner's audio lock. This
 * is an observation only; loading or resetting emulated state may move the
 * value backwards between calls. Audio-trace callbacks must use the event's
 * cycle_count instead of re-entering this service. */
uint64_t RtlApuCycleCount(void);
void RtlSetApuCatchupSuppressed(bool suppressed);
bool RtlUploadSpcImageFromDp(CpuState *cpu);
bool RtlHandleSpcUpload(CpuState *cpu);

enum { kSaveLoad_Save = 1, kSaveLoad_Load = 2 };
void RtlSaveLoad(int command, int slot);
void RtlSaveSnapshot(const char *filename);
bool RtlLoadSnapshot(const char *filename);
void RtlMigrateLegacySram(const char *legacy_title);
void RtlReadSram(void);
void RtlWriteSram(void);

/** Delta counters since RtlApuProfileReset. Total cycles come from the
 * serialized semantic APU clock. The four attributed cycle categories plus
 * unattributed equal total unless RTL_APU_PROFILE_INCONSISTENT is set. */
typedef struct RtlApuProfile {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t lock_wait_ns;
    uint64_t port_sync_ns;
    uint64_t apu_cycles_total;
    uint64_t apu_cycles_audio_demand;
    uint64_t apu_cycles_port_sync;
    uint64_t apu_cycles_upload_control;
    uint64_t apu_cycles_timeline;
    uint64_t apu_cycles_unattributed;
    uint64_t hook_ns;
    uint64_t upload_ns;
    uint64_t scheduled_latency_max;
    uint64_t audio_wait_max_ns;
    uint32_t port_sync_calls;
    uint32_t port_reads;
    uint32_t port_writes;
    uint32_t reserved32;
    const char *last_port_function;
} RtlApuProfile;

#define RTL_APU_PROFILE_INCONSISTENT UINT32_C(0x00000001)

#define RTL_APU_PROFILE_V2_SIZE                                        \
    ((uint32_t)(offsetof(RtlApuProfile, last_port_function) +           \
                sizeof(((RtlApuProfile *)0)->last_port_function)))

bool RtlApuProfileIsEnabled(void);
void RtlApuProfileReset(void);
void RtlApuProfileRead(RtlApuProfile *out_profile);
void RtlApuProfileRecordHostWait(uint64_t wait_ns, bool lock_wait);
uint64_t RtlApuProfileTakeAudioWaitMax(void);

enum {
    kJoypadL_A = 0x80, kJoypadL_X = 0x40,
    kJoypadL_L = 0x20, kJoypadL_R = 0x10,
    kJoypadH_B = 0x80, kJoypadH_Y = 0x40,
    kJoypadH_Select = 0x20, kJoypadH_Start = 0x10,
    kJoypadH_Up = 0x08, kJoypadH_Down = 0x04,
    kJoypadH_Left = 0x02, kJoypadH_Right = 0x01,
    kJoypadH_AnyDir = 0x0f
};

#ifdef __cplusplus
}
#endif

#endif
