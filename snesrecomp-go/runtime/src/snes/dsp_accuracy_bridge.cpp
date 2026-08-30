#include "dsp_accuracy_bridge.h"

#include "snaggletooth/apu/dsp.h"

extern "C" {
#include "dsp.h"
#include "snesrecomp/host/audio_trace.h"
#include "saveload.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>

using snaggletooth::DspState;
using snaggletooth::EnvPhase;
using snaggletooth::SampleWindow;
using snaggletooth::SlotResult;
using snaggletooth::VoiceState;

namespace {

constexpr int kBankCount = 5;
constexpr int kVoicesPerBank = 8;
constexpr int kVoiceCount = kBankCount * kVoicesPerBank;
constexpr std::uint8_t kDspKon = 0x4C;
constexpr std::uint8_t kDspKoff = 0x5C;
constexpr std::uint8_t kDspEndx = 0x7C;

constexpr std::array<std::uint8_t, 4> kVirtualSharedRegisters = {
    0x0C, 0x1C, 0x5D, 0x6C,
};

int clamp16(int value) noexcept {
  return std::clamp(value, -32768, 32767);
}

std::uint8_t voiceRegister(int voice, std::uint8_t reg) noexcept {
  return static_cast<std::uint8_t>(voice * 0x10 + reg);
}

void resetBank(DspState& state) noexcept {
  state = DspState{};
  state.regs[0x6C] = 0xE0;
}

void acknowledgeEndx(DspState& state) noexcept {
  state.regs[kDspEndx] = 0;
  state.preparedEndx = 0;
}

void writeRegister(DspState& state, std::uint8_t address,
                   std::uint8_t value) noexcept {
  address &= 0x7F;
  if (address == kDspEndx) {
    acknowledgeEndx(state);
    return;
  }
  state.regs[address] = value;
  if (address == kDspKon) state.internalKon = value;
}

bool isVirtualSharedRegister(std::uint8_t address) noexcept {
  return std::find(kVirtualSharedRegisters.begin(),
                   kVirtualSharedRegisters.end(), address) !=
         kVirtualSharedRegisters.end();
}

void syncVirtualTimeline(DspState& bank, const DspState& native) noexcept {
  for (const std::uint8_t address : kVirtualSharedRegisters)
    bank.regs[address] = native.regs[address];
  bank.globalCounter = native.globalCounter;
  bank.sampleIndex = native.sampleIndex;
  bank.noiseLevel = native.noiseLevel;
  bank.slotCursor = native.slotCursor;
  bank.primed = native.primed;
  bank.echoWritePending = false;
  bank.echoFirOutLeft = 0;
  bank.echoFirOutRight = 0;
  bank.echoGateLeft = false;
  bank.echoGateRight = false;
}

struct VolumeHold {
  std::array<std::uint8_t, 16> addresses;
  std::array<std::uint8_t, 16> bytes;
  std::uint8_t count;
};

void applyVoiceGain(DspState& state, int bank, int voice, int side,
                    const std::uint8_t gains[kVoiceCount],
                    const std::uint8_t muted[kVoiceCount],
                    VolumeHold& hold) noexcept {
  const int channel = bank * kVoicesPerBank + voice;
  const int percent = muted[channel] != 0 ? 0 : gains[channel];
  if (percent == 100) return;
  const std::uint8_t address =
      voiceRegister(voice, static_cast<std::uint8_t>(side));
  const std::uint8_t raw = state.regs[address];
  hold.addresses[hold.count] = address;
  hold.bytes[hold.count] = raw;
  ++hold.count;
  const int scaled = static_cast<std::int8_t>(raw) * percent / 100;
  state.regs[address] =
      static_cast<std::uint8_t>(static_cast<std::int8_t>(scaled));
}

void applySlotVoiceGains(
    DspState& state, int bank, const std::uint8_t gains[kVoiceCount],
    const std::uint8_t muted[kVoiceCount], VolumeHold& hold) noexcept {
  const int slot = state.slotCursor;
  if (!state.primed) {
    if (slot != 31) return;
    for (int voice = 0; voice < kVoicesPerBank; ++voice) {
      applyVoiceGain(state, bank, voice, 0, gains, muted, hold);
      applyVoiceGain(state, bank, voice, 1, gains, muted, hold);
    }
    return;
  }
  if (slot <= 22) {
    const int phase = slot % 3;
    if (phase == 0)
      applyVoiceGain(state, bank, slot / 3, 0, gains, muted, hold);
    else if (phase == 1)
      applyVoiceGain(state, bank, (slot - 1) / 3, 1, gains, muted, hold);
  }
}

void restoreVoiceGains(DspState& state, const VolumeHold& hold) noexcept {
  for (std::uint8_t index = 0; index < hold.count; ++index)
    state.regs[hold.addresses[index]] = hold.bytes[index];
}

bool bankIsQuiescent(const DspState& state) noexcept {
  if (state.internalKon != 0 || state.mixLeft != 0 || state.mixRight != 0 ||
      state.echoSendLeft != 0 || state.echoSendRight != 0 ||
      state.slotFrame.left != 0 || state.slotFrame.right != 0)
    return false;
  for (int voice = 0; voice < kVoicesPerBank; ++voice) {
    const VoiceState& source = state.voices[voice];
    if (source.envelope != 0 || source.phase != EnvPhase::Release ||
        source.konDelay != 0 || source.restartPending ||
        state.voiceAmplitude[voice] != 0 ||
        state.modulatorAmplitude[voice] != 0)
      return false;
  }
  return true;
}

void saveloadInt(SaveLoadInfo *info, int& value) {
  std::int32_t encoded = static_cast<std::int32_t>(value);
  saveload_i32(info, &encoded);
  if (!info->saving && !info->failed) value = static_cast<int>(encoded);
}

void saveloadWindow(SaveLoadInfo *info, SampleWindow& window) {
  saveload_i16(info, &window.newest);
  saveload_i16(info, &window.old);
  saveload_i16(info, &window.older);
  saveload_i16(info, &window.oldest);
}

void saveloadVoice(SaveLoadInfo *info, VoiceState& voice) {
  saveload_u16(info, &voice.brrAddress);
  saveload_u16(info, &voice.decoderAddress);
  saveload_u16(info, &voice.headerAddress);
  saveload_u8(info, &voice.brrSampleIndex);
  saveload_u16(info, &voice.pitchCounter);
  saveloadWindow(info, voice.window);
  for (auto& sample : voice.pending) saveload_i16(info, &sample);
  saveload_u8(info, &voice.pendingHead);
  saveload_u8(info, &voice.pendingCount);
  for (auto& decode : voice.scheduledDecodes) {
    saveload_u16(info, &decode.address);
    saveload_u8(info, &decode.offset);
  }
  saveload_u8(info, &voice.scheduledDecodeCount);
  saveload_i16(info, &voice.decodePrev1);
  saveload_i16(info, &voice.decodePrev2);
  saveload_u16(info, &voice.envelope);
  std::uint8_t phase = static_cast<std::uint8_t>(voice.phase);
  saveload_u8(info, &phase);
  if (!info->saving && !info->failed)
    voice.phase = static_cast<EnvPhase>(phase & 3u);
  saveload_u8(info, &voice.konDelay);
  saveload_u8(info, &voice.computesSinceKeyOn);
  saveload_u8(info, &voice.computesAtRestart);
  saveload_u8(info, &voice.pitchCaptureHold);
  saveload_bool(info, &voice.restartPending);
  saveload_bool(info, &voice.startupWalks);
  saveload_u16(info, &voice.bentGainRef);
}

void saveloadBank(SaveLoadInfo *info, DspState& state) {
  saveload_bytes(info, state.regs.data(), state.regs.size());
  saveload_u16(info, &state.globalCounter);
  saveload_u32(info, &state.sampleIndex);
  saveload_u8(info, &state.internalKon);
  saveload_bytes(info, state.envxStage.data(), state.envxStage.size());
  saveload_u8(info, &state.preparedEndx);
  saveload_i16(info, &state.noiseLevel);
  saveload_u16(info, &state.echoIndex);
  saveload_u16(info, &state.echoLength);
  saveload_u8(info, &state.echoAppliedEsa);
  for (auto& sample : state.echoFirLeft) saveload_i16(info, &sample);
  for (auto& sample : state.echoFirRight) saveload_i16(info, &sample);
  saveload_u8(info, &state.echoFirPos);
  for (auto& voice : state.voices) saveloadVoice(info, voice);
  saveload_u8(info, &state.slotCursor);
  saveload_bool(info, &state.primed);
  saveload_i32(info, &state.mixLeft);
  saveload_i32(info, &state.mixRight);
  saveload_i32(info, &state.echoSendLeft);
  saveload_i32(info, &state.echoSendRight);
  saveload_i16(info, &state.slotFrame.left);
  saveload_i16(info, &state.slotFrame.right);
  saveload_bool(info, &state.echoWritePending);
  saveload_u16(info, &state.echoWriteEntry);
  saveload_bytes(info, state.echoWriteBytes.data(), state.echoWriteBytes.size());
  for (auto& amplitude : state.voiceAmplitude) saveloadInt(info, amplitude);
  for (auto& amplitude : state.modulatorAmplitude) saveloadInt(info, amplitude);
  saveload_bytes(info, state.preparedOutx.data(), state.preparedOutx.size());
  saveload_bytes(info, state.preparedEnvx.data(), state.preparedEnvx.size());
  for (auto& pitch : state.pitchLatch) saveload_u16(info, &pitch);
  for (auto& pitch : state.pitchLatchOld) saveload_u16(info, &pitch);
  saveload_u8(info, &state.pitchReloadPending);
  saveload_u8(info, &state.pitchReloadAge);
  saveloadInt(info, state.echoFirOutLeft);
  saveloadInt(info, state.echoFirOutRight);
  saveload_bool(info, &state.echoGateLeft);
  saveload_bool(info, &state.echoGateRight);
  saveload_u8(info, &state.echoLatchedEsa);
  saveload_u8(info, &state.echoLatchedEdl);
}

}  // namespace

struct SrDspAccuracy {
  std::array<DspState, kBankCount> banks{};
  std::array<bool, kBankCount> bankActive{};
  bool extendedWasEnabled = false;
};

extern "C" SrDspAccuracy *sr_dsp_accuracy_create(void) {
  void *storage = std::malloc(sizeof(SrDspAccuracy));
  SrDspAccuracy *accuracy = storage == nullptr
      ? nullptr : new (storage) SrDspAccuracy;
  if (accuracy != nullptr) sr_dsp_accuracy_reset(accuracy);
  return accuracy;
}

extern "C" void sr_dsp_accuracy_destroy(SrDspAccuracy *accuracy) {
  if (accuracy == nullptr) return;
  accuracy->~SrDspAccuracy();
  std::free(accuracy);
}

extern "C" void sr_dsp_accuracy_reset(SrDspAccuracy *accuracy) {
  if (accuracy == nullptr) return;
  for (auto& bank : accuracy->banks) resetBank(bank);
  accuracy->bankActive.fill(false);
  accuracy->bankActive[0] = true;
  accuracy->extendedWasEnabled = false;
}

extern "C" std::uint8_t sr_dsp_accuracy_read(
    const SrDspAccuracy *accuracy, std::uint8_t address) {
  return accuracy == nullptr ? 0 : accuracy->banks[0].regs[address & 0x7F];
}

extern "C" void sr_dsp_accuracy_write(SrDspAccuracy *accuracy,
                                        std::uint8_t address,
                                        std::uint8_t value) {
  if (accuracy == nullptr) return;
  address &= 0x7F;
  writeRegister(accuracy->banks[0], address, value);
  if (isVirtualSharedRegister(address)) {
    for (int bank = 1; bank < kBankCount; ++bank)
      accuracy->banks[bank].regs[address] = value;
  }
}

extern "C" void sr_dsp_accuracy_write_hardware_mask(
    SrDspAccuracy *accuracy, std::uint8_t address, std::uint8_t value,
    std::uint8_t update_mask) {
  if (accuracy == nullptr) return;
  DspState& native = accuracy->banks[0];
  address &= 0x7F;
  const std::uint8_t combined = static_cast<std::uint8_t>(
      (native.regs[address] & ~update_mask) | (value & update_mask));
  native.regs[address] = combined;
  if (address == kDspKon) {
    native.internalKon = static_cast<std::uint8_t>(
        (native.internalKon & ~update_mask) | (value & update_mask));
  }
}

extern "C" void sr_dsp_accuracy_write_virtual_register(
    SrDspAccuracy *accuracy, int channel, std::uint8_t source_address,
    std::uint8_t value) {
  if (accuracy == nullptr || channel < 8 || channel >= kVoiceCount) return;
  const int bank = channel / kVoicesPerBank;
  const int voice = channel % kVoicesPerBank;
  const std::uint8_t reg = source_address & 0x0F;
  if (reg > 7) return;
  accuracy->banks[bank].regs[voiceRegister(voice, reg)] = value;
}

extern "C" void sr_dsp_accuracy_write_virtual_control(
    SrDspAccuracy *accuracy, int channel, std::uint8_t global_address,
    bool enabled) {
  if (accuracy == nullptr || channel < 8 || channel >= kVoiceCount) return;
  const int bank = channel / kVoicesPerBank;
  const int voice = channel % kVoicesPerBank;
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << voice);
  DspState& state = accuracy->banks[bank];
  global_address &= 0x7F;
  if (global_address != 0x2D && global_address != 0x3D &&
      global_address != kDspKon && global_address != kDspKoff &&
      global_address != 0x4D) return;
  if (enabled)
    state.regs[global_address] |= bit;
  else
    state.regs[global_address] &= static_cast<std::uint8_t>(~bit);
  if (global_address == kDspKon) {
    if (enabled)
      state.internalKon |= bit;
    else
      state.internalKon &= static_cast<std::uint8_t>(~bit);
  }
  if (global_address == kDspKon && enabled && !accuracy->bankActive[bank]) {
    syncVirtualTimeline(state, accuracy->banks[0]);
    state.internalKon |= bit;
    accuracy->bankActive[bank] = true;
  }
}

extern "C" SrDspAccuracyFrame sr_dsp_accuracy_clock(
    SrDspAccuracy *accuracy, std::uint8_t *apu_ram, bool extended_enabled,
    bool mix_controls_unity,
    const std::uint8_t voice_gain_percent[kVoiceCount],
    const std::uint8_t voice_muted[kVoiceCount]) {
  SrDspAccuracyFrame output{};
  if (accuracy == nullptr || apu_ram == nullptr ||
      voice_gain_percent == nullptr || voice_muted == nullptr) return output;

  DspState& native = accuracy->banks[0];
  const std::uint8_t slot = native.slotCursor;
  std::array<SlotResult, kBankCount> result{};
  std::span<std::uint8_t, 65536> writable(apu_ram, 65536);
  std::span<const std::uint8_t, 65536> readonly(apu_ram, 65536);

  if (extended_enabled && !accuracy->extendedWasEnabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if (accuracy->bankActive[bank])
        syncVirtualTimeline(accuracy->banks[bank], native);
    }
  }
  accuracy->extendedWasEnabled = extended_enabled;

  if (extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if (!accuracy->bankActive[bank]) continue;
      DspState& virtualBank = accuracy->banks[bank];
      VolumeHold hold;
      hold.count = 0;
      if (!mix_controls_unity)
        applySlotVoiceGains(virtualBank, bank, voice_gain_percent,
                            voice_muted, hold);
      result[bank] = snaggletooth::stepDspVoiceCycle(virtualBank, readonly);
      restoreVoiceGains(virtualBank, hold);
    }
  }

  if (slot == 24 && extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if (!accuracy->bankActive[bank]) continue;
      native.echoSendLeft = clamp16(
          native.echoSendLeft + accuracy->banks[bank].echoSendLeft);
      native.echoSendRight = clamp16(
          native.echoSendRight + accuracy->banks[bank].echoSendRight);
    }
  }
  VolumeHold nativeHold;
  nativeHold.count = 0;
  if (!mix_controls_unity)
    applySlotVoiceGains(native, 0, voice_gain_percent, voice_muted,
                        nativeHold);
  result[0] = snaggletooth::stepDspCycle(native, writable);
  restoreVoiceGains(native, nativeHold);

  if (!result[0].delivered) return output;
  int left = result[0].frame.left;
  int right = result[0].frame.right;
  output.active_bank_mask = 1u;
  if (extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if (!accuracy->bankActive[bank]) continue;
      output.active_bank_mask |= static_cast<std::uint8_t>(1u << bank);
      left = clamp16(left + result[bank].frame.left);
      right = clamp16(right + result[bank].frame.right);
      if (bankIsQuiescent(accuracy->banks[bank])) {
        accuracy->banks[bank].preparedEndx = 0;
        accuracy->bankActive[bank] = false;
      }
    }
  }
  output.left = static_cast<std::int16_t>(left);
  output.right = static_cast<std::int16_t>(right);
  output.delivered = true;
  return output;
}

extern "C" void dsp_clock(Dsp *dsp) {
  if (dsp == nullptr || dsp->accuracy == nullptr) return;
  auto *accuracy = static_cast<SrDspAccuracy *>(dsp->accuracy);
  if (accuracy->banks[0].slotCursor == 0) dsp_refreshMixControls(dsp);
  const SrDspAccuracyFrame frame = sr_dsp_accuracy_clock(
      accuracy, dsp->apu_ram, g_dsp_extended_voices_enabled,
      dsp->mixControlsUnity, dsp->voiceGainPercent, dsp->voiceMuted);
  if (!frame.delivered) return;

  DspState& native = accuracy->banks[0];
  for (int voice = 0; voice < kVoicesPerBank; ++voice) {
    dsp->ram[voiceRegister(voice, 8)] =
        native.regs[voiceRegister(voice, 8)];
    dsp->ram[voiceRegister(voice, 9)] =
        native.regs[voiceRegister(voice, 9)];
  }
  dsp->ram[kDspEndx] = native.regs[kDspEndx];
  for (int bank = 0; bank < kBankCount; ++bank) {
    if ((frame.active_bank_mask & (1u << bank)) == 0) continue;
    const DspState& state = accuracy->banks[bank];
    for (int voice = 0; voice < kVoicesPerBank; ++voice) {
      const int channel = bank * kVoicesPerBank + voice;
      const VoiceState& source = state.voices[voice];
      DspChannel& destination = dsp->channel[channel];
      destination.pitchCounter = source.pitchCounter;
      destination.gain = source.envelope;
      destination.sampleOut =
          static_cast<std::int16_t>(state.voiceAmplitude[voice]);
      destination.decodeOffset = source.brrAddress;
      destination.srcn = state.regs[voiceRegister(voice, 4)];
      destination.adsrState = source.phase == EnvPhase::Release
          ? 4u : static_cast<std::uint8_t>(source.phase);
    }
  }
  const std::uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
  const bool dropped = fill >= DSP_SAMPLE_RING;
  if (!dropped) {
    const std::uint32_t index = dsp->sampleWrite & (DSP_SAMPLE_RING - 1u);
    dsp->sampleBuffer[index * 2u] = frame.left;
    dsp->sampleBuffer[index * 2u + 1u] = frame.right;
    ++dsp->sampleWrite;
  }
  audio_trace_on_sample(frame.left, frame.right, dropped ? 1 : 0,
                        dropped ? fill : fill + 1u);
  dsp->evenCycle = !dsp->evenCycle;
}

extern "C" void sr_dsp_accuracy_copy_registers(
    const SrDspAccuracy *accuracy, std::uint8_t registers[128]) {
  if (accuracy == nullptr || registers == nullptr) return;
  std::memcpy(registers, accuracy->banks[0].regs.data(), 128);
}

extern "C" void sr_dsp_accuracy_get_voice(
    const SrDspAccuracy *accuracy, int channel, SrDspAccuracyVoice *voice) {
  if (voice == nullptr) return;
  *voice = SrDspAccuracyVoice{};
  if (accuracy == nullptr || channel < 0 || channel >= kVoiceCount) return;
  const int bank = channel / kVoicesPerBank;
  const int index = channel % kVoicesPerBank;
  const DspState& state = accuracy->banks[bank];
  const VoiceState& source = state.voices[index];
  voice->pitch_counter = source.pitchCounter;
  voice->envelope = source.envelope;
  voice->brr_address = source.brrAddress;
  voice->source_number = state.regs[voiceRegister(index, 4)];
  voice->phase = static_cast<std::uint8_t>(source.phase);
  voice->key_on_delay = source.konDelay;
  voice->amplitude = static_cast<std::int16_t>(state.voiceAmplitude[index]);
}

extern "C" std::uint8_t sr_dsp_accuracy_slot(
    const SrDspAccuracy *accuracy) {
  return accuracy == nullptr ? 0 : accuracy->banks[0].slotCursor;
}

extern "C" void sr_dsp_accuracy_saveload(SrDspAccuracy *accuracy,
                                           SaveLoadInfo *info) {
  if (accuracy == nullptr || info == nullptr || info->func == nullptr) return;
  for (auto& bank : accuracy->banks) saveloadBank(info, bank);
  if (!info->saving && !info->failed) {
    accuracy->bankActive[0] = true;
    for (int bank = 1; bank < kBankCount; ++bank) {
      accuracy->banks[bank].regs[0x2C] = 0;
      accuracy->banks[bank].regs[0x3C] = 0;
      accuracy->bankActive[bank] = !bankIsQuiescent(accuracy->banks[bank]);
    }
  }
}

extern "C" void sr_dsp_accuracy_decode_brr(
    const std::uint8_t block[9], std::int16_t old, std::int16_t older,
    std::int16_t samples[16]) {
  if (block == nullptr || samples == nullptr) return;
  std::span<const std::uint8_t, 9> source(block, 9);
  const snaggletooth::BrrBlock decoded =
      snaggletooth::decodeBrrBlock(source, old, older);
  std::copy(decoded.samples.begin(), decoded.samples.end(), samples);
}

extern "C" std::int16_t sr_dsp_accuracy_gauss(
    const std::int16_t window[4], std::uint8_t index) {
  if (window == nullptr) return 0;
  return snaggletooth::gaussInterpolate(
      SampleWindow{.newest = window[0], .old = window[1],
                   .older = window[2], .oldest = window[3]}, index);
}
