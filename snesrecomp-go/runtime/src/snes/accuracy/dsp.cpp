// Portions adapted from Snaggletooth src/dsp.cpp at commit
// 321cb3eddde1fe0474eab31e5a88d32f413a7ff6.
// Copyright (c) 2026 Eric Tomasso. SPDX-License-Identifier: MIT.
// See runtime/licenses/Snaggletooth-LICENSE.txt.

#include "snaggletooth/apu/dsp.h"

namespace snaggletooth {

namespace {

// DSP register addresses the voice stream reads and writes. Per-voice
// registers live at voice*10h plus the per-voice offset.
constexpr std::uint8_t kDspMvolLeft = 0x0C;
constexpr std::uint8_t kDspMvolRight = 0x1C;
constexpr std::uint8_t kDspEfb = 0x0D;
constexpr std::uint8_t kDspEvolLeft = 0x2C;
constexpr std::uint8_t kDspEvolRight = 0x3C;
constexpr std::uint8_t kDspPmon = 0x2D;
constexpr std::uint8_t kDspNon = 0x3D;
constexpr std::uint8_t kDspEon = 0x4D;
constexpr std::uint8_t kDspKon = 0x4C;
constexpr std::uint8_t kDspKoff = 0x5C;
constexpr std::uint8_t kDspDir = 0x5D;
constexpr std::uint8_t kDspFlg = 0x6C;
constexpr std::uint8_t kDspEsa = 0x6D;
constexpr std::uint8_t kDspEndx = 0x7C;
constexpr std::uint8_t kDspEdl = 0x7D;

// FLG bit masks: bits 0-4 are the noise rate, bit 5 disables echo writes, bit 6
// mutes the output amplifier, bit 7 is the per-sample soft reset.
constexpr std::uint8_t kFlgNoiseRate = 0x1F;
constexpr std::uint8_t kFlgEchoWriteDisable = 0x20;
constexpr std::uint8_t kFlgMute = 0x40;
constexpr std::uint8_t kFlgSoftReset = 0x80;

// The per-voice offset of the echo FIR coefficient registers: FIR0..FIR7 live at
// $0F, $1F, ... $7F (tap*10h + this), each a signed 8-bit coefficient.
constexpr std::uint8_t kFirCoeff = 0x0F;

// The pitch counter's in-group position ceiling. The low fourteen bits of the
// counter place the interpolation cursor within the four-sample group it is
// consuming (bits 12-13 the sample, bits 0-11 the fraction); a step is added to
// that position and the sum clamps here before the crossed positions are
// counted, so one output sample consumes at most four source samples (128 kHz)
// and a position that hit the ceiling stands at 3FFFh — sample 3 of its group
// at the maximum fraction — once the group turns. The base 14-bit step tops out
// at 3FFFh, so only a PMON-scaled step reaches the ceiling.
constexpr std::uint32_t kMaxGroupPosition = 0x7FFF;
constexpr std::uint32_t kGroupPositionMask = 0x3FFF;
constexpr std::uint8_t kVoiceVolLeft = 0x00;
constexpr std::uint8_t kVoiceVolRight = 0x01;
constexpr std::uint8_t kVoicePitchLow = 0x02;
constexpr std::uint8_t kVoicePitchHigh = 0x03;
constexpr std::uint8_t kVoiceSrcn = 0x04;
constexpr std::uint8_t kVoiceAdsr1 = 0x05;
constexpr std::uint8_t kVoiceAdsr2 = 0x06;
constexpr std::uint8_t kVoiceGain = 0x07;
constexpr std::uint8_t kVoiceEnvx = 0x08;
constexpr std::uint8_t kVoiceOutx = 0x09;

// The global counter's power-on / wrap value: it counts down from 0x77FF.
constexpr std::uint16_t kGlobalCounterReload = 0x77FF;

// The envelope/noise rate tables — parser-generated, cross-checked against the
// references (see the generated file's header). kEnvelopePeriod[0] == 0 is the
// 'never' sentinel (rate 0 = Infinite); kEnvelopePeriod[31] == 1 fires every
// sample. kEnvelopeOffset delays each rate's phase within its period.
#include "generated/envelope_counter_tables.inc"

[[nodiscard]] std::uint8_t voiceRegister(std::size_t voice, std::uint8_t offset) noexcept {
  return static_cast<std::uint8_t>(voice * 0x10 + offset);
}

// The S-DSP's Gaussian interpolation table — hardware ROM data, transcribed
// from the references by tools/parse_dsp_tables.py (see the generated file's
// header). The four-tap sums land in 7FFh..801h rather than a constant 800h;
// the 801h rows are what make the kernel's documented wrap reachable.
constexpr std::array<std::int16_t, 512> kGaussTable = {
#include "generated/gauss_table.inc"
};

// Converts a signed 4-bit BRR nibble (-8..+7) to a sample: sample = nibble
// scaled by the block's shift, then arithmetic-shifted right one. A shift of
// 13..15 is anomalous — decoding proceeds as if shift were 12 over (nibble SAR
// 3). The scale is a multiply (never a left shift of a negative value) so the
// arithmetic is well-defined and warning-clean across compilers.
[[nodiscard]] int decodeNibble(int nibble, int shift) noexcept {
  if (shift >= 13) {
    shift = 12;
    nibble >>= 3;  // arithmetic: -8..-1 -> -1, 0..+7 -> 0
  }
  return (nibble * (1 << shift)) >> 1;
}

// Extracts one of a BRR data byte's two signed nibbles: the high nibble is the
// first sample, the low nibble the second.
[[nodiscard]] int signedNibble(std::uint8_t byte, bool second) noexcept {
  const int raw = second ? (byte & 0x0F) : (byte >> 4);
  return raw - ((raw & 0x08) ? 16 : 0);  // sign-extend to -8..+7
}

// The four BRR filters, in fullsnes's exact integer forms (cross-checked 1:1
// against Anomie's S-DSP doc). `old` is the previous 15-bit output, `older` the
// one before it. The right shifts are arithmetic over signed values.
[[nodiscard]] int applyFilter(int filter, int sample, int old, int older) noexcept {
  switch (filter) {
    case 0:
      return sample;
    case 1:
      return sample + old + ((-old) >> 4);
    case 2:
      return sample + old * 2 + ((-old * 3) >> 5) - older + (older >> 4);
    default:  // filter 3
      return sample + old * 2 + ((-old * 13) >> 6) - older + ((older * 3) >> 4);
  }
}

// Clamps to signed 16 bits, then clips to signed 15 by re-reading bit 14 as the
// sign. This is the hardware's clamp-then-clip: a value below -8000h clamps to
// -8000h and then clips to 0 (the dirt-effect), and a magnitude that overflows
// 15 bits folds to the opposite sign (lost-sign). In-range values pass through.
[[nodiscard]] std::int16_t clampAndClip(int value) noexcept {
  if (value > 0x7FFF) value = 0x7FFF;
  if (value < -0x8000) value = -0x8000;
  return static_cast<std::int16_t>(((value & 0x7FFF) ^ 0x4000) - 0x4000);
}

// The exponential envelope decrease shared by Decay, Sustain and GAIN
// Exp-Decrease: level -= 1, then level -= level >> 8 (the right shift is
// arithmetic, so the two-step form never drives the level below 0).
[[nodiscard]] int expDecrease(int level) noexcept {
  const int stepped = level - 1;
  return stepped - (stepped >> 8);
}

// Saturates an envelope value to the unsigned 11-bit range 0..0x7FF (the
// hardware clamps rather than wraps: a decrease past 0 sticks at 0, an attack
// past 0x7FF sticks at 0x7FF).
[[nodiscard]] std::uint16_t clampEnvelope(int level) noexcept {
  if (level < 0) return 0;
  if (level > 0x7FF) return 0x7FF;
  return static_cast<std::uint16_t>(level);
}

// The voice's base 14-bit pitch step, read live from VxPITCHL/H; the register
// pair's bits 14-15 are stored but never used.
[[nodiscard]] std::uint32_t voicePitch(const DspState& dsp, std::size_t voice) noexcept {
  const std::uint32_t low = dsp[voiceRegister(voice, kVoicePitchLow)];
  const std::uint32_t high = dsp[voiceRegister(voice, kVoicePitchHigh)];
  return ((high << 8) | low) & 0x3FFF;
}

// The pitch step this output sample, including pitch modulation. For a voice in
// 1..7 with its PMON bit set, the base step is scaled by the previous voice's
// amplitude from the previous sample: factor = (prevAmplitude SAR 4) + 400h (the range
// 000h..7FFh, i.e. 0.00..1.99), and step = (base * factor) SAR 10. A silent
// previous voice gives factor 400h, an unmodulated x1.0, so no special case is
// needed. The step itself is never capped (a modulated step reaches 7FEEh); the
// 128 kHz ceiling is applied to the position the step is added to, in
// advanceVoiceStream.
// The pitch step a voice's stream advance uses this sample — the value its
// pitch slots read, never the live register pair (see DspState::pitchLatch).
[[nodiscard]] std::uint32_t latchedPitch(const DspState& dsp, std::size_t voice) noexcept {
  return dsp.pitchLatch[voice];
}

[[nodiscard]] std::uint32_t pitchStep(const DspState& dsp, std::size_t voice,
                                      int prevAmplitude) noexcept {
  std::uint32_t step = latchedPitch(dsp, voice);
  const bool modulate = voice > 0 && ((dsp.latchedPmon >> voice) & 1) != 0;
  if (modulate) {
    const int factor = (prevAmplitude >> 4) + 0x400;  // -400h..+3FFh -> 000h..7FFh
    step = static_cast<std::uint32_t>((static_cast<int>(step) * factor) >> 10);
  }
  return step;
}

// The startup countdown a key-on arms — the documented five empty samples. The
// keying poll precedes voice 0's compute in the slot they share, so the load's
// own slot is the keyed voice's first silent startup call for voice 0, while
// voices 1-7 take theirs in the following samples.
inline constexpr std::uint8_t kKeyOnStartupCalls = 5;

// The voice's silent key-on span in compute calls: the five startup calls plus
// the first live compute, whose output is still silent because a sample is
// scaled by the envelope standing before its update. The keying poll absorbs a
// key-on landing inside this span (see pollKeying).
inline constexpr std::uint8_t kKeyOnSilentCalls = kKeyOnStartupCalls + 1;

// A key-on's startup keeps no interpolation fraction, and this window is
// where that holds: from the load through the first sounding compute. For a
// walking startup (a sounding voice re-keyed — see VoiceState::startupWalks)
// the pitch counter's fractional bits are cleared after each advance:
// whole-sample crossings stand, so the cursor still walks at the pitch
// (`KON/kon decoding when another kon`), while the first audible sample
// interpolates from index 0 and the fraction begins accumulating only with
// the advance after it (`KON/pitch at kon`: at pitch $0010 the audible ramp
// reads the Gaussian kernel at indices 0, 1, 2, … — a retained startup
// fraction starts it six indices deep). For a held startup the same window
// bounds the stream hold itself — the advance is skipped outright, so there
// is no fraction to clear (`Misc/brr addr wrap-around`). The window is one
// call longer than the silent span because the advance precedes the
// interpolation within a compute, so the first sounding call's own advance
// still contributes no fraction. It is counted from the load, so an in-span
// rewind does not reopen it — the rewound stream keeps walking with its
// fraction intact.
[[nodiscard]] bool keyOnPinsFraction(const VoiceState& v) noexcept {
  return v.computesSinceKeyOn <= kKeyOnSilentCalls + 1;
}

// A BRR header's low two bits are its end/loop code. Code 1 — end set, loop clear
// — is End+Mute: the voice releases and its level drops to 0. Code 3 loops without
// muting, and codes 0 and 2 run on into the next block.
[[nodiscard]] bool headerIsEndMute(std::uint8_t header) noexcept {
  return (header & 0x03) == 0x01;
}

// Advances the shared noise generator one step: a 15-bit right rotation whose new
// top bit is bit0 XOR bit1 of the old level. The level is held as the signed
// sample it outputs (-4000h..+3FFFh), so the step works over its 15-bit pattern.
[[nodiscard]] std::int16_t nextNoiseLevel(std::int16_t level) noexcept {
  const unsigned pattern = static_cast<unsigned>(level) & 0x7FFFu;
  const unsigned feedback = ((pattern ^ (pattern >> 1)) & 1u) << 14;
  const unsigned next = ((pattern >> 1) & 0x3FFFu) | feedback;
  return static_cast<std::int16_t>((next ^ 0x4000u) - 0x4000u);  // sign-extend 15 bits
}

// The decoder runs a full block-priming ahead of the interpolation cursor:
// a key-on decodes the whole first block before the voice sounds, and each
// group the cursor consumes afterward has the decoder filling the buffer one
// group further on. So the decoder enters a block while the cursor is still
// eight samples back in the block before — this is the cursor's in-block
// consumed-sample count at which decoderAddress moves on (`Misc/brr early
// end at many pitches` pins the eight; `KON/kon as prev sample ends` bounds
// it from the other side).
inline constexpr std::uint8_t kDecoderLead = 8;

// Shifts one consumed sample into a voice's interpolation window.
void shiftWindow(VoiceState& v, std::int16_t sample) noexcept {
  v.window.oldest = v.window.older;
  v.window.older = v.window.old;
  v.window.old = v.window.newest;
  v.window.newest = sample;
}

// The slot of each voice's BRR load — where the header and the first data
// byte of a group decode are read from RAM: T26 for voice 0, five slots before
// its compute, and the compute slot itself (kVoiceS3Slot) for voices 1-7, read
// before the compute runs in it. The second data byte is read at the decode,
// one slot after the compute (kVoiceS4Slot). Measured against spc_dsp6's
// `Timing/Voice/V3 BRR.header.03`, `BRR.sample.lsb` and `BRR.sample.msb`,
// which pulse each byte for five cycles at every offset and hash which voices
// caught it.

// The slot of each voice's directory read — where the sample directory entry
// `DIR`/`VxSRCN` select is read from RAM: T22 for voice 0, nine slots before
// its compute, and one slot before the compute for voices 1-7. The loop
// address read there is the one the decoder's loop jump at the compute takes.
// Measured against spc_dsp6's `Timing/Voice/V2 dir.loop.lsb` and
// `dir.loop.msb`, which pulse the entry's loop bytes for five cycles at every
// offset and hash which voices looped through the pulsed value.

// The slot of each voice's source read — where `VxSRCN` is read from the
// register array for the directory reads that follow: T18 for voice 0 and
// T21 for voice 1 (four and twelve slots before their directory slots, voice
// 1's in the previous sample), T(3v-6) for voices 2-7, four before theirs.
// Measured against spc_dsp6's `Timing/Voice/V1 srcn.start` and `srcn.loop`,
// which pulse the register for five cycles at every offset and hash which
// voices started or looped through the pulsed source.

// Reads, at a voice's source slot, the `VxSRCN` its next directory reads
// select the entry with.
static void loadSourceNumber(DspState& dsp, std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  v.srcn = dsp[voiceRegister(voice, kVoiceSrcn)];
  v.srcnLoaded = true;
}

// The source number a directory read selects the entry with: the source
// slot's capture, or the live register on a state no source slot has run on.
static std::uint8_t voiceSourceNumber(const DspState& dsp, std::size_t voice) noexcept {
  const VoiceState& v = dsp.voices[voice];
  return v.srcnLoaded ? v.srcn : dsp[voiceRegister(voice, kVoiceSrcn)];
}

// The slot of each voice's `VxPITCHH` read — one after the directory slot,
// where `VxPITCHL` is read: T23 for voice 0 and the compute slot itself for
// voices 1-7, read before the compute runs in it. The pair joined there is the
// step that compute's advance takes. Measured against spc_dsp6's
// `Timing/Voice/V2 pitchl` and `V3 pitchh`, which pulse each byte for five
// cycles at every offset and hash which voices advanced through it.

// Reads, at a voice's directory slot, the `VxPITCHL` byte its pitch-high slot
// joins into the compute's step, and the `VxADSR1` the compute's envelope step
// runs under (`Timing/Voice/V2 adsr0.0F`/`.70`/`.80` pulse the register for
// five cycles at every offset and hash which voices' envelopes took it). A
// voice inside its key-on hold keeps the step the key-on's scheduled capture
// loaded (see DspState::pitchLatch); the `VxADSR1` read has no such hold.
static void loadDirectorySlotRegisters(DspState& dsp, std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (v.pitchCaptureHold == 0) v.pitchLow = dsp[voiceRegister(voice, kVoicePitchLow)];
  v.adsr1 = dsp[voiceRegister(voice, kVoiceAdsr1)];
  v.adsr1Loaded = true;
}

// Reads, at a voice's pitch-high slot, the `VxPITCHH` byte and joins it with
// the directory slot's `VxPITCHL` into the pair the next sample's advance
// takes (VoiceState::pitchPending).
static void loadPitchHigh(DspState& dsp, std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (v.pitchCaptureHold != 0) return;
  const std::uint32_t high = dsp[voiceRegister(voice, kVoicePitchHigh)];
  v.pitchPending = static_cast<std::uint16_t>(((high << 8) | v.pitchLow) & 0x3FFF);
  v.pitchPendingValid = true;
}

// Reads PMON at slot T28, and NON, EON and DIR together at T29 — the four
// registers that speak for all eight voices, each read once a sample at its own
// slot (see DspState::latchedPmon). A schedule without those slots reads them
// as it enters, so a single-voice call and the frame-at-once sample see the
// registers as they stand.
static void loadPitchModulation(DspState& dsp) noexcept { dsp.latchedPmon = dsp[kDspPmon]; }

static void loadVoiceWideRegisters(DspState& dsp) noexcept {
  dsp.latchedNon = dsp[kDspNon];
  dsp.latchedEon = dsp[kDspEon];
  dsp.latchedDir = dsp[kDspDir];
}

static void latchGlobalRegisters(DspState& dsp) noexcept {
  loadPitchModulation(dsp);
  loadVoiceWideRegisters(dsp);
}

// Reads, at a voice's directory slot, the loop address of its directory entry
// for the loop jump the compute may make this sample.
void loadLoopPointer(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                     std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  v.loopPointer = readBrrSource(ram, dsp.latchedDir, voiceSourceNumber(dsp, voice)).loop;
  v.loopPointerLoaded = true;
}

// The in-group sample index whose consumption schedules the decode of the
// group two on: the third sample, so the group's first data byte lands at the
// load slot two samples after the group boundary's own consumption and its
// second at that sample's S4 — where `Timing/Voice/V3 BRR.sample.lsb/msb`
// measure the hardware reading them — while the header is read at the
// scheduling itself, the sample `Order/pitch after brr` pins.
constexpr std::uint8_t kDecodeTriggerIndex = 2;

// Loads, at a voice's BRR load slot, the header the sample's end/loop check
// reads and the header and first data byte of every group decode the voice
// has scheduled.
void loadScheduledGroupBytes(VoiceState& v, std::span<const std::uint8_t, 65536> ram) noexcept {
  v.loadedHeader = ram[v.headerAddress];
  v.headerLoaded = true;
  for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n) {
    VoiceState::GroupDecode& g = v.scheduledDecodes[n];
    if (g.bytesLoaded) continue;
    if (!g.headerCaptured) g.header = ram[g.address];
    g.firstByte = ram[static_cast<std::uint16_t>(g.address + 1 + g.offset / 2)];
    g.bytesLoaded = true;
  }
}

// Decodes samples [from, to) of one scheduled group into a voice's pending
// ring. The header's shift and filter and the group's first data byte are the
// ones the voice's BRR load slot captured (or, for a decode the slot schedule
// has not loaded, read from RAM now); the second data byte is read from RAM at
// the call decoding its samples. The samples are decoded ahead of their
// consumption, so a RAM write after these reads does not reach them. The
// filter history is the decoder's own — the two samples decoded before these,
// which run ahead of the window's consumed taps; halves of one group must
// therefore decode in order with nothing between them, which the split
// schedule guarantees (the S4 half lands before the next compute can decode
// or consume anything newer).
void decodeGroupAhead(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                      std::size_t voice, const VoiceState::GroupDecode& group, int from,
                      int to) noexcept {
  VoiceState& v = dsp.voices[voice];
  const std::uint16_t address = group.address;
  const int offset = group.offset;
  const std::uint8_t header =
      (group.bytesLoaded || group.headerCaptured) ? group.header : ram[address];
  const std::uint8_t firstByte = group.bytesLoaded
                                     ? group.firstByte
                                     : ram[static_cast<std::uint16_t>(address + 1 + offset / 2)];
  const std::uint8_t secondByte = ram[static_cast<std::uint16_t>(address + 2 + offset / 2)];
  for (int k = from; k < to; ++k) {
    const int index = offset + k;
    const std::uint8_t byte = k < 2 ? firstByte : secondByte;
    const int sample = decodeNibble(signedNibble(byte, (index & 1) != 0), header >> 4);
    const std::int16_t decoded = clampAndClip(
        applyFilter((header >> 2) & 0x03, sample, v.decodePrev1, v.decodePrev2));
    v.decodePrev2 = v.decodePrev1;
    v.decodePrev1 = decoded;
    v.pending[(v.pendingHead + v.pendingCount) % 12] = decoded;
    ++v.pendingCount;
  }
}

// Advances a voice's decode cursor by one sample; the DECODER — the read that
// fills the voice's sample buffer — runs a group of four samples ahead of it.
// When the cursor consumes a block's kDecoderLead-th sample, the decoder
// leaves that block for the next: the chain is resolved there — the following
// block, or for an end block the loop address read live from DIR and VxSRCN,
// which is how a DIR or VxSRCN change takes effect at the next loop and not
// before. Leaving a block whose header carries the end flag is what sets the
// voice's ENDX bit: the bit is staged when the decoder has decoded the end
// block through and jumps to the loop address, not when it enters the end
// block (`Order/endx after final brr decode` reads the bit clear at the
// cursor's seventh sample of the end block and set at its eighth), and it
// reaches the register at the voice's S7 slot (DspState::preparedEndx). The
// exhausted cursor follows the address the decoder resolved.
//
// The consumed sample comes from the pending ring, decoded ahead of time: the
// key-on primed the first three groups, and each group-aligned consume here
// schedules the decode of the group eight stream samples on — always inside
// the decoder's block, at in-block offset (index + 8) & 15 — which
// advanceVoiceStream performs at the voice's next sample, before the cursor
// moves again (a modulated step can cross two boundaries in one sample, and
// both groups are scheduled). The hardware decodes a group in its sample's V4 step and only
// then advances the position (Anomie's V4 order), so a crossing's group is
// read from RAM one sample after the crossing: spc_dsp6 `Order/pitch after
// brr` rewrites a moving voice's header shift sample by sample and reads,
// through the echo tape, that the group crossed into under one header takes
// the shift of the header standing one sample later. The ring therefore holds
// four to twelve decoded samples and RAM writes cannot reach the samples
// already in it. A state seeded mid-stream without priming has an empty ring
// and decodes at consumption, from the cursor's own block with the window as
// filter history — the pre-ring arithmetic, unreachable from a machine-driven
// voice.
//
// The decoder entering an End+Mute block (header code 1: end set, loop clear)
// has no further effect here: the silencing is the per-sample header check's,
// which reads the header standing at each sample's start — so it lands at the
// NEXT sample's check, one sample after the decoder moved in.
void decodeStreamSample(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                        std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (v.brrSampleIndex >= 16) {
    v.brrAddress = v.decoderAddress;
    v.brrSampleIndex = 0;
  }
  std::int16_t consumed;
  if (v.pendingCount == 0) {
    const std::uint8_t header = ram[v.brrAddress];
    const std::uint8_t byte =
        ram[static_cast<std::uint16_t>(v.brrAddress + 1 + v.brrSampleIndex / 2)];
    const int sample = decodeNibble(signedNibble(byte, (v.brrSampleIndex & 1) != 0),
                                    header >> 4);
    consumed = clampAndClip(
        applyFilter((header >> 2) & 0x03, sample, v.decodePrev1, v.decodePrev2));
  } else {
    // The group scheduled is always the one two on from the cursor's — the
    // in-group offset formula is the same for every index inside the group,
    // so both scheduling modes share it.
    const auto scheduleNextGroup = [&v, &ram]() {
      const auto offset = static_cast<std::uint8_t>(((v.brrSampleIndex & ~3) + 8) & 15);
      for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n)
        if (v.scheduledDecodes[n].address == v.decoderAddress &&
            v.scheduledDecodes[n].offset == offset)
          return;  // this group is already on the schedule
      if (v.scheduledDecodeCount < v.scheduledDecodes.size()) {
        // The group's header is read at the scheduling itself; the load slot
        // one sample on supplies the first data byte.
        v.scheduledDecodes[v.scheduledDecodeCount] =
            VoiceState::GroupDecode{.address = v.decoderAddress,
                                    .offset = offset,
                                    .header = ram[v.decoderAddress],
                                    .headerCaptured = true};
        ++v.scheduledDecodeCount;
      }
    };
    if (v.brrSampleIndex % 4 == kDecodeTriggerIndex) scheduleNextGroup();
    consumed = v.pending[v.pendingHead];
    v.pendingHead = static_cast<std::uint8_t>((v.pendingHead + 1) % 12);
    --v.pendingCount;
  }
  shiftWindow(v, consumed);
  ++v.brrSampleIndex;
  if (v.brrSampleIndex == kDecoderLead) {
    if ((ram[v.brrAddress] & 0x01) != 0) {
      // The loop jump takes the address the voice's directory slot read this
      // sample; a schedule without that slot reads the directory here.
      v.decoderAddress =
          v.loopPointerLoaded
              ? v.loopPointer
              : readBrrSource(ram, dsp[kDspDir], dsp[voiceRegister(voice, kVoiceSrcn)]).loop;
      dsp.preparedEndx |= static_cast<std::uint8_t>(1u << voice);
    } else {
      v.decoderAddress = static_cast<std::uint16_t>(v.brrAddress + 9);
    }
  }
}

// Finishes, at a voice's S4 slot, the group decode the compute halved this
// sample — its last two samples, the second data byte read from RAM now — and
// keeps anything the compute scheduled since for the next sample's load.
void performLoadedDecodes(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                          std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  std::uint8_t kept = 0;
  for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n) {
    if (v.scheduledDecodes[n].decodedSamples == 2) {
      decodeGroupAhead(dsp, ram, voice, v.scheduledDecodes[n], 2, 4);
    } else {
      v.scheduledDecodes[kept++] = v.scheduledDecodes[n];
    }
  }
  v.scheduledDecodeCount = kept;
}

// Advances a voice's stream by the samples this 32 kHz output sample passes: the
// counter's in-group position gains `step`, the sum clamps at
// kMaxGroupPosition, and every whole sample position the clamped sum crosses is
// decoded. The clamp is what bounds the advance at four source samples per
// output sample; a step that would carry the position past the ceiling leaves
// it at 3FFFh within the next group, where a later step of even 1 crosses the
// group boundary at once (`Misc/interp pos clamped at $7FFF`). Shared by
// stepVoice (which reports the interpolated result) and stepDspSample (which
// supplies a pitch-modulated step). The group decodes the previous advance's
// crossings scheduled run first, reading RAM now — the decode-then-advance
// order of the hardware's V4 step (see decodeStreamSample).
void advanceVoiceStream(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                        std::size_t voice, std::uint32_t step) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (!dsp.primed) {
    // The frame-at-once and single-voice paths have no slot schedule: decode
    // everything scheduled in full now, every byte read from RAM here.
    for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n)
      decodeGroupAhead(dsp, ram, voice, v.scheduledDecodes[n], 0, 4);
    v.scheduledDecodeCount = 0;
  } else {
    // The slot-scheduled path decodes the groups whose bytes this sample's
    // load slot captured — two load slots after their trigger. The ordinary
    // case is one group: its first two samples decode now, ahead of this
    // advance, and the S4 slot finishes it, reading the second data byte
    // there — the measured read slots — still ahead of anything the next
    // advance can consume even through the position clamp. A double
    // crossing's pair decodes in full now, in stream order; a group still
    // waiting for its load stays.
    std::uint8_t loaded = 0;
    for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n)
      if (v.scheduledDecodes[n].bytesLoaded && v.scheduledDecodes[n].decodedSamples == 0) ++loaded;
    if (loaded == 1) {
      for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n) {
        VoiceState::GroupDecode& g = v.scheduledDecodes[n];
        if (g.bytesLoaded && g.decodedSamples == 0) {
          decodeGroupAhead(dsp, ram, voice, g, 0, 2);
          g.decodedSamples = 2;
        }
      }
    } else if (loaded > 1) {
      std::uint8_t kept = 0;
      for (std::uint8_t n = 0; n < v.scheduledDecodeCount; ++n) {
        VoiceState::GroupDecode& g = v.scheduledDecodes[n];
        if (g.bytesLoaded && g.decodedSamples == 0) {
          decodeGroupAhead(dsp, ram, voice, g, 0, 4);
        } else {
          v.scheduledDecodes[kept++] = g;
        }
      }
      v.scheduledDecodeCount = kept;
    }
  }
  const std::uint32_t position = v.pitchCounter & kGroupPositionMask;
  std::uint32_t advanced = position + step;
  if (advanced > kMaxGroupPosition) advanced = kMaxGroupPosition;
  const std::uint32_t passed = (advanced >> 12) - (position >> 12);
  if (passed > v.pendingCount) {
    // A clamp-grade drain can reach a group's second pair one advance after
    // its scheduling, before the S4 slot decodes it. The hardware cannot
    // starve — its decode and advance share that slot, decode first — so the
    // outstanding halves are finished here, their second data byte read one
    // slot early, only when this advance would otherwise outrun the ring.
    performLoadedDecodes(dsp, ram, voice);
  }
  v.pitchCounter = static_cast<std::uint16_t>((v.pitchCounter & ~kGroupPositionMask) + advanced);
  for (std::uint32_t n = 0; n < passed; ++n) decodeStreamSample(dsp, ram, voice);
}

// Clamps a mix accumulator to signed 16 bits — the hardware clamps (never wraps)
// after each addition in the output sum.
[[nodiscard]] std::int32_t clampSigned16(std::int32_t value) noexcept {
  if (value > 0x7FFF) return 0x7FFF;
  if (value < -0x8000) return -0x8000;
  return value;
}

// The echo unit's per-channel FIR output, added into the main mix through EVOL.
struct EchoOutput {
  int left = 0;
  int right = 0;
};

// Reads one 16-bit little-endian word of a ring entry. Echo samples are 15 bits
// stored left-justified, so the arithmetic SAR 1 recovers the value the filter
// takes. The address wraps within the 64KB space.
std::int16_t readEchoWord(std::span<const std::uint8_t, 65536> ram, std::uint16_t entry,
                          int lowByte) noexcept {
  const auto at = [&](int offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(entry + offset);
  };
  const int word = static_cast<std::int16_t>(ram[at(lowByte)] | (ram[at(lowByte + 1)] << 8));
  return static_cast<std::int16_t>(word >> 1);
}

// Captures a run of FIR coefficients into the sample's own copies. Each lands at
// its own slot, so a CPU write reaches this sample's filter only while its
// coefficient is still ahead.
void latchFirCoefficients(DspState& dsp, std::size_t first, std::size_t count) noexcept {
  for (std::size_t tap = first; tap < first + count; ++tap)
    dsp.echoFirCoeff[tap] = static_cast<std::int8_t>(dsp[voiceRegister(tap, kFirCoeff)]);
}

// Slot T23: the left channel's word comes out of the ring, and FIR0 with it. The
// entry this sample reads and writes is fixed here — the ring index only moves at
// T31, so the right read, the feedback and the write-back all address it.
void loadEchoLeft(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                  std::uint8_t baseEsa) noexcept {
  const std::uint16_t base = static_cast<std::uint16_t>(baseEsa * 0x100);
  dsp.echoWriteEntry = static_cast<std::uint16_t>(base + dsp.echoIndex * 4);
  dsp.echoFirLeft[dsp.echoFirPos] = readEchoWord(ram, dsp.echoWriteEntry, 0);
  latchFirCoefficients(dsp, 0, 1);
}

// Slot T24: the right channel's word, and FIR1 and FIR2.
void loadEchoRight(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept {
  dsp.echoFirRight[dsp.echoFirPos] = readEchoWord(ram, dsp.echoWriteEntry, 2);
  latchFirCoefficients(dsp, 1, 2);
}

// Slot T26: the last two coefficients arrive and the filter runs, leaving each
// channel's output for the output slots and the feedback to take.
//
// FIR: taps oldest*FIR0 ... newest*FIR7, each product SAR 6. The first seven
// additions wrap at 16 bits; only the final (newest) addition saturates. The
// newest history slot is echoFirPos, so tap k reads slot (echoFirPos+1+k) & 7.
// The output is a 15-bit sample left-aligned in 16 bits, so the sum's low bit is
// dropped before EVOL and EFB read it. The two channels filter separately with
// the same coefficients.
//
// Each product is itself a 16-bit value and wraps. A coefficient of -128 against
// a sample of -4000h gives +8000h, one past the range, and the wrap makes it
// -8000h. Only the saturating final add can tell the two apart: for the seven
// wrapping adds, truncating the product and truncating the running sum are the
// same arithmetic modulo 2^16.
void filterEcho(DspState& dsp) noexcept {
  latchFirCoefficients(dsp, 6, 2);
  const auto filter = [&](const std::array<std::int16_t, 8>& history) -> int {
    int sum = 0;
    for (int tap = 0; tap < 8; ++tap) {
      const std::uint8_t slot = static_cast<std::uint8_t>((dsp.echoFirPos + 1 + tap) & 7);
      const int product = static_cast<std::int16_t>(
          (history[slot] * dsp.echoFirCoeff[static_cast<std::size_t>(tap)]) >> 6);
      sum = tap < 7 ? static_cast<std::int16_t>(sum + product) : clampSigned16(sum + product);
    }
    return sum & ~1;
  };
  dsp.echoFirOutLeft = filter(dsp.echoFirLeft);
  dsp.echoFirOutRight = filter(dsp.echoFirRight);

  // Advance the FIR history cursor. The ring index advances at T31, not here.
  dsp.echoFirPos = static_cast<std::uint8_t>((dsp.echoFirPos + 1) & 7);
}

// Slot T27: EFB is read and the write-back value formed — the EON send
// (sendLeft/sendRight, the EON-enabled voices' post-VxVOL sums) plus fir*EFB
// SAR 7, clamped, bit 0 cleared, because the entry holds a 15-bit sample and the
// volume multiply can reintroduce the low bit.
//
// The volume multiply is 16 bits wide and wraps; the add onto the send is what
// saturates. EFB 80h against a FIR output of -8000h gives +8000h, which the wrap
// takes to -8000h where a saturating product would give 7FFFh.
//
// The bytes do not land here: the buffer writes have their own slots — the left
// word at T30, the right word at T31 — so the value is staged and the slot runner
// (or the frame-at-once caller) performs the write when its slot arrives, each
// word under the FLG bit-5 gate loaded one slot before it. A read-only caller
// (echoRam null) stages nothing and sees the static buffer FLG bit 5 produces.
void stageEchoWrite(DspState& dsp, std::uint8_t* echoRam, int sendLeft, int sendRight) noexcept {
  if (echoRam == nullptr) return;
  const int efb = static_cast<std::int8_t>(dsp[kDspEfb]);
  const int writeLeft =
      clampSigned16(sendLeft + static_cast<std::int16_t>((dsp.echoFirOutLeft * efb) >> 7)) & ~1;
  const int writeRight =
      clampSigned16(sendRight + static_cast<std::int16_t>((dsp.echoFirOutRight * efb) >> 7)) & ~1;
  dsp.echoWritePending = true;
  dsp.echoWriteBytes[0] = static_cast<std::uint8_t>(writeLeft & 0xFF);
  dsp.echoWriteBytes[1] = static_cast<std::uint8_t>((writeLeft >> 8) & 0xFF);
  dsp.echoWriteBytes[2] = static_cast<std::uint8_t>(writeRight & 0xFF);
  dsp.echoWriteBytes[3] = static_cast<std::uint8_t>((writeRight >> 8) & 0xFF);
}

// Runs the echo unit's read-and-filter half of one 32 kHz sample in one act, for
// the frame-at-once path that has no slots to spread it over, and returns the FIR
// output for the two channels. The slot-scheduled path calls the same steps at
// the slots they belong to (T23, T24, T25, T26, T27).
EchoOutput stepEcho(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                    std::uint8_t* echoRam, std::uint8_t baseEsa, int sendLeft,
                    int sendRight) noexcept {
  loadEchoLeft(dsp, ram, baseEsa);
  loadEchoRight(dsp, ram);
  latchFirCoefficients(dsp, 3, 3);
  filterEcho(dsp);
  stageEchoWrite(dsp, echoRam, sendLeft, sendRight);
  return EchoOutput{.left = dsp.echoFirOutLeft, .right = dsp.echoFirOutRight};
}

// The end-of-sample echo ring advance (slot T31): applies the ESA and EDL values
// loaded at T30, then steps the ring index. EDL is applied only when the index is
// 0 — the entry count is EDL<<9, and a count of 0 collapses to the single-entry
// buffer — so a mid-ring EDL change waits for the next wrap, while the applied
// base is what the following sample's read and write address. A CPU write to ESA
// therefore reaches the buffer one sample after the write (anomie's access chart,
// cycles 29-30; spc_dsp6 `Misc/$F0-$FF are not ram` flips ESA mid-ring and its
// echo burst is calibrated to the in-flight sample landing at the old base).
void advanceEchoRing(DspState& dsp, std::uint8_t rawEsa, std::uint8_t rawEdl) noexcept {
  dsp.echoAppliedEsa = rawEsa;
  if (dsp.echoIndex == 0)
    dsp.echoLength = static_cast<std::uint16_t>((rawEdl & 0x0F) << 9);
  dsp.echoIndex = static_cast<std::uint16_t>(dsp.echoIndex + 1);
  if (dsp.echoIndex >= dsp.echoLength) dsp.echoIndex = 0;
}

}  // namespace

static void runEnvelopeMode(DspState& dsp, std::size_t voice) noexcept;
static void keyOnVoiceImpl(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                           std::size_t voice, bool primeNow) noexcept;

BrrSource readBrrSource(std::span<const std::uint8_t, 65536> ram, std::uint8_t dir,
                        std::uint8_t srcn) noexcept {
  const int base = dir * 0x100 + srcn * 4;
  auto at = [&](int offset) -> std::uint16_t {
    return ram[static_cast<std::uint16_t>(base + offset)];
  };
  return BrrSource{
      .start = static_cast<std::uint16_t>(at(0) | (at(1) << 8)),
      .loop = static_cast<std::uint16_t>(at(2) | (at(3) << 8)),
  };
}

BrrBlock decodeBrrBlock(std::span<const std::uint8_t, 9> block, std::int16_t old,
                        std::int16_t older) noexcept {
  const std::uint8_t header = block[0];
  const int shift = header >> 4;
  const int filter = (header >> 2) & 0x03;

  BrrBlock out;
  out.endBlock = (header & 0x01) != 0;
  out.loopBlock = (header & 0x02) != 0;

  int recent = old;     // S(x-1) entering each sample
  int earlier = older;  // S(x-2)
  for (int i = 0; i < 16; ++i) {
    const std::uint8_t byte = block[1 + i / 2];
    const int sample = decodeNibble(signedNibble(byte, (i & 1) != 0), shift);
    const std::int16_t decoded = clampAndClip(applyFilter(filter, sample, recent, earlier));
    out.samples[static_cast<std::size_t>(i)] = decoded;
    earlier = recent;
    recent = decoded;
  }
  out.last = static_cast<std::int16_t>(recent);
  out.prev = static_cast<std::int16_t>(earlier);
  return out;
}

std::int16_t gaussInterpolate(SampleWindow window, std::uint8_t index) noexcept {
  const int i = index;
  const auto tap = [&](int entry, std::int16_t sample) {
    return (kGaussTable[static_cast<std::size_t>(entry)] * sample) >> 10;
  };
  // The first value cannot exceed 16 bits, and the first addition cannot
  // overflow them. The second addition has no overflow handling — it wraps,
  // reproducing the documented bug the table's 801h rows expose. The third
  // addition saturates.
  int out = tap(0x0FF - i, window.oldest);
  out = static_cast<std::int16_t>(out + tap(0x1FF - i, window.older));
  out = static_cast<std::int16_t>(out + tap(0x100 + i, window.old));
  out = out + tap(0x000 + i, window.newest);
  if (out > 0x7FFF) out = 0x7FFF;
  if (out < -0x8000) out = -0x8000;
  return static_cast<std::int16_t>(out >> 1);
}

// Wipes a voice's stream state for a key-on, reading no RAM: the addresses
// are taken when the start pointer is read (loadStartPointer) and the ring
// filled when the stream is primed (primeVoiceStream).
static void resetVoiceForStart(DspState& dsp, std::size_t voice) noexcept {
  dsp.voices[voice] = VoiceState{};
}

// Reads a keyed voice's start pointer from its directory entry. On the slot
// schedule this runs at the voice's directory slot of the sample after the
// key-on's consuming compute — T22 for voice 0, T(3v-2) for voices 1-7 — the
// slot `Timing/Voice/V2 dir.start.lsb`/`.msb` measure: a five-cycle pulse into
// either start byte is caught only when it covers that slot. The entry is
// selected by the source slot's `VxSRCN` capture (loadSourceNumber); its loop
// address is the directory slot's own per-sample read (loadLoopPointer).
static void loadStartPointer(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                             std::size_t voice) noexcept {
  const std::uint16_t start =
      readBrrSource(ram, dsp.latchedDir, voiceSourceNumber(dsp, voice)).start;
  VoiceState& v = dsp.voices[voice];
  v.brrAddress = start;
  v.decoderAddress = start;
  v.headerAddress = start;
}

// Primes a keyed voice's stream from its start block: decodes the block's
// first three groups — twelve samples — into the ring, reading their bytes
// from RAM now, and shifts the first four into the window. A write to those
// bytes after this read does not reach the primed samples (`Misc/brr not
// always decoding` rewrites a parked voice's block and the voice still plays
// all twelve when it moves). On the slot schedule this runs at the voice's
// BRR load slot of the sample the start pointer was read in.
static void primeVoiceStream(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                             std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  for (int group = 0; group < 3; ++group)
    decodeGroupAhead(dsp, ram, voice,
                     VoiceState::GroupDecode{.address = v.decoderAddress,
                                             .offset = static_cast<std::uint8_t>(group * 4)},
                     0, 4);
  for (int n = 0; n < 4; ++n) {
    shiftWindow(v, v.pending[v.pendingHead]);
    v.pendingHead = static_cast<std::uint8_t>((v.pendingHead + 1) % 12);
    --v.pendingCount;
  }
  v.brrSampleIndex = 4;
  // Priming leaves ENDX alone: the bit is set when the decoder LEAVES an
  // end block (decodeStreamSample), so a start block carrying the end flag
  // sets it four cursor samples on, when the decoder resolves its loop.
}

void startVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept {
  // The single-voice call has no T29 to read the directory register at, so it
  // reads it here and the pointer read below takes that value.
  loadVoiceWideRegisters(dsp);
  resetVoiceForStart(dsp, voice);
  loadStartPointer(dsp, ram, voice);
  primeVoiceStream(dsp, ram, voice);
}

std::int16_t stepVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                       std::size_t voice) noexcept {
  advanceVoiceStream(dsp, ram, voice, voicePitch(dsp, voice));
  // The single-voice call has no slot schedule to stage against: an end-block
  // exit's ENDX set is readable as soon as the call returns.
  dsp[kDspEndx] |= dsp.preparedEndx;
  dsp.preparedEndx = 0;
  return interpolatedSample(dsp, voice);
}

std::int16_t interpolatedSample(const DspState& dsp, std::size_t voice) noexcept {
  const VoiceState& v = dsp.voices[voice];
  return gaussInterpolate(v.window, static_cast<std::uint8_t>((v.pitchCounter >> 4) & 0xFF));
}

std::uint16_t nextGlobalCounter(std::uint16_t counter) noexcept {
  return counter == 0 ? kGlobalCounterReload : static_cast<std::uint16_t>(counter - 1);
}

bool envelopeRateFires(std::uint16_t counter, std::uint8_t rate) noexcept {
  const int period = kEnvelopePeriod[rate & 0x1F];
  if (period == 0) return false;  // rate 0 (Infinite) never fires
  const int offset = kEnvelopeOffset[rate & 0x1F];
  return (counter + offset) % period == 0;
}

void tickDspSample(DspState& dsp) noexcept {
  dsp.globalCounter = nextGlobalCounter(dsp.globalCounter);
  ++dsp.sampleIndex;
}

// One voice's per-sample body: advance it by one 32 kHz output sample and return
// its enveloped amplitude — the internal -4000h..+3FFFh value VxOUTX reports and
// the next voice's PMON reads one sample later. `prevAmplitude` is the previous
// voice's amplitude from the previous sample (for pitch modulation); `softReset`
// is FLG bit 7. Writes VxOUTX;
// advances the stream and the envelope (or the key-on countdown). Extracted so the
// frame-at-once path and the per-slot schedule compute a voice bit-for-bit alike.
static int computeVoiceAmplitude(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                 std::size_t voice, int prevAmplitude, bool softReset) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (v.computesSinceKeyOn != 0xFF) ++v.computesSinceKeyOn;
  const std::uint32_t step = pitchStep(dsp, voice, prevAmplitude);

  // The header byte is loaded from RAM every sample, whatever the pitch counter is
  // doing, and its end/loop bits are read the same sample. So a block that turns
  // End+Mute under a voice whose stream has stopped still releases it. This check
  // is the envelope's ONLY End+Mute source, and it reads headerAddress — the
  // decoder's block as of the voice's live samples, one sample behind them —
  // so a startup whose decoder crossed into an End+Mute block is seen here
  // only from the third live sample's check (see VoiceState::headerAddress
  // for the rule and what pins it).
  //
  // The check goes live two sample periods after the keying poll loads a key-on.
  // With the load preceding voice 0's compute in the slot they share, the load's
  // slot is every voice's reference compute and the cutoff is a uniform count:
  // live from the third compute after (and including) the load's own sample.
  // The counter stops at the sixth compute, which is past it for every voice.
  const int computeSinceKeyOn = kKeyOnStartupCalls + 1 - v.konDelay;
  const bool headerEndMute = computeSinceKeyOn >= 3 &&
                             headerIsEndMute(v.headerLoaded ? v.loadedHeader
                                                            : ram[v.headerAddress]);

  // A key-on the keying poll consumed THIS sample shields its voice from the
  // sample's soft reset: the fresh consumption wins, exactly as KON applied
  // after KOFF wins at the poll itself, and the startup proceeds as if the
  // reset were not standing. One sample later the reset arm below keys the
  // voice off like any other (spc_dsp6 `KON/kon then flg.80`: a one-sample
  // FLG bit-7 pulse over the consuming poll's sample leaves the startup's
  // ENVX schedule untouched; the same pulse one sample later — or anywhere
  // else in or past the countdown — silences the voice for good). The
  // consumption sample is the only one that can see the countdown still at
  // its armed value: every later compute has decremented it.
  const bool keyOnConsumedThisSample = v.konDelay == kKeyOnStartupCalls;

  int amplitude;
  if (v.restartPending) {
    // The final pre-key-on sample. The keying poll consumed a full restart for
    // this voice and armed its countdown, but the stream, envelope and phase
    // still stand: this compute advances the old stream once more — the final
    // pre-key-on decode — and emits its sample under the standing envelope,
    // and only then applies the restart. The countdown ticks on this very
    // call, so the ENVX schedule is exactly the armed key-on's; the old
    // decode's ENDX side effect is erased by the key-on's clear at the voice's
    // S7 slot, which is the suppression `KON/kon stops endx of prev sample`
    // measures. A standing
    // soft reset does not reach this branch — the fresh consumption wins, as
    // at the poll itself (`KON/kon then flg.80`). Measured against `KON/kon
    // unaffected by pitch`: a re-key on a sounding voice emits the old data
    // once more on the consuming sample, at every pitch.
    v.restartPending = false;
    advanceVoiceStream(dsp, ram, voice, step);
    const bool noise = ((dsp.latchedNon >> voice) & 1) != 0;
    const int sample = noise ? dsp.noiseLevel : interpolatedSample(dsp, voice);
    amplitude = (sample * static_cast<int>(v.envelope)) >> 11;
    // The standing envelope takes this sample's own update before the restart
    // reads it — a header standing End+Mute kills it, otherwise the selected
    // mode runs once — so whether the startup walks or holds is decided by
    // the level the voice would have carried into the next sample, not the
    // one it emitted with. `Order/endx after final brr decode` re-keys its
    // sync gadget's voice 4 with the gadget's direct-gain-0 restore landing
    // on the compute before the poll: the gadget's full level stands at the
    // emission and is 0 by the restart, and the ROM's row for that voice is a
    // held startup's, identical to the seven fresh key-ons.
    if (headerEndMute) {
      v.envelope = 0;
    } else {
      runEnvelopeMode(dsp, voice);
    }
    // The restart proper: wipe and re-prime the stream, envelope from 0,
    // Attack, ENDX cleared. The counter and the capture hold carry the values
    // the poll left (keyOnVoice resets them for its direct callers), and the
    // armed countdown takes this call as the startup's first.
    // On the slot schedule the start pointer is read at the voice's directory
    // slot of the NEXT sample and the stream primed at that sample's load
    // slot, ahead of its compute; the frame-at-once path reads both here.
    const std::uint8_t count = v.computesSinceKeyOn;
    const std::uint8_t hold = v.pitchCaptureHold;
    keyOnVoiceImpl(dsp, ram, voice, !dsp.primed);
    v.computesSinceKeyOn = count;
    v.pitchCaptureHold = hold;
    --v.konDelay;
    dsp[voiceRegister(voice, kVoiceEnvx)] = 0;
  } else if (softReset && !keyOnConsumedThisSample) {
    // FLG bit 7 keys every voice off and forces its envelope to 0 each sample. BRR
    // decoding keeps running (ENDX and loop transitions still fire). The reset
    // is read AFTER the sample's amplitude is formed, so a live voice still
    // emits this sample under the level it carried in; the zeroed level is
    // what the next sample scales by, the same one-sample lag every envelope
    // update has (Anomie's V3c: the envelope is applied, VxOUTX formed, and
    // FLG bit 7 checked before the update). spc_dsp6's `Order/flg.80 after
    // env used` measures it on the echo tape: a voice keyed on and reset nine
    // samples later writes one more full-scale frame than a model that
    // silences the reset sample itself. A voice still inside its startup
    // countdown emits silence either way.
    v.phase = EnvPhase::Release;
    if (v.konDelay > 0) {
      --v.konDelay;
      amplitude = 0;
    } else {
      advanceVoiceStream(dsp, ram, voice, step);
      v.headerAddress = v.decoderAddress;
      const bool noise = ((dsp.latchedNon >> voice) & 1) != 0;
      const int sample = noise ? dsp.noiseLevel : interpolatedSample(dsp, voice);
      amplitude = (sample * static_cast<int>(v.envelope)) >> 11;
    }
    v.envelope = 0;
    dsp[voiceRegister(voice, kVoiceEnvx)] = 0;
  } else if (v.konDelay > 0) {
    // Startup: the voice outputs silence, and whether its stream advances is
    // the key-on's walk split (VoiceState::startupWalks). A walking startup —
    // a young sounding voice re-keyed — advances at the pitch, except on the first
    // startup call, which decodes nothing (the start pointer is read and the
    // stream primed at the next sample's directory and load slots);
    // measured against spc_dsp6's `KON/kon decoding when another kon`, which
    // freezes the pitch mid-startup of a re-keyed sounding voice and reads
    // where the cursor stood — both published references hold every startup's
    // stream still, which that ROM refutes for this case. A held startup — a
    // silent voice keyed on — stands at its primed start, exactly as those
    // references describe (`Misc/brr addr wrap-around`). The envelope holds
    // through the countdown either way (stepVoiceEnvelope decrements it), and
    // the header check above still runs. The first-call test reads the
    // compute count, not the countdown alone, because an in-span re-key
    // reloads the countdown without re-priming the stream — that reload's next
    // call advances as normal (`KON/kon then another kon`).
    if (v.startupWalks && (v.konDelay < kKeyOnStartupCalls || v.computesSinceKeyOn != 1))
      advanceVoiceStream(dsp, ram, voice, step);
    if (keyOnPinsFraction(v)) v.pitchCounter &= 0xF000;
    stepVoiceEnvelope(dsp, voice, headerEndMute);
    amplitude = 0;
  } else {
    // The envelope's End+Mute input was read above, before this advance — the
    // hardware checks the header early in the voice's sample and decodes after
    // (Anomie's V3c before V4) — and headerAddress takes the decoder's block
    // only now that the check has passed. So the decoder entering an End+Mute
    // block sets ENDX at once but silences only at the next sample's check,
    // and a startup whose decoder crossed during the countdown silences after
    // its first live steps. The gap is audible: those steps publish their
    // level, which is what spc_dsp6's shared sync spins on (`KON/kon when
    // prev sample at end` races a stream into an End+Mute block mid-startup,
    // then waits for VxENVX != 0; on hardware the wait exits).
    // A non-walking startup's hold extends through this, the first live
    // compute: the sample interpolates the primed stream's first four samples
    // at fraction 0, and advancing begins the sample after.
    if (v.startupWalks || !keyOnPinsFraction(v)) advanceVoiceStream(dsp, ram, voice, step);
    if (keyOnPinsFraction(v)) v.pitchCounter &= 0xF000;
    // The check's view stands one sample longer than the countdown: the first
    // live compute's advance does not reach the check until the SECOND live
    // sample, so a walking startup whose decoder crossed into an End+Mute
    // block on that advance still publishes its first level before the check
    // lands (`KON/kon as prev sample ends` reads that level after every one
    // of its swept key-ons).
    if (v.computesSinceKeyOn != kKeyOnSilentCalls) v.headerAddress = v.decoderAddress;
    // A voice whose NON bit is set outputs the shared noise level in place of its
    // interpolated BRR sample; the stream's advance above is untouched by the
    // substitution, so decoding and ENDX are unaffected.
    const bool noise = ((dsp.latchedNon >> voice) & 1) != 0;
    const int sample = noise ? dsp.noiseLevel : interpolatedSample(dsp, voice);
    // The level scaling this sample is the one already standing; the update below
    // is what the next sample reads. So a voice leaving its startup samples emits
    // one more silent sample as its envelope begins moving, and a level in motion
    // reaches the output a sample after the register driving it is read.
    const auto envelope = static_cast<int>(v.envelope);
    amplitude = (sample * envelope) >> 11;
    stepVoiceEnvelope(dsp, voice, headerEndMute);
  }

  // VxOUTX returns the high byte of the 15-bit amplitude (-128..+127).
  dsp[voiceRegister(voice, kVoiceOutx)] = static_cast<std::uint8_t>((amplitude >> 7) & 0xFF);
  return amplitude;
}

// Folds one voice's amplitude into the running left mix and the EON echo send
// through its left volume (the S4 slot's work), clamping to signed 16 bits.
static void applyVoiceLeft(DspState& dsp, std::size_t voice) noexcept {
  const int vol = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolLeft)]);
  const int send = (dsp.voiceAmplitude[voice] * vol) >> 6;
  dsp.mixLeft = clampSigned16(dsp.mixLeft + send);
  if (((dsp.latchedEon >> voice) & 1) != 0)
    dsp.echoSendLeft = clampSigned16(dsp.echoSendLeft + send);
}

// The S5 slot's work — the same fold for the right channel.
static void applyVoiceRight(DspState& dsp, std::size_t voice) noexcept {
  const int vol = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolRight)]);
  const int send = (dsp.voiceAmplitude[voice] * vol) >> 6;
  dsp.mixRight = clampSigned16(dsp.mixRight + send);
  if (((dsp.latchedEon >> voice) & 1) != 0)
    dsp.echoSendRight = clampSigned16(dsp.echoSendRight + send);
}

// The frame-at-once pipeline, run for a freshly seeded (unprimed) DspState's first
// sample. An unprimed state has no prepared voice-0 output, so its first sample
// reads every register at once and every voice in-frame — byte-identical to a
// machine with no intra-sample schedule — then primes the schedule for the samples
// that follow. echoRam is the machine RAM the echo unit writes into, or null for a
// read-only caller (the FLG bit 5 case).
static StereoFrame stepDspSampleAtomic(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                       std::uint8_t* echoRam,
                                       bool processEcho) noexcept {
  // A freshly seeded state holds no pitch captures yet: take both stages from
  // the registers as they stand, so this frame reads the same values a live
  // register read would — byte-identical to the frame-at-once model.
  for (std::size_t v = 0; v < 8; ++v)
    dsp.pitchLatch[v] = static_cast<std::uint16_t>(voicePitch(dsp, v));
  // The voice-wide registers likewise: this frame has no T28/T29 to read them
  // at, so it takes them as they stand.
  latchGlobalRegisters(dsp);
  pollKeying(dsp, ram);

  const std::uint8_t flg = dsp[kDspFlg];
  const bool softReset = (flg & kFlgSoftReset) != 0;

  std::int32_t left = 0;
  std::int32_t right = 0;
  std::int32_t echoLeft = 0;
  std::int32_t echoRight = 0;
  for (std::size_t voice = 0; voice < 8; ++voice) {
    // Pitch modulation reads the previous voice's amplitude from the previous
    // sample, exactly as the slot schedule does.
    const int prevAmplitude = dsp.modulatorAmplitude[(voice + 7) & 7];
    dsp.modulatorAmplitude[voice] = dsp.voiceAmplitude[voice];
    const int amplitude = computeVoiceAmplitude(dsp, ram, voice, prevAmplitude, softReset);
    // Record each voice's amplitude: voice 0's becomes the value the following
    // slot-scheduled frame applies (its output rides one frame behind), so the
    // first sample's voice-0 advance is not repeated. Seed the prepared OUTX/ENVX
    // too, so the first slot-scheduled frame's visibility slots publish this
    // sample's values rather than an empty scratch.
    dsp.voiceAmplitude[voice] = amplitude;
    dsp.preparedOutx[voice] = dsp[voiceRegister(voice, kVoiceOutx)];
    dsp.preparedEnvx[voice] = dsp[voiceRegister(voice, kVoiceEnvx)];
    const int volLeft = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolLeft)]);
    const int volRight = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolRight)]);
    const int sendLeft = (amplitude * volLeft) >> 6;
    const int sendRight = (amplitude * volRight) >> 6;
    left = clampSigned16(left + sendLeft);
    right = clampSigned16(right + sendRight);
    if (((dsp.latchedEon >> voice) & 1) != 0) {
      echoLeft = clampSigned16(echoLeft + sendLeft);
      echoRight = clampSigned16(echoRight + sendRight);
    }
  }
  // Voice 0's next compute is at T31 of the following frame, after voice 1's
  // S3 has read the modulator there; this sample's amplitude is what that read
  // must see, so it is published once every voice of this frame has read.
  dsp.modulatorAmplitude[0] = dsp.voiceAmplitude[0];

  const int mvolLeft = static_cast<std::int8_t>(dsp[kDspMvolLeft]);
  const int mvolRight = static_cast<std::int8_t>(dsp[kDspMvolRight]);
  left = static_cast<std::int16_t>((left * mvolLeft) >> 7);
  right = static_cast<std::int16_t>((right * mvolRight) >> 7);

  // The frame-at-once path has no slot interleave, so the echo unit reads the
  // live ESA, the write lands within the call under the live FLG bit-5 gate, and
  // the ring advance applies the live registers — which warms the applied base
  // for the slot-scheduled samples that follow the seed frame.
  EchoOutput echo{};
  if (processEcho) {
    echo = stepEcho(dsp, ram, echoRam, dsp[kDspEsa], echoLeft, echoRight);
    if (dsp.echoWritePending && echoRam != nullptr &&
        (dsp[kDspFlg] & kFlgEchoWriteDisable) == 0) {
      for (int b = 0; b < 4; ++b)
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + b)] = dsp.echoWriteBytes[b];
    }
    advanceEchoRing(dsp, dsp[kDspEsa], dsp[kDspEdl]);
  }
  dsp.echoWritePending = false;
  const int evolLeft = static_cast<std::int8_t>(dsp[kDspEvolLeft]);
  const int evolRight = static_cast<std::int8_t>(dsp[kDspEvolRight]);
  left = clampSigned16(left + static_cast<std::int16_t>((echo.left * evolLeft) >> 7));
  right = clampSigned16(right + static_cast<std::int16_t>((echo.right * evolRight) >> 7));

  if ((flg & kFlgMute) != 0) {
    left = 0;
    right = 0;
  }

  if (envelopeRateFires(dsp.globalCounter, flg & kFlgNoiseRate))
    dsp.noiseLevel = nextNoiseLevel(dsp.noiseLevel);
  // The frame-at-once sample has no slots for a staged ENDX set or a scheduled
  // clear to land on: every voice's clear applies and every set is readable
  // when the sample is complete.
  dsp[kDspEndx] &= static_cast<std::uint8_t>(~dsp.pendingEndxClear);
  dsp.preparedEndx &= static_cast<std::uint8_t>(~dsp.pendingEndxClear);
  dsp.pendingEndxClear = 0;
  dsp[kDspEndx] |= dsp.preparedEndx;
  dsp.preparedEndx = 0;
  tickDspSample(dsp);
  return StereoFrame{.left = static_cast<std::int16_t>(left),
                     .right = static_cast<std::int16_t>(right)};
}

// Whether a CPU write stamped `stamp` was issued in the two cycles before the
// cycle now running — the window in which it survives the DSP's own write to
// the same register (see DspState::cycleCount).
static bool cpuWriteStands(const DspState& dsp, std::uint64_t stamp) noexcept {
  return stamp != kNoCpuWrite && dsp.cycleCount >= stamp && dsp.cycleCount - stamp <= 2;
}

// The per-voice schedule. s3Slot is where a voice runs its whole compute body;
// s4Slot/s5Slot are where its left/right volume folds into the mix. Voice 0's
// compute sits at slot T31 and feeds the following frame's output at T0..T5 — the
// one-update lag the S-DSP's envelope pipeline produces.
// The visibility slots: a voice's ENDX set becomes readable at S7, VxOUTX at S8,
// VxENVX at S9 — three, four and five slots after the compute, so voice 0's
// land in the following sample.

// Computes voice `voice` at its S3 slot, storing the amplitude the S4/S5 slots
// apply. voice n's pitch modulation reads voice n-1's amplitude from the
// PREVIOUS sample — the value standing before voice n-1's compute three slots
// earlier in this sample replaced it (voice 0 reads voice 7's; it does not
// modulate, so the value is inert). The voice's own standing amplitude moves to
// modulatorAmplitude as it is replaced, which is what the following voice reads.
// VxOUTX and VxENVX are computed here but held back from the register file until
// the voice's S8/S9 slots — a CPU read before then sees the previous sample's
// value, and a CPU write up to two cycles before the slot outlives the slot's
// own write (see DspState::cycleCount).
static void computeVoiceSlot(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                             std::size_t voice, bool softReset) noexcept {
  const int prev = dsp.modulatorAmplitude[(voice + 7) & 7];
  const std::uint8_t heldOutx = dsp[voiceRegister(voice, kVoiceOutx)];
  const std::uint8_t heldEnvx = dsp[voiceRegister(voice, kVoiceEnvx)];
  dsp.modulatorAmplitude[voice] = dsp.voiceAmplitude[voice];
  dsp.voiceAmplitude[voice] = computeVoiceAmplitude(dsp, ram, voice, prev, softReset);
  // The pair this sample's pitch slots read is the next advance's step: the
  // hardware advances after the output is formed, one step after the read
  // (see DspState::pitchLatch). A key-on's hold on those reads counts down
  // one compute at a time.
  VoiceState& v = dsp.voices[voice];
  if (v.pitchPendingValid) {
    dsp.pitchLatch[voice] = v.pitchPending;
    v.pitchPendingValid = false;
  }
  if (v.pitchCaptureHold > 0) --v.pitchCaptureHold;
  dsp.preparedOutx[voice] = dsp[voiceRegister(voice, kVoiceOutx)];
  dsp.preparedEnvx[voice] = dsp[voiceRegister(voice, kVoiceEnvx)];
  dsp[voiceRegister(voice, kVoiceOutx)] = heldOutx;
  dsp[voiceRegister(voice, kVoiceEnvx)] = heldEnvx;
}

// Finalizes the left output (slot T27): the dry mix scaled by MVOLL, the echo FIR
// output added through EVOLL, and the mute gate — the same arithmetic the
// frame-at-once path runs, split by channel so MVOLL is consumed one slot before
// MVOLR. Both volume multiplies are 16 bits wide and wrap; only the add onto the
// dry mix saturates.
static void finalizeLeft(DspState& dsp) noexcept {
  const int mvol = static_cast<std::int8_t>(dsp[kDspMvolLeft]);
  const int evol = static_cast<std::int8_t>(dsp[kDspEvolLeft]);
  std::int32_t out = static_cast<std::int16_t>((dsp.mixLeft * mvol) >> 7);
  out = clampSigned16(out + static_cast<std::int16_t>((dsp.echoFirOutLeft * evol) >> 7));
  if ((dsp[kDspFlg] & kFlgMute) != 0) out = 0;
  dsp.slotFrame.left = static_cast<std::int16_t>(out);
}

// Finalizes the right output (slot T28): MVOLR, EVOLR and the mute gate.
static void finalizeRight(DspState& dsp) noexcept {
  const int mvol = static_cast<std::int8_t>(dsp[kDspMvolRight]);
  const int evol = static_cast<std::int8_t>(dsp[kDspEvolRight]);
  std::int32_t out = static_cast<std::int16_t>((dsp.mixRight * mvol) >> 7);
  out = clampSigned16(out + static_cast<std::int16_t>((dsp.echoFirOutRight * evol) >> 7));
  if ((dsp[kDspFlg] & kFlgMute) != 0) out = 0;
  dsp.slotFrame.right = static_cast<std::int16_t>(out);
}

// Runs one 32-clock slot of a primed sample. Each register read lands on its
// documented slot, so a mid-frame DSPDATA write is seen only if it precedes its
// consuming slot; every no-write sample reproduces the frame-at-once output for
// voices 1-7, and voice 0 rides one update behind.
template<std::uint8_t slot, bool processEcho>
static void runPrimedSlot(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                          std::uint8_t* echoRam) noexcept {
  const bool softReset = (dsp[kDspFlg] & kFlgSoftReset) != 0;

  if (slot == 0) {
    // Frame start: clear the mix the previous frame delivered, then apply voice 0's
    // amplitude — prepared at the last frame's T31 (the pipeline lag). Take a
    // key-on's scheduled pitch capture (see DspState::pitchLatch); a live
    // voice's pair is read at its own pitch slots later in the sample.
    dsp.mixLeft = 0;
    dsp.mixRight = 0;
    dsp.echoSendLeft = 0;
    dsp.echoSendRight = 0;
    for (std::size_t v = 0; v < 8; ++v) {
      const std::uint8_t bit = static_cast<std::uint8_t>(1u << v);
      if ((dsp.pitchReloadPending & bit) != 0 && dsp.sampleIndex % 2 == 0) {
        // The capture a consumed key-on scheduled: on the poll-parity sample
        // following the poll, read the register pair — voice 0's compute takes
        // it this sample, voices 1-7's the next (their pending pair lands
        // once this sample's compute has run, as a slot read's does).
        const auto pair = static_cast<std::uint16_t>(voicePitch(dsp, v));
        if (v == 0) {
          dsp.pitchLatch[v] = pair;
        } else {
          dsp.voices[v].pitchPending = pair;
          dsp.voices[v].pitchPendingValid = true;
        }
        dsp.pitchReloadPending = static_cast<std::uint8_t>(dsp.pitchReloadPending & ~bit);
      }
    }
  }

  // Resolve each scheduled voice directly; no eight-voice scans per slot.
  if (slot <= 21 && slot % 3 == 0)
    loadSourceNumber(dsp, static_cast<std::size_t>((slot / 3 + 2) & 7));
  if (slot >= 1 && slot <= 22 && slot % 3 == 1) {
    const std::size_t voice = static_cast<std::size_t>(((slot - 1) / 3 + 1) & 7);
    if (dsp.voices[voice].startPending) loadStartPointer(dsp, ram, voice);
    loadLoopPointer(dsp, ram, voice);
    loadDirectorySlotRegisters(dsp, voice);
  }
  if (slot >= 2 && slot <= 23 && slot % 3 == 2)
    loadPitchHigh(dsp, static_cast<std::size_t>(((slot - 2) / 3 + 1) & 7));
  if ((slot >= 2 && slot <= 20 && slot % 3 == 2) || slot == 26) {
    const std::size_t voice = slot == 26 ? 0 : (slot - 2) / 3 + 1;
    if (dsp.voices[voice].startPending) {
      primeVoiceStream(dsp, ram, voice);
      dsp.voices[voice].startPending = false;
    }
    loadScheduledGroupBytes(dsp.voices[voice], ram);
  }
  if (slot >= 2 && slot <= 20 && slot % 3 == 2)
    computeVoiceSlot(dsp, ram, static_cast<std::size_t>((slot - 2) / 3 + 1), softReset);

  // Preserve the native voice-order visibility before the following voice's
  // volume fold on shared slots, including the CPU-write arbitration window.
  if (slot >= 3 && slot <= 24 && slot % 3 == 0) {
    const std::size_t voice = slot / 3 - 1;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << voice);
    if ((dsp.pendingEndxClear & bit) != 0) {
      dsp[kDspEndx] &= static_cast<std::uint8_t>(~bit);
      dsp.pendingEndxClear &= static_cast<std::uint8_t>(~bit);
      dsp.preparedEndx &= static_cast<std::uint8_t>(~bit);
    } else if ((dsp.preparedEndx & bit) != 0) {
      if (!cpuWriteStands(dsp, dsp.endxWriteCycle)) dsp[kDspEndx] |= bit;
      dsp.preparedEndx &= static_cast<std::uint8_t>(~bit);
    }
  } else if (slot >= 4 && slot <= 25 && slot % 3 == 1) {
    const std::size_t voice = (slot - 4) / 3;
    if (!cpuWriteStands(dsp, dsp.outxWriteCycle[voice]))
      dsp[voiceRegister(voice, kVoiceOutx)] = dsp.preparedOutx[voice];
  } else if (slot >= 5 && slot <= 26 && slot % 3 == 2) {
    const std::size_t voice = (slot - 5) / 3;
    if (!cpuWriteStands(dsp, dsp.envxWriteCycle[voice]))
      dsp[voiceRegister(voice, kVoiceEnvx)] = dsp.envxStage[voice];
    dsp.envxStage[voice] = dsp.preparedEnvx[voice];
  }
  if (slot <= 21 && slot % 3 == 0) {
    const std::size_t voice = slot / 3;
    performLoadedDecodes(dsp, ram, voice);
    applyVoiceLeft(dsp, voice);
  } else if (slot >= 1 && slot <= 22 && slot % 3 == 1)
    applyVoiceRight(dsp, static_cast<std::size_t>((slot - 1) / 3));

  switch (slot) {
    case 23:
      // The echo unit's ladder opens: the left channel's word leaves the ring —
      // addressing the APPLIED base, the ESA the previous sample's T31 applied —
      // and FIR0 is captured with it. The EON sends are all folded by T22, so
      // nothing the voices owe the echo is still outstanding.
      if (processEcho) loadEchoLeft(dsp, ram, dsp.echoAppliedEsa);
      break;
    case 24:
      // The right channel's word, and FIR1 and FIR2.
      if (processEcho) loadEchoRight(dsp, ram);
      break;
    case 25:
      // FIR3, FIR4 and FIR5.
      if (processEcho) latchFirCoefficients(dsp, 3, 3);
      break;
    case 26:
      // FIR6 and FIR7 complete the set, and the filter runs over the eight
      // captured coefficients, leaving each channel's output for the slots below.
      if (processEcho) filterEcho(dsp);
      else dsp.echoFirOutLeft = dsp.echoFirOutRight = 0;
      break;
    case 27:
      finalizeLeft(dsp);
      // EFB is read here, and the feedback it scales is staged for the write
      // slots.
      if (processEcho) stageEchoWrite(dsp, echoRam, dsp.echoSendLeft, dsp.echoSendRight);
      break;
    case 28:
      finalizeRight(dsp);
      // PMON is read here, for every voice at once.
      loadPitchModulation(dsp);
      break;
    case 29:
      // The write gate for T30's left word: FLG bit 5 is loaded one slot before
      // the word it governs.
      if (processEcho) dsp.echoGateLeft = (dsp[kDspFlg] & kFlgEchoWriteDisable) == 0;
      // NON, EON and DIR are read here, in one act, for every voice at once.
      loadVoiceWideRegisters(dsp);
      break;
    case 30:
      // The left echo word lands at its write slot, T30, under the gate loaded at
      // T29 — the value was formed at T27, where EFB is read, and held since.
      if (processEcho && dsp.echoWritePending && echoRam != nullptr && dsp.echoGateLeft) {
        echoRam[dsp.echoWriteEntry] = dsp.echoWriteBytes[0];
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 1)] = dsp.echoWriteBytes[1];
      }
      // Load the raw ESA/EDL for T31's ring advance to apply, and the right
      // word's own FLG bit-5 gate.
      dsp.echoLatchedEsa = dsp[kDspEsa];
      dsp.echoLatchedEdl = dsp[kDspEdl];
      dsp.echoGateRight = (dsp[kDspFlg] & kFlgEchoWriteDisable) == 0;
      // The internal key-on loses the bits the previous poll took, one slot
      // before this sample's own poll reads it, and a KON write issued in the
      // two cycles before this slot goes with them.
      if (dsp.sampleIndex % 2 == 0 && cpuWriteStands(dsp, dsp.konWriteCycle))
        dsp.internalKon = static_cast<std::uint8_t>(dsp.internalKon & ~dsp.consumedKon);
      break;
    case 31: {
      // The KON/KOFF load runs first (the poll keeps the even-sample parity —
      // it reads the pre-tick sample index, as the frame-at-once entry did),
      // then the global counter advances, and only then does voice 0 compute:
      // its envelope check — and the noise step below — read the value this
      // slot just produced, the same value voices 1-7 read at their compute
      // slots in the following frame. So all eight voices' checks between two
      // advances see one counter value (measured against spc_dsp6
      // `Misc/counter rate synchronizations`, which locks the counter phase
      // through a voice-4 gadget and measures voice 0's time-to-first-step at
      // every rate). A keyed voice 0 takes this very slot as the first of its
      // five silent startup calls, which is what makes the eight voices'
      // key-on startup read-uniform through VxENVX (spc_dsp6 `KON/envx during
      // kon`).
      pollKeying(dsp, ram);
      tickDspSample(dsp);
      // The noise step precedes voice 0's compute as well: the rate in FLG
      // bits 0-4 is read here and the level advances before voice 0 takes it,
      // so all eight voices' amplitudes for one delivered sample carry the
      // same noise level — voice 0's from this slot, voices 1-7's from their
      // compute slots in the frame that follows. A voice 0 that read the
      // pre-step level would trail the others by one step (spc_dsp6
      // `Order/noise rate flg.1F` spins on the echo tape until voice 0's noise
      // sample reads back as zero, then stops the rate: the level it leaves
      // standing, and the frame the next stepped level reaches the tape on,
      // are both one sample earlier than a trailing voice 0 produces).
      if (envelopeRateFires(dsp.globalCounter, dsp[kDspFlg] & kFlgNoiseRate))
        dsp.noiseLevel = nextNoiseLevel(dsp.noiseLevel);
      computeVoiceSlot(dsp, ram, 0, softReset);
      // The right echo word lands at its write slot, T31, under the gate loaded
      // at T30; then the ring advance applies the T30-loaded ESA/EDL.
      if (processEcho && dsp.echoWritePending && echoRam != nullptr &&
          dsp.echoGateRight) {
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 2)] = dsp.echoWriteBytes[2];
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 3)] = dsp.echoWriteBytes[3];
      }
      if (processEcho) {
        dsp.echoWritePending = false;
        advanceEchoRing(dsp, dsp.echoLatchedEsa, dsp.echoLatchedEdl);
      }
      break;
    }
    default:
      break;
  }
}

// Resolve the fixed hardware schedule once per cycle. Compile-time slots remove
// repeated range/modulo tests and voice-address calculations, especially across
// sparse virtual banks. No voice work, RAM read, latch, or cycle is skipped.
template<bool processEcho>
static void dispatchPrimedSlot(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                std::uint8_t* echoRam, std::uint8_t slot) noexcept {
  switch (slot) {
    case 0: runPrimedSlot<0, processEcho>(dsp, ram, echoRam); break;
    case 1: runPrimedSlot<1, processEcho>(dsp, ram, echoRam); break;
    case 2: runPrimedSlot<2, processEcho>(dsp, ram, echoRam); break;
    case 3: runPrimedSlot<3, processEcho>(dsp, ram, echoRam); break;
    case 4: runPrimedSlot<4, processEcho>(dsp, ram, echoRam); break;
    case 5: runPrimedSlot<5, processEcho>(dsp, ram, echoRam); break;
    case 6: runPrimedSlot<6, processEcho>(dsp, ram, echoRam); break;
    case 7: runPrimedSlot<7, processEcho>(dsp, ram, echoRam); break;
    case 8: runPrimedSlot<8, processEcho>(dsp, ram, echoRam); break;
    case 9: runPrimedSlot<9, processEcho>(dsp, ram, echoRam); break;
    case 10: runPrimedSlot<10, processEcho>(dsp, ram, echoRam); break;
    case 11: runPrimedSlot<11, processEcho>(dsp, ram, echoRam); break;
    case 12: runPrimedSlot<12, processEcho>(dsp, ram, echoRam); break;
    case 13: runPrimedSlot<13, processEcho>(dsp, ram, echoRam); break;
    case 14: runPrimedSlot<14, processEcho>(dsp, ram, echoRam); break;
    case 15: runPrimedSlot<15, processEcho>(dsp, ram, echoRam); break;
    case 16: runPrimedSlot<16, processEcho>(dsp, ram, echoRam); break;
    case 17: runPrimedSlot<17, processEcho>(dsp, ram, echoRam); break;
    case 18: runPrimedSlot<18, processEcho>(dsp, ram, echoRam); break;
    case 19: runPrimedSlot<19, processEcho>(dsp, ram, echoRam); break;
    case 20: runPrimedSlot<20, processEcho>(dsp, ram, echoRam); break;
    case 21: runPrimedSlot<21, processEcho>(dsp, ram, echoRam); break;
    case 22: runPrimedSlot<22, processEcho>(dsp, ram, echoRam); break;
    case 23: runPrimedSlot<23, processEcho>(dsp, ram, echoRam); break;
    case 24: runPrimedSlot<24, processEcho>(dsp, ram, echoRam); break;
    case 25: runPrimedSlot<25, processEcho>(dsp, ram, echoRam); break;
    case 26: runPrimedSlot<26, processEcho>(dsp, ram, echoRam); break;
    case 27: runPrimedSlot<27, processEcho>(dsp, ram, echoRam); break;
    case 28: runPrimedSlot<28, processEcho>(dsp, ram, echoRam); break;
    case 29: runPrimedSlot<29, processEcho>(dsp, ram, echoRam); break;
    case 30: runPrimedSlot<30, processEcho>(dsp, ram, echoRam); break;
    case 31: runPrimedSlot<31, processEcho>(dsp, ram, echoRam); break;
  }
}

// Runs one slot. An unprimed state runs its whole first sample at the wrap slot
// T31 — the same cycle the frame-at-once model delivered on, so a write during the
// first sample's slots reaches it exactly as before — then primes the schedule for
// voice 0; every later sample is slot-scheduled.
template<std::uint8_t Slot, bool ProcessEcho>
SlotResult detail::stepDspCycleAtSlot(DspState& dsp,
    std::span<const std::uint8_t, 65536> ram, std::uint8_t* echoRam) noexcept {
  static_assert(Slot < 32);
  ++dsp.cycleCount;
  if (!dsp.primed) {
    if constexpr (Slot == 31) {
      dsp.slotFrame = stepDspSampleAtomic(dsp, ram, echoRam, ProcessEcho);
      dsp.primed = true;
    }
  } else {
    runPrimedSlot<Slot, ProcessEcho>(dsp, ram, echoRam);
  }
  dsp.slotCursor = static_cast<std::uint8_t>((Slot + 1) & 31);
  SlotResult result{};
  if constexpr (Slot == 31) {
    result.frame = dsp.slotFrame;
    result.delivered = true;
  }
  return result;
}

template<bool processEcho>
static SlotResult stepDspCycleImpl(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                   std::uint8_t* echoRam) noexcept {
  const std::uint8_t slot = dsp.slotCursor;
  ++dsp.cycleCount;  // the clock a CPU write's stamp is measured against

  if (!dsp.primed) {
    if (slot == 31) {
      // The frame-at-once path advances voice 0 exactly once and records its
      // amplitude in voiceAmplitude[0]; the next (slot-scheduled) frame applies
      // that value at T0..T5, so voice 0's output rides one frame behind while its
      // state trajectory stays the frame-at-once one.
      dsp.slotFrame = stepDspSampleAtomic(dsp, ram, echoRam, processEcho);
      dsp.primed = true;
    }
  } else {
    dispatchPrimedSlot<processEcho>(dsp, ram, echoRam, slot);
  }

  dsp.slotCursor = static_cast<std::uint8_t>((slot + 1) & 31);
  SlotResult result;
  if (dsp.slotCursor == 0) {
    result.frame = dsp.slotFrame;
    result.delivered = true;  // the sample's 32 slots are complete
  }
  return result;
}

SlotResult stepDspCycle(DspState& dsp, std::span<std::uint8_t, 65536> ram) noexcept {
  return stepDspCycleImpl<true>(dsp, ram, ram.data());
}

SlotResult stepDspCycle(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept {
  return stepDspCycleImpl<true>(dsp, ram, nullptr);
}

SlotResult stepDspVoiceCycle(DspState& dsp,
                             std::span<const std::uint8_t, 65536> ram) noexcept {
  return stepDspCycleImpl<false>(dsp, ram, nullptr);
}

// Runs a whole sample's 32 slots and returns the frame they finalize. The machine
// drives the DSP one slot per cycle instead (stepDspCycle), delivering the frame at
// the wrap; a standalone caller uses this.
static StereoFrame stepDspSampleLoop(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                     std::uint8_t* echoRam) noexcept {
  StereoFrame frame{};
  for (int n = 0; n < 32; ++n) {
    const SlotResult result = stepDspCycleImpl<true>(dsp, ram, echoRam);
    if (result.delivered) frame = result.frame;
  }
  return frame;
}

StereoFrame stepDspSample(DspState& dsp, std::span<std::uint8_t, 65536> ram) noexcept {
  return stepDspSampleLoop(dsp, ram, ram.data());
}

StereoFrame stepDspSample(DspState& dsp,
                          std::span<const std::uint8_t, 65536> ram) noexcept {
  return stepDspSampleLoop(dsp, ram, nullptr);
}

// The key-on's restart. `primeNow` reads the start pointer and primes the
// stream at once — the single-voice call and a state without a slot schedule;
// the slot-scheduled restart (computeVoiceAmplitude's restartPending branch)
// leaves the voice startPending, and the next sample's directory and load
// slots read the pointer and prime.
static void keyOnVoiceImpl(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                           std::size_t voice, bool primeNow) noexcept {
  // Whether the startup walks is decided by the voice the key-on lands on, as
  // the restart applies: it walks only when it interrupts a voice whose level
  // stands above zero while its old OUTPUT is silent. A re-key of a sounding
  // voice holds its stream, whatever its age or level, and so does a key-on
  // at level zero (see VoiceState::startupWalks). Which sample the silence is
  // read from — this compute's predecessor here, through voiceAmplitude — and
  // whether the test sees the full amplitude or its OUTX byte is constrained
  // by nothing that passes; the two sub-tests sensitive to the walk fail
  // under every reading (docs/s-dsp-behavior.md, "What remains open").
  const bool walks = dsp.voices[voice].envelope != 0 && dsp.voiceAmplitude[voice] == 0;
  if (primeNow) {
    startVoice(dsp, ram, voice);  // reads the pointer, primes, resets the voice state
  } else {
    resetVoiceForStart(dsp, voice);
    dsp.voices[voice].startPending = true;
  }
  VoiceState& v = dsp.voices[voice];
  v.startupWalks = walks;
  v.phase = EnvPhase::Attack;
  // The startup countdown — the documented five empty samples, uniform for every
  // voice. The poll precedes voice 0's compute in the slot they share, so voice
  // 0's first silent call is the load's own slot and voices 1-7's follow in the
  // next samples; the count of five plus that shared-slot asymmetry is exactly
  // what makes the eight voices' startup read-uniform through VxENVX
  // (spc_dsp6 `KON/envx during kon`) while keeping voice 0's first live compute
  // at the instant `Envelope/hidden env 0 at kon` pins.
  v.konDelay = kKeyOnStartupCalls;
  v.computesSinceKeyOn = 0;
  // Key-on clears this voice's ENDX bit, staged or readable: a same-sample
  // end-block set does not override the clear. On the slot schedule the clear
  // is written at the voice's S7 slot, four slots after this compute, like the
  // set it can erase (see DspState::pendingEndxClear); the direct call and the
  // frame-at-once path clear at once.
  if (primeNow) {
    dsp[kDspEndx] &= static_cast<std::uint8_t>(~(1u << voice));
    dsp.preparedEndx &= static_cast<std::uint8_t>(~(1u << voice));
  } else {
    dsp.pendingEndxClear |= static_cast<std::uint8_t>(1u << voice);
  }
}

void cpuWriteDspRegister(DspState& dsp, std::uint8_t reg, std::uint8_t value) noexcept {
  if (reg > 0x7F) return;  // DSPDATA writes beyond $7F are ignored
  if (reg == kDspEndx) {
    // ENDX: any write acknowledges all end flags. A set still staged for its
    // voice's S7 slot is left to land there — unless this write is what the
    // slot's write then loses to.
    dsp[kDspEndx] = 0;
    dsp.endxWriteCycle = dsp.cycleCount;
    return;
  }
  dsp[reg] = value;
  // A KON write also arms the internal key-on the poll consumes. The value
  // replaces whatever was pending, so of two writes between polls only the
  // second one keys anything on. The write carries its cycle: the T30 strike
  // and the poll each ask whether it landed in the two cycles before them.
  if (reg == kDspKon) {
    dsp.internalKon = value;
    dsp.konWriteCycle = dsp.cycleCount;
  }
  // The DSP-written voice registers carry the write's cycle, for the S8/S9
  // write that may have to yield to it.
  const std::size_t voice = reg >> 4;
  const std::uint8_t offset = reg & 0x0F;
  if (offset == kVoiceOutx) dsp.outxWriteCycle[voice] = dsp.cycleCount;
  if (offset == kVoiceEnvx) dsp.envxWriteCycle[voice] = dsp.cycleCount;
}

void keyOnVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept {
  keyOnVoiceImpl(dsp, ram, voice, true);
}

void keyOffVoice(DspState& dsp, std::size_t voice) noexcept {
  dsp.voices[voice].phase = EnvPhase::Release;
}

void pollKeying(DspState& dsp,
                [[maybe_unused]] std::span<const std::uint8_t, 65536> ram) noexcept {
  // The poll itself touches no RAM: a consumed restart's stream re-prime — the
  // one RAM-reading key-on action — runs at the voice's own compute (see
  // computeVoiceAmplitude's restartPending branch).
  // The poll runs on even-indexed samples counted from power-on — a fixed
  // choice that settles the hardware's probabilistic power-on poll phase.
  if (dsp.sampleIndex % 2 != 0) return;

  // KOFF is read from the register, which exerts its influence at every poll
  // for as long as the bit stays set. KON is read from the internal value a
  // write arms, and the poll clears it — so a key-on happens once per write,
  // however long the register keeps the bit.
  const std::uint8_t kon = dsp.internalKon;
  const std::uint8_t koff = dsp[kDspKoff];
  for (std::size_t voice = 0; voice < 8; ++voice) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << voice);
    if ((koff & bit) != 0) keyOffVoice(dsp, voice);
    // KON is applied after KOFF, so a voice with both bits set keys on. A
    // key-on landing on a voice still inside its silent key-on span — the five
    // startup calls plus the first live compute, whose output is still silent —
    // splits by which poll takes delivery of it. At the poll immediately after
    // the one that keyed the voice, it is absorbed outright: countdown, stream
    // and envelope schedule all stand (`KON/kon clears independent`). At any
    // later poll inside the span, the stream still stands — the cursor keeps
    // walking at the pitch, un-re-primed (`KON/kon decoding when another kon`)
    // — but the silent hold rewinds: the countdown reloads and the envelope
    // drops to 0, so the level emerges as late as a full restart would place
    // it (`KON/kon then another kon`). A re-key past the span restarts the
    // voice in full (the documented click/pop case).
    //
    // Absorption covers a write that was standing before the poll's slot came
    // round. One issued in the two cycles before the poll rewinds the hold
    // instead, at the poll that takes it: `Timing/Misc/29 kon cleared`'s T30
    // row reads its voices reaching the tape later than the rows whose write
    // the strike took, and earlier than the rows the next poll serves.
    //
    // Both in-span tiers protect a STANDING startup. A voice in Release has no
    // startup left to absorb into or rewind — a soft reset landing inside the
    // span keys the voice off and kills the startup for good — so a key-on
    // consumed on a Released voice restarts it in full even inside the span
    // (`KON/kon then flg.80 then kon`). The same arm is what lets KON win over
    // KOFF when one poll delivers both to an in-span voice.
    //
    // The full restart arms its countdown and counter here, but the wipe waits
    // for the voice's own compute: that compute still prepares one sample from
    // the standing stream and envelope — the final pre-key-on sample, which
    // still sounds — before the restart applies (`KON/kon unaffected by
    // pitch`: a re-key on a sounding voice emits the old data once more on the
    // consuming sample, at every pitch; Anomie's key-on account places the
    // final pre-KON decode on that same sample).
    if ((kon & bit) != 0) {
      VoiceState& v = dsp.voices[voice];
      if (v.computesSinceKeyOn > kKeyOnSilentCalls || v.phase == EnvPhase::Release) {
        v.konDelay = kKeyOnStartupCalls;
        v.computesSinceKeyOn = 0;
        v.restartPending = true;
      } else if (v.computesSinceKeyOn > 2 || cpuWriteStands(dsp, dsp.konWriteCycle)) {
        v.konDelay = kKeyOnStartupCalls;
        v.envelope = 0;
      }
      // Every consumed key-on — restarting, rewinding, or absorbed — schedules
      // a pitch capture for the voice at the next poll-parity sample's start.
      // That is the only path a pitch write reaches a voice through while the
      // key-on's capture hold stands (`KON/kon decoding when another kon`'s
      // freezes ride their key-on; `KON/kon then change pitch`'s bare writes
      // do not), and its timing is pinned from both sides: a register write
      // nine cycles behind the KON write must be seen (the freezes), while one
      // landing three cycles after the parity sample's first slot must not be
      // (that ROM's earliest pulse). The hold itself is counted in the voice's
      // own computes — the six of its silent span — so the seventh compute's
      // slot reads are the first live ones and the eighth compute the first
      // advance at a live pitch, uniform for the eight voices: voice 0's
      // first compute follows this poll in the slot they share, the others'
      // come in the next sample.
      // A consumed key-on clears the voice's ENDX bit — restarting the voice,
      // rewinding a standing startup, or absorbed into one alike. Both
      // references state the clear for every key-on the poll takes, without
      // qualification. It is written at the voice's S7 slot, where a
      // same-sample end-block set cannot override it (see
      // DspState::pendingEndxClear). No spc_dsp6 row that passes distinguishes
      // this from clearing on a restart alone; `Random/kon pitch` is the one
      // row that moves, and it fails either way.
      dsp.pendingEndxClear |= bit;
      dsp.pitchReloadPending |= bit;
      dsp.voices[voice].pitchCaptureHold = kKeyOnSilentCalls;
    }
  }
  // The poll disarms itself, and records what it took for the next sample's
  // strike to remove from a value written since.
  dsp.consumedKon = kon;
  dsp.internalKon = 0;
}

std::uint16_t stepVoiceEnvelope(DspState& dsp, std::size_t voice, bool brrEndMute) noexcept {
  VoiceState& v = dsp.voices[voice];
  const auto writeEnvx = [&] {
    dsp[voiceRegister(voice, kVoiceEnvx)] = static_cast<std::uint8_t>(v.envelope >> 4);
  };

  // A BRR End+Mute block standing at the sample's start moves the voice to
  // Release and drops the level to 0 before any envelope operation runs (the
  // documented -0x800 step reaches 0 from any level), so the kill sample
  // publishes 0 (spc_dsp6 `KON/kon then set sample's end flag`: an End+Mute
  // pulse landing on the first live step reads as a voice that never sounded).
  // It is read before the startup countdown below, because the header check
  // that raises it goes live while the countdown still has samples left to run.
  if (brrEndMute) {
    v.phase = EnvPhase::Release;
    v.envelope = 0;
    v.bentGainRef = 0;
    if (v.konDelay > 0) --v.konDelay;
    writeEnvx();
    return 0;
  }

  // Post-key-on startup: the level holds at 0 (set at key-on) while the
  // countdown runs. The call that takes the counter to 0 is itself silent, so a
  // fresh key-on yields five silent calls before the first live step (see
  // keyOnVoice for the count).
  if (v.konDelay > 0) {
    --v.konDelay;
    writeEnvx();
    return v.envelope;
  }

  runEnvelopeMode(dsp, voice);
  writeEnvx();
  return v.envelope;
}

// Runs the selected envelope mode once for voice `voice`: the candidate level,
// the rate counter's decision, the phase switches and the Bent-Increase
// reference. The countdown and End+Mute cases around it are stepVoiceEnvelope's;
// a consumed restart runs this alone to settle the standing envelope before the
// restart reads it.
static void runEnvelopeMode(DspState& dsp, std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  // `VxADSR1` is the directory slot's read (see VoiceState::adsr1); `VxADSR2`
  // and `VxGAIN` are read here, at the compute.
  const std::uint8_t adsr1 = v.adsr1Loaded ? v.adsr1 : dsp[voiceRegister(voice, kVoiceAdsr1)];
  const std::uint8_t adsr2 = dsp[voiceRegister(voice, kVoiceAdsr2)];
  const std::uint8_t gain = dsp[voiceRegister(voice, kVoiceGain)];
  const int level = v.envelope;

  // The selected mode computes a candidate level every sample. The rate counter
  // decides only whether the envelope takes that candidate; the Attack->Decay
  // switch below reads it either way, which is what lets a voice change phase
  // while a rate of 0 holds its level still.
  int newLevel = level;
  bool fires = false;

  if (v.phase == EnvPhase::Release) {
    fires = true;  // Release runs every sample (rate 31)
    newLevel = level - 8;
  } else if ((adsr1 & 0x80) != 0) {
    switch (v.phase) {
      case EnvPhase::Attack: {
        const auto rate = static_cast<std::uint8_t>((adsr1 & 0x0F) * 2 + 1);
        fires = envelopeRateFires(dsp.globalCounter, rate);
        newLevel = level + ((adsr1 & 0x0F) == 0x0F ? 1024 : 32);
        break;
      }
      case EnvPhase::Decay: {
        const auto rate = static_cast<std::uint8_t>(((adsr1 >> 4) & 0x07) * 2 + 16);
        fires = envelopeRateFires(dsp.globalCounter, rate);
        newLevel = expDecrease(level);
        break;
      }
      case EnvPhase::Sustain: {
        fires = envelopeRateFires(dsp.globalCounter, static_cast<std::uint8_t>(adsr2 & 0x1F));
        newLevel = expDecrease(level);
        break;
      }
      case EnvPhase::Release:
        break;  // handled above
    }
  } else if ((gain & 0x80) == 0) {
    fires = true;  // Direct Gain: a fixed level, no rate
    newLevel = (gain & 0x7F) << 4;
  } else {
    fires = envelopeRateFires(dsp.globalCounter, static_cast<std::uint8_t>(gain & 0x1F));
    switch ((gain >> 5) & 0x03) {
      case 0: newLevel = level - 32; break;          // Linear Decrease
      case 1: newLevel = expDecrease(level); break;  // Exp Decrease
      case 2: newLevel = level + 32; break;          // Linear Increase
      default:                                       // Bent Increase
        newLevel = level + (v.bentGainRef < 0x600 ? 32 : 8);
        break;
    }
  }

  // The Attack->Decay switch fires when the candidate leaves the unsigned 11-bit
  // range: an attack overshooting 0x7FF, or a decreasing mode driven below zero.
  // The level itself is clamped, so the overshoot is only visible here.
  const bool attackToDecay = (v.phase == EnvPhase::Attack) && ((newLevel & ~0x7FF) != 0);

  // The boundary Decay->Sustain compares against comes from whichever register
  // is driving the envelope: ADSR2's sustain level under ADSR, and GAIN's top
  // three bits while a GAIN mode drives the level. Those bits are the gain
  // mode and its enable, not a sustain level, so a voice keyed on under GAIN
  // reaches Sustain at a boundary its author never chose.
  const int sustainBoundary =
      (adsr1 & 0x80) != 0 ? ((adsr2 >> 5) & 0x07) : ((gain >> 5) & 0x07);

  // Decay->Sustain reads the candidate, and reads it every sample: a voice
  // parked in a rate-0 mode never takes the value, yet still changes phase when
  // the value the mode computes reaches the boundary. A candidate outside the
  // 11-bit range is the Attack->Decay case above and matches no boundary.
  const bool decayToSustain = (v.phase == EnvPhase::Decay) && ((newLevel & ~0x7FF) == 0) &&
                              ((newLevel >> 8) == sustainBoundary);

  // Under ADSR, a decay whose stored level already sits in the sustain band is
  // in Sustain before its step: the phase changes and the level holds, whatever
  // the decay rate does this sample. spc_dsp6 `Random/voice volumes` keys eight
  // voices at ADSR $EF/$E0 — the attack ends at 0x7FF, sustain level 7, sustain
  // rate 0, decay rate 28 firing one sample in four — from two arrival phases
  // two samples apart, and its tape carries no -8 step from either; a store
  // gated by the decay rate steps on one of them. The stored-level check is
  // ADSR's alone: under a GAIN mode the ROM's `Envelope/attack->decay during
  // gain` reads the candidate, not the stored level. Which rate gates that one
  // store is unconstrained: gating it by the sustain rate instead leaves every
  // passing sub-test green and every assertion here green, and the single
  // sub-test sensitive to the choice is reproduced by neither reading; gating it
  // by both rates at once is byte-identical to suppressing it. Suppressing it
  // keeps the documented step-then-check order everywhere the level is still
  // above the band.
  const bool alreadySustaining = (v.phase == EnvPhase::Decay) && (adsr1 & 0x80) != 0 &&
                                 (level >> 8) == sustainBoundary;

  // The counter decides only whether the level takes the candidate.
  if (fires && !alreadySustaining) v.envelope = clampEnvelope(newLevel);

  // The Bent-Increase reference is the value the mode computes, saved every
  // sample whether or not the counter fires — so a voice parked in a rate-0
  // mode still hands the next Bent-Increase step that mode's own value rather
  // than the level standing still. It is saved unclipped: a candidate driven
  // below zero and one carried past 0x7FF both read at or past 0x600 and take
  // the +8 branch.
  v.bentGainRef = static_cast<std::uint16_t>(newLevel);
  if (decayToSustain || alreadySustaining) v.phase = EnvPhase::Sustain;
  if (attackToDecay) v.phase = EnvPhase::Decay;
}

}  // namespace snaggletooth
