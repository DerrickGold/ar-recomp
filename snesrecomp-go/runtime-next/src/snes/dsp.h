#ifndef DSP_H
#define DSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct Dsp Dsp;

typedef enum DspVoiceBus {
    kDspVoiceBus_Unclassified = 0,
    kDspVoiceBus_Music,
    kDspVoiceBus_Sfx
} DspVoiceBus;

enum {
    kDspHardwareVoiceCount = 8,
    kDspExtendedVoiceCount = 32,
    kDspMaximumVoiceCount = kDspHardwareVoiceCount + kDspExtendedVoiceCount,
    DSP_SAMPLE_RING = 8192
};

typedef struct DspChannel {
    uint16_t pitch;
    uint16_t pitchCounter;
    bool pitchModulation;
    int16_t decodeBuffer[19];
    uint8_t srcn;
    uint16_t decodeOffset;
    uint8_t previousFlags;
    int16_t old;
    int16_t older;
    bool useNoise;
    uint16_t adsrRates[4];
    uint16_t rateCounter;
    uint8_t adsrState;
    uint16_t sustainLevel;
    bool useGain;
    uint8_t gainMode;
    bool directGain;
    uint16_t gainValue;
    uint16_t gain;
    bool keyOn;
    bool keyOff;
    int16_t sampleOut;
    int8_t volumeL;
    int8_t volumeR;
    bool echoEnable;
} DspChannel;

struct Dsp {
    uint8_t *apu_ram;
    void *shadow;
    uint8_t voiceBus[kDspMaximumVoiceCount];
    uint8_t ram[0x80];
    DspChannel channel[kDspMaximumVoiceCount];
    uint16_t dirPage;
    bool evenCycle;
    bool mute;
    bool reset;
    int8_t masterVolumeL;
    int8_t masterVolumeR;
    int16_t noiseSample;
    uint16_t noiseRate;
    uint16_t noiseCounter;
    bool echoWrites;
    int8_t echoVolumeL;
    int8_t echoVolumeR;
    int8_t feedbackVolume;
    uint16_t echoBufferAdr;
    uint16_t echoDelay;
    uint16_t echoRemain;
    uint16_t echoBufferIndex;
    uint8_t firBufferIndex;
    int8_t firValues[8];
    int16_t firBufferL[8];
    int16_t firBufferR[8];
    int16_t sampleBuffer[DSP_SAMPLE_RING * 2];
    uint32_t sampleWrite;
    uint32_t sampleRead;
};

Dsp *dsp_init(uint8_t *ram);
void dsp_free(Dsp *dsp);
void dsp_reset(Dsp *dsp);
void dsp_cycle(Dsp *dsp);
uint8_t dsp_read(Dsp *dsp, uint8_t address);
void dsp_write(Dsp *dsp, uint8_t address, uint8_t value);
void dsp_getSamples(Dsp *dsp, int16_t *samples, int sample_count);
void dsp_getSamplesResampled(Dsp *dsp, int16_t *samples, int sample_count,
                             double native_step, double *phase);
void dsp_saveload(Dsp *dsp, SaveLoadInfo *info);
void dsp_setVoiceBus(Dsp *dsp, int channel, DspVoiceBus bus);
DspVoiceBus dsp_getVoiceBus(const Dsp *dsp, int channel);
void dsp_setBusGains(int music_percent, int sfx_percent);
void dsp_getBusGains(int *music_percent, int *sfx_percent);
void dsp_setMusicBusMuted(bool muted);
void dsp_setExtendedVoicesEnabled(bool enabled);
bool dsp_extendedVoicesEnabled(void);
int dsp_activeVoiceCount(void);
void dsp_writeVirtualVoiceRegister(Dsp *dsp, int channel,
                                   uint8_t source_address, uint8_t value);
void dsp_writeVirtualVoiceControl(Dsp *dsp, int channel,
                                  uint8_t global_address, bool enabled);
void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t address, uint8_t value,
                                uint8_t update_mask);

extern int g_dsp_voice_mute_srcn_min;
extern void (*g_dsp_voice_kon_hook)(int channel, uint8_t source_number,
                                    uint16_t brr_address, int volume_left,
                                    int volume_right, uint16_t pitch);

#ifdef __cplusplus
}
#endif

#endif
