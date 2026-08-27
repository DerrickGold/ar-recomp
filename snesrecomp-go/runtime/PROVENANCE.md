# Provenance

`runtime` is an independently authored portable runner started in August
2026. Its source is covered by the MIT grant in [`LICENSE`](LICENSE).

Except for the compatible component called out below, the code in this
directory was written against the public interfaces emitted by `snesrecomp-go`
and the behavioral contracts exercised by this repository's tests. No code
from the retired, restrictively licensed comparison runner was copied here.

The cartridge decoder and ROM-header analyzer were implemented from the
documented SNES LoROM/HiROM bank-window and internal-header contracts. Their
adapters retain the generated/runtime ABI while their mapping and analysis
cores have no dependency on the historical comparison runner.

The SHA-256 implementation follows FIPS 180-4 and uses a 16-word rolling
message schedule. The CPU register shell, frame-dump writer, widescreen copy,
and internal-register adapter were implemented from their public runtime
contracts and the repository's tests.

The DMA controller follows the public SNES DMA register map and transfer-mode
patterns. Its bus access and optional tracing are narrow host callbacks, so the
controller can be tested without a game, renderer, or platform runtime.

The SNES orchestration and memory bus follow the published system address map,
CPU-I/O register behavior, and the public subsystem lifecycle contracts. Game
pacing and diagnostic observation enter only through explicit callbacks.

The APU controller follows the public SPC700 I/O and timer register contracts.
Its recomp bootstrap was assembled specifically for this project and contains
no vendor IPL ROM; recomp hosts install the uploaded ARAM entry point through
the existing upload boundary.

The SPC700 core is a C implementation of the complete opcode contract first
implemented independently in this repository's MIT-licensed pure-Go audio
preview package. It does not derive from the inherited C runner.

The S-DSP is likewise based on the repository's MIT pure-Go preview device and
the public S-DSP register, BRR, envelope, noise, and echo contracts. The C
implementation adds the project's documented music/SFX buses, optional virtual
voices, continuous output resampling, and host observation seams without using
the inherited C implementation.

The engine-neutral shadow-verifier design is derived from Jrickey's reusable
recomp verifier under its MIT OR Apache-2.0 grant; its attribution is retained
in `audio_shadow.h`. The S-DSP shadow renderer around it was implemented for
the new DSP's state and interpolation model.

The MSU-1 adapter follows the published register and PCM-container contracts.
Its file access is isolated from the mixer, which uses an endian-neutral,
callback-invariant two-frame streaming resampler.

The present-only color LUT derives from the recomp ecosystem's compatible
MIT OR Apache-2.0 color-science component. It uses published D65, sRGB,
SMPTE-C, and CRT-gamma values and retains attribution in its public header.

The audio observability module is a new fixed-capacity recorder around the
public tracing ABI. It includes endian-neutral WAV output and monotonic clock
adapters for Windows and POSIX hosts.

The unified JSONL trace is a new portable event writer around the runtime's
published observation points. Its anomaly watch mode uses an explicit line
ring rather than platform-specific custom `FILE` implementations.

The common types and utility layer preserves the generated-code ABI while
using endian-neutral packed-table readers, checked file and buffer operations,
and a bounds-checked implementation of the public BPS1 patch format.

The launcher implements executable-relative paths, ROM discovery, header-aware
CRC32/SHA-256 verification, and platform file-selection adapters behind a
small portable API. The `SNESRECOMP_ROM` environment seam permits deterministic
headless and embedded hosts without weakening command-line compatibility.

The key-binding module parses and persists the documented controller schema
using stable USB HID/SDL scancode values directly. It therefore needs no SDL
linkage and can be tested or reused by any host input frontend.

The local debug-server boundary compiles production calls to no-ops and
provides portable trace-build linkage without bringing in the inherited TCP
server or its large static capture buffers.

The 65816 state boundary implements the generated-code register ABI, hardware
flag synchronization, WRAM/register/SRAM/ROM routing, interrupt frames, and a
bounded flat dispatcher with LoROM mirroring and explicit miss restoration.
Game-specific diagnostics are observers rather than part of memory semantics.

The common runtime bridge implements the generated game's public register,
memory, frame, audio, save, and SPC-upload contracts using the independent
devices above. The PPU implements the SNES register/memory model and the
project's tested tile, sprite, Mode-7, window, color-math, widescreen,
authentic-surface, and overlay-extraction contracts.

The versioned component API is an independently designed fixed-width C table
over those local devices. Its opaque handles, capability negotiation, borrowed
span lifetimes, generation counters, and CPU-state descriptor do not expose or
depend on the retired runner's concrete layouts. A registered game adapter
publishes the generated CPU register view without introducing title-specific
state into the portable runner. Fixed-width PPU descriptors and typed borrowed
VRAM/CGRAM/OAM views are derived directly from the independent PPU state and
likewise carry no game-specific addresses or semantics. The compact frame-state
view describes generic BG/OBJ overlay composition and presentation geometry;
it contains no ActRaiser scene policy. A separate retained-surface snapshot
describes the host-owned main, authentic, BG/OBJ priority-band, and Mode-7
bindings using generic pixel format, extent, pitch, origin, scale, and lifetime
metadata. It neither owns nor copies those pixels, and its independent binding
generation is maintained by this runner's PPU surface API.
The derived OBJ raster operation is a thin versioned adapter over this runner's
independently implemented OAM resolver and tile rasterizer. It writes directly
to caller-owned storage and likewise contains no title-specific classification.
The external HD tile census and Mode-7 canvas dumper use only those fixed-width
PPU descriptions and immutable generation-scoped views; their tile and palette
classification remains game-side tooling rather than runner policy.
The generated-execution snapshot and filtered synchronous observer registry are
likewise new versioned adapters over this runner's own instrumentation seams.
They describe generic block, stack, and dynamic-dispatch state only. The game
adapter owns symbol publication, while application diagnostics own retention
and JSON formatting; no title-specific address or crash policy enters the
runner.
Memory/register observers extend the same registry at the independently
implemented CPU, system-bus, DMA-register, and PPU-memory seams. They publish
canonical region-relative byte offsets and CPU-visible register addresses;
the unified trace remains an external formatting/filtering consumer. A cold
runner-side PPU association avoids changing the alignment of emulated memory
arrays merely to route optional observations.
DMA activation observers extend that registry at this runner's channel-enable
boundary. Their generic channel, mode, bus-endpoint, direction, address-control,
and byte-count metadata follows the public SNES DMA register contract. Events
are emitted once per selected channel rather than per byte or scanline, and
the unified trace remains an external consumer.
Frame, interrupt, and error observers extend the registry at the common host
tick, game-adapter interrupt, dispatcher, and ROM-mapping boundaries. Their
generic phase, vector, raster position, PC, and stable error-code metadata
follows the runner's independently implemented execution model. Title-specific
interrupt scheduling and recovery policy remain outside the ABI, while the
unified trace is only a filtered formatting consumer.
Final-mix audio observers extend the registry at the common runtime's host PCM
boundary. Their sample format, rate, channel count, block size, and monotonic
output-frame metadata are generic host-audio properties. The zero-copy sample
view is callback-scoped and is published after this runner's DSP, MSU-1, and
replacement-music stages complete; it contains no title-specific track or
sound-effect meaning.
The safe-point mutation queue is a new fixed-capacity synchronization boundary
around this runner's component state. Its copied byte writes, packed controller
override, monotonic command IDs, completion states, and frame cutoff are generic
runner mechanisms. It neither exposes a concrete component layout nor assigns
game meaning to an address or input bit.

The sole runner manifest links and includes only files from `runtime`.
The historical comparison runner was retired after parity validation and is
not part of the repository or distribution.
