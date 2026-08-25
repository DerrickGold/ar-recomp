
#ifndef DSP_H
#define DSP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "saveload.h"

typedef struct Dsp Dsp;

typedef struct Apu Apu;

/* Host-side bus provenance. The emulated S-DSP still owns exactly eight
 * hardware voices; a game-specific observer may label each live voice so
 * presentation controls can scale music and effects independently before
 * their authentic summation. Unclassified voices remain at unity gain. */
typedef enum DspVoiceBus {
  kDspVoiceBus_Unclassified = 0,
  kDspVoiceBus_Music,
  kDspVoiceBus_Sfx,
} DspVoiceBus;

enum {
  kDspHardwareVoiceCount = 8,
  /* The Aitos boss-death burst reaches 22 simultaneous effect lanes. Thirty-
   * two leaves measured headroom while keeping state fixed and deterministic. */
  kDspExtendedVoiceCount = 32,
  kDspMaximumVoiceCount =
      kDspHardwareVoiceCount + kDspExtendedVoiceCount,
};

// Output-sample ring capacity (stereo pairs). Must be a power of two so
// the monotonic write/read counters can index with a mask and survive
// uint32 wraparound. 8192 samples ≈ 256 ms at 32 kHz — far larger than
// any single-frame APU catch-up burst, while typical fill stays near one
// callback's native-time requirement, so playback latency is unchanged. See
// the sampleBuffer
// comment in struct Dsp and the music-tick post-mortem in MMX ISSUES.md.
#define DSP_SAMPLE_RING 8192

typedef struct DspChannel {
  // pitch
  uint16_t pitch;
  uint16_t pitchCounter;
  bool pitchModulation;
  // brr decoding
  int16_t decodeBuffer[19]; // 16 samples per brr-block, +3 for interpolation
  uint8_t srcn;
  uint16_t decodeOffset;
  uint8_t previousFlags; // from last sample
  int16_t old;
  int16_t older;
  bool useNoise;
  // adsr, envelope, gain
  uint16_t adsrRates[4]; // attack, decay, sustain, gain
  uint16_t rateCounter;
  uint8_t adsrState; // 0: attack, 1: decay, 2: sustain, 3: gain, 4: release
  uint16_t sustainLevel;
  bool useGain;
  uint8_t gainMode;
  bool directGain;
  uint16_t gainValue; // for direct gain
  uint16_t gain;
  // keyon/off
  bool keyOn;
  bool keyOff;
  // output
  int16_t sampleOut; // final sample, to be multiplied by channel volume
  int8_t volumeL;
  int8_t volumeR;
  bool echoEnable;
} DspChannel;

struct Dsp {
  uint8_t *apu_ram;
  // MP2K-style verified-enhancement shadow mixer (opt-in; default off).
  // Placed BEFORE `ram` so it lies outside the dsp_saveload region
  // (which serializes from `ram` to end) — savestate layout is unchanged.
  // void* to keep dsp.h free of the shadow header; dsp.c owns the type.
  void *shadow;
  // Presentation-only voice provenance. Kept before `ram`, outside the frozen
  // dsp_saveload region; the game observer reconstructs it after a state load.
  uint8_t voiceBus[kDspMaximumVoiceCount];
  // mirror ram
  uint8_t ram[0x80];
  // Eight hardware channels plus optional game-owned virtual channels.
  // Authentic mode cycles only the first eight. The extra state is serialized
  // with the rest of the DSP so an extended-mode save resumes sample-exactly.
  DspChannel channel[kDspMaximumVoiceCount];
  // overarching
  uint16_t dirPage;
  bool evenCycle;
  bool mute;
  bool reset;
  int8_t masterVolumeL;
  int8_t masterVolumeR;
  // noise
  int16_t noiseSample;
  uint16_t noiseRate;
  uint16_t noiseCounter;
  // echo
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
  // Output-sample ring. Two producers feed it (serialized by RtlApuLock):
  // the audio thread (RtlRenderAudio) and the CPU thread (snes_catchupApu,
  // on APU-port access). The old fixed 534-sample buffer dropped every
  // sample produced past 534, so a catch-up burst between audio callbacks
  // lost samples → music-rate ticks + timing jitter. The ring buffers the
  // burst instead; the audio thread consumes the oldest 534 per block at
  // the steady output rate, smoothing bursty production. (1 native block
  // = 534 samples @ ~32 kHz; *2 for stereo.)
  int16_t sampleBuffer[DSP_SAMPLE_RING * 2];
  uint32_t sampleWrite; // total samples produced (monotonic; index = & mask)
  uint32_t sampleRead;  // total samples consumed (monotonic; index = & mask)
};


Dsp *dsp_init(uint8_t *ram);
void dsp_free(Dsp* dsp);
void dsp_reset(Dsp* dsp);
void dsp_cycle(Dsp* dsp);
uint8_t dsp_read(Dsp* dsp, uint8_t adr);
void dsp_write(Dsp* dsp, uint8_t adr, uint8_t val);
void dsp_getSamples(Dsp* dsp, int16_t* sampleData, int samplesPerFrame);
/* Continuously resample the native DSP FIFO. `native_step` is native samples
 * per output frame and `phase` is retained across calls. The caller must make
 * sure the FIFO covers both the advanced cursor and the second interpolation
 * sample at the final output position. */
void dsp_getSamplesResampled(Dsp* dsp, int16_t* sampleData,
                             int samplesPerFrame, double native_step,
                             double *phase);
void dsp_saveload(Dsp *dsp, SaveLoadInfo *sli);
/* Presentation bus controls. Callers serialize these with dsp_cycle using the
 * APU lock. Gains are percentages in [0,100]; 100/100 is the exact legacy
 * path. Voice labels are outside emulated/save-state state. */
void dsp_setVoiceBus(Dsp *dsp, int ch, DspVoiceBus bus);
DspVoiceBus dsp_getVoiceBus(const Dsp *dsp, int ch);
void dsp_setBusGains(int music_percent, int sfx_percent);
void dsp_getBusGains(int *music_percent, int *sfx_percent);
/* Replacement-stream gate. Classified music voices are muted by provenance;
 * the existing SRCN threshold remains only as an unclassified startup
 * fallback. */
void dsp_setMusicBusMuted(bool muted);

/* Optional game-owned virtual voice extension. The ordinary S-DSP register
 * interface remains eight-voice; a provenance bridge uses the targeted APIs
 * below for logical tracks that have been remapped beyond the hardware mask.
 * Enablement is fixed at boot (the player setting is restart-class). */
void dsp_setExtendedVoicesEnabled(bool enabled);
bool dsp_extendedVoicesEnabled(void);
int dsp_activeVoiceCount(void);
void dsp_writeVirtualVoiceRegister(Dsp *dsp, int ch, uint8_t source_addr,
                                   uint8_t val);
void dsp_writeVirtualVoiceControl(Dsp *dsp, int ch, uint8_t global_addr,
                                  bool enabled);
/* Apply a hardware mask register to only the selected native bits. This lets
 * the extension split an original $40/$80 effect bit from the physical song
 * voice without disturbing the remaining authentic mask update. */
void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t addr, uint8_t val,
                                uint8_t update_mask);

#endif
