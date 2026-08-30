#include "dsp_accuracy_bridge.h"

#include "snaggletooth/apu/dsp.h"

extern "C" {
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

constexpr std::array<std::uint8_t, 16> kSharedRegisters = {
    0x0C, 0x1C, 0x2C, 0x3C, 0x0D, 0x5D, 0x6C, 0x6D,
    0x7D, 0x0F, 0x1F, 0x2F, 0x3F, 0x4F, 0x5F, 0x6F,
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

void syncVirtualGlobals(DspState& bank, const DspState& native) noexcept {
  for (const std::uint8_t address : kSharedRegisters)
    bank.regs[address] = native.regs[address];
  bank.regs[0x7F] = native.regs[0x7F];
  bank.globalCounter = native.globalCounter;
  bank.sampleIndex = native.sampleIndex;
  bank.noiseLevel = native.noiseLevel;
}

struct VolumeHold {
  std::array<std::uint8_t, 16> bytes{};
};

VolumeHold applyVoiceGains(DspState& state, int bank,
                           const std::uint8_t gains[kVoiceCount],
                           const std::uint8_t muted[kVoiceCount]) noexcept {
  VolumeHold hold;
  for (int voice = 0; voice < kVoicesPerBank; ++voice) {
    const int channel = bank * kVoicesPerBank + voice;
    for (int side = 0; side < 2; ++side) {
      const std::uint8_t address = voiceRegister(voice, static_cast<std::uint8_t>(side));
      const std::uint8_t raw = state.regs[address];
      hold.bytes[voice * 2 + side] = raw;
      const int percent = muted[channel] != 0 ? 0 : gains[channel];
      const int scaled = static_cast<std::int8_t>(raw) * percent / 100;
      state.regs[address] = static_cast<std::uint8_t>(static_cast<std::int8_t>(scaled));
    }
  }
  return hold;
}

void restoreVoiceGains(DspState& state, const VolumeHold& hold) noexcept {
  for (int voice = 0; voice < kVoicesPerBank; ++voice) {
    state.regs[voiceRegister(voice, 0)] = hold.bytes[voice * 2];
    state.regs[voiceRegister(voice, 1)] = hold.bytes[voice * 2 + 1];
  }
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
}

extern "C" std::uint8_t sr_dsp_accuracy_read(
    const SrDspAccuracy *accuracy, std::uint8_t address) {
  return accuracy == nullptr ? 0 : accuracy->banks[0].regs[address & 0x7F];
}

extern "C" void sr_dsp_accuracy_write(SrDspAccuracy *accuracy,
                                        std::uint8_t address,
                                        std::uint8_t value) {
  if (accuracy == nullptr) return;
  writeRegister(accuracy->banks[0], address, value);
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
}

extern "C" SrDspAccuracyFrame sr_dsp_accuracy_clock(
    SrDspAccuracy *accuracy, std::uint8_t *apu_ram, bool extended_enabled,
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

  if (extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      DspState& virtualBank = accuracy->banks[bank];
      syncVirtualGlobals(virtualBank, native);
      const std::uint8_t evolLeft = virtualBank.regs[0x2C];
      const std::uint8_t evolRight = virtualBank.regs[0x3C];
      virtualBank.regs[0x2C] = 0;
      virtualBank.regs[0x3C] = 0;
      VolumeHold hold = applyVoiceGains(
          virtualBank, bank, voice_gain_percent, voice_muted);
      result[bank] = snaggletooth::stepDspCycle(virtualBank, readonly);
      restoreVoiceGains(virtualBank, hold);
      virtualBank.regs[0x2C] = evolLeft;
      virtualBank.regs[0x3C] = evolRight;
    }
  }

  if (slot == 24 && extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      native.echoSendLeft = clamp16(
          native.echoSendLeft + accuracy->banks[bank].echoSendLeft);
      native.echoSendRight = clamp16(
          native.echoSendRight + accuracy->banks[bank].echoSendRight);
    }
  }
  VolumeHold nativeHold = applyVoiceGains(
      native, 0, voice_gain_percent, voice_muted);
  result[0] = snaggletooth::stepDspCycle(native, writable);
  restoreVoiceGains(native, nativeHold);

  if (!result[0].delivered) return output;
  int left = result[0].frame.left;
  int right = result[0].frame.right;
  if (extended_enabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      left = clamp16(left + result[bank].frame.left);
      right = clamp16(right + result[bank].frame.right);
    }
  }
  output.left = static_cast<std::int16_t>(left);
  output.right = static_cast<std::int16_t>(right);
  output.delivered = true;
  return output;
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
