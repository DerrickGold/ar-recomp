# Pure-Go audio preview core

This package is an independently authored, audio-only SNES APU implementation
for local ROM-owner preview generation. It does not import, link, translate, or
copy the inherited C runner under `runtime/`.

The preview path models:

- the complete 256-opcode SPC700 instruction map and direct-page/stack rules;
- APU input/output ports and all three hardware timers;
- the S-DSP register interface, eight voices, BRR decoding and loop/end flags;
- ADSR/gain/release envelopes, pitch modulation, noise, stereo volume, and the
  echo/FIR path; and
- ActRaiser's verified boot/common/song image format and stage-two BRR chunk
  scripts.

It intentionally stops at the audio boundary. There is no 65816, PPU, browser
SPC plug-in, system converter, or dependency on the playable C runtime. The
ActRaiser adapter performs the 65816-side upload transaction directly, then the
original SPC700 driver controls playback normally. WAV output uses the same
32.04 kHz native cadence as the project runtime.

The interpolator is linear rather than a bit-exact reproduction of the S-DSP's
Gaussian table. This keeps the preview component small and independently
maintainable while preserving the source recording's notes, timing,
instruments, envelopes, stereo placement, and echo for replacement A/B work.
The playable runtime remains the oracle for sample-exact hardware conformance.

Generated WAVs contain extracted game audio. They are local cache artifacts,
not MIT-licensed project assets, and must never be checked into or packaged
with this module.
