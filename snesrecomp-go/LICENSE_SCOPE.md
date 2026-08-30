# License scope

The MIT grant in [`LICENSE`](LICENSE) applies to the original source code,
build logic, tests, and documentation in this module. In particular, it covers
the project-authored portable runner under [`runtime/`](runtime/): the CPU and
generated-code ABI, memory and cartridge mapping, DMA, PPU, APU/SPC700, host
audio and video services, save state, tracing, launcher, public component API,
and integration support.

No part of that replacement runner remains subject to the license of the
retired comparison runner. The comparison runner is not present in this
repository or in release packages.

The complete current runner is redistributable under MIT-compatible terms. Its
S-DSP accuracy component contains adaptations from Eric Tomasso's MIT-licensed
[Snaggletooth](https://github.com/etroimcasso/Snaggletooth) project. Those
specific portions retain Eric Tomasso's copyright and upstream MIT notice; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and
[`runtime/licenses/Snaggletooth-LICENSE.txt`](runtime/licenses/Snaggletooth-LICENSE.txt).
The surrounding bridge, extended-voice topology, host integration, and other
runner subsystems are project-authored code under this module's MIT grant.

The module license does not relicense material for which this repository does
not own the necessary rights, including:

- game ROMs or generated/recompiled code derived from them;
- extracted audio, graphics, memory captures, or other game content; and
- retail/manual/cover media under `internal/buildgui/assets/` or other
  third-party artwork.

Third-party files or components carrying their own notices remain governed by
those notices. Their presence does not narrow the MIT grant for the original
code identified above.
