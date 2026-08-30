#include "snesrecomp/host/audio_trace.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/saveload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int snes_frame_counter;
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canon_l, int canon_r,
                        int *out_l, int *out_r) {
  (void)shadow; (void)dsp;
  *out_l = canon_l; *out_r = canon_r;
}
void audio_trace_on_sample(int16_t l, int16_t r, int dropped,
                           uint32_t ring_fill) {
  (void)l; (void)r; (void)dropped; (void)ring_fill;
}
void audio_trace_on_reg_write(uint8_t addr, uint8_t val) {
  (void)addr; (void)val;
}
void audio_trace_on_consume(uint64_t read_idx, uint32_t count,
                            uint32_t avail_after) {
  (void)read_idx; (void)count; (void)avail_after;
}

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void InstallBrr(uint8_t *ram) {
  static const uint8_t payload[8] = {
    0x01, 0x23, 0x45, 0x67, 0x01, 0x23, 0x45, 0x67
  };
  ram[0x0208] = 0x00; ram[0x0209] = 0x03;
  ram[0x020a] = 0x00; ram[0x020b] = 0x03;
  ram[0x0300] = 0xc3;
  memcpy(ram + 0x0301, payload, sizeof(payload));
}

static Dsp *NewDsp(uint8_t *ram) {
  memset(ram, 0, 0x10000);
  InstallBrr(ram);
  Dsp *dsp = dsp_init(ram);
  CHECK(dsp != NULL);
  if (!dsp) return NULL;
  dsp_reset(dsp);
  dsp_write(dsp, 0x0c, 127);
  dsp_write(dsp, 0x1c, 127);
  dsp_write(dsp, 0x5d, 2);
  dsp_write(dsp, 0x6c, 0x20);
  return dsp;
}

static void WriteVoice(Dsp *dsp, int voice, uint8_t reg, uint8_t value) {
  if (voice < 8)
    dsp_write(dsp, (uint8_t)(voice * 0x10 + reg), value);
  else
    dsp_writeVirtualVoiceRegister(
      dsp, voice, (uint8_t)((voice & 7) * 0x10 + reg), value);
}

static void ControlVoice(Dsp *dsp, int voice, uint8_t reg, bool enabled) {
  if (voice < 8) {
    const uint8_t bit = (uint8_t)(1u << voice);
    dsp_writeHardwareVoiceMask(dsp, reg, enabled ? bit : 0, bit);
  } else {
    dsp_writeVirtualVoiceControl(dsp, voice, reg, enabled);
  }
}

static void ConfigureVoice(Dsp *dsp, int voice, DspVoiceBus bus,
                           bool echo) {
  WriteVoice(dsp, voice, 0, 83);
  WriteVoice(dsp, voice, 1, (uint8_t)-37);
  WriteVoice(dsp, voice, 2, 0);
  WriteVoice(dsp, voice, 3, 0x10);
  WriteVoice(dsp, voice, 4, 2);
  WriteVoice(dsp, voice, 5, 0);
  WriteVoice(dsp, voice, 6, 0);
  WriteVoice(dsp, voice, 7, 0x7f);
  ControlVoice(dsp, voice, 0x4d, echo);
  ControlVoice(dsp, voice, 0x4c, true);
  dsp_setVoiceBus(dsp, voice, bus);
}

static void Run(Dsp *dsp, int samples) {
  while (samples-- > 0) dsp_cycle(dsp);
}

static void TestUnityAndIndependentGains(void) {
  uint8_t ref_ram[0x10000], tagged_ram[0x10000], muted_ram[0x10000];
  Dsp *reference = NewDsp(ref_ram);
  Dsp *tagged = NewDsp(tagged_ram);
  Dsp *muted = NewDsp(muted_ram);
  if (!reference || !tagged || !muted) goto done;
  ConfigureVoice(reference, 0, kDspVoiceBus_Unclassified, false);
  ConfigureVoice(tagged, 0, kDspVoiceBus_Sfx, false);
  ConfigureVoice(muted, 0, kDspVoiceBus_Music, false);

  dsp_setMusicBusMuted(false);
  dsp_setBusGains(100, 100);
  Run(reference, 16);
  Run(tagged, 16);
  CHECK(memcmp(reference->sampleBuffer, tagged->sampleBuffer,
               16 * 2 * sizeof(int16_t)) == 0);
  CHECK(reference->sampleBuffer[10 * 2] != 0);

  dsp_setBusGains(0, 100);
  Run(muted, 16);
  CHECK(muted->sampleBuffer[10 * 2] == 0);
done:
  dsp_free(reference); dsp_free(tagged); dsp_free(muted);
  dsp_setBusGains(100, 100);
}

static void TestExtendedControlAndPcmParity(void) {
  uint8_t hardware_ram[0x10000], virtual_ram[0x10000];
  Dsp *hardware = NewDsp(hardware_ram);
  Dsp *virtual_dsp = NewDsp(virtual_ram);
  if (!hardware || !virtual_dsp) goto done;
  dsp_setExtendedVoicesEnabled(true);
  /* Channel 8 is lane 0 of the first parallel bank, so channel 0 is its
   * cycle-for-cycle hardware counterpart. */
  ConfigureVoice(hardware, 0, kDspVoiceBus_Sfx, false);
  ConfigureVoice(virtual_dsp, 8, kDspVoiceBus_Sfx, false);
  Run(hardware, 256);
  Run(virtual_dsp, 256);
  CHECK(memcmp(hardware->sampleBuffer, virtual_dsp->sampleBuffer,
               256 * 2 * sizeof(int16_t)) == 0);
  CHECK(hardware->sampleBuffer[128 * 2] != 0);

  dsp_writeHardwareVoiceMask(virtual_dsp, 0x4c, 0x40, 0x40);
  dsp_writeVirtualVoiceControl(virtual_dsp, 8, 0x4c, true);
  dsp_writeHardwareVoiceMask(virtual_dsp, 0x4c, 0x40, 0xbf);
  CHECK(virtual_dsp->channel[6].keyOn);
  CHECK(virtual_dsp->channel[8].keyOn);
  CHECK((dsp_read(virtual_dsp, 0x4c) & 0x40) != 0);
done:
  dsp_free(hardware); dsp_free(virtual_dsp);
  dsp_setExtendedVoicesEnabled(false);
}

static bool EchoRegionNonzero(const uint8_t *ram) {
  for (unsigned i = 0x4000; i < 0x4200; ++i)
    if (ram[i] != 0) return true;
  return false;
}

static void TestVirtualEchoUsesSharedUnitAndBusGain(void) {
  uint8_t live_ram[0x10000], muted_ram[0x10000];
  Dsp *live = NewDsp(live_ram);
  Dsp *muted = NewDsp(muted_ram);
  if (!live || !muted) goto done;
  dsp_setExtendedVoicesEnabled(true);
  dsp_write(live, 0x6c, 0x00);
  dsp_write(muted, 0x6c, 0x00);
  dsp_write(live, 0x6d, 0x40);
  dsp_write(muted, 0x6d, 0x40);
  dsp_write(live, 0x7d, 1);
  dsp_write(muted, 0x7d, 1);
  ConfigureVoice(live, 8, kDspVoiceBus_Sfx, true);
  ConfigureVoice(muted, 8, kDspVoiceBus_Sfx, true);

  dsp_setBusGains(100, 100);
  Run(live, 32);
  CHECK(EchoRegionNonzero(live_ram));
  dsp_setBusGains(100, 0);
  Run(muted, 32);
  CHECK(!EchoRegionNonzero(muted_ram));
done:
  dsp_free(live); dsp_free(muted);
  dsp_setBusGains(100, 100);
  dsp_setExtendedVoicesEnabled(false);
}

typedef struct MemoryState {
  SaveLoadInfo sli;
  uint8_t bytes[131072];
  size_t offset;
  bool loading;
} MemoryState;

static void TransferMemoryState(SaveLoadInfo *sli, void *data, size_t size) {
  MemoryState *state = (MemoryState *)sli;
  CHECK(state->offset + size <= sizeof(state->bytes));
  if (state->offset + size > sizeof(state->bytes)) {
    sli->failed = true;
    return;
  }
  if (state->loading)
    memcpy(data, state->bytes + state->offset, size);
  else
    memcpy(state->bytes + state->offset, data, size);
  state->offset += size;
}

static void TestExtendedVoiceStateIsSerialized(void) {
  uint8_t ram[0x10000];
  int16_t expected[32], actual[32];
  Dsp *dsp = NewDsp(ram);
  if (!dsp) return;
  dsp_setExtendedVoicesEnabled(true);
  ConfigureVoice(dsp, 8, kDspVoiceBus_Sfx, false);
  Run(dsp, 24);

  MemoryState state;
  memset(&state, 0, sizeof(state));
  state.sli.func = TransferMemoryState;
  state.sli.saving = true;
  state.sli.portable = true;
  const uint32_t cursor = dsp->sampleWrite;
  dsp_saveload(dsp, &state.sli);
  CHECK(!state.sli.failed && state.offset > sizeof(dsp->sampleBuffer));
  Run(dsp, 16);
  memcpy(expected, dsp->sampleBuffer +
         (cursor & (DSP_SAMPLE_RING - 1u)) * 2, sizeof(expected));

  state.offset = 0;
  state.loading = true;
  state.sli.saving = false;
  dsp_saveload(dsp, &state.sli);
  CHECK(!state.sli.failed && dsp->sampleWrite == cursor);
  Run(dsp, 16);
  memcpy(actual, dsp->sampleBuffer +
         (cursor & (DSP_SAMPLE_RING - 1u)) * 2, sizeof(actual));
  CHECK(memcmp(expected, actual, sizeof(actual)) == 0);

  dsp_free(dsp);
  dsp_setExtendedVoicesEnabled(false);
}

static void TestDormantVirtualBankResumesAtNativeParity(void) {
  uint8_t hardware_ram[0x10000], virtual_ram[0x10000];
  Dsp *hardware = NewDsp(hardware_ram);
  Dsp *virtual_dsp = NewDsp(virtual_ram);
  if (!hardware || !virtual_dsp) goto done;
  dsp_setExtendedVoicesEnabled(true);
  ConfigureVoice(hardware, 0, kDspVoiceBus_Sfx, false);
  ConfigureVoice(virtual_dsp, 8, kDspVoiceBus_Sfx, false);
  Run(hardware, 32);
  Run(virtual_dsp, 32);

  dsp_writeHardwareVoiceMask(hardware, 0x5c, 0x01, 0x01);
  dsp_writeVirtualVoiceControl(virtual_dsp, 8, 0x5c, true);
  Run(hardware, 400);
  Run(virtual_dsp, 400);
  CHECK(hardware->sampleBuffer[(hardware->sampleWrite - 1) * 2] == 0);
  CHECK(virtual_dsp->sampleBuffer[(virtual_dsp->sampleWrite - 1) * 2] == 0);

  dsp_writeHardwareVoiceMask(hardware, 0x5c, 0x00, 0x01);
  dsp_writeVirtualVoiceControl(virtual_dsp, 8, 0x5c, false);
  dsp_writeHardwareVoiceMask(hardware, 0x4c, 0x01, 0x01);
  dsp_writeVirtualVoiceControl(virtual_dsp, 8, 0x4c, true);
  {
    const uint32_t hardware_cursor = hardware->sampleWrite;
    const uint32_t virtual_cursor = virtual_dsp->sampleWrite;
    Run(hardware, 24);
    Run(virtual_dsp, 24);
    CHECK(memcmp(hardware->sampleBuffer +
                     (hardware_cursor & (DSP_SAMPLE_RING - 1u)) * 2,
                 virtual_dsp->sampleBuffer +
                     (virtual_cursor & (DSP_SAMPLE_RING - 1u)) * 2,
                 24 * 2 * sizeof(int16_t)) == 0);
    CHECK(virtual_dsp->sampleBuffer[
              ((virtual_cursor + 16) & (DSP_SAMPLE_RING - 1u)) * 2] != 0);
  }
done:
  dsp_free(hardware);
  dsp_free(virtual_dsp);
  dsp_setExtendedVoicesEnabled(false);
}

int main(void) {
  dsp_setExtendedVoicesEnabled(false);
  dsp_setMusicBusMuted(false);
  TestUnityAndIndependentGains();
  TestExtendedControlAndPcmParity();
  TestVirtualEchoUsesSharedUnitAndBusGain();
  TestExtendedVoiceStateIsSerialized();
  TestDormantVirtualBankResumesAtNativeParity();
  if (s_failures) {
    fprintf(stderr, "dsp_bus_mix_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("dsp_bus_mix_test: PASS");
  return 0;
}
