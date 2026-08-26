#ifndef SNESRECOMP_NEXT_COMMON_RTL_H
#define SNESRECOMP_NEXT_COMMON_RTL_H

#include "types.h"
#include "snes/dma.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Apu Apu;
typedef struct CpuState CpuState;
typedef struct Dma Dma;
typedef struct Ppu Ppu;

typedef struct SimpleHdma {
    const uint8 *table;
    const uint8 *indir_ptr;
    uint8 rep_count;
    uint8 mode;
    uint8 ppu_addr;
    uint8 indir_bank;
} SimpleHdma;

extern uint8 g_ram[kSnesWramSize];
extern uint8 *g_sram;
extern int g_sram_size;
extern const uint8 *g_rom;
extern Ppu *g_ppu;
extern Dma *g_dma;
extern uint8 g_snesrecomp_last_hdmaen;
extern int snes_frame_counter;

extern uint64_t g_main_cpu_cycles_estimate;
extern uint64_t g_apu_pace_cycles_estimate;
extern uint64_t g_apu_last_sync_cycles;

extern void (*g_rtl_inidisp_hook)(uint8 value);
extern void (*g_rtl_apu_port_hook)(uint8 port, uint8 value);
extern void (*g_rtl_apu_port_pace_hook)(uint8 port, uint8 value);
extern void (*g_rtl_apu_port_trace_hook)(uint8 port, uint8 value);
extern void (*g_rtl_spc_upload_hook)(uint32 source);
extern void (*g_rtl_spc_upload_trace_hook)(uint32 source);
extern void (*g_rtl_apu_state_loaded_hook)(Apu *apu);
extern void (*g_rtl_music_mix_hook)(int16 *buffer, int frames);

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
void RtlApuLock(void);
void RtlApuUnlock(void);
void RtlApuWrite(uint16 address, uint8 value);
void rtl_accumulate_apu_catchup(void);
void RtlSetApuCatchupSuppressed(bool suppressed);
bool RtlUploadSpcImageFromDp(CpuState *cpu);
bool RtlHandleSpcUpload(CpuState *cpu);
void ar_uploader_complete_tick(void);

enum { kSaveLoad_Save = 1, kSaveLoad_Load = 2 };
void RtlSaveLoad(int command, int slot);
void RtlSaveSnapshot(const char *filename);
bool RtlLoadSnapshot(const char *filename);
void RtlMigrateLegacySram(const char *legacy_title);
void RtlReadSram(void);
void RtlWriteSram(void);

void SimpleHdma_Init(SimpleHdma *channel, DmaChannel *dma_channel);
void SimpleHdma_DoLine(SimpleHdma *channel);

int ApuProfEnabled(void);
void ApuProfFrameReset(void);
extern uint64_t g_apuprof_lockwait_ns;
extern uint64_t g_apuprof_catchup_ns;
extern uint64_t g_apuprof_catchup_cyc;
extern uint64_t g_apuprof_hook_ns;
extern uint64_t g_apuprof_upload_ns;
extern uint64_t g_apuprof_sched_lat_max;
extern uint64_t g_apuprof_audiowait_max_ns;
extern uint32_t g_apuprof_catchup_calls;
extern uint32_t g_apuprof_port_reads;
extern uint32_t g_apuprof_port_writes;
extern const char *g_apuprof_last_port_func;

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
