#ifndef SNESRECOMP_DSP_ACCURACY_BRIDGE_H
#define SNESRECOMP_DSP_ACCURACY_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct SnesSemanticWriter SnesSemanticWriter;
typedef struct SrDspAccuracy SrDspAccuracy;

typedef struct SrDspAccuracyFrame {
    int16_t left;
    int16_t right;
    bool delivered;
    uint8_t active_bank_mask;
} SrDspAccuracyFrame;

typedef struct SrDspAccuracyVoice {
    uint16_t pitch_counter;
    uint16_t envelope;
    uint16_t brr_address;
    uint8_t source_number;
    uint8_t phase;
    uint8_t key_on_delay;
    int16_t amplitude;
} SrDspAccuracyVoice;

SrDspAccuracy *sr_dsp_accuracy_create(void);
void sr_dsp_accuracy_destroy(SrDspAccuracy *accuracy);
void sr_dsp_accuracy_reset(SrDspAccuracy *accuracy);
uint8_t sr_dsp_accuracy_read(const SrDspAccuracy *accuracy, uint8_t address);
void sr_dsp_accuracy_write(SrDspAccuracy *accuracy, uint8_t address,
                           uint8_t value);
void sr_dsp_accuracy_write_hardware_mask(SrDspAccuracy *accuracy,
                                         uint8_t address, uint8_t value,
                                         uint8_t update_mask);
void sr_dsp_accuracy_write_virtual_register(SrDspAccuracy *accuracy,
                                            int channel,
                                            uint8_t source_address,
                                            uint8_t value);
void sr_dsp_accuracy_write_virtual_control(SrDspAccuracy *accuracy,
                                           int channel,
                                           uint8_t global_address,
                                           bool enabled);
SrDspAccuracyFrame sr_dsp_accuracy_clock(
    SrDspAccuracy *accuracy, uint8_t *apu_ram, bool extended_enabled,
    bool mix_controls_unity, const uint8_t voice_gain_percent[40],
    const uint8_t voice_muted[40]);
void sr_dsp_accuracy_copy_registers(const SrDspAccuracy *accuracy,
                                    uint8_t registers[128]);
void sr_dsp_accuracy_get_voice(const SrDspAccuracy *accuracy, int channel,
                               SrDspAccuracyVoice *voice);
uint8_t sr_dsp_accuracy_slot(const SrDspAccuracy *accuracy);
void sr_dsp_accuracy_saveload(SrDspAccuracy *accuracy, SaveLoadInfo *info);
void sr_dsp_accuracy_write_semantic_v2(
    const SrDspAccuracy *accuracy, SnesSemanticWriter *writer);

void sr_dsp_accuracy_decode_brr(const uint8_t block[9], int16_t old,
                                int16_t older, int16_t samples[16]);
int16_t sr_dsp_accuracy_gauss(const int16_t window[4], uint8_t index);

#ifdef __cplusplus
}
#endif

#endif
