#include "dsp_accuracy_bridge.h"

#include "snaggletooth/apu/dsp.h"

extern "C" {
#include "dsp.h"
#include "support/audio_audit_internal.h"
#include "saveload.h"
#include "semantic_state.h"
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
  bank.cycleCount = native.cycleCount;
  bank.latchedDir = native.latchedDir;
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
        source.konDelay != 0 || source.restartPending || source.startPending ||
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
    saveload_u8(info, &decode.header);
    saveload_u8(info, &decode.firstByte);
    saveload_bool(info, &decode.bytesLoaded);
    saveload_u8(info, &decode.decodedSamples);
    saveload_bool(info, &decode.headerCaptured);
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
  saveload_u8(info, &voice.pitchCaptureHold);
  saveload_bool(info, &voice.restartPending);
  saveload_bool(info, &voice.startupWalks);
  saveload_u16(info, &voice.bentGainRef);
  saveload_u8(info, &voice.loadedHeader);
  saveload_bool(info, &voice.headerLoaded);
  saveload_u16(info, &voice.loopPointer);
  saveload_bool(info, &voice.loopPointerLoaded);
  saveload_u8(info, &voice.srcn);
  saveload_bool(info, &voice.srcnLoaded);
  saveload_u8(info, &voice.pitchLow);
  saveload_u16(info, &voice.pitchPending);
  saveload_bool(info, &voice.pitchPendingValid);
  saveload_u8(info, &voice.adsr1);
  saveload_bool(info, &voice.adsr1Loaded);
  saveload_bool(info, &voice.startPending);
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
  saveload_bytes(info, state.echoFirCoeff.data(), state.echoFirCoeff.size());
  for (auto& stamp : state.outxWriteCycle) saveload_u64(info, &stamp);
  for (auto& stamp : state.envxWriteCycle) saveload_u64(info, &stamp);
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
  saveload_u8(info, &state.pitchReloadPending);
  saveloadInt(info, state.echoFirOutLeft);
  saveloadInt(info, state.echoFirOutRight);
  saveload_bool(info, &state.echoGateLeft);
  saveload_bool(info, &state.echoGateRight);
  saveload_u8(info, &state.echoLatchedEsa);
  saveload_u8(info, &state.echoLatchedEdl);
  saveload_u8(info, &state.consumedKon);
  saveload_u8(info, &state.latchedPmon);
  saveload_u8(info, &state.latchedNon);
  saveload_u8(info, &state.latchedEon);
  saveload_u8(info, &state.latchedDir);
  saveload_u8(info, &state.pendingEndxClear);
  saveload_u64(info, &state.cycleCount);
  saveload_u64(info, &state.endxWriteCycle);
  saveload_u64(info, &state.konWriteCycle);
}

/* Schema 2 is intentionally spelled out separately from saveloadBank. A
 * save-state revision must not redefine the canonical replay digest. */
void semanticWindowV2(SnesSemanticWriter *writer,
                      const SampleWindow& window) {
  snes_semantic_write_i16(writer, window.newest);
  snes_semantic_write_i16(writer, window.old);
  snes_semantic_write_i16(writer, window.older);
  snes_semantic_write_i16(writer, window.oldest);
}

void semanticVoiceV2(SnesSemanticWriter *writer, const VoiceState& voice) {
  snes_semantic_write_u16(writer, voice.brrAddress);
  snes_semantic_write_u16(writer, voice.decoderAddress);
  snes_semantic_write_u16(writer, voice.headerAddress);
  snes_semantic_write_u8(writer, voice.brrSampleIndex);
  snes_semantic_write_u16(writer, voice.pitchCounter);
  semanticWindowV2(writer, voice.window);
  for (const auto sample : voice.pending)
    snes_semantic_write_i16(writer, sample);
  snes_semantic_write_u8(writer, voice.pendingHead);
  snes_semantic_write_u8(writer, voice.pendingCount);
  for (const auto& decode : voice.scheduledDecodes) {
    snes_semantic_write_u16(writer, decode.address);
    snes_semantic_write_u8(writer, decode.offset);
    snes_semantic_write_u8(writer, decode.header);
    snes_semantic_write_u8(writer, decode.firstByte);
    snes_semantic_write_bool(writer, decode.bytesLoaded);
    snes_semantic_write_u8(writer, decode.decodedSamples);
    snes_semantic_write_bool(writer, decode.headerCaptured);
  }
  snes_semantic_write_u8(writer, voice.scheduledDecodeCount);
  snes_semantic_write_i16(writer, voice.decodePrev1);
  snes_semantic_write_i16(writer, voice.decodePrev2);
  snes_semantic_write_u16(writer, voice.envelope);
  snes_semantic_write_u8(
      writer, static_cast<std::uint8_t>(voice.phase));
  snes_semantic_write_u8(writer, voice.konDelay);
  snes_semantic_write_u8(writer, voice.computesSinceKeyOn);
  snes_semantic_write_u8(writer, voice.pitchCaptureHold);
  snes_semantic_write_bool(writer, voice.restartPending);
  snes_semantic_write_bool(writer, voice.startupWalks);
  snes_semantic_write_u16(writer, voice.bentGainRef);
  snes_semantic_write_u8(writer, voice.loadedHeader);
  snes_semantic_write_bool(writer, voice.headerLoaded);
  snes_semantic_write_u16(writer, voice.loopPointer);
  snes_semantic_write_bool(writer, voice.loopPointerLoaded);
  snes_semantic_write_u8(writer, voice.srcn);
  snes_semantic_write_bool(writer, voice.srcnLoaded);
  snes_semantic_write_u8(writer, voice.pitchLow);
  snes_semantic_write_u16(writer, voice.pitchPending);
  snes_semantic_write_bool(writer, voice.pitchPendingValid);
  snes_semantic_write_u8(writer, voice.adsr1);
  snes_semantic_write_bool(writer, voice.adsr1Loaded);
  snes_semantic_write_bool(writer, voice.startPending);
}

void semanticBankV2(SnesSemanticWriter *writer, const DspState& state) {
  snes_semantic_write_bytes(writer, state.regs.data(), state.regs.size());
  snes_semantic_write_u16(writer, state.globalCounter);
  snes_semantic_write_u32(writer, state.sampleIndex);
  snes_semantic_write_u8(writer, state.internalKon);
  snes_semantic_write_bytes(
      writer, state.envxStage.data(), state.envxStage.size());
  snes_semantic_write_u8(writer, state.preparedEndx);
  snes_semantic_write_i16(writer, state.noiseLevel);
  snes_semantic_write_u16(writer, state.echoIndex);
  snes_semantic_write_u16(writer, state.echoLength);
  snes_semantic_write_u8(writer, state.echoAppliedEsa);
  for (const auto sample : state.echoFirLeft)
    snes_semantic_write_i16(writer, sample);
  for (const auto sample : state.echoFirRight)
    snes_semantic_write_i16(writer, sample);
  snes_semantic_write_bytes(writer, state.echoFirCoeff.data(), state.echoFirCoeff.size());
  for (auto stamp : state.outxWriteCycle) snes_semantic_write_u64(writer, stamp);
  for (auto stamp : state.envxWriteCycle) snes_semantic_write_u64(writer, stamp);
  snes_semantic_write_u8(writer, state.echoFirPos);
  for (const auto& voice : state.voices) semanticVoiceV2(writer, voice);
  snes_semantic_write_u8(writer, state.slotCursor);
  snes_semantic_write_bool(writer, state.primed);
  snes_semantic_write_i32(writer, static_cast<std::int32_t>(state.mixLeft));
  snes_semantic_write_i32(writer, static_cast<std::int32_t>(state.mixRight));
  snes_semantic_write_i32(
      writer, static_cast<std::int32_t>(state.echoSendLeft));
  snes_semantic_write_i32(
      writer, static_cast<std::int32_t>(state.echoSendRight));
  snes_semantic_write_i16(writer, state.slotFrame.left);
  snes_semantic_write_i16(writer, state.slotFrame.right);
  snes_semantic_write_bool(writer, state.echoWritePending);
  snes_semantic_write_u16(writer, state.echoWriteEntry);
  snes_semantic_write_bytes(
      writer, state.echoWriteBytes.data(), state.echoWriteBytes.size());
  for (const auto amplitude : state.voiceAmplitude)
    snes_semantic_write_i32(writer, static_cast<std::int32_t>(amplitude));
  for (const auto amplitude : state.modulatorAmplitude)
    snes_semantic_write_i32(writer, static_cast<std::int32_t>(amplitude));
  snes_semantic_write_bytes(
      writer, state.preparedOutx.data(), state.preparedOutx.size());
  snes_semantic_write_bytes(
      writer, state.preparedEnvx.data(), state.preparedEnvx.size());
  for (const auto pitch : state.pitchLatch)
    snes_semantic_write_u16(writer, pitch);
  snes_semantic_write_u8(writer, state.pitchReloadPending);
  snes_semantic_write_i32(
      writer, static_cast<std::int32_t>(state.echoFirOutLeft));
  snes_semantic_write_i32(
      writer, static_cast<std::int32_t>(state.echoFirOutRight));
  snes_semantic_write_bool(writer, state.echoGateLeft);
  snes_semantic_write_bool(writer, state.echoGateRight);
  snes_semantic_write_u8(writer, state.echoLatchedEsa);
  snes_semantic_write_u8(writer, state.echoLatchedEdl);
  snes_semantic_write_u8(writer, state.consumedKon);
  snes_semantic_write_u8(writer, state.latchedPmon);
  snes_semantic_write_u8(writer, state.latchedNon);
  snes_semantic_write_u8(writer, state.latchedEon);
  snes_semantic_write_u8(writer, state.latchedDir);
  snes_semantic_write_u8(writer, state.pendingEndxClear);
  snes_semantic_write_u64(writer, state.cycleCount);
  snes_semantic_write_u64(writer, state.endxWriteCycle);
  snes_semantic_write_u64(writer, state.konWriteCycle);
}

}  // namespace

struct SrDspAccuracy {
  std::array<DspState, kBankCount> banks{};
  std::uint8_t activeBankMask = 1u;
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
  accuracy->activeBankMask = 1u;
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
  snaggletooth::cpuWriteDspRegister(accuracy->banks[0], address, value);
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
  const std::uint8_t pendingKon = native.internalKon;
  snaggletooth::cpuWriteDspRegister(native, address, combined);
  if (address == kDspKon) {
    native.internalKon = static_cast<std::uint8_t>(
        (pendingKon & ~update_mask) | (value & update_mask));
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
  const std::uint8_t bankBit = static_cast<std::uint8_t>(1u << bank);
  if (global_address == kDspKon && enabled &&
      (accuracy->activeBankMask & bankBit) == 0) {
    syncVirtualTimeline(state, accuracy->banks[0]);
    accuracy->activeBankMask |= bankBit;
  }
  const std::uint8_t pendingKon = state.internalKon;
  const std::uint8_t value = enabled ? state.regs[global_address] | bit
      : state.regs[global_address] & static_cast<std::uint8_t>(~bit);
  snaggletooth::cpuWriteDspRegister(state, global_address, value);
  if (global_address == kDspKon)
    state.internalKon = enabled ? pendingKon | bit
        : pendingKon & static_cast<std::uint8_t>(~bit);
}

// Slot 32 retains the independently dispatched single-cycle reference path.
template<unsigned Slot>
static SrDspAccuracyFrame clockAccuracy(
    SrDspAccuracy *accuracy, std::uint8_t *apu_ram, bool extended_enabled,
    bool mix_controls_unity,
    const std::uint8_t voice_gain_percent[kVoiceCount],
    const std::uint8_t voice_muted[kVoiceCount]) {
  SrDspAccuracyFrame output{};
  if (accuracy == nullptr || apu_ram == nullptr ||
      voice_gain_percent == nullptr || voice_muted == nullptr) return output;

  DspState& native = accuracy->banks[0];
  const std::uint8_t slot = Slot == 32 ? native.slotCursor : Slot;
  const std::uint8_t virtualBankMask = extended_enabled
      ? static_cast<std::uint8_t>(accuracy->activeBankMask & ~1u) : 0u;
  std::array<SlotResult, kBankCount> result{};
  std::span<std::uint8_t, 65536> writable(apu_ram, 65536);
  std::span<const std::uint8_t, 65536> readonly(apu_ram, 65536);

  if (extended_enabled && !accuracy->extendedWasEnabled) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if ((accuracy->activeBankMask & (1u << bank)) != 0)
        syncVirtualTimeline(accuracy->banks[bank], native);
    }
  }
  accuracy->extendedWasEnabled = extended_enabled;

  if (virtualBankMask != 0) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if ((virtualBankMask & (1u << bank)) == 0) continue;
      DspState& virtualBank = accuracy->banks[bank];
      VolumeHold hold;
      hold.count = 0;
      if (!mix_controls_unity)
        applySlotVoiceGains(virtualBank, bank, voice_gain_percent,
                            voice_muted, hold);
      if constexpr (Slot == 32)
        result[bank] = snaggletooth::stepDspVoiceCycle(virtualBank, readonly);
      else
        result[bank] = snaggletooth::detail::stepDspCycleAtSlot<Slot, false>(
            virtualBank, readonly, nullptr);
      restoreVoiceGains(virtualBank, hold);
    }
  }

  if (slot == 23 && virtualBankMask != 0) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      if ((virtualBankMask & (1u << bank)) == 0) continue;
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
  if constexpr (Slot == 32)
    result[0] = snaggletooth::stepDspCycle(native, writable);
  else
    result[0] = snaggletooth::detail::stepDspCycleAtSlot<Slot, true>(
        native, readonly, apu_ram);
  restoreVoiceGains(native, nativeHold);

  if (!result[0].delivered) return output;
  int left = result[0].frame.left;
  int right = result[0].frame.right;
  output.active_bank_mask = static_cast<std::uint8_t>(1u | virtualBankMask);
  if (virtualBankMask != 0) {
    for (int bank = 1; bank < kBankCount; ++bank) {
      const std::uint8_t bankBit = static_cast<std::uint8_t>(1u << bank);
      if ((virtualBankMask & bankBit) == 0) continue;
      left = clamp16(left + result[bank].frame.left);
      right = clamp16(right + result[bank].frame.right);
      if (bankIsQuiescent(accuracy->banks[bank])) {
        accuracy->banks[bank].preparedEndx = 0;
        accuracy->activeBankMask &= static_cast<std::uint8_t>(~bankBit);
      }
    }
  }
  output.left = static_cast<std::int16_t>(left);
  output.right = static_cast<std::int16_t>(right);
  output.delivered = true;
  return output;
}

static void publishFrame(Dsp *dsp, SrDspAccuracy *accuracy,
                         const SrDspAccuracyFrame& frame) {
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

extern "C" SrDspAccuracyFrame sr_dsp_accuracy_clock(
    SrDspAccuracy *accuracy, std::uint8_t *apu_ram, bool extended_enabled,
    bool mix_controls_unity, const std::uint8_t gains[kVoiceCount],
    const std::uint8_t muted[kVoiceCount]) {
  return clockAccuracy<32>(accuracy, apu_ram, extended_enabled,
                            mix_controls_unity, gains, muted);
}

extern "C" void dsp_clock(Dsp *dsp) {
  if (dsp == nullptr || dsp->accuracy == nullptr) return;
  auto *accuracy = static_cast<SrDspAccuracy *>(dsp->accuracy);
  if (accuracy->banks[0].slotCursor == 0) dsp_refreshMixControls(dsp);
  const auto frame = clockAccuracy<32>(accuracy, dsp->apu_ram,
      g_dsp_extended_voices_enabled, dsp->mixControlsUnity,
      dsp->voiceGainPercent, dsp->voiceMuted);
  if (frame.delivered) publishFrame(dsp, accuracy, frame);
}

template<unsigned Slot>
static void clockDspSlot(Dsp *dsp, SrDspAccuracy *accuracy) {
  if constexpr (Slot == 0) dsp_refreshMixControls(dsp);
  const auto frame = clockAccuracy<Slot>(accuracy, dsp->apu_ram,
      g_dsp_extended_voices_enabled, dsp->mixControlsUnity,
      dsp->voiceGainPercent, dsp->voiceMuted);
  if constexpr (Slot == 31) {
    if (frame.delivered) publishFrame(dsp, accuracy, frame);
  }
}

extern "C" void dsp_clockMany(Dsp *dsp, std::uint32_t cycles) {
  if (dsp == nullptr || dsp->accuracy == nullptr || cycles == 0) return;
  auto *accuracy = static_cast<SrDspAccuracy *>(dsp->accuracy);
  // A single entry dispatch, followed by the same ordered slots for every
  // bank. Never run a complete virtual bank ahead of shared native echo RAM.
  while (cycles != 0) {
    switch (accuracy->banks[0].slotCursor) {
      case 0: clockDspSlot<0>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 1: clockDspSlot<1>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 2: clockDspSlot<2>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 3: clockDspSlot<3>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 4: clockDspSlot<4>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 5: clockDspSlot<5>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 6: clockDspSlot<6>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 7: clockDspSlot<7>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 8: clockDspSlot<8>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 9: clockDspSlot<9>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 10: clockDspSlot<10>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 11: clockDspSlot<11>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 12: clockDspSlot<12>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 13: clockDspSlot<13>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 14: clockDspSlot<14>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 15: clockDspSlot<15>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 16: clockDspSlot<16>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 17: clockDspSlot<17>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 18: clockDspSlot<18>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 19: clockDspSlot<19>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 20: clockDspSlot<20>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 21: clockDspSlot<21>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 22: clockDspSlot<22>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 23: clockDspSlot<23>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 24: clockDspSlot<24>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 25: clockDspSlot<25>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 26: clockDspSlot<26>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 27: clockDspSlot<27>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 28: clockDspSlot<28>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 29: clockDspSlot<29>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 30: clockDspSlot<30>(dsp, accuracy);
        if (--cycles == 0) return;
        [[fallthrough]];
      case 31: clockDspSlot<31>(dsp, accuracy);
        if (--cycles == 0) return;
        break;
      default:
        // A malformed legacy state must not leave this loop spinning.
        dsp_clock(dsp);
        --cycles;
        break;
    }
  }
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
    accuracy->activeBankMask = 1u;
    for (int bank = 1; bank < kBankCount; ++bank) {
      accuracy->banks[bank].regs[0x2C] = 0;
      accuracy->banks[bank].regs[0x3C] = 0;
      if (!bankIsQuiescent(accuracy->banks[bank]))
        accuracy->activeBankMask |= static_cast<std::uint8_t>(1u << bank);
    }
  }
}

extern "C" void sr_dsp_accuracy_write_semantic_v2(
    const SrDspAccuracy *accuracy, SnesSemanticWriter *writer) {
  if (accuracy == nullptr || writer == nullptr) {
    if (writer != nullptr) writer->failed = true;
    return;
  }
  for (const auto& bank : accuracy->banks) semanticBankV2(writer, bank);
  snes_semantic_write_u8(writer, accuracy->activeBankMask);
  snes_semantic_write_bool(writer, accuracy->extendedWasEnabled);
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
