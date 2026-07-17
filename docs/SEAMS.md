# ActRaiser — Logic ↔ Hardware Seams (HAL inventory)

This is the **living inventory of boundaries between game logic and SNES hardware** — the seams
we will eventually widen into a platform interface (HAL) to allow enhanced/custom graphics and
audio that exceed SNES limits. See `DEBUG.md` and the §"future" discussion for the rationale.

**Discipline (read once):** this is captured *opportunistically* while debugging — record a seam
only when you already understood it chasing a bug. **Do NOT go on documentation expeditions, and
do NOT design HAL signatures yet.** Correctness of the recomp comes first; the boundary isn't
stable enough to abstract until the game runs.

**The two columns that matter** are **Intent** (what the logic is *trying* to do) and **Logical
ID / table** (the index/pointer that carries asset identity — the future HAL's vocabulary and the
asset-substitution point). The *hardware* column is mechanically recoverable later; intent and
identity are the perishable, expensive-to-rederive parts.

**Status legend:**
- 🟢 **HOOKABLE** — already a clean semantic seam (an ID/event). Good HAL candidate; easy to
  intercept and reroute to an enhanced backend.
- 🟡 **CHOKE** — funnels through one runtime function; interceptable but the payload is still
  hardware-encoded (needs some decode to reach intent).
- 🔴 **LOW-LEVEL** — intent is entangled in raw hardware writes; surfacing it needs asset-pipeline
  decomp.

---

## Audio  (closest to a clean interface — start here)

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Music / event | `LDA #id; COP` → `$035A`; COP vector `$FFE4→$8526`; hook `ActRaiser_CopHook` | APU ports `$2140-43` | "play music / fire event N" | event/song id (A → `$035A`) — song images via the `$02:C7E5` pointer table + inline script pointers (see below) | 🟢 |
| Sound effect | `LDA #id; BRK` → `$035B`; BRK vector `$FFE6→$852F`; hook `ActRaiser_BrkHook` | APU ports | "play SFX N" | sfx id (A → `$035B`) | 🟢 |
| Dialogue glyph blip | message glyph helper `$01:901C`; non-space request block `$01:902D` does `LDA #$07; COP` | `$035A` request path | "glyph printed" | exact call site + id `$07`; `audio_dialog_blip` suppresses only this site because `$07` is reused elsewhere | 🟢 |
| Song upload (image identity) | `$02:9964` HLE — stage 1 (`$9A56` block image) + stage 2 (BRR streaming) | APU ports + ARAM | "load song N's sequence + instruments" | image src addr = song identity (`06:AC00` = common sample bank, `1A:94B8` = title = song 7); song table `$02:C7E5` (17 entries, 3-byte ptrs, all enumerated in `game-assets/manifest.ini`); more pointers inline in the `[$A2]` command scripts read via `$02:B4C0` | 🟢 |
| **BRR sample bank (per-sample!)** | stage 2 of the `$9964` HLE (`RtlUploadSpcImageFromDpInternal`, common_rtl.c) | ARAM `$3000-$6E67` (common) / `$795F+` (per-song) | "install instrument waveforms" | chunk pool at ROM `$08:8000` — length-prefixed `[len16][BRR data]` chunks, selected by index; script = image terminator's target word (lo byte = count, hi byte onward = chunk indices); dest base = WRAM `$0358` | 🟢 |
| Sample directory (DSP `DIR`) | uploaded as image blocks targeting ARAM `$2C00` (`DIR` page = `$2C`) | DSP `$5D` | "sample N lives at ARAM addr X, loops at Y" | 4-byte entries `{start16, loop16}` per srcn; common srcn `00-0B`, per-song `0C+` (block target `$2C30`) | 🟢 |
| Final PCM out | `RtlRenderAudio` (common_rtl.c) → continuous `dsp_getSamplesResampled` + MSU-1/OGG mix → SDL `AudioCallback` | host audio | "the mixed stereo stream" | native DSP stays 32.04 kHz; actual SDL rate controls time-based resampling, so frequency/buffer changes preserve pitch; `audio_master_volume` applies atomic post-mix gain here | 🟢 |
| Raw APU port write | `RtlApuWrite` (`$2140-$2143`) | APU I/O | low-level handshake / param | — | 🔴 |

> Audio is the highest-payoff first HAL target: the `$035A`/`$035B` events are already ID-based.
> Found while fixing the boss-music handshake and the silent-DSP bug (memory:
> `spc-upload-dp-pointer-fix`, `cop-syscall-hook-fix`, `post-boss-four-issues`; DEBUG.md §7.11).

### APU port-0 command protocol (decoded 2026-07-16 for music replacement)

Static decode of `$02:B63B` (script-driven song change), `$00:A3FE/A410/A427`
(boss-music go-signal chain), and the NMI tail `$02:AC29-AC3C`, confirmed live
via an `AR_APULOG` boot capture:

| Port | Value | Meaning |
|---|---|---|
| `$2140` | `$F0` | halt music (driver acks by clearing the port to 0) |
| `$2140` | `$F1` | attention/prepare (driver echoes `$F1` back; a song number follows) |
| `$2140` | `$F2` | pause current song; a repeated current-song command resumes it |
| `$2140` | `$FF` | enter the resident uploader (the `$9964` HLE bypasses the stream) |
| `$2140` | `$00` | idle/clear |
| `$2140` | else | **play song N** of the most recently uploaded song image |
| `$2142` | id | event/music command forwarded once per NMI from COP → `$035A` |
| `$2143` | id | SFX id forwarded once per NMI from BRK → `$035B` (16-bit store with `$2142`) |

A song change is always `$F0` → (`$FF` + upload) → song number; the boss chain
plays an already-loaded bank via `$F1`-echo → song number. Boot order: `02:9ACD`
= SPC mini-driver, **`06:AC00` = the COMMON sample bank** (srcn 00-0B — the
earlier "= title" note in the table above was wrong), `1A:94B8` = the title
song (= entry 7 of the `$02:C7E5` table), played with song number `$01`.
Fade commands have not been observed yet — `AR_MUSICLOG=1` logs unknown port-0
values and nonzero port-2 ids to catch them in play.

### Audio swap/enhancement tiers (tier 1 SHIPPED 2026-07-16)

1. **Track-level replacement (stream swap) — IMPLEMENTED** as
   `src/music_replacements.c` + `[music:<name>]` sections of
   `game-assets/manifest.ini` (all 17 table songs enumerated; the tracked
   manifest ships them inert until their .ogg exists). Identity = the stage-1
   image source address captured by an engine upload hook
   (`g_rtl_spc_upload_hook`); start/stop keyed off the port-0 protocol above
   via `g_rtl_apu_port_hook`; OGG Vorbis streaming (stb_vorbis) with
   sample-accurate loops (manifest `loop_start/loop_end` >
   `LOOPSTART/LOOPLENGTH` Vorbis tags > whole file) mixed in
   `RtlRenderAudio`'s locked region via `g_rtl_music_mix_hook`, msu1-style.
   Muting: every handshake/port write stays authentic (zero soft-lock risk);
   instead the DSP excludes voices with srcn >= 0x0C from the dry mix and
   echo input (`g_dsp_voice_mute_srcn_min`, dsp.c) — per-song instruments
   live there while SFX use the common bank by design (KONLOG-verified on the
   title: music keys srcn 0C/0D). Per-entry `when =` gates (shared HD gate
   grammar, sampled at song start) select level/state-dependent variants;
   first matching entry wins, ungated entry = fallback. `music_replacements`
   setting / `AR_MUSIC_REPLACEMENTS` toggles live; `AR_MUSICLOG=1` traces.
   Known gap: driver-side fades aren't captured yet (streams hard-stop on
   `$F0` exactly when the driver halts, so transitions stay correct).
2. **Instrument-level replacement (per-sample HD swap).** Stage 2 of the HLE installs each
   instrument as a discrete, identifiable unit: chunk index N from the `$08:8000` pool → known
   ARAM range → known `srcn` via the `$2C00` directory. Because our code performs the copy, it
   can substitute a different BRR chunk (or tag `srcn` → external hi-fi sample for a custom
   mixer) per instrument. The chunk index is a stable, ROM-wide instrument ID.
3. **SFX-level replacement.** Already the cleanest seam: `$035B` (BRK hook) carries the SFX id
   before any APU involvement. Map id → modern sample in the hook, suppress the port write.
4. **Output-quality tier.** All mixed audio funnels through the continuous
   `dsp_getSamplesResampled` boundary inside `RtlRenderAudio` (44.1 kHz stereo
   S16 by default; the settings registry offers restart-class
   32.04/44.1/48 kHz `AudioFreq` presets plus `AudioSamples`). The native FIFO
   advances at 32.04 kHz based on callback duration, not one fixed 534-frame
   block per SDL callback. OGG and MSU-1 use the same elapsed-time rule. This
   fixes the former callback-rate-dependent pitch/tempo change while retaining
   fractional source-frame carry across callbacks.
   Resampling quality, interpolation upgrades (the DSP's gaussian filter lives in
   `dsp.c` `dsp_getSample`), reverb/echo behavior (`dsp.c` echoWrites/FIR), and any
   post-processing belong here. MSU-1-style streaming already has scaffolding in
   `runner/src/snes/msu1.{c,h}` (mix point documented there as `RtlRenderAudio`'s locked
   region).

   The Phase-4 settings work now uses this seam for `audio_master_volume`
   (`0..100`). It deliberately does not label any post-mix control "music" or
   "SFX": independent levels require stable DSP voice classification or native
   SPC-driver bus controls, including a defined echo policy. See
   `settings-system.md`, "Audio control seams".

Diagnostics for all of it: `AR_APULOG=1` (uploads incl. per-chunk stage-2 lines + port
traffic), `AR_AUDIODBG=1` (DSP health: mvol/mute/peak/cyc-rate), `AR_KONLOG=1` (per-voice
key-on state: srcn/pitch/volumes/ADSR + first BRR bytes — all-zero BRR = samples missing).

---

## Graphics / PPU  (low-level; intent lives in the *loaders*, not the *draws*)

> Deep-dive companion: **[rendering-engine.md](rendering-engine.md)** — the
> consolidated decomp-style reference for the drawing machinery (complete
> NMI chain, the 4-buffer upload-record system, tilemap-ring streaming,
> per-section video config table, camera/parallax/bounds, tile animation,
> char loading + VRAM layout, OAM pipeline, palette paths, and the §13
> widescreen design constraints), with per-routine addresses and evidence.

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Per-frame display build / DMA | NMI path; `ActRaiserDrawPpuFrame`; DMA descriptors in ZP `$D0-$D5` | VRAM/OAM/CGRAM DMA, `$2100`-bus | "blit this frame's tiles/sprites/palette" | DMA descriptor tables | 🔴 |
| Sprite (OAM) build | object loop `$8915` → OAM | OAM | "place object's sprite" | `$06A0` object struct (X/Y/handler) | 🟡 |
| **Action OAM pipeline (fully mapped 2026-07-10)** | `$00:8C98` per-frame cull (obj vs 256×224 window keyed on camera `$22/$24`; sets/clears offscreen bit `$0400` in obj+`$30`) → `$00:8D68` sprite builder (walks 7-byte sprite defs at bank obj+`$18`, ptr obj+`$20`+5; draw window x∈[-16,256) y∈[-16,224); writes `$0380` shadow + packed high-table bits via `$00/$9A/$9C`) → `$00:923A`→`$9258` **HUD sprites** (from the `$06:A800` bank-6 table: FIXED screen positions, no camera math, no cull — widescreen-safe/centered) → OAM DMA `$02:ACA6` (544B `$0380`→`$2104`) | OAM | "which objects are visible + build their hardware sprites" | see ram-map.md "OAM shadow + sprite-build working vars" | 🟡 Stages C/D1/D2 and the combined wide path are directly validated across every action level in regions `$01-$07`, including the complete Death Heim flow. Wide activation is default-on; `AR_WS_MARGIN_ACTIVATION=0` is the fidelity gate. |
| **Action BG streaming (FULLY mapped 2026-07-12 — rendering-engine.md §3/§4/§12a)** | Camera `$02:B091` (clamp vs level dims `$2E/$30`) sets 16px-crossing flags in `$93` → dispatcher `$02:B127` (TRB per bit) → `$02:B158` column strips (2 cols × 64 rows) / `$02:B1AF` row strips (2 rows × 64 cols, span `[cam&~$FF,+512)` — 256-aligned, page-keyed decode) per layer (X=0 BG1, X=4 BG2) → ONE record into the layer's fixed buffer (`$3900/$3A02/$3B04/$3C06`, capacity 1/frame) → NMI drain `$02:ACC8/$ACE5` → `$02:ADA8` 64B chunks. 64×64-tile ring per layer (BG1 `$6000`, BG2 `$7000`); entry draw = inline mega-burst (full ring, one frame). The old "tier-2 burst" = `$B1AF` row strips (walk bob). Northwall `0601`: `$00:E7BC` → `$02:945E` builds `$7E:6000`; `$02:96B6` points HDMA ch2 at `$210F` (`BG2HOFS`). | VRAM BG1/BG2 tilemaps | "keep the resident 512×512px tilemap ring fed as the camera moves" | level map decode via section config `$02:893E` + metatile tables | 🟡 original recompiled builders remain active and regions `$01-$07`, including the complete Death Heim flow, are directly validated. Stage B transactionally refreshes valid world margins while restoring CPU/WRAM/math state. Full margin columns remain cached on `$24&$FF00`; newly exposed vertical rows use `$B8A0` at `$24&$FFF0`, selectively draining neighboring-band columns when the 342px view straddles the row decoder's 512px page span. Narrow `$32<$0200` BG2 is never refreshed: the presentation layer pads an isolated authentic render (reflection normally; cyclic repeat for Aitos/Northwall maps `$01-$05/$08`), or clamps with `AR_WS_BG2_MIRROR=0`; none of these paths changes game VRAM/state. Remaining: presentation-aware camera/world-edge limits. |
| UI/dialog tilemap compose+upload (sim engine) | `$02:BF60` dispatches message text into BG3 buffer `$7F:B000` → `$02:AEEB`, while the visible box frame and offscreen work boxes remain BG2; whole-map BG refreshes use `$02:B727`/`$B825` record mega-bursts and `$02:ADA8` as a 64B DMA helper. Sky Palace setup at `$02:B6F8-$B726` conditionally copies ROM `$07:D0A0` to `$7E:C200`. | BG3 text plus WRAM/VRAM BG2 map state | "draw the active UI while retaining offscreen work pages" | message-type IDs via `$14` (see Save/persistence note on `$14` reuse) | 🟡 major paths separated. Live `$B825` decode reproduced staging (`runs/20260712-232230`); narrow raw-edge reflection produced broken columns; center reflection copied the BG2 box (11:36 PM capture). Current render transaction reads `$07:D0A0`, reconstructs the box-covered rows per column class (row-major quadrants; shaft continuation; seam base halves at meta cols 0/15; floor top 2 rows under the box bottom; `$41/$49` flare + `$40/$48`/`$42/$4A` skirts at shaft columns — base art exists only in the metatile table), expands via `$7E:2900`, patches only margin BG2 columns, then restores VRAM. ✅ Validated 2026-07-13: byte-identical to the game's boot colonnade (scratch cols 56-63 rows 18-31); user-confirmed clean in dialogue + submenu states. |
| **Native dialog presentation assets (verified 2026-07-16)** | compressed BG3 font at ROM `$17:ECFB` (file `$0BECFB`) → `$02:C5C9` decode → `$1000` bytes; Sky Palace BG char bank at ROM `$0D:C000` (file `$06C000`, `$4000` bytes); palette 7 at ROM `$1C:BF73` (file `$0E3F73`) | BG3 font / BG1+BG2 chars / CGRAM | "render ActRaiser's text and beveled dialog frame" | font tile index = character code; frame chars `$CE/$CF` corners, `$DE/$DF` sides, `$EE` horizontal edge, `$FF` black fill | 🟢 F2 captures `runs/20260716-072558/snapshots/snap_00_gf460` and `snap_01_gf668` supplied byte-exact VRAM/CGRAM identity. The reusable frame is an 8×8 nine-slice: vertical flips produce the top edge/lower corners. Do not reuse 16×16 metatiles `$4E/$4F`; each includes palette-1 Sky Palace scenery tile `$18` beside the real corner. Host settings decode only immutable ROM assets and never sample live scene VRAM. |
| BG mode / layers | `$2105` BGMODE, `$212C/2D` main/sub, scroll regs | PPU | "set up background layers/scroll" | — | 🔴 |
| HDMA (raster fx, HBlank) | HDMAEN `$420C`; ActRaiser drives ch 2/3 (and others) | HDMA | "per-scanline effect" | HDMA tables | 🔴 |
| Mode 7 (overworld / transitions) | m7matrix; `$2134` multiply; the act-select spin | PPU mode 7 | "rotate/zoom the world map" | m7 transform params | 🔴 |
| Brightness / forced blank | `$2100` INIDISP | PPU | "fade in/out, blank during build" | — | 🔴 |
| Palette load | CGRAM writes | CGRAM (15-bit ×256) | "set palette N" | **palette id / table TBD** | 🔴→🟡 |
| **Action OBJ atlas + effect overlays** | `$02:BC9E`: ROM `$07:8000-$07:9FFF` → VRAM `$2000-$2FFF`, palette `$07:D040-$D09F` → CGRAM `$C0-$EF`, then magic-selected `$06:A400+` → VRAM `$2D40` (128 words). `$00:96C3-$96F5`: object `$38` selects `$06:A000+` → descriptor slot 0 → VRAM `$2D80` (128 bytes). | VRAM/CGRAM | "load the shared action atlas and replace the reserved magic/effect tiles" | common atlas plus magic/effect selector; **not a per-enemy sheet allocator** | 🟡 loaders and destinations mapped; individual selector-to-effect names still need cataloguing |
| Sim-mode object sprite/behavior identity | ROM tables `$01:E099` (behavior/anim data ptrs) + `$01:E7D9` (sprite-frame ptrs), one 16-bit entry per object type — see "Sim-mode object/sprite spawn & OAM-build system" below | not VRAM directly — feeds world record `+00`/`+08`, which downstream OAM code (`ADAD`/`AE6F`) reads | "this object type's behavior and frame-composition asset" | **located 2026-07-01; bases corrected 2026-07-02** | 🟡 identity/assigner/emitter chain mapped; ROM character-upload identity remains to be catalogued |
| Sim-mode per-frame building/icon update | `$01:8000` (bank 1) — see "Sim-mode dispatch structure" below | VRAM/OAM (downstream, not yet traced) | "update this frame's city/building/icon visuals" | region (`$19`) gates which sub-block runs; several `JSR (abs,X)` tables (`$2920`, `$208E`, `$B420`) select per-building/icon variants | 🔴 (dispatch structure mapped; the actual VRAM writes inside the deep `$018170+` body not yet traced) |

Death Heim boss-warp presentation seam (`0701`, 2026-07-14): snapshot
`runs/20260714-174654/snapshots/snap_00_gf1436` has camera `$22=0` and both
BG widths `$0200`. VRAM/CGRAM reconstruction identifies BG1 as the causeway and
BG2 as faces plus animated fog/water. The room therefore bypasses world-edge
side-space reduction, clamps BG1+BG2, and uses the shared PPU's banded cyclic
repeat on BG2 `y=144-223`; the margin tilemap transaction is unnecessary and
skipped. This is a rendering seam only—the original BG scroll, animation, VRAM,
and gameplay state remain authoritative. Direct testing on 2026-07-14
confirmed centered complete faces/causeway and clean animated fog across both
margins.

The post-final-boss `0701` switch requires boss-rush progress `$0347>=7`, but
must not wait for current-song id `$0334>=3`. Paired captures in
`runs/20260714-184728/` prove why `$0347` alone is too early:
`snap_01_gf14676` is `$0347=7/$0334=0` with faces still visible, while
`snap_02_gf15031` is `$0334=3` with sky/cloud/water. The finer boundary is the
game's own background-page handoff: `$00:F5C2-$F5E3` runs the fixed-color
fade-to-black, `$F5E4-$F5EF` waits for the statue-removal child, and
`$F5F0-$F619` writes BG1SC/BG2SC `$64/$74` before starting the fade-in at
`$F625`. Song id `$0334=3` is not selected until `$F650`, after that fade-in
and an additional `$0349` wait, which direct testing in
`runs/20260714-185817/` found visibly late. The render policy therefore
switches when `$0347>=7` and the live BGSC page bases are `$64/$74` (with
song id `$0334>=3` only as a settled-state fallback). It clamps BG1 and
whole-scanline mirrors BG2 instead of using the
normal `y=144-223` band. Mirroring removes the cyclic cloud-edge seam;
resident face tiles remain irrelevant because padding copies the live
post-window rendered BG2. Direct testing on 2026-07-14 confirmed that the clamp
changes during the black frame: no statues leak into the margins and the
mirrored sky is already active when the fade-in begins.

Death Heim narrow-BG2 presentation evidence (2026-07-14): maps `0704-0707`
directly show the same mountain/parallax motion; `0702` and `0703` are
provisionally classified with that family. With `$32=$0100`, reflection makes
the margins move opposite the authentic center; `$19=$02-$07` therefore uses
isolated scanline cyclic repeat. See `docs/rendering-engine.md` §13 and
`runs/20260714-173750/snapshots/snap_00_gf4875`.

Final-boss map `0708` is a separate raster seam. Captures
`runs/20260714-183142/snapshots/snap_00_gf12574` and `snap_01_gf12654` show
camera `$22=0`, BG1/BG2 width `$0100/$0100`, and black margins. Snapshot
reconstruction identifies stacked BG1 star-road and BG2 star-field layers;
both are presentation effects with scanline/sine motion. The arena bypasses
world-edge side-space reduction. The first isolated-repeat implementation
(`repeat=$03`) filled both margins but caused the performance regression in
`runs/20260714-184728/`. BG1SC/BG2SC `$60/$70` prove both tilemaps already wrap
at 256px, so the optimized path draws both raw on the symmetric canvas
(`repeat=$00`), removing two clears and priority merges per scanline. Direct
testing on 2026-07-14 confirmed both the full-width raster effect and removal
of the isolated-repeat performance regression.

> **The asset-substitution seam is the loaders, not the draws.** When you find the routines that
> copy graphics ROM→VRAM and select animation frames, capture the table index they use — that
> index is the logical sprite/tileset identity. For **sim-mode objects** the type→behavior/frame
> tables are located. Action mode instead has a shared resident atlas plus small reserved effect
> overlays; the object definition pointer identifies the composition inside that atlas. F2
> snapshots dump VRAM/CGRAM/OAM when correlating logical identities with pixels — see `DEBUG.md` §9.

---

## Sim-mode dispatch structure (mapped 2026-07-01, chasing a graphics-corruption bug)

The main loop (`ResetHandler_M1X1`, bank 0) branches at `$00805F: LDA $18; BNE $8066; BRL $80E5` —
`$18 != 0` (action stage) goes to `$8066` (the `$8915` object loop + per-frame action routines,
already documented above); `$18 == 0` (intro/overworld/sim — see the "Game-state anchors" table
below for why sim mode reads as `$18==0`, not `08` as an earlier assumption had it) goes to `$80E5`,
the **sim-mode per-frame dispatcher**:

```
$0080E5  PHB; LDA #$1; PHA; PLB        ; DB = $1 for the rest of this dispatcher
$0080EA  LDA $19; CMP #$9; BEQ $8129   ; region 9 = a separate sub-flow (not yet traced)
$0080F0  JSL $018000                   ; sim-mode building/icon per-frame update (bank 1)
$0080F4  BCS $8125                     ; skip the rest of this frame's update if carry SET
$0080F6  JSL $2AFF8 / $1B21B / $1ACD9 / $3D06A   ; always-run subsystems
$008106  LDA $19; CMP #$7; BCS $8125   ; SECOND skip gate: region >= 7 skips the rest too
$00810C  JSL $2BEFC; JSR $845F; JSL $38193; JSL $19193; JSR $88D6; JSL $2C206; JSR $8465
$008125  PLB; BRL $8059                ; back to the main loop (re-yields at the vblank wait)
```

**Both `BCS $8125` gates are ruled out as corruption sources** (`AR_SIMTRACE` confirmed the
sim-mode update runs to completion; `$0019` matched the oracle). The 2026-07-01 corruption they
were checked for was the `$03:F5BE` per-town handler subsystem, since fixed — bug-ledger §7.13.

**`$01:8000`** (bank 1) is the sim-mode building/icon updater itself. Its own entry does a similar
region-gated early-exit (`LDA $19; CMP #7`/`#8` at the top decides whether to even enter the deep
body at `$018170`), then a further gate at `$018010-$018024` (checks DP `$347`, DP `$A1`, and long
address `$7F9750` — meaning ALL THREE must be in specific states to reach the deep body) before
finally running `JSL $1B1C7` and the actual per-building update logic. This function ALSO drives
the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables flagged as `SUPPRESSED` in the regen report (see
`DEBUG.md` §7.9) — `$B420` is a genuine static ROM table (5 real entries, confirmed by reading the
bytes), but `$2920`/`$208E` resolve to SNES hardware-register space (`$2000-$5FFF`) under LoROM,
meaning either they're populated at runtime via DMA (not yet confirmed) or the `JSR (abs,X)`
instructions decoding there are themselves decode artifacts from a wrong entry width — not yet
resolved, `AR_INDIRLOG=1` is armed to help if a future investigation reaches these sites.

---

## Sim-mode object/sprite spawn & OAM-build system (mapped 2026-07-01, tracing a graphics-corruption bug)

This is the deepest-mapped seam in the codebase so far — a full pipeline from **ROM asset tables** →
**object records** → **per-frame OAM output**, mapped end-to-end while chasing a real bug (a graphics
corruption traced to one missing spawn). Read this before touching sim-mode sprites/decorations, or
before designing a HAL replacement for them — it's the clearest existing map of "how does a sprite
get from ROM to screen" anywhere in this codebase, action-stage included.

### The pipeline, ROM asset → screen

```
ROM def-tables (per-object-type asset identity)
  $01:E099   behavior/anim-data pointer table (16-bit ptrs, one per object type)
  $01:E7D9   sprite-frame pointer table       (16-bit ptrs, one per object type)
        │
        ▼  $01:D072 and the per-type dispatch/update web
World object record (WRAM $7E:$0A00+, 44 records, stride $26)
  +00/+02  behavior pointers/state derived from $01:E099
  +06      object type
  +08      current frame-composition pointer derived from $01:E7D9 / frame lists
  +0A/+0C  world X/Y (also populated for one cohort by $03:813F staging copy)
  +0E      list/type id used by update/animation code; NOT ADAD's tile count
  +10      render status tested by ACD9 ($C000 set = skip)
  +12      behavior dispatch selector used outside the OAM leaf
  +25      delay/timer byte checked and decremented by ACD9 before drawing
        │
        ▼  per-frame $01:ACD9 → $01:ADAD or $01:AE6F
Frame composition at record +08
  byte 0     tile count
  then N × 5-byte parts:
    +0 flags/size, +1 signed X offset, +2 signed Y offset,
    +3/+4 tile+attribute word
        │
        ▼
OAM shadow $7E:$0380-$059F (512-byte low table + 32-byte high table)
        │  common NMI OAM DMA
        ▼
PPU OAM → screen
```

There is also a distinct fixed/overlay record array at `$06A0`: 48 records,
stride `$12`, ending exactly at `$0A00`. It is animated by `$01:AC70` and rendered
through the same leaf emitters, but uses fixed screen origins rather than the town
camera. Do not model `$06A0-$0Fxx` as one universal 38-byte object table in a
decompilation: the `$12`-byte fixed records and `$26`-byte world records have
different owners and field meanings.

### `bank_03_813F` — the position-staging copy (misleading at first glance, actually correct)

Called from `bank_01_AA56` during sim-mode setup. Copies a `$130`-byte block from a WRAM bank-`$7F`
staging area into the live object table at `$0B30,Y`:

```
$038141  REP #$20                      ; A = 16-bit, NO subsequent SEP in this routine
$03814A  LDA $7F7BFB ; ... ; LDA $038111,X ; TAX ; LDY #0     ; X = ROM table lookup -> block base
$038157  LDA $7F0000,X                 ; 16-bit READ from staging
$03815B  STA $0B30,Y                   ; 16-bit WRITE, advance BOTH X and Y by 1 (not 2!)
$03815E  INX ; INY ; CPY #$0130 ; BNE $038157
```

**This is a deliberate SNES overlapping-byte-copy idiom**, not a bug: doing 16-bit stores while
advancing the index by 1 means each store's high byte gets immediately overwritten by the next
store's low byte, netting a clean byte-for-byte copy despite 16-bit access width. `cpu_write16`
reproduces it exactly — confirmed correct against the ROM disassembly (2026-07-01). **Don't mistake
this pattern for a misdecode/off-by-one if you see odd-offset 16-bit writes coming from here** — it's
correct, and matches real hardware bit for bit.

`X_start` for a given object comes from `ROM[$03:8111 + word_at($7F:7BFB)]` — a second indirection
table selecting *which* `$130`-byte staging block to copy from. This only fills the **position**
fields; it does NOT populate `+00/+06/+08/+12` (behavior/type/sprite/status) — those come from the
missing spawn below.

### Town camera writer: `$01:B4C6`

Both known callers (`$01:80AD` and `$01:B22A`) invoke `$B4C6` before the town
behavior/OAM pass. It derives `$22=clamp($0AEE-$80,0,$100)` and
`$24=clamp($0AF0-$70,0,$11F)`, conditionally adds one-frame shake from
`$7F:9F65/$9F67`, then clears both shake fields. Because `ACD9` later uses
`$22/$24` as its world origin, this writer is the safe shared seam for camera,
BG scroll, sprites, and projectile lifetime.

The faithful HLE changes only corrected-widescreen town X bounds to
`[extra,$100-extra]` (16:9: `[$002B,$00D5]`) and applies the same interval to
shake acceptance. This keeps `[camera-extra,camera+$100+extra)` inside the
512px town. Vertical follow, register/flag preservation, shake clearing, and
the JSL/RTL stack contract remain authentic. `AR_WS_SIM=0` and RAW wide use
`[0,$100]`; `AR_WS_SIM_CAMDBG=1` reports boundary transitions. The cfg
registration was regenerated and direct simulation-mode testing on 2026-07-14
confirmed the camera remains inside the corrected-wide bounds as expected.

### `bank_01_ACD9` / `bank_01_ADAD` — the per-frame OAM rebuild (runs every frame, by design)

`ACD9` runs unconditionally every sim-mode frame (called from `bank_01_9284`). It is **NOT** a
one-time init despite superficially looking like one — real hardware ALSO rebuilds the entire
decoration-OAM table from scratch every single frame (confirmed via oracle: `$0380-$057F` gets
writes every frame on real HW too, just with *stable* output since nothing moves). This is standard
SNES practice — don't try to "fix" it into a one-shot.

Per frame, `ACD9`:
1. **Initializes the shared destination state:** low-table cursor `$98=0`, high-table cursor
   `$9A=$0580`, high-table mask `$9C=1`. It hide-fills the full 512-byte low table
   `$0380-$057F` with `(x=$00,y=$E0)` words; the packed high table is maintained by the emitters.
2. **Fixed/overlay scan:** origin `$94=$FFF0`, `$96=$FFEE`; set attribute bias `$8F|=$1000`;
   scan 48 `$12`-byte records from `$06A0` to `$09FF`. Record `+10 & $8000` skips the entry;
   otherwise call `$01:AC70`, then `$01:ADAD`. These coordinates are camera-independent and
   must remain on the authentic screen in a widescreen implementation.
3. **World scan:** origin `$94=$22-$10`, `$96=$24-$10`; clear `$8F&=~$1000`; scan 44
   `$26`-byte records from `$0A00` (reduced to one record while `$7F:9754 != 0`).
   Record `+10 & $C000` skips the entry. Byte `+25` is a delay/timer gate and is decremented
   when nonzero. `$7F:9752 & 2` selects alternate emitter `$01:AE6F`; otherwise it calls
   `$01:ADAD`.
4. **Leaf emission:** `ADAD`/`AE6F` load the frame pointer from record `+08`; the first byte,
   not a live-record field, is the tile count. Each following five-byte part supplies flags/size,
   signed X/Y offsets, and the tile/attribute word. Accepted parts advance the low-table cursor
   four bytes, pack x-high and size into `$0580+`, and stop at `$0200`. Rejected parts park
   `Y=$E0` in the current slot without advancing it. On return the emitter saves the advanced
   cursor to `$98`, so all scan segments share one allocation stream. This hand-off was
   lldb-verified (`$00→$10→$14→$3C→$54...`).

`ADAD` uses the frame word unchanged except for OR-ing `$8F`. `AE6F` applies
`(attr & $F1FF) | $0600 | $8F`; this is an alternate attribute/palette path.
The selection bit is known, but the gameplay meaning should remain unnamed until
it is correlated with a captured event.

For a decompilation, keep the emitter as a pure composition function with an
explicit OAM allocator. Its native geometry is:

- base X/Y = `record(+0A/+0C) - origin($94/$96)`;
- component offsets are sign-extended for byte values `$81-$FF`; `$80` follows
  the ROM's special non-negative comparison path and must not be simplified to
  ordinary `int8_t` without proving equivalence;
- horizontal biased coordinate must be `< $0110`; accepted x is stored minus 16;
- vertical biased coordinate must be `< $00F0`; accepted y is stored minus 17;
- component flag bit 0 becomes the OAM size bit, while x bit 8 is separately
  packed into the high table.

The natural widescreen seam is therefore the horizontal predicate in `ADAD` and
`AE6F`, gated to the world-record scan (`record >= $0A00`). The fixed scan must
retain the authentic predicate so fixed overlays do not leak into the margins.

Implementation regenerated and directly validated 2026-07-14:
`recomp/bank01.cfg` HLEs both leaves to a
shared faithful composition port. Its alternate mode preserves AE6F's exact
`(attr & $F1FF) | $0600 | $8F` transform; all offset, vertical, rejected-slot,
cursor, and packed-high-table behavior remains shared with ADAD. Only bases in
`$0A00-$1087` can receive a widened horizontal bound, computed from current
camera `$22` and the same `left<=cameraX` / `right<=$0100-cameraX` caps as BG1.
`AR_WS_SIM_SPRITES=0` is the authentic fidelity gate and
`AR_WS_SIM_SPRDBG=1` reports newly admitted margin components. The direct town
run confirmed that enemies now compose completely into the margins without
changing the fixed/UI segment.

### Angel arrow lifetime: `$01:B41A-$B4AE`

The angel arrow is not a separate fixed-screen renderer. It occupies the
dedicated world record at `$0B0A`, inside the same `$0A00-$1087` scan widened
above. `$01:B41A` dispatches its three states through the handler-minus-one
table at `$B423`: idle `$B429`, spawn `$B42A`, and movement `$B44B`. Spawn copies
the angel's direction from `$0AE4+$22`, initializes the record through `$CFF2`
and `$AC70`, then selects state 2. Movement adds velocities `+1A/+1C`, calls
`$B473`, continues animation through `$AC70` on carry clear, or clears `+12`
and releases the slot through `$01:B810` on carry set.

`$B473` is therefore a **lifetime gate before OAM composition**, not another
sprite clipping leaf. Its authentic predicate tests the arrow's `x+4` against
both the 512px town hard bounds and `[cameraX,cameraX+$100)`, then tests Y
against the same 512px bounds and `[cameraY,cameraY+$E0)`. This explains why
the regenerated wide emitter fixed enemies but the player's arrow still
vanished at the old screen edge. The staged faithful HLE keeps the `x+4`
anchor, DP `$00` scratch writes, carry contract, 512x512 hard bounds, and the
entire vertical predicate; only the horizontal camera interval becomes
`[cameraX-leftMargin,cameraX+$100+rightMargin)`. `AR_WS_SIM_SPRITES=0` restores
the authentic interval, while `AR_WS_SIM_SPRDBG=1` adds
`[ws-sim-projectile]` evidence when an arrow is alive only because of the
margin extension. Regenerated direct testing on 2026-07-14 confirmed the arrow
remains alive and renders correctly across both live margins.

Static follow-up found no second shared world-projectile boundary to widen.
`$B473` has exactly one call site, the arrow movement state at `$B45F`, and
`$B810` has exactly one call site, the arrow-release path at `$B46E`.
Meanwhile `$01:B898` walks every active `$26`-byte world record from
`$0A00` through `$1087` and dispatches its type handler before `ACD9` composes
the result. Remaining enemy shots, construction actors, lair effects, and
rewards should therefore be validated as ordinary world records first; add a
new lifetime patch only after a captured symptom identifies a content-specific
gate. The `$06A0-$09FF` fixed/overlay array remains a separate screen-space
system and must not be widened speculatively.

**The OAM-flood bug mechanism** (found 2026-07-01, root cause is upstream — see next section): if an
object has a null/bad frame pointer at `+08`, the first byte read at that address becomes a bogus
definition count (in the traced case `$D8`=216 instead of a sane `$04`). `ADAD` then floods OAM with a
repeating garbage pattern (`X=77,Y=44,tile=$55` — coincidentally the Town Hall's own position/tile)
before hitting its `CPX #$0200` bound. This LOOKS like a cursor-collision or codegen bug (multiple
"different" objects appear to write the same slots) but isn't — it's one object's own loop running
far too long, stomping everything downstream of it in that frame's build. **If you see a destination
address get many rapid, differently-valued writes within one object's processing, check that
object's `+08` frame pointer and its pointed-to count byte before suspecting the loop/cursor logic.**

### Root cause found: a whole spawn cohort is missing, not a single field

*(2026-07-02 update: the "unconverted spawn / cutscene actors" hypothesis below was refined by the
full town-architecture mapping in the next section — the 4 records are the town's lair/decoration
ACTORS, their spawner `$01:D072` IS converted, and the actual break was the town handler dispatcher
`$03:F5BE` being misdecoded so its handler subsystem never ran. The forensics are kept because the
asset-identity conclusion stands.)*

Comparing our recomp's F2-snapshot object records against the oracle's (same save, same repro),
found a **clean cohort of 4 consecutive objects** (`$0B30, $0B56, $0B7C, $0BA2`, stride `$26`) that
are almost entirely **uninitialized** in our recomp — only their position fields (written by the
`bank_03_813F` staging copy above) are present; `+00/+02/+06/+08/+12` are all `$0000` where real HW
has real values. Every *other* active object in the same frame has correct (non-null) values in
these fields — this isn't a general corruption, it's **exactly these 4 objects never getting
spawned**.

**This is the cleanest illustration in the codebase of the asset-identity seam** flagged in the
Graphics/PPU table above ("the asset-substitution seam is the loaders, not the draws"): the ROM
tables at `$01:E099`/`$01:E7D9` (bases corrected 2026-07-02) ARE the per-object-type
behavior/sprite identity — a future graphics-replacement HAL would intercept here (read "which
asset slot", substitute new art) rather than in the OAM-write plumbing (`ADAD`), which is pure
mechanism with no asset knowledge of its own.

---

## Sim-mode town architecture — the full map (2026-07-02, root-caused + FIXED the corruption+freeze)

The complete town simulation decomposes into FOUR cooperating subsystems. This is the most
completely-mapped gameplay system in the project and the natural starting template for a full
decomp of sim mode. (Forensic trail: `DEBUG.md` #18-25; confirmed fixed in-game 2026-07-02.)

**Bug-hunt note for future readers:** the F5BE fix below (#2) was necessary but NOT sufficient
— it fixed the lair-mask/event dispatcher, but the actual actor corruption/freeze needed a
SECOND fix, in #3's per-type dispatch chain (`$01:B898`/`B8C0` → `D04E` family → the spawn
battery). Both are now fixed and confirmed working; the sections below describe the final,
correct architecture, not the intermediate broken states.

### 1. The per-frame master loop — `$03:8193`

Called every sim frame. Skeleton:

```
PHP; PHB; REP #$20; LDA #$007F; JSL $008519      ; DB=$7F for town state
JSR $8238                                          ; (per-frame sub)
LDA $00:0347; CMP #7; BEQ exit                     ; sub-phase gate
LDA $7F:9750; BNE -> JSR $C147                     ; demo/attract hook (always 0 -> skipped)
... flag checks ($7F:91xx) ...
JSR $F5BE                                          ; TOWN HANDLER DISPATCHER (see #2)
INC $7F:91FE (16-bit); CMP #$02D0                  ; 720-frame (12s) periodic counter
  >= 720: reset + JSR $8271                        ; the 12-second periodic event
else:      JSR $8E0C                               ; the every-frame sibling
JSR $B898 ($01), $B1B7 ($01), ...                  ; object-update loops
```

`$7F:91FE/$9200` are frame counters (`$9200` resets on some per-frame condition — an idle timer).
The `$0347` word is never written in normal play (stays at boot value) — the CMP #7 gate is for a
special mode.

### 2. The per-town handler dispatcher — `$03:F5BE` (the bug site)

```
PHP; PHB; REP #$20; LDA #$007F; JSL $008519  ; DB=$7F
LDX $7BFB                                     ; town index * 2 (set by the act<->sim transition)
LDA $03:F5ED,X; TAX                           ; X = this town's handler-list pointer
loop: LDA $03:0000,X; CMP #$FFFF; BEQ exit    ; read handler-1 word; $FFFF terminates
      PHX; LDY #$F5E2; PHY; PHA               ; push cursor, push return-1, push handler-1
      SEP #$20; RTS                           ; RTS-trick CALL: jump to handler+1 at m=1,x=0
$F5E3: REP #$20; PLX; INX; INX; BRA loop      ; each handler RTSes back here
exit: PLB; PLP; RTS                           ; flag-transparent to the caller
```

- Outer table `$03:F5ED`: **6 towns** (Fillmore, Bloodpool, Kasandora, Aitos, Marahna, Northwall)
  → inner list pointers `$F5F9/$F601/$F609/$F60D/$F615/$F61B`.
- Inner lists (packed at `$03:F5F9-$F620`, each `$FFFF`-terminated): per-town handler sets.
  14 unique handlers; code starts at `$F621, $F671, $F68A, $F6BF, $F6FF, $F791, $F7AE, $F7D1,
  $F7F8, $F822, $F857, $F870, $F8A5, $F8CC`.
- The handlers are the town's **lair/event logic**: they test and maintain the per-town lair
  bitmask state at `$7E:9107+` (4 bytes/town, "open lairs") and `$7E:911F+` ("spawned lairs")
  via the helpers `$03:F46E` (test) / `$F479` (set) / `$F484` (clear), which use `$03:F497`
  (bit compute, scratch `$7E:914F`) and the WRAM-pointer tables `$03:DCA2`/`$DCAE`. They also
  drive the spawn-list engine (see #3) and post events.
- **Recomp seam note:** the `PHY #ret; PHA handler; SEP; RTS` idiom is invisible to static
  decoding. Fixed 2026-07-02 with `indirect_dispatch F5DF 20 idx:A tables:F5F9 ret:F5E3 sep:20`
  (bank03.cfg) plus a new value-keyed `idx:A` + `sep:` form of the directive (cfg_loader/decoder/
  codegen). Before the fix the whole handler subsystem silently never ran — town lairs/monsters
  never spawned (the graphics-corruption/freeze root cause) and the SEP leaked m=1 to `$8193`.

### 3. The spawn-list engine — `$033C/$033D` + `$01:AC36` (processes) and `$01:D072` (actors)

The town is populated by numbered SPAWN LISTS run through one engine, with parallel machinery for
the two object tiers (see #4):

- `$033C` = list id, `$033D` = cycle sub-variant (0-3 = a rotating variant selector the oracle
  advances every ~180 frames — day-cycle/blink phases; 4 = the special B1C7 tail pass).
- `bank_01_CFF2(A)` = store A into `$033C/D`, call `AC36`. `bank_01_AC36(X=entry)` =
  `entry.+02/+06 = ROM[$01:A227[list*2] + sub*2]` (script ptr), `+04=0`, `+00=1` — assign +
  activate a stride-`$12` process (tier 1).
- `$01:D072(A=type, Y=record)` = the stride-`$26` twin: `record.+00 = ROM[$01:E099 + type*2]`
  (behavior script), `+02 = ptr-4`, `+04 = 1`. Sprite half at `$01:D0F5-D127`:
  `record.+08 = ROM[$01:E7D9 + type*2]` (frame ptr) — invoked via the **56-routine per-type
  spawn battery `$01:BA23-$C793`** (one setup routine per object type, each ending
  `JSR $D072`).
- Town-entry sweep: `bank_01_AA56` (after the `$7F:97DA -> $0B30` staging restore via
  `bank_03_813F`) walks the stride-12 table calling `CFF2(entry.+0E)` per entry — the `+0E`
  field IS the entry's current list id, restored from the save and advanced by gameplay.
  `bank_01_8029 -> B1C7` wraps the sweep (`B52F` = switch-all-to-variant pass, `B6AE` =
  hide-all pass, `CFB3` ($03) = hide-sweep on town exit).
- Event path: `$01:8819` dispatches the event code at `$033E` through a jump table at
  `$01:F223`; one-shot list runners at `$01:8E11/8E22` (`LDA $000E,X; STA $033C; JSR $AC36`).

**The missing link: how a record's per-frame TICK reaches the spawn battery.** `D072` only
INITIALIZES a record once; what runs it every frame (and is what actually populates a freshly-
placed record's position/script/sprite fields) is a chain starting from the master loop's
`JSR $B898` (see #1's skeleton):

```
$01:B898  per-record TICK entry, called once per active record by the $8193 loop.
  $01:B8C0  per-TYPE class dispatch: PHX (save record ptr); LDA $B8D0,X (X = type*2,
            byte offset, NOT the record index); PLX (restore X = record ptr, so X is
            NOT usable as the dispatch key at the PHA/RTS site below); PHA the table
            word; RTS -> class handler (26-entry table at $B8D0, e.g. type $12/$13 ->
            $B9EC/$BE4F). QUIRK worth remembering for any future manual decomp/asm
            work: because of the PHX/PLX bracket, the dispatch index must be read from
            the PUSHED VALUE (A at push time), not from X at the RTS -- X has already
            been overwritten with the record pointer by the time the RTS fires.
  class handler (e.g. $B9EC)
    JSR $D063           ; latch: "have I run my one-time init?" (record.+12 bit15)
    if not yet init:  LDY #<battery-table>; JSR $D04E   ; $01:D04E-D062 family --
                        selector = record.+12 & $7FFF, table ptr passed in Y by THIS
                        caller -> lands in the 56-routine spawn battery ($BA23-$C793)
    battery routine (e.g. $BA18)
      sets type-specific fields, JSR $D072              ; position/script/sprite init
```

So the full per-record lifecycle is: town-entry sweep (`AA56`) or event (`8819`) assigns a
type via `CFF2`/`AC36`-equivalent bookkeeping -> every frame, `B898` ticks the record -> `B8C0`
routes by type to a class handler -> the class handler's one-time latch (`D063`) triggers the
`D04E`-dispatched battery routine -> the battery calls `D072` to actually populate the record.
Skip ANY link in this chain (as the recomp did, for months, at `B8C0`/`D04E`/their targets) and
a record gets its type field only, never its position/script/sprite -> permanently-garbage
sprite. See `DEBUG.md` #18-25 for the full bug-hunt trail; `$01:B898`/`B8C0` also appear in
"Function roles discovered" below with the specific PHX/PLX index-model bug that hid this for
an extra round.

### 4. The two object tiers (shared design, separate engines)

| | Tier 1: stride-`$12` "processes" | Tier 2: stride-`$26` "actors/records" |
|---|---|---|
| Table base | `$7E:06A0+` | `$7E:0AE4+` (records 2-5 = `$0B30/56/7C/A2`) |
| Assigner | `$01:AC36` (list tables `$01:A227`) | `$01:D072` (type tables `$01:E099`/`$E7D9`) |
| Executor | `$01:AC70` (per frame, from `ACD9`) | `$01:D08F` stepper + `$01:B0xx-B1xx` update loop |
| Script format | `[delay, frameptr16]*`, `$FD`=hide, `$FE`=loop(count `+04`), `$FF`=set-loop | behavior scripts around `$01:DDxx-DFxx` |
| Key fields | `+00` timer, `+02` cursor, `+04` loop count, `+06` script base, `+08` current frame ptr, `+0A/+0C` position, `+0E` list id, `+10` status (bit0 visible, bit15 hidden) | `+00/+02` behavior ptrs, `+04` flag, `+08` frame ptr, `+0A/+0C` position, `+0E` list id, `+10` status word, `+12` dispatch selector |
| Behavior dispatch | (scripts only) | per-TYPE via `$01:B898`/`B8C0` -> class handler -> `$01:D04E-D062` (table ptr in **Y**, selector = `+12 & $7FFF`) -> spawn battery -> `D072` |
| Rendered by | `ACD9` scan -> `ADAD` OAM build | same (second scan segment, X from `$0A00`) |

What blinks/animates in a healthy town: the oracle shows the whole family toggle every ~131
frames (stride-12 `+10` words, records 0/2-5/20 status words) plus per-record frame animation
(`+08` stepping through `$01:E838`'s frame list, e.g. `$E6CA -> $E6D0 -> $E6D6`).

**Decomp guidance:** the four subsystems above are the natural C module boundaries
(`town_mainloop.c`, `town_handlers.c` per-town data-driven, `spawn_lists.c`, `objects.c` with the
two tiers as structs). The ROM data tables (`$03:F5ED+` handler lists, `$01:A227` script lists,
`$01:E099/$E7D9` type identities, `$01:D128+` placement records, `$01:E838` frame lists) are the
**level/asset script seam** — a future editor or HD-asset pipeline replaces THESE, not code.

### 5. The development cycle (hourglass → town growth), mapped 2026-07-04

The whole chain, from timer to tiles (DEBUG.md §7.13 has the debugging story):

1. **Attempt** — each hourglass expiry, the consumer loop in the `$8Fxx` scan pushes
   continuation `$9315` + a handler-1 word planted in WRAM `$7C45/$7C47` (by `$91AE/$91BC`)
   and RTS-dispatches to one of FOUR development-mode handlers: `$9390 / $944B / $9505 /
   $95B3`. Each gates on `$7C37` (attempts remaining) and compares town population
   `$6B26,X` against the per-town threshold table at WRAM `$021C,X` (different +offset per
   mode — these four ARE the "grow / grow-more / shrink / clamp" development flavors).
2. **Scheduling** — on a pass: allocate a development record (`$9D9F`, per-town list from
   table `$03:DC74`), pick the target site (`$8D18` + direction tables `$03:8D82+`,
   landmark-relative via `$7C9D/$7C9F`), write the map-marker bytes (`$8C84`→`$8CF9`:
   eligibility slot `$7F:9758+X` = target coords <<4 from `$7D3F/$7D41`, coord tables
   `$03:D2FA/$D306`), post **COP event `$9C`** and activate **spawn-list 6** (`$033C`) —
   that list spawns the builder/people actors (records at `$0E02+`).
3. **Execution** — while construction is active (`$7CFB` nonzero) the master loop `$8193`
   swaps its per-frame call to `$89F7`: an 8-frame-tick step machine that walks a step
   table at `$03:8A7E` and runs the scanner `$9DE4/$9E5A` over development records. Each
   record dispatches (pushed continuation `$9E31/$9EC4`) through 7 outer handlers
   (`$A011/$A0CB/$A19B/$A237/$A296/$A2EF/$A35E`, one per development kind), each of which
   re-dispatches through its per-type table (`LDY #table; BRL $9ED3`; dispatch RTS at
   `$9EF3`; 7 tables × 8 types = 49 build-step handlers in `$A004-$A4B8`) — the routines
   that write house/road tiles into the `$7F:2000+` town map and stage the visual updates.
   Chain continuations: handler RTS → `$9EF4` → `$9E32/$9EC5` (loop resume).
4. **Completion** — `$839C` (dev-eligible census over `$7F:9758`) and `$83EF+` (the
   `$8440`-family consumers) apply the 2×2 house-tile marks (`$08`/`$E0` bytes at
   `$7F:2000,Y`) and retire the slot (`$9758,X = $FFFF`).

**Decomp seam value:** the four mode handlers + the 49 build-step handlers + their tables
are the complete "town growth ruleset" — a `development.c` with the step tables as data.
The population thresholds at `$021C` and the step table at `$03:8A7E` are the obvious
balance/mod knobs. Every layer of this is pushed-continuation RTS dispatch — cfg model
notes (why `rts_dispatch`, not `func`) live in bank03.cfg comments + DEBUG.md §7.13.

### 6. Town actor behavior + animation system (bank $01, mapped 2026-07-04)

The people/builders/effect sprites (dev-cycle walkers, church-cutscene pair, etc.) are
tier-2 actor records (`$0E02`+, stride `$26`). Their per-frame life-cycle is a small
data-driven VM:

- **Behavior-state dispatch** — `$01:CD0C` (`LDY #$CD12; BRL $D04E`) selects the per-frame
  handler by the actor's state field `record+$12` via the `$D04E` PHA/RTS dispatcher. Table
  `$01:CD12`: state 0 → `CD22` (spawn/idle, *paced*), 1 → `CD35` (walk-script executor,
  *unpaced advance*), 2 → `CEEB`, 3 → `CEFA` (paced walk), 4-6 → `CF09`, 7 → `CFAA`.
- **The animation SCRIPT** — each actor walks a byte stream at `$7F:xxxx` (pointer in
  `record+$16`). `$01:CFC7` reads one byte per tick, gated by a per-actor delay so a step can
  hold for N frames (this is what paces the walk — it is NOT a fixed frame rate). `CD35` then
  dispatches a non-`$7F` byte through table **`$01:CD6F`** (18 command handlers `$CD93..$CEE5`,
  RTS at `$CD6B`, ret continuation `$CD6C` → `BRL $AC70`): byte handlers set position,
  advance the behavior state (`$CDCC` etc. do `LDA #$3; STA $0012,X` → enter the paced walk),
  install delays, spawn OAM, etc. A `$7F` byte is the segment-end command.
- **cfg:** the `$CD6F` dispatch needs `indirect_dispatch CD6A 18 idx:A tables:CD6F ret:CD6C`
  (bank01.cfg). Without it the handlers are undecoded and the actor spawns but never advances
  its state → frozen sprite. See DEBUG.md §7.14.
- **Decomp seam:** table `$CD6F` = the "actor script opcode table"; the `$7F` byte streams are
  the per-actor animation programs. A future editor edits the streams; the 18 handlers are the
  opcode implementations (`actor_vm.c`).

---

## Sim-mode town-map GRAPHICS pipeline (VRAM seam, mapped 2026-07-05)

The town map reaches the PPU through two WRAM→VRAM DMAs — **the seam an HD/replacement tile
backend hooks**:

| What | WRAM source | VRAM dest | DMA'd by | Notes |
|---|---|---|---|---|
| BG tilemap (32×32 tile-index grid) | `$7F:1000` | VRAM `$6800` | `bank_02_AEBB` | full-map upload, size $800 words; the map's tile *layout* |
| Animated tile GRAPHICS (bitplanes) | `$7F:BA00` | VRAM `$0000` | `bank_02_AF3D`/`AF42` | column-stride mode-1 DMA (`$2116` dest = `$D3`, src `$D2:$D0`, size `$D5`); the tile *pixels* for animated/scrolled tiles |

The `$7F:1000` tilemap buffer is built from town map data; `$7F:BA00` holds
decompressed/animated tile character data (written by `bank_02_BAF5`). A modern backend can
intercept at either the WRAM buffer (replace tile indices / graphics) or the DMA (redirect to
a hi-res path).

**Confirmed BG register layout for the sim town map** (2026-07-05, `AR_TRACE reg` channel):
`bgmode=$09` (mode 1, BG3 priority), `bgTileAdr=$0500` → **BG1 char/tiles base = VRAM `$0000`**,
BG3 char base `$5000`; `bgXsc=[$63,$73,$58,$00]` → BG1 map `$6000`, BG2 map `$7000`, BG3 map `$5800`.
So BG1 (the town playfield) reads its **graphics from `$0000`** and its **tilemap from `$6000`** —
this is the layer a replacement-tile HAL must match.

**The sim-mode graphics-upload orchestrator is `bank_03_8053`** (called from the master loop). It
sets `$2116` and streams: the town **tilemap → VRAM `$6000`** (its `$8100` byte-copy loop, source
`[DB:$0000]`) and **tile graphics → VRAM `$0000`** via the upload primitive **`bank_02_B28E`→`B6C8`**
(reads ROM `$05:8000`, byte-extract `& $FF`). **`bank_02_BAF5`** is the inverse — a VRAM→WRAM
*readback* (save) of `$0000` through the `$2139` read port into `$7F:B800`, later re-DMA'd back;
this save/restore loop *perpetuates* whatever is in `$0000` frame-to-frame.

**Lair-seal corruption — ROOT-CAUSED (DEBUG.md §7.15, fix `exit_mx_at 039D4D 0 0`):** it was not a
graphics-pipeline bug at all — `bank_03_8053` ran its `LDA #$6000; STA $2116` at m=1 (an exit-mx
leak from `$9D4D`), so the tilemap upload's VMADD truncated to `$0000` and dumped tilemap indices
into BG1's *character* VRAM. Lesson for this seam: a "graphics corruption" here can originate in the
**m/x width** of the upload's address setup, not in the tile data — check `AR_TRACE --vmadd/--leaks`
before suspecting the buffers.

---

## Story-event system — the `$03:F921` event VM (mapped 2026-07-06, the rock-zap/fire arc)

The sim-mode scripted events (rock zap, house fires, quakes, town-specific story beats) run
through one table-driven dispatcher. Fully mapped and registered (bug-ledger §7.16); this is
both a decomp target (`event_vm.c`) and a clean mod seam (the record table is pure data).

**Dispatcher `$03:F921`:** `PHP; PHB; REP #$20; LDA #$007F; JSL $008519` (DB←`$7F`), computes the
event's grid coords `$7C11/$7C13` from pixel coords `$90E1/$90E5 >> 2` (or `$7C11=$FFFF` when the
event type carries no position), then walks the **record table at `$03:F99A–$F9F4`** from `$F951`:

- 6-byte records `[event_type, town, grid_x, grid_y, handler-1 (word)]`, `$FF`-terminated.
- Match: `rec[0]` vs **`$90EB`** (event type), `rec[1]` vs **`$7BF9`** (current town),
  `rec[2]/rec[3]` vs **`$7C11`/`$7C13`** (grid coords; skipped when `$7C11` is negative).
- On match: `PHX` (record ptr), `LDY #$F989; PHY` (push continuation), `LDA $030004,X; PHA;
  SEP #$20; RTS` — PHA/RTS dispatch to the record's handler at **m=1, x=0**.
- Handler RTSes back to `$F98A`: `PLX; X += 6; BRA $F951` — the loop CONTINUES, so multiple
  records may fire for one event. Loop end: `REP; PLB; PLP; RTL` at `$F997`.
- 15 records → 11 unique handlers (`$F9F5 $FA2A $FA5F $FAB8 $FAF8 $FB3C $FB5F $FB8F $FBD1
  $FBD7 $FC1B`), all registered in bank03.cfg. `$FA5F` = the Fillmore rock-zap; the `$FB5F`
  family (×5 records) is the shared per-town story-beat handler.

| Event WRAM | Meaning |
|---|---|
| `$90EB` | pending event type (record `rec[0]` key; gate `< 4` selects the coord-bearing class) |
| `$90E1` / `$90E5` | event X / Y in pixels (zap target); `>>2` → grid coords |
| `$7C11` / `$7C13` | derived grid coords compared against `rec[2]/rec[3]`; `$FFFF` = no position |
| `$7BF9` | current town id (record `rec[1]` key) |
| `$90F7` | set to 1 by handlers on event accept (event-active flag) |
| `$7CC9,X` / `$7CD5,X` / `$7CE1,X` | per-town event state / timer-reload / timer (driven by the `$03:8700` sub-dispatcher table `$8713`; `$872A` decrements `$7CE1`, reloads from `$7CD5`, advances state via `$7CC9`) |

**Modding note:** adding/removing/re-positioning a scripted event = editing a 6-byte record in
the `$F99A` table (plus a handler if it's a new behavior). The handler set is closed and small.

**Known-unmapped sub-seam (risk):** ~45 sites across `$03:E0xx–$F9xx` (inside the event handlers'
own bodies) dispatch via **runtime WRAM JMP vectors `($6E20)` and `($7920)`** — currently emitted
as trap stubs (regen report "UNRESOLVED INDIRECT DISPATCH"). No event exercised them yet in play;
whichever event first walks into one will `[dispatch-oob]` loudly. Closing this needs the vector
WRITERS traced once (who stores to `$6E20`/`$7920`), then a cfg/indirect-vector authorization —
it cannot be closed statically.

**Resolved thread (2026-07-07):** the one-of-N cutscene actor sprites (DEBUG.md §7.17 —
lair-seal attackers, Bloodpool lightning pair) were fixed by registering the **`$9220`
coroutine-resume family**, NOT the `$9FCD` dispatcher family (which remains statically
censused but symptom-free). The town-event code yields via a resume slot: a trampoline
does `LDA #<resume-1>; PHA; LDA $9220; PHA; RTS` and the counterpart stores the next
resume-1 into `$9220`. Three known members, all dispatch-only entries (no C fall-through):
`$03:CA7A` (JSL $01B790; RTS), `$03:CDAD` and `$03:CE57` (PLX; BRL loop-resume). CE57 was
found by tools/resolve_miss.py's first run, closing bug-ledger #13's untraced `$CE56`.

---

## Sim-mode REWARD-GRANT web — `$01:9C6F` (mapped 2026-07-07, the lost-scroll arc, DEBUG.md #18b)

All sim rewards (scroll grants, extra-life/max-stat gifts, town offerings' effects) go through
one RTS-trick dispatcher — a clean `rewards.c` decomp target and the model example of the
SAFE-to-register TAIL-dispatch shape (the containing function ENDS at the dispatch RTS, so
handlers single-execute; proven by `$0295`=01 after one grant):

- **`$01:9C6F`**: `REP #$20; AND #$00FF; DEC; ASL; TAX; LDA $019C94,X; LDX #$9C82; PHX; PHA;
  SEP #$20; RTS` — reward id (1-based) indexes the 20-entry handler-1 table `$01:9C94-$9CBB`;
  every handler enters **m=1 x=0** and RTSes to the shared `$9C83` (`PLP; RTS`).
- 17 unique handlers `$9CBC-$A02F`, all registered in bank01.cfg. Known semantics:
  `$9CBC` = no-op (ids 1-4), `$9CBD` = +1 max-stat `$02AB`, **`$9CD6` = +1 magic scroll**
  (`INC long $0295` persistent + `INC long $0021` working + message `LDY #$8994; JSR $93A8`
  + sound + BRK syscall $0D).

**MP/scroll persistence model (DEBUG.md #18b):** `$0295` (in the `$0290` save-stats block:
$0291 level, $0293 HP, $0295 MP, $0297 next-level pop, $0299+ HAVE flags, $02A2+ items,
$02AB lives) is the PERSISTENT count; `$21` is the act-mode WORKING copy, loaded at
`$02:84E0` (`LDA $0295; STA $21`) — MP refills to the persistent max each act by design.
Act-mode pickups INC only `$21` ($00:887E via the $00:87BD item dispatch); sim grants INC
both. The stats block has NO `8D`-form direct writers — event/reward handlers use `AF/8F`
long addressing (grep lesson; `tools/romxref.py` handles this).

---

## Magic system — full wiring map (2026-07-07, the "magic dead" arc, DEBUG.md #18)

End-to-end seam map for casting; every stage verified. Decomp target: `magic.c`.

1. **Unlock**: HAVE flags `$0299-$029C` = 01/02/03/04 (Fire/Stardust/Aura/Light), granted by
   reward web / act pickups; persisted in the `$0290` stats block.
2. **Equip** (sim/menu): `$01:915D` derives the SELECTED-magic byte **`$02AC`** from
   `$0299,X` (`AND #$7F`).
3. **Input**: NMI joypad shadow at `$02:AC4E`: **`$00A0` = `$4218` & `$F4`** (held byte:
   bit7=A, bit6=X; `$F4` is an input-enable mask the cast STZs) — `$00A1` = the `$4219`
   byte (B/Y/Select/Start/dpad). LEVEL-sensitive, not edge.
4. **Trigger**: player-object handler `$00:9832` (obj base `$08A0`, handler ptr `$08B2`)
   tests `$A0` `BIT #$00C0` at `$00:9843` → `BRL $00:9DE1`. (Y-attack test on `$A1` sits
   7 bytes earlier in the same handler — if sword works, the gate is being reached.)
5. **Gate** `$00:9DE1` (all four must pass; each failure BRLs to the shared bail `$00:984E`):
   `$F8`==0 (no cast in progress) → `$02AC`!=0 (equipped) → player state `$08D0`
   `BIT #$2008` CLEAR (not hurt/INVULN — the AR_NO_KNOCKBACK interaction, see dev-config) →
   `$21`>0 (MP).
6. **Cast**: `DEC $21`; `$08D0 |= $0010`; `INC $F9`; `STZ $F4`; `JSR $862A/$8E2F/$8623`
   (effect spawn chain). The spell-projectile objects run in the bank-0 object system —
   **first-ever-executed 2026-07-07; `$00:B8AB` garbage-variant is the open item there
   (DEBUG.md #19)**.

---

## Frame / timing  (mostly already HLE'd — the model is understood)

| Seam | Routine / address | Hardware | Intent | Notes | Status |
|---|---|---|---|---|---|
| VBlank wait | `$00:8418`, `$02:A85E` (HLE → `ActRaiser_WaitForVblank`); inline 3-read spins (`$01:9284` et al) | RDNMI `$4210` | "wait one frame" | Three-tier model (`snes.c`): HLE'd routines yield; the 7 statically-whitelisted inline spin blocks (`kSpinBlocks`, from `find_yield_points.py`) yield once per read in the coroutine; in NON-yieldable contexts (NMI/IRQ — e.g. the mode-`$85` story-event wait chain `$01:9270→8C43→9284`) whitelisted spins FAST-EXIT bit7=1, unpaced (a hang there is otherwise unbreakable — bug-ledger §7.16); `[4210-wedge]` tripwire names the refusing gate if a spin ever sticks 4096 reads | 🟢 |
| NMI handler | `$8520` (`NmiHandler`) | NMI | "per-frame vblank service" | game frame `$0088` bumped here | 🟢 |
| Frame coroutine | `RunOneFrameOfGame` (`actraiser_rtl.c`) | — | host frame ↔ game frame mapping | coroutine yields at vblank-wait | 🟢 |

---

## Frame-rate decoupling — high-refresh presentation WITHOUT changing pacing (forward-looking seam map)

**The hard constraint first.** ActRaiser's entire notion of time is the **60 Hz (NTSC) logic tick**:
every timer, event trigger, physics step, animation-script advance, and the `$0088` game-frame
counter is keyed to one tick == one `NmiHandler` == one `RunOneFrameOfGame`. You therefore **cannot
speed up the logic** to get smoothness — that *is* the pacing. The only correct way to a higher
refresh rate is the classic **fixed-timestep logic + interpolated presentation** split: keep ticking
logic at exactly 60 Hz, and render *extra, interpolated* frames between ticks at the monitor's rate.

**The tick boundary (the seam you must preserve).** `RunOneFrameOfGame` = one atomic 60 Hz logic
tick (host yields to the game coroutine, which runs until its next vblank-wait, then NMI services
the frame). The `AR_TRACE` **`frame`** channel marks both edges (`vblank` = tick about to run,
`nmi` = tick serviced) — use it to *verify the tick cadence is clean* before building on it (a mode
that yields N times per tick — the 1/N-speed pacing-bug class, DEBUG.md §7.12/§7.13 — would break a
naïve accumulator; those must be fixed first).

**The presentable-state seams to interpolate** (all live in `g_ppu`, read by `RtlDrawPpuFrame` →
`g_pixels`; snapshot each at tick N-1 and N, lerp for in-between presents), in order of visible payoff:

| Seam | PPU state | Registers | Payoff | Caveats |
|---|---|---|---|---|
| **Sprite positions** | `ppu->oam[]` (rebuilt each tick by `bank_01_ACD9`/`ADAD`, §"per-frame OAM rebuild") | OAM via `$2104` | Biggest — smooth moving characters/enemies/effects | Must match sprites across ticks by slot/id; **snap (don't lerp) on spawn/despawn/teleport** (large Δ) or you smear |
| **BG scroll** | `ppu->hScroll[layer]` / `ppu->vScroll[layer]` | BGnHOFS/VOFS `$210D-$2114` | Smooth scrolling of the action-stage playfield | Per-layer; parallax layers scroll at different rates — lerp each independently |
| **Mode 7 matrix** | `ppu->m7matrix[0..7]` (`[6]/[7]`=scroll, `[4]/[5]`=center) | `$211B-$2120` | Smooth act→sim spiral + overworld map rotate/zoom | Interpolate the matrix, not the projected pixels |
| **Palette fades** | `ppu->cgram[]` | CGDATA `$2122` | Smooth fades (act-entry, INIDISP brightness ramps) | Lower priority; lerp in RGB, watch for wrap |

**Do NOT interpolate** (discrete — interpolating blurs/garbles): VRAM tile graphics, tilemap
indices, and anything the game *logic* reads. HUD/text layers usually look fine snapped — consider
interpolating world BG layers + sprites only.

**Where the hooks go.** The present loop is `src/main.c` (`RtlDrawPpuFrame` → `SDL_UpdateTexture`
→ `SDL_RenderCopy` → `SDL_RenderPresent`, ~line 737-781). The staged plan:
1. **Present-rate decouple (free, low-risk):** drive an accumulator so `RunOneFrameOfGame` fires at
   a fixed 60 Hz while `SDL_RenderPresent` runs at monitor Hz (vsync). No interpolation yet — just
   removes host-frame/logic-frame judder and validates the fixed-timestep loop. `AR_PACE`/`AR_PERF`
   already prove the 60 Hz cadence is separable from present.
2. **Sprite (OAM) interpolation** — snapshot OAM each tick, lerp positions, render the interpolated
   OAM for in-between presents. Biggest win.
3. **BG scroll interpolation** — the scrolling stages.
4. **Mode 7 + palette** — the sim overworld and fades.

Each phase re-runs the PPU rasteriser (`RtlDrawPpuFrame`) with the interpolated `g_ppu` fields
temporarily swapped in, then restores the true tick-N state so logic is never perturbed. The whole
scheme is **presentation-only**: `$0088`, timers, and logic never see the extra frames.

---

## Input

| Seam | Routine / address | Hardware | Intent | Status |
|---|---|---|---|---|
| Joypad read | auto-joypad enable `$4200` bit0; `$4218-$421F` | controller | "read player input" | 🟢 consumers mapped 2026-07-07: NMI shadow `$02:AC4E` → `$00A0` = `$4218`&`$F4` (A/X/L/R held) and `$00A1` = the `$4219` byte; game logic reads the SHADOWS (cast trigger `$00:9843`, attack test on `$A1`), never the ports. Only other direct port reads: `$00:8151` combo check + bank-2 `$4219` menu readers. See "Magic system" §3 |

---

## Save / persistence  (mapped 2026-07-01)

| Seam | Routine / address | Storage | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Save-file validity check | `$02:A88D` (checksum) called from `$02:A622` (title-screen gate) | SRAM `$700000-$701FEB` (checksum), stored expected values at `$701FEC`/`$701FEE` | "is this save data trustworthy enough to offer Continue?" | pass/fail via carry; no version/format ID beyond the checksum itself | 🟢 (clean pass/fail gate, algorithm fully understood) |
| Save-file body | SRAM `$700000+`, exact 8192-byte active `.srm`/lossless `.ini` | LoROM battery SRAM, banks `$70-$7D`; host `save_system.c` codec | lossless canonical image; town state combines `$1200+r*2` with `$13B6+r*2` bit 0; player status/inventory block `$1433-$147B`; unknown town-map block preserved raw | editor fields/address codecs 🟢; gameplay round-trip 🟡; town-map semantics 🔴 |

> **Load path:** `src/main.c` now attaches `save_system.c`, which transactionally
> decodes the boot-selected native or INI backend into the same `g_sram` buffer.
> The runner's old `RtlReadSram` remains available generically but no longer owns
> ActRaiser's runtime persistence. `cpu_sram_offset` (`cpu_state.c`) maps `(bank, addr)` for bank `$70`
> to a literal 1:1 offset (`(bank & 0xF) << 15 | addr`, and bank `$70`'s `&0xF` is `0`, so addr IS
> the offset) — confirmed correct against the checksum algorithm 2026-07-01. If a future save-format
> HAL is built, this is the one seam that's ALREADY a clean pass/fail gate; the body itself still
> needs its internal structure (city stats? per-building state? population?) mapped out.
> The version-1 INI stores every raw byte before applying only `SaveFieldDesc[]`
> overrides, so future decompilation can promote fields without losing the
> `$0000-$07FF+` town/terrain block or other unknown state. Runtime auto-persist
> compares a shadow image and atomically writes only the boot-selected backend;
> deliberate editor mutations re-sync that shadow so session-only edits cannot
> silently overwrite disk.
>
> **USA field correction (2026-07-16):** the reference editor publishes
> European-base offsets and subtracts two for USA saves. Applying that rule
> aligns Angel SP/HP, player name, Master level/HP/MP, magic, items, lives, and
> scores with the known `$0282-$02AC` WRAM status block. The raw `$01` town byte
> is not an “active” state: state is `SRAM[$1200+r*2]*2 +
> (SRAM[$13B6+r*2]&1)`, yielding Act 1=`0`, Act 1 cleared=`2`, Act 2=`3`, and
> Act 2 cleared=`4`. See `save-format.md` §3 for exact values.

---

## Game-state anchors (not hardware seams — symbol map for everything else)

These RAM addresses recur across all the debugging; keep `ram-map.md` authoritative.

| Addr | Meaning |
|---|---|
| `$18` | mode / region: `00`=**intro/overworld and simulation family** (`$19` distinguishes town/Sky Palace/temple/world-map submodes), `01`–`07`=action stage region N (Fillmore=`01`), `$20+`=transitions |
| `$19` | raw map/sub-flow within the region. With `$18=00`, `$01-$06` select Fillmore/Bloodpool/Kasandora/Aitos/Marahna/Northwall town views, `$07` Sky Palace, `$08` temple, and `$09` world map. The zero-based current-town ID also lives at `$7F:7BF9`, with doubled table index at `$7F:7BFB`. In action mode `$19` is **not** a uniform act number: e.g. Kasandora Act 2 is `$03`, not `$02` |
| `$1A`/`$1B` | transition **destination** (`$1B`→`$18`, `$1A`→`$19`, applied by the mode-switch `$00:8269`) |
| `$FB` | bit `0x80` = transition-request flag (set with `$1A`/`$1B` to stage a mode change; game consumes+clears it) |
| `$0100` | game-mode byte (watchdog dumps print it): observed `$85` during story-event cutscenes (rock-zap/seal wait chain); full value map not yet derived |

**Level warp** (`ActRaiser_Warp`, F6/overlay ACTION, registry-backed
`AR_WARP=<reghex><maphex>` e.g. `0202`): stages the
game's own sim→act transition — sets `$1B`=region, `$1A`=raw map, `$FB|=0x80` — so the game does the
full fade + level-load + switch itself. Trigger from the intro (`$18==00`, which works), bypassing
the broken post-act sim cascade. Observed: Fillmore act 1 entry = `$1B=01,$1A=01,$FB=80` (f=994) →
`$18` flips `00→01` (f=997) → act live (f=1004).

Verified entry targets from direct runs:

| Region | Act 1 | Act 2 | Raw-map notes |
|---|---:|---:|---|
| `$01` Fillmore | `0101` | `0102` | |
| `$02` Bloodpool | `0201` | `0202` | |
| `$03` Kasandora | `0301` | `0303` | `0302` loads invalid/garbage state |
| `$04` Aitos | `0401` | `0404` | |
| `$05` Marahna | `0501` | `0504` | |
| `$06` Northwall | `0601` | `0605` | `0608` is the Act 2 boss arena |
| `$07` Death Heim | `0701` | — | boss-rush hub; verified end-to-end through every rematch and the final boss (2026-07-14) |

**Fidelity limit (2026-07-12):** the current test workflow reaches Fillmore
act 1 before pressing F6, producing an action→action request. Only the three
destination/request bytes are staged; this transition shape has not been
observed in natural progression and may inherit action timing/object state.
The warp now logs its source `$18/$19` and warns. It remains useful for visual
coverage, but timing anomalies require an authentic or pillarboxed A/B repro.
The Death Heim failure is not merely this caveat: the current `0701` flow
consistently teleports to the first boss and crashes, so region `$07` is marked
broken until that transition/handler path is diagnosed.

| Addr | Meaning |
|---|---|
| `$1D` | player HP |
| `$E6`/`$E7` | action-stage timer (BCD) |
| `$0088`/`$0089` | game-frame counter (16-bit) |
| `$06A0` +stride `$40` | object table (≥64 slots; fields in `DEBUG.md` §11) |
| `$08A0` | player object (slot 8) |
| `$7D1B` | saved stack ptr (act→sim transition stack relocation) |
| `$0014`-`$0017` | **SHARED DP SCRATCH — not a fixed-meaning anchor.** Used as a 16-bit ADD/XOR
  checksum accumulator by `$02:84F3` (save-data validity check) AND as a message-type-ID parameter
  by `$02:BF60` (dialog-box draw dispatcher) — two unrelated subsystems reusing the same 4 bytes.
  Do not treat a read/write/oracle-divergence on this range as evidence about EITHER subsystem
  without checking which one is actually executing at that PC/frame first. |

> **Direct-page addressing gotcha:** all `$XX`-style addresses in this document (including this
> table) are conventionally DP (direct-page) offsets, which the CPU resolves as `D + $XX`, NOT a
> literal WRAM address — `D` (the direct-page register) happens to be `0` in every context checked
> so far (confirmed via runtime inspection, not assumed), making `D + $XX == $XX` in practice, but
> this has NOT been verified for every code path in the game. Before relying on a watch/trace
> pointed at a literal address, check the live `cpu->D` value for the code path in question.

---

## Object & spawn-handler model (moved from DEBUG.md §11, 2026-07-06)

The most common in-level crash/freeze class (§7.6) comes from this system, so it's worth knowing.

### Object table
- Base **`$06A0`**, stride **`$40`**, **80 slots exactly** (`$06A0-$1AA0`): the `$853D` allocator's
  bound is `CPY #$1AA0`. The Fillmore bridge segments live in slots 36–49; the Death Heim victory
  driver runs in slot 50 (`$1320`). `AR_OBJLOG` only scans 24 — **scan all 80 slots** when hunting
  a missing/late object.
- Per-slot fields (offset from slot base): `+$00` **status word** (high bits `0x4000`/`0x8000` set
  ⇒ inactive/free), `+$02` X (16-bit world), `+$04` Y, `+$12` **handler pointer** (`$12` — the main
  per-frame dispatch target), `+$14` **secondary handler** (field `$14`), `+$16/$18/$28/$30` spawn
  params, `+$24` **wait timer** (the loop `DEC`s it at `$8954` and only dispatches `$12` once it
  goes negative — the `$86FA` wait-N-frames mechanism), `+$30` flags (e.g. bit `0x0400`),
  `+$34/$36` spawn-X/Y source, `+$1E` nested-dispatch resume handler, `+$3A` spawner-slot
  backlink, `+$3E` **stashed continuation** (`$F778` pops its caller's return frame into it;
  `$F7C9` re-pushes it for `$F807`'s RTS — a third capture field besides `$12`/`$1E`).
  Player = slot 8 (`$08A0`). Game-frame = `$0088/$0089` (16-bit).

### Dispatch
- The **`$8915` object loop** iterates slots, dispatching each active object's `$12` via push-RTS
  (`$895C: LDA $12,X; DEC; PHA; RTS`); the handler's RTS lands at the `$8966` continuation; the
  loop exits at `$896E` (`PLP; RTS`, restoring M). `$896F` returns to the per-frame update routines
  `$8078`/`$80B4` — those `->008078/->0080b4 from 00896f` "dispatch misses" happen **every frame
  and are normal** (filter them out).
- Nested dispatch `$8664: LDA $1E,X; PHA; RTS` runs the field-`$1E` secondary handler.
- **`JSR $8657` / `$8668` / `$8669`** = coroutine yields: each stores its own return address as the
  object's `$1E` resume handler and dispatches it (`$8669` also takes a param in A → field `$38`) —
  so **the instruction right after each such `JSR` is itself a handler entry**, resumed next frame
  via the nested `$1E` dispatch (`$8664`/`$868F`). These form chains. `find_handler_chain.py
  --all-yields` (§5) closes the whole class. A miss on one of these (`->… from 00868f`) leaks m=0 →
  `B127` misdecode → `B90D` crash.
- **Primary-field `$12` yields** use the same idea with a different destination. Helpers `$8623`,
  `$86FA`, and `$A66A` read their caller's pushed return address and store the next instruction in
  object field `$12`; that post-JSR instruction is therefore a next-frame MAIN-handler entry.
  `$A66A` was found from Bloodpool act 2's moving-platform handlers (`$B990/$B9BC`).
- **Field-`$3E` deferred continuations**: `$F778` (the boss/summon spawn helper) pops its caller's
  return frame into `+$3E`; the shared `$F7C9` handler later re-pushes it and its `$F807` RTS
  dispatches there — so every `JSR $F778` site+3 is an RTS-dispatch entry too (the Death Heim
  boss-stub family `$F6DF/$F6F7/$F70F/$F727/$F73F/$F775/$F82D`).
- **Coverage guard (2026-07-14, DEBUG.md §7.20):** chain-walking from known handler seeds
  (`find_handler_chain.py --all-yields`) CANNOT reach a handler whose address only enters `$12`
  via spawn-record **data words** (`$FE89` was stored by `$F6D4/F6EC/…` records and hid two
  soft-lock continuations that way). `tools/find_yield_helpers.py` closes the class from the
  other end — it finds every helper-shaped JSR **target** by byte shape (pull/peek of the caller
  frame → object-field store) and verifies every call site's continuation is a registered cfg
  func, with no seed walk and no hand-maintained helper list. Run it after any bank00.cfg
  handler work; a MISSING hit is a future silent soft-lock.

### Per-level handler tables + spawn dispatcher
- Spawn dispatcher **`$9557`** reads game-mode `$18` (region index) → indexes the 8-pointer list at
  **`$95DD`** → that region's **handler table**:

  | `$18` index | `$00` | `$01` | `$02` | `$03` | `$04` | `$05` | `$06` | `$07` |
  |---|---|---|---|---|---|---|---|---|
  | table | `$96AF` | `$A8F6` | `$B449` | `$C11E` | `$CD9B` | `$D928` | `$E722` | `$F39A` |

  `$01-$06` are the six kingdom action regions, each with acts 1 and 2. `$07` is
  **Death Heim**, a distinct no-act final action flow: it teleports through the six
  kingdoms' act-2 boss encounters as a boss rush, then transitions to the final boss.
  Its `$19` sub-flow is now mapped (full rush user-verified end-to-end 2026-07-14): `$19=1` =
  hub (spawn record `$F3C8`/handler `$F3D4` stages the next boss from progress
  counter `$0347`), `$19=2..7` = boss arenas, `$19=8` = final boss — see
  docs/rom-map.md and DEBUG.md §7.20. `$00` is a
  separate dispatcher case, not Fillmore.

  `$19` chooses the act/map/sub-flow but does **not** participate in the `$95DD`
  table lookup. Therefore walking every type in one `$18` table is a conservative
  superset audit for both ordinary acts. It cannot enumerate handler roots installed
  later by object state, which is why each act still needs a runtime ring/snapshot pass.

- Each table is indexed by **object type** → a **record base `B`** (or, for a few special
  entries, a direct code pointer). The dispatcher (`$95ED`) then:
  copies spawn X/Y (`$34/$36`→`$02/$04`); reads record params (`rec[0]→$16`, `rec[2]→$18/$28`,
  `rec[4]→$30`, **`rec[0x0A]→$14`**); and sets the handler:
  - normal object: **`$12 = B + 0x0C`**, the exact primary-handler root. When that root starts
    with `JSR`/`JSL`, the instruction at **`B + 0x0F`** is also a valid post-init dispatch entry;
    many records instead begin their ordinary handler logic directly at `B+0x0C`.
  - special (`field $38 == $FF`): `$12 = B` directly.
- The tables have no stored count. Their pointed-to payload begins immediately after the pointer
  array, so the smallest forward target encountered while walking supplies the end bound. A zero
  pointer is an unused **type slot**, not a terminator. Bloodpool table `$B449` proves this layout:
  slots `$19-$1D` are zero, then slots `$1E-$27` contain ten valid records. A decompilation should
  preserve those sparse type indices rather than compacting the table.

### Why handlers go unconverted (the crash class)
- Exact **`B+0x0C`**, post-call **`B+0x0F`**, direct-`B`, and **field-`$14`** secondary handlers
  are installed through data/runtime dispatch rather than ordinary static control-flow edges.
  Any entry the static decoder does not happen to reach remains unconverted. Runtime dispatch to
  such an entry then misses → m-leak/misdecode → freeze or crash.
- The computed values (e.g. `$AC11 = $AC0E+3`) **never appear as literal bytes** in the ROM, so
  byte/pointer scans can't find them — only the table-derivation or a runtime snapshot can.

### Deriving them (the anti-whack-a-mole)
- `tools/find_handler_chain.py --tables` walks all 8 bounded, potentially sparse tables; seeds
  every exact descriptor root `B+0x0C`, JSR/JSL-gated `B+0x0F`, and direct-code `B`, then follows
  `$1E`- and `$12`-yield chains → `func` lines for every unconverted handler, all regions at once.
- **Field-`$14` secondary handlers** (`rec[0x0A]`, e.g. the bridge's `$ACEA`) are *polymorphic* — a
  handler for some object types, plain data (counter/coord/velocity) for others — so they can't be
  derived by value. `find_handler_chain.py --field14` handles them via the data signature instead
  (drop consecutive-address clusters + require handler-shaped coherent decode); it deliberately
  skips ambiguous (`COP`/`BNE`/`BRK`-led) values, which fall back to runtime discovery.

**Coverage boundary:** the three modes together — `--tables` (`B+0x0C/+0x0F`), `--all-yields`
(every detected `$1E`/`$12` yield continuation), and `--field14` — close the statically
recognizable forms. They do **not** prove that every runtime-installed root value is enumerable.
Bloodpool act 2 proved the residual class: `runs/20260712-193357/dump_dispatch_log.json` recorded
`$B990/$B9BC/$BAF1/$BB84/$BCC1/$BCCF`, from object-loop source `$8965`, as `found:0` every frame.
The first two are `$A66A` continuations and now appear in `--all-yields`; the other four require
runtime roots. Feeding all six to `find_handler_chain.py` expands the complete 12-entry web
(`B990 B9BC BAF1 BB08 BB16 BB84 BB93 BB99 BB9F BBA5 BCC1 BCCF`). Therefore the final closure
pass remains: inspect `dump_dispatch_log.json` for non-benign `found:0`, or scan ≥64 object slots
in an F2 snapshot. `find_handler_chain.py --snapshot <wram.bin> […]` automates the latter and
fixpoints all captures together. Snapshot decoding must distinguish `$12` (exact handler value;
the `$895C` dispatcher DECs before PHA/RTS) from `$1E` (stored JSR return address = target-1;
the `$8664` nested dispatcher PHA/RTSes without a DEC, so seed `$1E+1`).

**Bloodpool act 2 full-run extension (2026-07-12):** three F2 captures from
`runs/20260712-200334/` supplied later-room roots `$BD82` (gf1876), `$BD36/$BBB4` (gf6013),
and boss root `$BE0B` (gf9188). The exit ring independently records `$BE0B found:0` from `$8965`
on all 204 retained boss frames. Their combined fixpoint is 12 entries:
`BBB4 BBDB BBE9 BBFD BC66 BD36 BD45 BD56 BD6A BD82 BE0B BE7E`.
The later slow-window F9 in `runs/20260712-202151/` exposed a still-later live root `$BD90`;
its independently computed closure is `BD90 BD9F BDA5 BDAE`. This is why coverage remains a
runtime census even after several well-spaced snapshots: object state can install a root that
no earlier saved instant contained.

**Bloodpool completion + sparse-table correction (2026-07-12):** the next regenerated build
restored every later enemy and the boss; one enemy type remained present/killable but inert in
`runs/20260712-205842/`. F2 snapshot `snap_00_gf6378` holds two live objects whose exact `$12`
root is `$BB25` (record `$BB19`, table `$B449` type `$21`). `$BB25` performs offscreen gating,
movement/proximity checks, and animation before yielding, so skipping it precisely explains an
inert but still drawable/collidable enemy. The old table walker stopped at `$B449`'s first zero
slot and never reached this sparse tail. Correct bounded sparse walking finds `$BB25` and, by
seeding exact already-converted roots before following yields, 24 more safe gaps across all regions.
For Stage 3 (`$C11E`) the static batch is `$C48A/$C653/$C90E`; runtime captures are still needed
for roots installed later by state logic rather than directly by the table.

The first Stage-3 act-1 exit capture (`runs/20260712-211830/`, `$18=03`) immediately supplies that
runtime-only class. Slots 34-39 contain exact `$12` roots `$C7FA/$C7FF/$C804` (two objects per
root), and the retained dispatch ring records 94 `found:0` calls to each from `$8965`. `$C804`
executes `JSR $8657`, so `$C80A` is its next-frame nested continuation. These four entries are
queued with the static batch. Their object identity is deliberately left unnamed until correlated
with the playthrough report; the handler/state evidence itself is exact.

**Remaining-region static preflight (2026-07-12):** the corrected table, field-`$14`, literal
`STA $12`, and yield scans cover every statically recognizable root for regions `$04-$07`:

| region | table shape | table/yield entries awaiting generation |
|---|---|---|
| `$04` Aitos | `$CD9B`: 31 slots, no holes; 24 descriptors + 7 direct roots | `C653 CF11 CFBD D24B` |
| `$05` Marahna | `$D928`: 38 slots, one hole; 32 descriptors + 5 direct roots | `E44A E5DB E5DE E612` |
| `$06` Northwall | `$E722`: 30 slots, no holes; 22 descriptors + 8 direct roots | `ECA5 ECED EE5A EEC1` |
| `$07` Death Heim | `$F39A`: 23 slots, one hole; 18 descriptors + 4 direct roots | `E5DE F43C F44C F454 F5AF F609 F61C F74E F75D` |

Death Heim has 13 additional primary-handler continuations outside the direct table closure:
`F674 F684 F69C F6A7 F6B2 FA5F FAB3 FAD7 FB07 FB3A FB4C FB5E FB8A`. Each is immediately
after `JSR $86FA`, which stores its caller return into object field `$12`; they are exact
next-frame dispatch entries and are queued in `bank00.cfg`. The same final global pass found the
analogous Stage-1 `$A66A` continuation `$AD35`. After including them, `--tables`, `--all-yields`,
`--field14`, and direct literal-`$12` closures have no unconfigured results. This is static
closure, not runtime proof: per-act playthroughs can still expose computed/state-table roots.

### Action OBJ asset loading (decompilation boundary)

Action objects select compositions, but do not allocate enemy sheets dynamically.
The level-entry loader `$02:BC9E` establishes a common resident atlas:

- set VMADD `$2000`, copy 4,096 words from ROM `$07:8000-$07:9FFF` to
  VRAM `$2000-$2FFF`;
- copy 96 palette bytes from `$07:D040-$07:D09F` to CGRAM `$C0-$EF`;
- use magic selector `$02AC` with the bank-6 table/source rooted at `$06:A400`
  and upload 128 words to reserved OBJ target `$2D40`.

A later effect path at `$00:96C3-$96F5` is gated by object flag `$30 & $0040`
and descriptor-idle `$D5==0`. It derives a bank-6 source from object `$38`
(`$A000 + ((selector << 8) / 2)` in the ROM's word-address arithmetic), arms
descriptor slot 0 for VRAM `$2D80`, size `$0080`, and then advances paired
object handlers/states. This explains the observed slot-0 DMA: it replaces a
small reserved magic/effect area, not a whole enemy atlas.

For a decompilation, preserve four identities separately:

1. region/object handler identity (`$18` → `$95DD` table → object record);
2. sprite composition identity (object bank/pointer → seven-byte definitions);
3. resident action atlas and palette identity (`$07:8000`, `$07:D040`);
4. dynamic magic/effect selector (`$02AC`/object `$38` → `$2D40/$2D80`).

Collapsing these into an OAM-level “sprite id” loses the seam needed for asset
replacement and makes dynamic effect uploads look like object-sheet churn.

---

---

## Gameplay / Tunable seams  (cheats, rebalance, mods)

A second class of seam: the **value-clamp and mechanic-intercept points** where game logic reads/
writes a tunable parameter or makes a gameplay decision. Hooking these enables infinite health,
moonjump, sword reach, score forcing, etc. — the gameplay analog of the AV HAL above.

**Two hook kinds:**
- 🅥 **VALUE** — a RAM/SRAM byte/word; hook = freeze/clamp/force it (e.g. infinite health = pin
  the HP byte). Easiest; just needs the address.
- 🅒 **CODE** — a routine/constant that *computes* a mechanic (sword reach, jump velocity, damage);
  hook = intercept the routine or patch the constant. Needs the code site located.

**Discipline:** the addresses below marked **TBD** are NOT yet found — do NOT invent them. Each row
carries the **discovery method** so we capture it the moment debugging takes us through it. Only
promote a row to a real address once confirmed (a wrong cheat address is worse than a TODO).

| Seam | Where (RAM / routine) | Mod use | Kind | Status / how to find |
|---|---|---|---|---|
| Player HP (current) | `$1D` | infinite health (pin), god-mode | 🅥 | **WIRED** — `AR_INF_HP=1` (high-water auto-pin) or `=<n>`; per-frame in `ActRaiser_ApplyCheats` (actraiser_rtl.c). |
| Player max HP / bar size | TBD | bigger/smaller health bar | 🅥/🅒 | find the HP-init constant (new-game / stage-entry sets `$1D` to max) — `AR_WATCHOBJ`/`AR_WATCH16` on `$1D` at stage start; the writer's immediate is max. |
| Invincibility frames (i-frames) | i-frame timer `$08C6` (+$26); **invuln flag = `$08D0` bit `0x2000`** (the gate) | **no-knockback / invuln** (speedrun "ignore hits") | 🅥 | **WIRED** — `AR_NO_KNOCKBACK=1` pins timer `$08C6`=0xFF AND sets flag `$08D0\|=0x2000` each frame -> invuln from frame ONE. (Hit-check gates on the FLAG; the game sets it on a hit and clears it when the timer hits 0 — so pin timer + set flag = permanent, no first-hit needed. `=26` alone only worked after one hit.) On hit: handler -> `$9C64` (hurt), knockback into `$08A6/$08A8`. **Confirmed 2026-07-12:** this pinned authentic invulnerability state also suppresses water drag; disable it for terrain/movement-physics validation and timing-sensitive recordings. |
| Player lives | TBD (RAM or SRAM) | infinite / set N lives | 🅥 | watch the lives display value, `AR_WATCH16` on it; the death routine decrements it. |
| Player sword damage (dealt) | routine that subtracts enemy HP | one-hit kills, weak sword | 🅒 | `AR_WATCHOBJ` on an enemy slot's HP field while you hit it → the writer is the damage routine; the amount is its operand. |
| Player sword length / reach | TBD (hitbox/collision calc) | double reach | 🅒 | the sword-vs-enemy hit test — the attack hitbox extent (a constant offset from player X). Hardest (geometry); find via the attack-frame collision routine. |
| Player fly / moonjump | Y-**position** = `$08A4` (+$04) | moonjump / fly | 🅥 | **WIRED** — `AR_MOONJUMP=1` (default 6 px/frame) or `=<n>`; `AR_MOONJUMP_BTN=<mask>` (def `0x8000`=B). Moves Y-pos up while B held (`ActRaiser_ApplyCheats`). NOTE: uses Y-pos, NOT Y-vel `$08A8` — `$08A8` is "Y-velocity" only in the AIR state (polymorphic field); writing it while grounded did nothing. |
| Boss HP / health bar | boss object slot HP field (offset TBD) | set boss HP, instant-kill | 🅥/🅒 | boss HP lives in the boss object's slot (we have `saves/act1-boss*.bin` snapshots). `AR_WATCHOBJ` on the boss slot while damaging it → the HP field + the boss-damage writer. |
| Enemy HP (general) | object slot HP field (offset TBD) | — | 🅥 | same as boss — a per-object HP field in the `$06A0` table (offset not yet mapped). |
| Act score / population | TBD (likely SRAM, per-act) | force score thresholds → sim gating | 🅥/🅒 | the routine that compares act score/population to a threshold to gate sim-mode progression — `AR_WATCH16` on the displayed score; find the threshold-compare site. |
| Action-stage timer | `$E6`/`$E7` (BCD) | freeze timer / infinite time | 🅥 | **WIRED** — `AR_FREEZE_TIMER=1` pins `$E6/$E7` (per-frame in `ActRaiser_ApplyCheats`). |

> **Anchor:** most player mechanics hang off the **player object `$08A0`** (slot 8 of the `$06A0`
> table). Mapped so far (via `AR_WATCHOBJ=08A0`, 2026-06-25):
>
> | Offset | Addr | Field |
> |---|---|---|
> | +$02 | `$08A2` | X position |
> | +$04 | `$08A4` | Y position |
> | +$06 | `$08A6` | **X velocity** (signed; knockback fallback target) |
> | +$08 | `$08A8` | **Y velocity** (signed, neg=up; gravity +1/frame — moonjump target) |
> | +$12 | `$08B2` | handler ptr (state machine: `$9832` ground, `$98D9`/`$993F` jump, `$9884` walk, `$9A07`, `$9C64` **hurt**) |
> | +$1C | `$08BC` | **Crest** walking-cycle phase; increments while beginning movement (TAS terminology) |
> | +$24 | `$08C4` | **Boost** walking-speed countdown; cycles with Crest and can temporarily produce 3 px/frame movement |
> | +$26 | `$08C6` | **i-frame timer** (set 0x20 on hit, counts down) |
> | +$30 | `$08D0` | flags — bit `0x2000` = invuln (set during i-frames) |
>
> Still TBD and highest-value: the **HP field offset** (player/boss/enemy share the `$06A0`-table
> layout) — find once → unlocks boss-HP, enemy-HP, and damage seams together. Keep promoting these
> into `ram-map.md`.

---

## Function roles discovered (decomp groundwork)

Capture the *role* of a routine when you understand it — names are perishable. These are NOT yet
renamed in the cfg (see below); this is the candidate list.

The concise, confidence-rated candidate-name index now lives in
[research-symbol-map.md](research-symbol-map.md). Keep the detailed evidence and
investigation history here; update the research map whenever that evidence
establishes or changes a semantic identity.

| Address | Role |
|---|---|
| `$00:8000` `ResetHandler` | reset / boot (named) |
| `$00:8520` `NmiHandler` | per-frame NMI service (named) |
| `$00:8525` `IrqHandler` | IRQ (named) |
| `$00:8915` | object loop — dispatch each active object's `$12` handler |
| `$00:8526`/`852F` | COP / BRK syscall entry (audio events) |
| `$00:9557` | spawn dispatcher (reads `$18`, indexes per-act handler table at `$95DD`) |
| `$03:9156` | **act→sim transition handler dispatcher** (relocates stack to `$1FFF`, RTS-trick chain through `$9B22`/`$9B4A`/`$9195`) |
| `$03:8053` | **enter-sim SETUP** (runs on ANY entry to `$18=00`, incl. act→sim AND a warp to `$18=00`). Sequence of `JSR`s (`$9156`, `$AC8E`, …) → `$8193` → `$C147` → `$B20C`/`$B21F`. The 2026-06-26 act→sim hang in this cascade was a 1-byte SNES-stack leak per call in `$01:B898`'s jump-table RTS-trick, fixed via `indirect_dispatch B8C0 … ret:B8C2` (bank01.cfg) — see the `$01:B898` row below and bug-ledger §7.7. |
| `$01:B898` | **per-record per-type dispatcher, called once per active actor record every frame** (from the `$8193` master loop — see the sim-mode town architecture section above for the full chain into the spawn battery). `$B8C0` PHA-dispatches through the handler table at `$01:B8D0` keyed by object type, returns to `$B8C2`. History of THREE separate bugs found at this one site, in order: (1) a 1-byte SNES-stack leak that hung the act→sim transition (fixed 2026-06-26, see `$03:8053` above); (2) the table's `count` was left capped at 16 as a workaround for a since-fixed label-emission bug, but town actor types are 18/19 — above the cap, so their class handlers never dispatched (fixed 2026-07-02: bumped to the real bound, 26); (3) even at count=26, `idx:X` was wrong AT THIS SITE specifically — the ROM wraps the table read in `PHX($B8AE)/PLX($B8BB)`, so by the `PHA/RTS` dispatch X has been restored to the RECORD POINTER, not the type index (fixed 2026-07-02: switched to the value-keyed `idx:A` form, which reads the PHA'd table word instead of a register). This is the site responsible for the sim-mode actor-spawn corruption/freeze (`DEBUG.md` #18-25) — NOT the earlier stack-leak hang, a different bug at the same address. |
| `$03:AC8E` | transition state-machine step (counter loop, calls `$97B0`) |
| `$00:80E5` (label inside `ResetHandler`) | **sim-mode per-frame dispatch entry** — reached when `$18==0`; see "Sim-mode dispatch structure" above. |
| `$01:8000` | **sim-mode building/icon per-frame updater** — region-gated (`$19`), drives the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables; deep body at `$018170` does the actual per-building work via `JSL $1B1C7`. |
| `$00:8465` | writes a hardware-register-style immediate (`LDA #$A1`) — same pattern as the NMITIMEN setup at `$008051`; called from `ResetHandler`'s sim-dispatch tail (`$008122`). Its own native width (`M1X0`) runs correctly — confirmed NOT a misdecode (2026-07-01), just legitimate register churn that was mistaken for corruption early in that investigation. |
| `$00:8241` | called twice per main-loop iteration (`$008056` and `$00805C`, sandwiching the `$8418` vblank wait) — role not yet traced. |
| `$02:A622` | **title-screen continue/new-game state machine.** Calls the save checksum (`$02:A88D`) at `$02A70A`, branches on the result (`$02A70D: BCC $A72F`) to one of two dialog flows, and owns selection byte `$0336`. Phase-5 host evidence captured the interactive menu at gf821 with `$18/$19=00/00`, `$0300/$0302=0100/0110`, `$92=0C`, and `$0336<=2`. These remain useful renderer-state observations, but the settings overlay no longer depends on them: Escape/F1 is intercepted globally before emulated input dispatch — see "Save / persistence" below. |
| `$02:A88D` | **save-data validity checksum.** Computes a 16-bit ADD-sum and a 16-bit XOR-sum over SRAM `$700000-$701FEB` (calls `$02:84F3` to do the accumulation), compares against stored expected values at `$701FEC`/`$701FEE`. Returns pass/fail via carry (`CLC`=pass, `SEC`=fail). Confirmed correct 2026-07-01 (`AR_SAVECHECK`) — passes cleanly against a real mid-game save. |
| `$02:84F3` | **checksum accumulator loop** — `LDX #0; loop: LDA $700000,X; ADC $14; STA $14; EOR $16; STA $16; INX INX; CPX #$1FEC; BNE loop`. `$14`/`$16` are its scratch accumulator — see the DP-scratch-reuse gotcha in `DEBUG.md` §0. Confirmed byte-identical across its `M0X0`/`M1X0` width variants (not a misdecode candidate despite an early `mxhist` flag). |
| `$02:BF60` | **dialog/message-box draw dispatcher.** Takes a message-type ID via `A` (stored into the SAME `$14` DP scratch the checksum uses), branches on ID (`CMP #0/1/6/8/9/$B`) to different message-rendering sub-routines. Called by both `$02:A622` branches with different message sets. |
| `$01:ACD9` | **per-frame sim OAM rebuild driver** (called from `$01:9284`). Hide-fills `$0380-$057F`, initializes `$98/$9A/$9C`, scans 48 fixed `$12`-byte records at `$06A0` and 44 world `$26`-byte records at `$0A00`, then selects `ADAD` or `AE6F`. See the detailed sim OAM section above. |
| `$01:ADAD` | **normal per-record composition emitter.** Count comes from byte 0 of the frame definition at record `+08`; each five-byte part writes one OAM component. Saves the advanced shared cursor to `$98`; cursor hand-off is verified correct. |
| `$01:AE6F` | **alternate sim composition emitter.** Same geometry and OAM allocation as `ADAD`, but rewrites attributes as `(attr & $F1FF) | $0600 | $8F`; selected for world records when `$7F:9752 & 2`. |
| `$01:AC70` | called by `ACD9` immediately before `ADAD` per active object; body is just `PHB;PHY;REP;JSL bank_00_8519` (a `PHA;PLB` DB-set idiom) + fallthrough — does NOT touch `$0098`/`$0094`/`$0096`/`$0380` (checked). Its role beyond the DB-set is not yet traced. |
| `$00:8519` | trivial `PHP;SEP #$20;PHA;PLB;PLP;RTL` — the classic "`PLB` from A's low byte" idiom for setting the Data Bank register. Not a meaningful gate/hook; appears at several call sites across banks 0/1/3 wherever DB needs setting to a literal. |
| `$03:813F` | **position-staging copy** for sim-mode object records — see "Sim-mode object/sprite spawn" above. Copies `$130` bytes from a `$7F`-bank staging buffer (selected via `$7F:7BFB` → ROM table `$03:8111`) into the live object table at `$0B30,Y`. The odd-offset 16-bit writes this produces are a CORRECT overlapping-byte-copy idiom, verified against ROM disasm — not a bug. Called from `$01:AA56`. |
| `$01:E099` (ROM data, not code) | **actor behavior-script pointer table** (base corrected from the earlier `$E09B` guess), indexed by object TYPE (`type*2`): 16-bit pointers to behavior scripts around `$01:DDxx-DFxx`. Read by `$01:D072` (`LDA $01E099,X; STA $0000,Y`). Object-type identity data. |
| `$01:E7D9` (ROM data, not code) | **actor sprite-frame pointer table** (base corrected from `$E7E1`), parallel to `$E099`: per-type sprite/animation-frame pointers (frames list continues at `$01:E838`, e.g. `$E6CA/$E6D0/$E6D6...`). Read by the `$01:D0F5-D127` sprite-assign code. **The asset-identity seam for sim-mode actor sprites.** |
| `$03:8193` | **sim-mode per-frame master loop** — see "Sim-mode town architecture" above. Sets DB=`$7F`, runs `$8238`, the `$F5BE` handler dispatch, the 720-frame periodic counter (`$7F:91FE` vs `#$02D0` → `$8271`), then the bank-01 object loops. |
| `$03:F5BE` | **per-town handler dispatcher** (PHY/PHA/SEP/RTS trick; 6-town outer table `$03:F5ED`, packed inner lists `$F5F9-$F620`, 14 handlers `$F621+`). Was silently dead in the recomp until the `idx:A`/`sep:` `indirect_dispatch` fix (2026-07-02) — the town-corruption/freeze root cause. |
| `$03:F46E`/`$F479`/`$F484`/`$F497` | **lair-bitmask helpers**: test / set / clear a per-town lair bit; `$F497` computes the bit + cell (scratch `$7E:914F`) from the WRAM-pointer tables `$03:DCA2` (open-lair masks `$7E:9107+`) / `$DCAE` (spawned masks `$7E:911F+`). |
| `$01:AC36` | **process-script assigner**: `entry.+02/+06 = ROM[$01:A227[$033C*2] + $033D*2]`, `+04=0`, `+00=1`. The stride-12 tier's spawn primitive. |
| `$01:CFF2` | thin wrapper: `A -> $033C/$033D`, `JSR $AC36`. The sweep's per-entry call. |
| `$01:AA56` | **town-entry sweep driver**: staging restore (via `$03:813F`) + walk the stride-12 table calling `CFF2(entry.+0E)` per entry. |
| `$01:8029` / `$01:B1C7` / `$01:B52F` / `$01:B6AE` | town-init wrapper (`8029` → `B1C7` = `CFF2`+`AC70`); `B52F` = switch-all-processes-to-variant pass; `B6AE` = hide-all pass. `$03:CFB3` = hide-sweep on town EXIT. |
| `$01:D072` | **actor spawner** (stride-26 tier): `record.+00 = ROM[$01:E099+type*2]`, `+02 = ptr-4`, `+04 = 1`. Reached via the 56-routine per-type battery `$01:BA23-$C793` (each battery routine sets type-specific fields then `JSR $D072`). |
| `$01:D04E-D062` | **actor behavior RTS-trick dispatcher** (mechanics corrected 2026-07-02): table ptr passed in **Y** by each caller (`LDY #table; JSR/BRL $D04E`), NOT inline-after-JSR; selector = `record.+12 & $7FFF`; `A = sel*2 + Y`, `PHA` the table word (target-1), `RTS` at `$D062`. 24 call sites / 24 tables in bank01 (`B246 B423 B90B B934 B965 B9F8 BE58 C1C9 C243 C505 C7C5 C886 C8B0 C8D3 C8F7 C93C C977 C99D CA6D CA98 CAC3 CC3E CCE0 CD12`), incl. the spawn battery. All targets registered as funcs in bank01.cfg (the sim-mode corrupt-actors root-cause fix). |
| `$01:D063` | **mark-record helper**: sets `record.+12` bit15 (`ORA #$8000`) and returns Z per prior state — the standard first call of every battery handler (`JSR $D063; BNE already-init`), i.e. the "run once per spawn" latch. |
| `$03:8700-8711` | **per-town state dispatcher #2**: `LDX $7BFB; LDA $7CC9,X` (per-town state byte) → static table `$03:8713` (5 entries `871D 873C 87B5 872A 8E30`), PHA/RTS at `$8710/11`. Same PHA/RTS family as F5BE; targets registered in bank03.cfg. |
| `$03:E1D2-E1EB` | **per-town event-handler dispatcher #3** (F5BE-shaped): `LDX $7BFB` → per-town table base from `$03:E66E` (`E67A/E93C/EBC2/EE3E/F049/F2D7`, exactly 32 entries each), selector*2 added, pushes shared continuation `$E1EC` (`REP #$20; PLX; PLA; RTS`), PHA/RTS at `$E1EA/EB`. Targets + `E1EC` registered in bank03.cfg. |
| `$01:D08F` | **actor script stepper** (records' analog of `AC70`): counts down `+04`, reads the script at `+02`, `$FF` terminator handling. |
| `$01:AC70` | **process-script executor** (role found 2026-07-02, supersedes the "role not yet traced" note): per frame per active entry, DEC `+00` timer; on expiry read script at `+02`: `[delay, frameptr]` default op (→ `+08`, `+10|=1`), `$FD` hide, `$FE` loop (DEC `+04`), `$FF` set loop count+target. |
| `$01:8819` | **town event dispatcher**: `LDA $033E; ASL; ADC #$F223; TAX` — jump table at `$01:F223` keyed by the event code in `$033E`. |
| `$01:A227` (ROM data) | **spawn-list table**: `A227[list_id*2]` → per-list block of 5 sub-variant script pointers (sub = `$033D`, 0-3 = day-cycle phases, 4 = init special). |
| `$01:D128+` (ROM data) | **placement records** (stride 6: type, ?, x, y) — the "spawn WHAT at WHERE" data consumed by spawn scripts (e.g. script `$A3E5`'s operands `$D19A/$D1AF/...`). Level-layout seam. |

---

## Symbol renaming — mechanism & convention

**How to rename a function:** the cfg `func` directive's **first argument is the emitted symbol
name** (that's how `ResetHandler`/`NmiHandler`/`IrqHandler` got real names instead of
`bank_00_8000`). So in `recomp/<bank>.cfg`:
```
func ActSimTransition_Dispatch 9156 entry_mx:0,0
```
names the generated function `ActSimTransition_Dispatch` instead of `bank_03_9156_*`. Changing a
cfg requires **regen + rebuild**.

**RAM symbols:** the emitted code uses raw offsets (`cpu_read8(0x7E, 0x18)`), so RAM "renaming" is
documentation only for now — keep `ram-map.md` authoritative. (Symbolizing RAM accesses in the
emitted C would need an emitter change — a *future* nicety, not now.)

**Convention (proposed):**
- Functions: `Subsystem_Verb` PascalCase — `ActSimTransition_Dispatch`, `ObjectLoop`,
  `SpawnDispatcher`, `Audio_PlayMusic` (HLE wrappers already follow this: `ActRaiser_WaitForVblank`).
- Only rename what you've *confirmed* the role of. A wrong name is worse than `bank_03_9156`.

**When to do a rename pass:** rename *after* a fix is confirmed working, not bundled into the same
regen — so a rename can't be confused with a behavior change if something breaks. (E.g., hold the
`$9156`/chain renames until the `$9195` transition fix is verified.)

---

*Living doc — append seams as you cross them. Keep it honest: intent + logical ID are the point;
the hardware column is the easy part.*
