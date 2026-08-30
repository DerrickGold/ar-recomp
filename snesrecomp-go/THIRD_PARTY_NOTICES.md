# Third-party notices

## Snaggletooth S-DSP accuracy core

The runner's slot-accurate S-DSP implementation contains code adapted from
[Snaggletooth](https://github.com/etroimcasso/Snaggletooth) by Eric Tomasso,
from commit
[`65668997ed58fe78cfcef1e53c0020bd92d0d287`](https://github.com/etroimcasso/Snaggletooth/commit/65668997ed58fe78cfcef1e53c0020bd92d0d287).
Snaggletooth is licensed under the MIT License, copyright (c) 2026 Eric
Tomasso.

The adapted local files are:

- `runtime/src/snes/accuracy/dsp.cpp`, from upstream `src/dsp.cpp`;
- `runtime/src/snes/accuracy/include/snaggletooth/apu/dsp.h`, from upstream
  `include/snaggletooth/apu/dsp.h`;
- `runtime/src/snes/accuracy/generated/envelope_counter_tables.inc`, from the
  upstream `src/generated/envelope_counter_tables.inc`; and
- `runtime/src/snes/accuracy/generated/gauss_table.inc`, from upstream
  `src/generated/gauss_table.inc`.

The verbatim upstream license is retained at
[`runtime/licenses/Snaggletooth-LICENSE.txt`](runtime/licenses/Snaggletooth-LICENSE.txt)
and is installed with both the source-built and binary runner SDKs.

The runner bridge, five-bank extended-voice topology, gain routing, shared echo
injection, serialization, PCM buffering, resampling, and host-facing APIs were
implemented in this repository and are not represented as Snaggletooth code.

## Other compatibility acknowledgements

The runtime provenance record identifies two small engine-neutral designs by
Jrickey used under their MIT OR Apache-2.0 terms: the shadow-verifier design in
`runtime/src/snes/audio_shadow.h` and the present-only color transform in
`runtime/src/snes/color_lut.h`. Their source-level notices are retained.

Historical projects that informed the recompiler ecosystem but whose code is
not distributed as part of this module are credited in
[`ATTRIBUTION.md`](ATTRIBUTION.md).
