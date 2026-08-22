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
| Final PCM out | `RtlRenderAudio` (common_rtl.c) → continuous `dsp_getSamplesResampled` + MSU-1/OGG mix → SDL `AudioCallback` | host audio | "the mixed stereo stream" | native DSP stays 32.04 kHz; actual SDL rate controls time-based resampling, so frequency/buffer changes preserve pitch; `audio_master_volume` applies atomic post-mix gain and `audio_enabled` applies an atomic post-mix mute without stopping any cursor | 🟢 |
| Raw APU port write | `RtlApuWrite` (`$2140-$2143`) | APU I/O | low-level handshake / param | — | 🔴 |
| **Voice key-on observation** | `g_dsp_voice_kon_hook` (dsp.c, fires once per applied key-on) | DSP `KON` | "voice C started sample S" | `(ch, srcn, decodeOffset, volL, volR, pitch)`; NULL by default; installed by `sfx_census.c`. Called on whichever thread is cycling the APU, APU lock held. Must be invoked **after** `decodeOffset` is resolved from the directory — earlier and it reports the previous note's advancing decode cursor | 🟢 |

Host-side engine seams installed for this subsystem (all NULL/-1 by default, so
other games are byte-identical): `g_rtl_spc_upload_hook` (image src = song
identity), `g_rtl_apu_port_hook` (every `$2140-43` write — **chained**, not
owned: `music_replacements.c` installs first and `sfx_census.c` forwards to it,
so init order in `main.c` matters), `g_rtl_music_mix_hook` (OGG mix inside
`RtlRenderAudio`'s locked region), `g_dsp_voice_mute_srcn_min` (the music mute
gate), `g_dsp_voice_kon_hook` (key-on observation).

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

A song change that uploads a new image is always `$F0` → `$FF` + upload → song
number. **A play command is not always a song change**, and nothing guarantees a
preceding `$F0`: the boss chain plays an already-loaded bank via `$F1`-echo →
song number, with no `$F0` and no upload ahead of it (`$00:A3FE` → `$00:A410`
writes `$F1` then the song number, and only reaches `$F0` afterwards — gen
`bank00_part04_v2.c:9688`, `:9796`, `:9943`). Three more `$F0`-less `$F1`-echo
play sites: `$02:9914`, `$01:8A9A`, `$03:E48E`. Any host-side logic that infers
"a live song means the driver never halted" from the absence of `$F0` is
therefore wrong — this is what caused F5 (the previous track replaying its
opening on a boss transition), since the upload-derived identity still named the
outgoing song. Key on identity, not on `$F0`. Boot order: `02:9ACD`
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
   live there while SFX use the common bank.
   The apparent counterexample captured on 2026-07-21 (music keying srcn
   `00`-`06` intermittently across several songs) was resolved on 2026-07-25:
   it was a bootstrap/upload ordering race, not intentional shared-bank music.
   The first SPC image starts at `$0400` and clears ARAM `$11FF`; the common
   image later sets `$11FF=$0C`, which the sequencer adds to song-local
   instrument ids. Because the upload HLE returned before the bootstrap had
   necessarily finished, a fast game thread could write `$0C` first and then
   let the bootstrap clear it back to zero. That shifted every music SRCN down
   by 12 into the unmuted common bank, simultaneously explaining the corrupt
   instruments, the leak, its all-session persistence, and its launch-time
   intermittency. `RtlUploadSpcImageFromDpInternal` now recognizes the exact
   ActRaiser bootstrap and advances it for 3032 emulated APU cycles to its
   `$0460/$0462` idle loop before returning from the first upload. The common
   upload therefore always wins the `$11FF` ordering, and song voices remain
   at srcn `$0C+`.
   Per-entry `when =` gates (shared HD gate
   grammar, sampled at song start) select level/state-dependent variants;
   first matching entry wins, ungated entry = fallback. `music_replacements`
   setting / `AR_MUSIC_REPLACEMENTS` toggles live; `AR_MUSICLOG=1` traces.
   The transport contract is event parity, not sample-position parity: native
   `$F0`/`$F2`/play commands are mirrored, and a host pause gates the whole SDL
   device, while an output mute keeps the callback running silently so SPC,
   OGG, and MSU cursors advance together. Toggling replacement on mid-song
   starts the OGG at frame zero because authentic and arranged tracks have no
   general position mapping. Unexpected loop-seek failure releases the DSP
   music gate and falls back to authentic playback.
   Known gap: driver-side fades aren't captured yet (streams hard-stop on
   `$F0` exactly when the driver halts, so transitions stay correct).
2. **Instrument-level replacement (per-sample HD swap).** Stage 2 of the HLE installs each
   instrument as a discrete, identifiable unit: chunk index N from the `$08:8000` pool → known
   ARAM range → known `srcn` via the `$2C00` directory. Because our code performs the copy, it
   can substitute a different BRR chunk (or tag `srcn` → external hi-fi sample for a custom
   mixer) per instrument. The chunk index is a stable, ROM-wide instrument ID.
3. **SFX-level replacement.** Already the cleanest seam: `$035B` (BRK hook) carries the SFX id
   before any APU involvement. Map id → modern sample in the hook, suppress the port write.

   **SFX census (`AR_SFXCENSUS=1`, `src/dev/sfx_census.c`)** joins the two ends of
   that path — `SfxCensus_OnRequest` from the BRK hook (id, calling recomp
   function, game frame, and CPU `X`/`Y` as the requesting-actor handle) against
   the new `g_dsp_voice_kon_hook` DSP seam — and correlates them by APU-cycle
   proximity (~2 frames), since the protocol carries no tag from request to
   key-on. Report lands at `<run-dir>/sfx_census.txt`. First real capture
   (2026-07-22, ~13500 frames of sim + act): 10 ids, 981 requests. Findings that
   constrain both this tier and the volume split:
   - **id `00` is the idle/clear post, not a sound** (754 posts, 12 key-ons,
     almost all from `$03:9E6B`). The census no longer arms correlation on it.
   - **The driver already pans per effect**: id `18` spans `volL/volR` of
     `-71..47 / -38..71` — asymmetric and sign-inverted, not a constant. Any
     positional-audio layer must *compose* with this, not replace it.
   - **Sound identity is (srcn, pitch envelope), not srcn**: id `08` stretches
     one sample across `pitch 0385-3aae` (~16x).
   - **~84% of shared-bank key-ons carry no request** — and are not music (in a
     clean run the leaking srcn set equals the census-attributed set exactly).
     They are the driver re-triggering a repeating effect, so a purely
     request-driven classifier would mute most legitimate SFX re-keys. A
     classifier must treat "srcn established by a recent request" as a
     continuation.
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
   `settings-system.md`, "Audio control seams". The SFX census (tier 3 above) is
   the groundwork for that classification; its orphan-key-on accounting is the
   measurement that says whether a proposed classifier is safe.

**Verified common-bank sample directory** (DIR page `$2C00`, constant; each srcn
resolves 1:1 to one BRR start, confirmed across a full session plus a replay):

| srcn | ARAM | srcn | ARAM | srcn | ARAM |
|---|---|---|---|---|---|
| `00` | `$3000` † | `04` | `$4F2F` † | `08` | `$6906` |
| `01` | `$3B01` † | `05` | `$5814` | `09` | `$6DA1` |
| `02` | `$44EB` | `06` | `$5DB4` | `0A` | `$6DF2` |
| `03` | `$4545` | `07` | `$5DD8` † | `0B` | `$6E4C` |

† documented from the stage-2 install order but not directly observed keying in
the captured sessions. Note `srcn 00` start == loop == `$3000`, so a key-on with
no key-off sustains indefinitely.

Diagnostics for all of it: `AR_APULOG=1` (uploads incl. per-chunk stage-2 lines + port
traffic), `AR_AUDIODBG=1` (DSP health: mvol/mute/peak/cyc-rate), `AR_KONLOG=1` (per-voice
key-on state: srcn/pitch/volumes/ADSR + first BRR bytes — all-zero BRR = samples missing),
`AR_MUSICLEAK=1` (per-voice 1s report of anything reaching the dry mix while the music
mute gate is engaged: `kon_srcn` vs `live_srcn`, resolved `dir`/`brr`, peak, duty, and a
`slipped%` column that separates a gate dropout from genuine shared-bank use),
`AR_SFXCENSUS=1` (the id → sample/caller/pan map above).

For audio diagnostics in headless mode, use `AR_PACE=1`: the audio thread then
advances the SPC at the same real-time cadence as normal play, so key-ons and
mix/leak reports are meaningful. Unpaced headless remains useful for
startup-order stress (it reliably exposes a game-thread-outruns-SPC race), but
it can finish thousands of game frames before enough audio callbacks occur for
a listening trace. Also note `.rec` input recordings do **not** replay
faithfully without their matching boot `save.srm` — the recording run itself
auto-persists SRAM, so a later replay starts from different state and diverges.

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
| **Vertical-extension per-layer bounds (confirmed/fixed 2026-08-09; symmetric 2026-08-10)** | Host `ActRaiser_ApplyVerticalMarginPolicy` sizes shared top/bottom capture from the semantic primary layer, then `PpuSetVerticalMarginLayerClip` supplies BG1/BG2 independent available counts from cameras `$24/$28` and heights `$30/$34`; `PpuBgVisibleOnMarginLine` clips only synthetic rows beyond each layer's own edge | PPU BG tilemap scanout plus WRAM cameras/dimensions | "show real rows on either vertical side without joining a bounded tilemap's bottom to its top" | per layer: top=`cameraY`; bottom=`height-225-cameraY`, each clamped to the configured budget | 🟢 Fillmore act 2 proved the top seam: BG1 `$24=$05E8` legitimately extends, BG2 `$28=$0000` does not. Pre-fix negative BG2 lines wrapped to rows 481..511 and half-added red BG2-high structures over the grey wall; replay `runs/20260809-085004` clears it with byte-identical WRAM. Bloodpool `runs/20260810-112529` proved the missing bottom seam: BG1 camera and player both moved up 48px (`232→184`, `312→264`) while `bottom=0`, so a resident lower platform fell through the capture floor. The shared capture, layer clips, uploads and compositor now carry independent top/bottom counts. Regressions cover unbounded/zero/partial bounds on both sides, authentic-row identity and exact bottom OBJ. See rendering §13i and ledger §39/§42. |
| Per-frame display build / DMA | NMI path; `ActRaiserDrawPpuFrame`; DMA descriptors in ZP `$D0-$D5` | VRAM/OAM/CGRAM DMA, `$2100`-bus | "blit this frame's tiles/sprites/palette" | DMA descriptor tables | 🔴 |
| Sprite (OAM) build | object loop `$8915` → OAM | OAM | "place object's sprite" | `$06A0` object struct (X/Y/handler) | 🟡 |
| **Action OAM draw/activation split (fully mapped 2026-07-10; arrival gate corrected 2026-08-12)** | `$00:8C98` scans each action object once but computes two independent decisions. **DRAW** uses horizontally fitted `$22`, native vertical camera `$24`, live L/R/T/B presentation margins and the resolve-only OBJ apron before calling `$00:8D68`; **ACTIVATION** alone sets/clears object `+$30` bit `$0400`. Normal wide activation uses fitted `$22` plus live L/R margins. While player `$08B2` is `$97A6/$97C9/$97E4`, only the additional horizontal margin is gated: activation uses the native 256px camera derived by the `$02:B030` subject-centre/clamp rule (arrival bootstrap X from `$08A2`) and retains authentic vertical `$24`; `$97E4` installs input-reading `$9832`, which restores wide activation on that same scan. `$00:923A`→`$9258` HUD emission remains fixed-screen and independent | OAM plus action-object WRAM | "draw the complete wide presentation without waking margin actors before the player receives control" | draw policy `ws_margin_objects`; activation policy `ws_margin_activation`; player lifecycle `$08B2`; camera subject `$8A`; flag `+$30 & $0400` | 🟢 Drawing, camera presentation and BG extension remain wide throughout arrival. The failed first gate merely zeroed activation margins around fitted `$22`; at Fillmore's left edge `$22=120`, so it shifted the supposed native world interval from `[0,256)` to `[120,376)` and perturbed the intro/fade object set. The corrected path reconstructs the native camera instead of disabling widescreen. Replay `runs/20260812-122927` follows `$97A6→$97C9→$97E4→$9832` at gf 962/1068/1140/1185 with no second Fillmore music stop/restart; 45/45 tests pass. Bloodpool `runs/20260813-112738` proved why `$24` must remain native: the old Diorama fit moved it `287→255`, drew required log platforms at row 225 with `$0400` set, and leaked that camera into flat mode. **BOTH OAM fields remain lossy encodings**: Y is 8 bits mod 256 and X is 9 bits mod 512; `PpuSetObjExactPosition` carries the emitter's signed untruncated value, and vertical margin scanlines accept exact slots only. See rendering-engine.md §6/§9, ram-map.md, and bug-ledger.md §58/§60. |
| **Presentation-aware action camera bounds (horizontal-only correction 2026-08-13)** | `$02:B030` computes the tracked-subject request in `$7C/$7E`; `$02:B091` → `ActRaiser_UpdateActionCamera` applies it. Pure `ActionCameraAxisBounds_Resolve` computes `[before, world-viewport-after]` iff the complete requested horizontal view fits; `ActionBgPlan_CanvasOwner` and `ActRaiserActionBg_HleEnabled` are the shared policy gates. Vertical `$24` always uses the ROM's native `0..height-225` tracking range; Diorama resolves asymmetric capture rows around it without fitting gameplay state | WRAM subject/focus `$8A/$82`, requested/effective deltas `$7C/$7E`, and BG1 camera/dimensions `$22/$24/$2E/$30`; ROM BG2 parallax and player-relative tail remain canonical | "let finite horizontal playfield edges meet the outer corrected canvas while vertical presentation remains gameplay-neutral" | provider-backed finite BG1 playfields across action groups `$01-$07`; horizontal fitting gated off in 4:3/Wide Raw/non-action/`ws_action=Off`/`AR_ACTION_BG_HLE=0` and non-finite authored scenes | 🟢 Full `0202` resolves X `26..486` flat and `120..392` in Diorama-32; Y remains native `0..287` in both. At the Bloodpool floor Diorama captures `top=32,bottom=0`, keeping the required logs at native row 193 and active instead of moving them to inactive row 225. `0703` remains native X `0..0`, Y `0..31`. Run `20260810-172649` proved that a newly fitted horizontal edge must publish actual camera motion, not an unfulfilled `$7C/$7E` request. The HLE retains one `$22/$24` source of truth, original 16px strip flags, `$B9D5/$BA0B` parallax and `$00:A1B0` player tail. `AR_WS_ACTION_CAMDBG=1` logs the resolved interval. |
| **Action BG streaming (FULLY mapped 2026-07-12 — rendering-engine.md §3/§4/§12a)** | `$02:B030` stages camera requests; `$02:B091` applies/clamps them against level dims `$2E/$30` and sets 16px-crossing flags in `$93` → dispatcher `$02:B127` (TRB per bit) → `$02:B158` column strips (2 cols × 64 rows) / `$02:B1AF` row strips (2 rows × 64 cols, span `[cam&~$FF,+512)` — 256-aligned, page-keyed decode) per layer (X=0 BG1, X=4 BG2) → ONE record into the layer's fixed buffer (`$3900/$3A02/$3B04/$3C06`, capacity 1/frame) → NMI drain `$02:ACC8/$ACE5` → `$02:ADA8` 64B chunks. 64×64-tile ring per layer (BG1 `$6000`, BG2 `$7000`); entry draw = inline mega-burst (full ring, one frame). The old "tier-2 burst" = `$B1AF` row strips (walk bob). Northwall `0601`: `$00:E7BC` → `$02:945E` builds `$7E:6000`; `$02:96B6` points HDMA ch2 at `$210F` (`BG2HOFS`). | VRAM BG1/BG2 tilemaps | "keep the resident 512×512px tilemap ring fed as the camera moves" | level map decode via section config `$02:893E` + metatile tables | 🟢 original recompiled builders remain active and regions `$01-$07`, including the complete Death Heim flow, are directly validated. The former Stage B transaction refreshed world-margin ring cells while restoring CPU/WRAM/math state; BH8 removed it after exact provider acceptance. Historically, full margin columns used an 8px camera key and exact interval drains: the 496px maximum live view leaves 16px of ring slack, while 16px cadence can expose a new edge tile too early. Newly exposed vertical rows used `$B8A0` at `$24&$FFF0`, selectively draining neighboring-band columns when the live view straddles the row decoder's 512px page span. `ws_build_band_rows` extended the same transaction ABOVE the viewport for the diorama vertical band (`world_y = camY - k*16`), and had to run on the COLUMN rebuild path too — rebuilding columns is what re-stomps those rows with filler, and moving down refreshes only the leading edge 256px BELOW the camera. Narrow `$32<$0200` BG2 is never refreshed: the presentation layer uses an isolated authentic render with an audited edge strategy (cyclic repeat for Aitos/Northwall maps `$01-$05/$08`; Bloodpool's unique upper art is now extent-bounded while its water repeats), or clamps with `AR_WS_BG2_MIRROR=0`; none of these paths changes game VRAM/state. `ActionBgWorld` owns the bounded finite decode, `ActionBgPlan` owns all 49 source/edge classifications, and `AR_ACTION_BG_HLE_COMPARE=1` retains the native-ring oracle (including 6,646,861 exact in-world matches across eight Death Heim publications). BH7 promotes the generic tile-word provider by default; exact `AR_ACTION_BG_HLE=0` preserves an authentic-center/native-ring control and clamps planned world margins. Phase, decoded-world topology, and bounds must agree before a world layer binds. Marahna's 512px BG2 is a decoded horizontal cycle when a wider BG1 shares its full camera X; `wrap_world_x` applies the same modulo to lookup and preflight across subsections. Exact native words permit provider ownership across authentic and margin pixels; an in-world ring contradiction instead retains a native authentic centre while the same finite provider remains bound for margins. Full cameras plus signed live 10-bit scroll deltas preserve HDMA; live VRAM/CGRAM, windows, priority, transparency, mosaic and color math remain native. Narrow/native layers keep post-raster mirror/repeat/raw policy. Five paired 12-entry presentation matrices cover 4:3, Wide Full, Wide Raw, and diorama extension 0/32; every authentic center and state artifact is exact, with one intentional 30-pixel finite-bound correction in the synthetic `0301` Full margin. Long Fillmore Full/Raw/diorama runs, a natural Death Heim transition soak, lifecycle/geometry/savestate gates, a real compositor A/B, debug/release builds, and the then-current 41/41 tests passed; the post-Marahna suite is now 45/45. BH8 removes the duplicated host ring repair, builder trampolines, partial drains, 128 KiB snapshot, band repair, and the unused clamp-band/margin-source-gap PPU prototypes. The repair-removal matrices accept 612/612 artifacts under the explicit VRAM contract, and three final release matrices are 612/612 byte-exact against that baseline. Run `20260810-172649` confirms why: an 8px Y move exposed eight stale words on row 95 before the native 16px strip publication, with byte-identical BG1 ring, WRAM map, and metatile table across the compared snapshots. No transition-time ROM writer was found; regression coverage pins margin-only recovery for this cadence seam and any future legitimate live patch. The native ring/streamers, live mirror/repeat/repeat-band policy, vertical clip, and oracle remain. |
| **Immutable action-room scene authority (2026-08-22)** | `ActionRoomScene_Load` replays cumulative `$05:8000` graphics commands and command 3; `ActionRoomScene_BuildFrameState` resolves `$02:893E + 28*profile`, native camera/parallax, CHR/page phases, the inherited `$7E:6000-$7FFF` raster workspace and R1-R10 per-line state; `ActionRoomScene_RenderNativeFrame` composites the stable Mode-1 BG1/BG2 scene. Editor exporter schema v4 emits that state plus per-room C golden hashes. `AR_ACTION_ROOM_SCENE_COMPARE=1` shadows both each live `ActionBgWorld` publication and the pre-scanline PPU registers after HDMA advances. `AR_ACTION_ROOM_SCENE_HLE=1` feature-gates the first production handoff: the immutable page maps/metatiles source the finite provider while the original bootstrap and live ring remain active | immutable ROM bytes plus host-owned value records; comparison is read-only; the gated provider changes only its tile-word source and automatically falls back to live WRAM on scene/shape failure | "load any stable action room without a save state, keep editor captures identical to the game's HLE staging, and validate the port against the live PPU" | 49 rooms render through the stock-ROM census; 49 pinned profiles, 30 character-animated rooms, `0402/0403` four-page cycle, 17 raster-bearing rooms, five forced-BG2-priority rooms | 🟡 Stable BG1/BG2 authority, JavaScript golden-frame self-check, camera-local Native/Diorama editor surfaces, frame-register comparator, ROM-free tests and stock-ROM render census are implemented. Live R2/R3/R5/R6/R9 acceptance compares 6,348,367 resolved registers with zero mismatch and identifies a retained R2 table during one skipped action update. The ROM-derived provider-source handoff is implemented default-off and retains native ring preflight/fallback; its natural-transition matrix is the promotion gate. Native CHR/CGRAM/gameplay staging is not skipped yet. Natural-entry fixtures remain for boss/transition-only R1/R4/R7/R8/R10; BG3/OBJ, fades and gameplay-object-driven windows remain outside the stable-room contract. The generated editor lives under ignored `build/action-editor/`. See `action-room-loader-hle.md`. |
| **Action BG plan → immutable presentation handoff (BH6, 2026-08-09)** | pure `ActionBgPlan` → producer-side global override projection (only when explicitly selected) → pending scanout record → post-scanout live latch → `FrameSlot.action_bg_plan` + `bg_capture_pad_to_budget` → `DioramaBgValidSpanPlan_Build` → one skybox quad per distinct row span | host value records; no emulated state | "present the same per-layer/per-band edge policy that produced this captured frame" | BG1/BG2 source + default fill/motion + up to four screen/world-anchored bands per layer; live asymmetric margins; capture-budget padding fact; vertical-extension row origin | 🟢 the `DioramaBg2MarginSource` enum/scalar/mask reversal is removed. Normal action frames retain canonical map policy; 4:3/Wide Raw/debug overrides are projected at their producer; non-action scenes carry native-source executed policy. The compiled PPU table supports multiple non-overlapping Clamp/Mirror/Repeat/raw bands and decouples reflected fill from normal apparent horizontal motion; zeroed legacy policies remain fill-relative. Bloodpool Mirror→Repeat rows coalesce full-width, Death Heim Clamp→Repeat rows remain separate, and zeroed slots fail closed to live/authentic extent. A rejected producer-side projection publishes a native plan rather than retaining stale metadata. Present performs no live `g_ppu` read. |
| **Action BG semantic roles + per-layer extents (2026-08-10/12)** | `ActionBgPlan_Build` classifies playfield/scene/backdrop and available/fixed L/R/T/B caps → `ActionBgPlan_CanvasOwner` / `PrimaryLayer` select finite horizontal and vertical capture owners → `ActRaiserActionBg_ApplyPlanExtents` seeds PPU row caps → immutable `FrameSlot` plan drives Diorama UV spans and debug guides | host policy only; authentic pixels and emulated state are inviolable | "let the playable world grow without repeating a finite landmark backdrop" | all 49 action map IDs; Bloodpool `136..224`; Death Heim `144..224`; Kasandora world `256..512`; Aitos `0402` BG2 fixed T/B `24/24`; `0708` scene exception | 🟢 Ordinary BG1 remains wide while each tuned Bloodpool backdrop follows its safe span: `0201` uses 76/100 with independently available water, `0202` uses 68/68 with its repeat-safe water inheriting the same bound, unbanded `0206` uses 68/68, and unbanded `0207` uses 92/92. Boss room `0208` deliberately keeps its provider-backed BG1 world but changes its independent edge/motion to Mirror/fill with a 16/16 cap; viewport BG2 uses Mirror/fill at 0/0. Aitos `0402` promotes the captured tuner draft exactly: BG2 remains native/raw Fill with available 0/0 horizontal extent and receives only a fixed 24/24 vertical budget; source/edge guards and a neighbouring-`0403` regression prevent accidental reclassification. The provider now accepts any validated world-source edge, and the fitted camera limits each side to the playfield's effective fixed cap instead of the larger display budget. A band that owns authentic row 0 or 223 carries fill, motion and extent into the adjacent synthetic vertical margin; internal bands remain bounded. Kasandora `0301/0302` stores its dune seam once in world coordinates and resolves it through live BG2 parallax; its mirrored upper backdrop is fixed to 128/128 while the repeat-safe dune band remains available. Settings → Layers → BG Extents can add/delete up to four bands per BG and tune anchor, interval, fill, motion, and extent, plus independent non-destructive side/vertical-bound bypasses. Mixed anchors are proven disjoint across full camera travel and stale drafts retain the canonical plan; promoted room policies live in one source/edge/band-guarded data table. The scene inspector uses the PPU's shared fill/motion/source-X resolver. Multi-band, world-anchor, migration, capacity, Diorama-span, provider-edge, and real-PPU motion-phase regressions pin the seam. No new ROM symbol was required; this is a confirmed host presentation seam. |
| **Action Diorama main/subscreen + colour-math handoff (mapped 2026-08-11)** | live `$212C/$212D` TM/TS and `$2130/$2131` CGWSEL/CGADSUB → capture eligibility from `TM | TS` → main-preferred/sub-only overlay export → resolved full-add TS winner filtering → immutable `FrameSlot.diorama_plane_additive_mask` → main/additive/BG3 Diorama passes | native PPU priority/window/math resolve plus extraction-only host metadata; no CPU/WRAM/VRAM/CGRAM/OAM mutation | "preserve a scene whose playable art exists only as a colour-math subscreen input" | Marahna measured state `TM=$06` (BG2+BG3), `TS=$11` (BG1+OBJ), `CGWSEL=$02`, `CGADSUB=$03`; relocated HUD OAM omitted only from the full-add scratch | 🟢 `0501` gf2331 and `0502` gf9728 carry the same disjoint full-add topology. The PPU exports sparse subscreen planes only where BG1/OBJ wins TS and the resolved main winner enables math; the compositor draws them additively after ordinary world planes and before BG3. Overlap, direct colour, unsupported window math, and non-reproducible subtract/half forms fail closed. See rendering-engine.md §13.4 and ledger #48. |
| UI/dialog tilemap compose+upload (sim engine) | `$02:BF60` dispatches message text into BG3 buffer `$7F:B000` → `$02:AEEB`, while the visible box frame and offscreen work boxes remain BG2; whole-map BG refreshes use `$02:B727`/`$B825` record mega-bursts and `$02:ADA8` as a 64B DMA helper. Sky Palace setup at `$02:B6F8-$B726` conditionally copies ROM `$07:D0A0` to `$7E:C200`. | BG3 text plus WRAM/VRAM BG2 map state | "draw the active UI while retaining offscreen work pages" | message-type IDs via `$14` (see Save/persistence note on `$14` reuse) | 🟡 major paths separated. Live `$B825` decode reproduced staging (`runs/20260712-232230`); narrow raw-edge reflection produced broken columns; center reflection copied the BG2 box (11:36 PM capture). Current render transaction reads `$07:D0A0`, reconstructs the box-covered rows per column class (row-major quadrants; shaft continuation; seam base halves at meta cols 0/15; floor top 2 rows under the box bottom; `$41/$49` flare + `$40/$48`/`$42/$4A` skirts at shaft columns — base art exists only in the metatile table), expands via `$7E:2900`, patches only margin BG2 columns, then restores VRAM. ✅ Validated 2026-07-13: byte-identical to the game's boot colonnade (scratch cols 56-63 rows 18-31); user-confirmed clean in dialogue + submenu states. |
| **Native dialog presentation assets (verified 2026-07-16)** | compressed BG3 font at ROM `$17:ECFB` (file `$0BECFB`) → `$02:C5C9` decode → `$1000` bytes; Sky Palace BG char bank at ROM `$0D:C000` (file `$06C000`, `$4000` bytes); palette 7 at ROM `$1C:BF73` (file `$0E3F73`) | BG3 font / BG1+BG2 chars / CGRAM | "render ActRaiser's text and beveled dialog frame" | font tile index = character code; frame chars `$CE/$CF` corners, `$DE/$DF` sides, `$EE` horizontal edge, `$FF` black fill | 🟢 F2 captures `runs/20260716-072558/snapshots/snap_00_gf460` and `snap_01_gf668` supplied byte-exact VRAM/CGRAM identity. The reusable frame is an 8×8 nine-slice: vertical flips produce the top edge/lower corners. Do not reuse 16×16 metatiles `$4E/$4F`; each includes palette-1 Sky Palace scenery tile `$18` beside the real corner. Host settings decode only immutable ROM assets and never sample live scene VRAM. |
| BG mode / layers + colour math | `$2105` BGMODE; `$212C/$212D` TM/TS; `$212E/$212F` TMW/TSW; `$2130/$2131` CGWSEL/CGADSUB; scroll regs | PPU | "select each main/subscreen source, resolve two priority winners, then optionally add/subtract them" | source bits BG1/BG2/BG3/BG4/OBJ = `$01/$02/$04/$08/$10` | 🟢 for the action-Diorama forms catalogued in rendering-engine.md §13.4; other game-wide register writers remain to map |
| HDMA (raster fx, HBlank) | HDMAEN `$420C`; ActRaiser drives ch 2/3 (and others) | HDMA | "per-scanline effect" | HDMA tables | 🔴 |
| **Mode 7 world navigation (mapped/host-owned 2026-07-27)** | `$02:8213` moves focus `$0300/$0302` and stages zoom `$0318`; `$02:83CD` derives matrices from rotation `$0314`/zoom `$0316`; `$02:8384` uploads `$0304-$030A` + focus; `$01:B6CA` selects location `$0341` from `$01:B73C`; `$02:B475` builds/uploads the map; `$02:AF86` animates water | PPU Mode 7 plus host full-plane scene | "move, zoom, and spin over the developed world; enter an act" | focus, signed 8.8 matrix, current/target zoom, Palace/UI OAM, active-location ID, host-owned map serial | 🟡 Host forced-top-down rendering, the action-entry spin/zoom snapshot, OAM composition, water, lighting/weather/haze/backdrop, and full fade ownership are implemented and trace/fixture tested. Remaining acceptance is a complete manual movement/destination and action-entry sweep; other Mode-7 screens are not covered by this row. |
| Brightness / forced blank | `$2100` INIDISP | PPU | "fade in/out, blank during build" | 4-bit master brightness + forced-blank bit | 🟡 World navigation now remains enhanced for every non-forced-blank brightness step: host layers receive one exact black overlay `(15-brightness)*17`, then already-brightness-adjusted Palace/UI captures are drawn. Forced blank still fails closed to authentic black. Other presentation modes remain to audit. |
| Palette load | CGRAM writes | CGRAM (15-bit ×256) | "set palette N" | **palette id / table TBD** | 🔴→🟡 |
| **Action OBJ atlas + effect overlays** | `$02:BC9E`: ROM `$07:8000-$07:9FFF` → VRAM `$2000-$2FFF`, palette `$07:D040-$D09F` → CGRAM `$C0-$EF`, then 256 bytes from `$06:A400 + (selected_magic-1)*$80` → VRAM `$2D40`. `$00:96C3-$96F5`: only for an object with `+$30 & $0040` and an idle descriptor, `(object.+38 & $FF)` selects 128 bytes at `$06:A000+n*$80` → VRAM `$2D80`. | VRAM/CGRAM | "load the shared action atlas and replace reserved effect tiles" | common atlas plus selected-magic window; dynamic `+38` is polymorphic and **not** a universal spell ID (the spell handlers use it as repeat counts) | 🟡 selected-magic mapping and all four spell composition/animation banks are catalogued; unrelated dynamic-overlay values remain to classify |
| **Action presentation gameplay clock (host, 2026-08-11)** | successful `$00:8C98` common epilogue → `ActionEffectGameplayClock_CompletePass`/`Serial` → game-thread `FrameSlot_Capture`/`ActionEffectTickClock_Capture` → one bounded elapsed-tick value shared by spell and scene-effect capture | host-only monotonic serial plus resettable consumer; no emulated RAM/ROM field and no savestate payload | "advance presentation-only action effects exactly when a complete gameplay/OAM pass was produced" | `$00:8C98` is skipped by native pause/freeze even while vblanks and `$0088` advance; publication occurs only after the nested HUD and admitted object builders return normally; both ordinary emulated-frame and action-gameplay deltas share `frame_timing.h`'s eight-tick clamp | 🟢 Publisher/read/delta chain is isolated in `action_effect_clock.c`; production and regression use the same seam. Tests pin unchanged/native-pause, one-pass, multi-pass, clamp, reset, and null behavior. Abnormal nested-HLE returns precede the sole publisher and therefore cannot advance the serial. |
| **Action spell host presentation** | successful `$00:8C98` common epilogue → `ActionEffectGameplayClock_CompletePass` → game-thread `FrameSlot_Capture`/`ActionEffectTickClock_Capture` → `ActionEffects_CaptureFrame` after authentic scanout → immutable `FrameSlot.action_effects` → pure bounded `ActionEffectRender_Build` → `DrawActionEffects` world overlay before HUD/HD overlays | capability-checked SDL additive untextured geometry | "decorate positively identified action spell actors without changing their gameplay or source art" | shared action-object ABI; explicit resettable observer with actor/phase/pulse clocks advanced by the completed `$00:8C98` gameplay-pass serial (unchanged during native pause/freeze or an abnormal nested-HLE return); shared `frame_timing.h` eight-tick observer clamp; generic geometry + OBJ priority metadata; Magical Fire controller `$0860+$38=1`, exact `$06A0-$0760` cohort, `$07:C000` animation identity, current `+20/+22/+28` and unsigned extents; diorama consumes the exact compositor-published priority-plane depth/rake/bow | 🟡 All four spells implemented against a data-driven rule table (controller kind → slots/roles/phases) with ROM-free capture, deterministic batch, capacity, fail-closed, and flat+diorama projection tests. The production clock seam is regression-tested for unchanged/native-pause, one-pass, multi-pass, clamp, and reset behavior. Magical Fire is MEASURED (pinned to a real WRAM snapshot) and confirmed rendering live; Stardust/Aura/Light rules are TRANSCRIBED from the ROM analysis and unproven against live WRAM — an unrecognised active cohort slot is censused to `[action-fx census]` rather than rendered on a guess, so one cast of each corrects the table. |
| **Action scene accent observation/presentation (2026-08-10/12)** | game-thread `ActionSceneEffects_CaptureFrame` → immutable `FrameSlot.action_scene_effects` with independent actor/decorative lists → pure bounded `ActionSceneEffectRender_Build` / `ActionSceneDecorationRender_Build` → pre-HUD world-overlay plus BG2-local veil and after-BG2 Diorama atmosphere submissions | read-only WRAM/BG-map observation plus capability-checked SDL additive/source-alpha/multiply untextured geometry; no new shader artifact or emulated write | "attach portable light/particles to exact authored torches, lava/water structures, enemy/player projectiles, trap lightning, and boss lightning without losing hardware occlusion" | Bloodpool BG1 torch pair `$47/$4F`; Marahna `$05/$04-$08` single-metatile `$43` torches with camera-local publication; Aitos `$04/$01` `$DC/$DD+/$DE` lava rims over `$DF/$E7`, `$CEEC/$CF16` launched molten rocks, and `$04/$02-$03` three-row `$36/$5E*/$81`, `$4E/$F4*/$4F`, `$F6/$FC*/$FE` waterfall-platform structures; map-backed styles use an observer clock seeded from `$0088` and subsequently advanced only by the `$00:8C98` gameplay-pass serial; Bloodpool, complete `$E047` Marahna orb/split, parent-validated `$DE96` Marahna snake shot, and `$CF9E/$CFCD` Aitos fireball lifecycles; Bloodpool trap pairs; Marahna `$4AA1/$4B82` connector children validated against their `$E18E/$E254` endpoint pair, backlink, orientation, and midpoint; Bloodpool map-$08 boss source `$BDFF` states `$02-$07` plus state-`$09`; Marahna boss source `$E483` exact charge/orb/diagonal-bolt/ground-charge phases and validated `$12E0` backlink, including `$E578` descent and `$E57E` three-frame floor lifecycle; global player sword beam handler `$9D1C`, animation `$06:8000`, attacker/player link, no V-flip, pairs `$13/$30/$99E8` or `$14/$31/$9A17`, and four explicit signed-origin OAM rectangles; Aitos boss source `$D646` exact `$D793` controller plus priority-2 state-1/2 diagonal crescent children in normal or matching controller/child H+V-reflected facings; shared bounded `ActionBgMapView` | 🟡 Hook and renderer are implemented for flat and Diorama modes. Torches, lava, and platform water project on compositor-published BG1-low; ordinary actors project on OBJ priority 0, while the Aitos boss crescents preserve OBJ priority 2. The waterfall veil is BG2-local: Diorama inserts it immediately after drawable BG2-low, while flat presentation clips it through the live BG2-winner mask. One-time success logs distinguish both routes. A paired after-BG2 Diorama atmosphere keeps BG2 camera/rake/bow projection but is deliberately unmasked, placing two tiers of three mist banks and thirty-two foam motes at the named 24px BG2 extension boundary to cover the finite-backdrop gap. Its verified source-alpha blend and staggered bank depths feather rather than brighten the BG2/skybox seam; visible bottoms differ by over 80px and the deepest exceeds 100px below the seam. BG projection publication uses the exact drawing eligibility predicate. A current visible world-overlay actor may additionally retain its exact visible/texture-backed OBJ priority transform through a four-bit mask derived from the immutable spell/scene frames, even when that isolated source band has no final winning pixels; immutable request/content/upload masks reject hidden planes and real upload failures, and the exception cannot publish BG1/BG2. The measured 14-splash camera window plus one veil and one mist exactly fills the separate 16-decoration budget and leaves all 16 actor records available; a forged fifteenth splash fails the decoration list closed; either list fails closed independently and one scratch batch is reused. Exact camera-local platform structure keeps the shared `0402` cave clean. Native pause/freeze leaves the gameplay serial unchanged. Slot continuity combines validated lifecycle keys with bounded motion. Run `20260812-000613` maps the separate molten actor family, full `$DF/$E7` bubbly lava volume, waterfall-platform structure, and the dragon's normal two-child sword volley; run `20260812-224123` completes the H+V-reflected family, giving four validated diagonal branches. The glow retains that full depth, while `20260812-105106/snap_00_gf3728` anchors spark births one quarter of the captured height above its geometric midpoint to match the isometric surface. Runs `20260812-151323` and `20260812-153127` prove both waterfall passes submitted but the first two tunings were too subtle and then too thin; the veil now uses 96 thicker streaks. Bloodpool boss filaments follow up to 24 exact OAM-row segments; Marahna reserves five links and one diagonal boss bolt; sword presentation reserves the measured three-stream peak and uses full-height haze plus forty-eight fixed crossed stars per stream. Impossible expanded streams fail closed. Production regressions cover independent BG1/BG2/OBJ projection, actor-requested empty-band OBJ projection, upload-failure rejection, drawable-plane eligibility, BG2 winner masking, non-coincident visible atmosphere depths, and decorative/actor capacity isolation. Fresh visual acceptance remains before promotion to 🟢. |
| **Scoped Diorama skybox source / ROM BG catalogue (2026-08-12)** | exact camera-local Aitos waterfall decoration in game-thread capture → immutable `FrameSlot.diorama_layer_section` → base `[layers:04:02]`/`[layers:04:03]` resolution followed by `[...:waterfall]` refinement → resolved Backdrop-record `source:rom-04-01-bg2` before the far-background pass → read-only ROM decode → native `(definition & $ECFF) \| (attribute << 8)` transform → SDL skybox quad | host-only scope/source metadata plus immutable ROM bytes; no WRAM/VRAM/CGRAM mutation, room-visit cache, or backend-specific shader | "let any action room select any stock action BG skybox, with camera-local refinement where one map contains different scenes" | section token `waterfall`; source tokens `captured` and `rom-GG-MM-bgN`; 49 valid action maps × BG1/BG2 = 98 ROM sources; stock asset-script mappings in `rom-map.md`; action mask `$ECFF` from `$02:B6D3-$B6F6`; attribute byte BG1 `$10`, BG2 `$01` from `$02:B4E8-$B54C`; Backdrop alpha/visibility applies only to residual in-box geometry and never gates the skybox source | 🟡 The shared `$04/$02` cave remains captured; exact waterfall publication can select another room's BG for the skybox only in `$04/$02-$03`. The editor labels and displays the inherited effective skybox source and writes the exact published scope. The lazy cache keys the complete source, so a selection cannot retain stale art. Decode/texture failure falls back to captured BG2; renderer resets rebuild lazily. Run `20260812-220252` proved the former residual-plane routing left the visible skybox stale despite successful decodes. Run `20260812-222309` proved an exact asset decode can still render garbled when native tile-word setup is omitted; the corrected Aitos BG2 tilemap and all 65,536 output pixels match the live publication. Source resolution, alpha-independent skybox selection, ROM widescreen UV repeats, and both layer attribute transforms now have production regressions. Synthetic compression/inheritance, scoped manifest/editor/overlay, shared map-domain, and all-98-source stock-ROM census regressions pass; fresh live visual acceptance remains. |
| **Fixed-screen HUD OBJ icon promotion (host, mapped 2026-08-05)** | `ActRaiser_WidescreenHudObjPromote` (actraiser_rtl.c) validates the icon in OAM and claims a `RemoveFromGame` OBJ capture over it; `ActRaiser_HudObjIconRange` publishes the validated slots; `FrameSlot.hud_icon_first/count` carries them to `present.c`, which anchors the icon beside the right HUD group | OAM capture → `g_hud_obj_pixels` host overlay surface | "pin the fixed-screen magic/hourglass icon to the widescreen HUD instead of leaving it at its authentic centre-screen X" | **action magic icon = OAM slots 0-3, tiles `$D4-$D7`, x `$94/$9C`, y `$0B/$13`, attr `$3C` → OBJ priority 3, palette 6 (CGRAM `$E0-$EF`, inside the `$07:D040` atlas palette upload), no flip.** Sim hourglass scans for its four-small-sprite animated signature because it begins at slot 0 normally and slot 11 under the captured menu; Sky Palace likewise scans because its 16×16 icon can begin at slot 0 or 6 and is encoded as four small sprites for Fire or one large sprite for the other spells | 🟡 original promotion + anchoring validated flat and diorama; exact menu-shift regression coverage is in place, with post-fix visual confirmation pending. See the host constraints below — the OAM range is a property of the capture, not a standing fact, and reading it back from `overlayCaptures[Obj]` is how the icon gets lost |
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

### Main/subscreen colour-math seam (host, mapped 2026-08-11)

`TM` and `TS` are source-membership masks, not “normal view” and “alternate
view.” The PPU independently resolves one winner from each mask on every pixel,
then `$2130/$2131` decide whether and how the subscreen winner participates in
the output. Consequently, a layer that is absent from TM can still be essential
to the visible game. Every layer census, HLE eligibility check, and separated
capture must use `TM | TS` unless it is explicitly asking about one operand.

The Marahna snapshots at `0501` gf2331 and `0502` gf9728 establish the concrete
game contract: TM `$06` = BG2+BG3, TS `$11` = BG1+OBJ, CGWSEL `$02` selects TS
as the second operand, and CGADSUB `$03` requests a full add on BG1/BG2 main
winners. Thus BG1 and OBJ are not an optional hidden view; they are the playable
level and actors supplied through the second colour operand. BG3 remains a
non-math main winner and must occlude the reconstructed world/HUD exactly as it
does in native scanout.

The host seam preserves that division instead of flattening it prematurely:

- capture setup admits the union of TM and TS;
- scanout prefers a source's main rendering but falls back to its subscreen-only
  rendering after per-line HDMA register changes;
- eligible disjoint full-add scenes are filtered to the actual resolved TS
  winner and to pixels whose resolved main winner enables math;
- the game-thread capture publishes only an additive plane bitmask through
  `FrameSlot`; present does not read live PPU state;
- the Diorama compositor draws main world, resolved additive TS, then BG3;
- a promoted HUD OBJ range remains in the ordinary icon capture but is excluded
  from the addend scratch so moving it cannot leave an icon-shaped world hole.

The full-add classifier deliberately requires CGWSEL exactly `$02`, no half or
subtract bit, disjoint TM/TS visual-source masks, and a math-enabled main source.
These conditions are the proof that one sparse additive plane can represent the
native operation. Unsupported overlap/window/direct-colour/subtract states are
not guessed. Half-add and fixed-colour subtraction have their own independently
measured representations; see rendering-engine.md §13.4.

The additive bit describes one resolved source contribution, not permission to
add every piece of host-generated geometry. Diorama `stack`, `voxel`, and
`thick` each submit the source texture more than once. The current compositor
therefore applies ADD once per copy when one of those shapes is authored on an
additive plane. Run `20260811-145909`, gf2097 demonstrates the failure with
Marahna BG1 and twelve voxel slices: eleven solid copies plus the original face
accumulate at overlapping pixels and saturate the scene white/yellow. Dividing
the source colour by the slice count is not valid because perspective and the
silhouette make the overlap count vary across the image. Until the shape can be
flattened internally and added once, multi-draw depth strategies are unsupported
on additive planes; `z`, `rake`, and `bow` remain single-submit shapes.

This finding added no ROM/WRAM/symbol-map entry. TM/TS/CGWSEL/CGADSUB are live
PPU hardware state, while the new masks and capture flags are host-only frame
metadata.

### OBJ overlay-capture constraints (host, learned 2026-08-05 the hard way)

Five properties of the PPU overlay-capture API that are invisible at the call
site and each cost a debugging session:

1. **One capture per source, and the OAM range belongs to the capture.**
   `PpuSetOverlayCapture` resets `oamFirst`/`oamCount` to 0 as it lands. So any
   later policy that claims the same source silently drops the earlier policy's
   OAM range. This is exactly how the selected-magic icon got lost: the promote
   claims OBJ over slots 0-3, then diorama mode re-claims OBJ as a full-frame
   scene layer over slots 0-127. Both claims are correct in isolation; the
   second simply wins.
2. **Therefore `overlayCaptures[Obj]` cannot answer "which sprites are the HUD
   icon."** It answers "what claimed the OBJ capture last." Use
   `ActRaiser_HudObjIconRange` (actraiser_rtl.h), which the promote latches
   independently of the capture. `present.c` and `dev_tools.c` both read it, so
   the anchored draw and the inspector hit-test cannot disagree.
3. **OBJ band index == OAM priority.** ppu.c's priority-split resolve does
   `band = z >> 14`, and `SPRITE_PRIO_TO_PRIO` puts the OAM priority in those
   two bits — so band N is the plane the diorama's `kPrioBands` table bound for
   band N (`kDioramaPlane_ObjN`; band 0 is the primary `kPpuOverlaySource_Obj`).
   A pixel lands in exactly one band.
4. **Capture VRAM/CGRAM-dependent OBJ pixels at their authentic raster time.**
   The OAM range and footprint can be resolved before scanout, but one complete
   `PpuRasterizeObjRange` at either end of the frame is not universally valid:
   mid-picture IRQ handlers and HDMA can change the tile or palette state before
   the relevant rows. Calling the same helper immediately after `ppu_runLine` is
   still a second decode: it re-resolves the range and reconstructs every part
   from a register snapshot instead of preserving the pixels the evaluator just
   fetched. The first Sky Palace game-over fix made exactly that mistake and the
   single-large Stardust form still produced an empty HUD copy. Semantic sprites
   that must survive a later capture-policy overwrite use
   `PpuSetObjRangeCapture`, which filters the selected slots inside the real OBJ
   evaluator and writes their ARGB pixels at fetch time. A caller whose source
   state is proven stable may still use `PpuRasterizeObjRange`; that is a
   source-specific fact, not an overlay-capture API rule.

5. **Promoting a sprite leaves a HOLE in whatever it was occluding.** Every
   captured OBJ pixel competes in ONE shared z-buffer
   (`overlayBuffers[kPpuOverlaySource_Obj]`, first opaque writer wins, OAM
   walked in slot order); the per-priority band split happens later, at
   scanout, on the pixel that already won. So a promoted sprite in the LEADING
   slots wins every overlap, and the sprite behind it is never captured into
   any plane at all. Correct on hardware — the promoted sprite is in front.
   Wrong the instant the host MOVES it: zeroing it out of its band then leaves
   a hole shaped like it, cut out of the scene. Diagnosed 2026-08-05 as a bite
   taken out of a level gargoyle that the magic icon overlapped; it needs only
   an on-screen overlap, so it is independent of the vertical band.

When a capture-policy conflict means the icon has to be separated from captured
*pixels* rather than by capture *policy*, the pattern is: resolve the icon's OAM
range and footprint before scanout, register a semantic range capture on the
flat overlay surface, let the real OBJ evaluator fill it, then clear those exact
opaque pixels from the scene plane that holds the icon's priority band and
**restore what it was covering** — replay the same first-writer-wins rule over
its footprint with the promoted slots removed, keeping each restored pixel's
colour and the priority band it belongs to, then write those back.
`ActRaiser_DioramaHudObjPrepare`/`…Finish` implement this split. The best-effort
restore is still sampled before scanout and does not replay the hardware
per-line sprite limits, so it fails toward showing a sprite the PPU would have
dropped rather than toward a hole.

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
the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables marked `Call indirect SUPPRESSED` in generated C
(`rg -n 'Call indirect SUPPRESSED' src/gen`; see `DEBUG.md` §7.9) — `$B420` is a genuine static
ROM table (5 real entries, confirmed by reading the
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
           (2026-07-22: this is the top-level record class indexing $01:B8D0 —
           $0C angel, $11 town position controller, $12-$15 the four enemy
           families. sim3d keys height policy on it)
  +10      render status tested by ACD9 ($C000 set = skip)
  +12      behavior dispatch selector used outside the OAM leaf
           (2026-07-22: masked to $7FFF this is the state index inside the
           class's own table, e.g. class $12 state 6 = Blue Dragon building
           strike. `(class, state)` is the pair sim3d's classifier consumes)
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

### Sim 3D presentation seams (D3c/D4a/D6a-c, updated 2026-08-03)

The enhanced town renderer adds host-only seams downstream of the composition
leaves. None of them touch gameplay: every one reads the record fields above
and writes only host presentation metadata.

| Seam | Where | Contract |
|---|---|---|
| Live-area clipping | `ComposeFlatPixelsPolicy` **and** `RestoreTownHudPolicy` (`src/sim/sim3d.c`) | Columns outside `live_x0/live_x1` are black below the promoted-HUD rows, for the base fill **and** for every composited plane. An OBJ with a wrapped-negative X rasterizes into the margin, so omitting the plane clip breaks D2 byte-equality at a map edge (ledger §23) |
| Semantic record metadata | `SimRenderMetadata_BeginRecord/RecordPart/EndRecord`, called from the `$01:ADAD`/`$AE6F` HLE leaves | Records the OAM range each source record emits, split when parts cross priority bands or OBJ colour-math eligibility. Producer state is game-thread only |
| Semantic effect metadata | `CaptureEffectInstances` (`src/sim/sim_render_metadata.c`) | Converts immutable fixed/world record snapshots into kind/phase/geometry/colour-family plus lifecycle, pulse, and generation ages. Lightning miracle uses its authentic user/posted outer lifecycle; Blue Dragon and Red Demon use class/state lifecycles; ground fire is composition-owned across the observed record-class transitions; scripted burning houses require packed world identity `$0A01` plus exact `$DD2D/$DD33/$DD39`; the two new-town creation strikes are world process `$000E` records whose polymorphic raw `+$06` retains script base `$A8BB`, plus exact `$01:A8BB` phases including `$E527` as a nonvisible continuity gap. Fixed and world slots occupy disjoint tracker indices. The producer also captures each source's raw `+$06` and emitted OBJ-palette mask because runtime-built `$E6CA/$E6D0/$E6D6` art is red on palette 1 and blue on palette 2. Presentation-only frames never tick it, invalid source metadata clears its trackers, ambiguous palettes fail closed, and overflow invalidates only the effect layer for that frame |
| Portable effect rendering | `DrawSimEffectLocalLighting`, `DrawSimEffectSceneFlash`, and `DrawSimEffectParticles` (`src/present.c`) | One style lookup maps semantic phase to light/particle policy. Semantic geometry remains the attachment/ground-contact point; style may independently lift screen-upright glow and particle origins when tall art would otherwise place them beneath its silhouette (house fire `(8,16)` contact to `(8,12)` presentation). The stages use SDL's renderer-provided additive blend and untextured geometry—not a platform shader—batch each geometry class, verify the applied blend, restore renderer state, and independently latch unsupported blend/geometry paths closed. Feature-off captures remain exact and the authentic framebuffer is never modified A third particle motion, `kSimEffectParticle_Trail`, lays flame and smoke along an effect's own retained path instead of around a fixed point, so a lobbed fireball's tail follows the authentic arc rather than a heading guessed from one frame; puff jitter is keyed on each sample's world position, never on the array index, because a sample slides one slot further back every tick and index-keyed noise would make the plume crawl forward through itself. The particle batch is cut to whichever of the point and trail budgets is larger. |
| Scripted-house-fire cadence | `SimVisualPatches_Apply` (`src/sim/sim_visual_patches.c`), called after cart load and before `Randomizer_Init` | Run `20260803-130945` proves three world `$0A01` actors use `$01:A838`: one-tick `$DD2D/$DD33/$DD39` then `FE 01`. The patch validates that entire signature before changing only the three duration bytes to four (15 source fps); mismatch and undersized images remain untouched. Applying before the randomizer snapshot makes later option re-application deterministic and prevents visual patches from leaking into renderer/game-state callbacks |
| Volcanic eruption effects | `ClassifyEffectSource` + `Sim3D_ClassifyObject` (`src/sim/sim_render_metadata.c`), styled by `SimEffectStyleFor` (`src/present_sim3d_effects.c`) | The end-of-region eruption, measured in run `20260818-070141`. Packed world identity `$0E01` plus one of three exact authored compositions; the identity alone and the composition alone both fail closed, and the bytes between two compositions are part records rather than further frames. `$E7D0` (script `$01:A853`) and `$E7A6` (`$01:A857`) are the airborne fireball on the `flying_projectile` plane, so terrain neither raises nor hides them and their record-origin-centred art keeps the ROM's own placement. `$DD9F/$DDA5/$DDAB` (`$01:A85B`) is the ground fire, and it is **not new art**: those are tiles `$086/$088/$08A` in palette 1, the exact frames the burning house uses, so they classify into the same `HouseFireA/B/C` phase family and reach the identical lighting ramp. Only the contact point differs — `(0,16)` for the centre-anchored eruption frames against `(8,16)` for the corner-anchored house. A separate kind keeps traces honest without forking the style. |
| Eruption fireball pathing | `SimEruptionScript_ResolveFlight` + `UpdateProjectileArc` + `ApplyProjectileArc` (`src/sim/sim_render_metadata.c`) | **In the projected town this pass owns the entire AIRBORNE visual; the ground fire stays the ROM's.** There is no authentic altitude to reproduce -- the sim town is a flat top-down map, the drawn position is exactly `world - camera`, and record `+$00` is the animation frame timer, not a height. Which applies is decided by the view and the master switch through the shared `SimFrameIsProjected`, never by a setting: a flat path drawn inside a projected town is faithful to nothing, and the authentic picture is the authentic *view*. <br><br>**ONE parabola replaces the ROM's THREE phases.** Measured end to end on record `$0FA4`: a crater placement at `(144,128)`, a 144px climb straight up the crater column to `y = -16`, a staging teleport SIDEWAYS to the landing column, a 76-frame wait on the release countdown, then a descent to the landing row. None of the three is drawn as itself -- the climb up-map and the teleport are top-down stand-ins for going up, and rendered literally they are a spark sliding backwards along the grass followed by a fireball dropping vertically out of the sky. The arc runs continuously across all three, including the countdown, so the fireball is never withheld between launch and landing. (An earlier version mapped the climb to the arc's first half and the descent to its second, which put the wait on the apex and hid the fireball for two thirds of its life -- the disappear-and-reappear the frame clock below retired.) <br><br>**The flight is read, not inferred, and the two walks fail independently.** Class-`$01` scripts are static bank-`$0A` ROM. `SimEruptionScript_ResolveFlight` walks FORWARD from the record's cursor for the descent left, the frames left, and — while the staging teleport is still ahead, which is the whole climb — the destination, so the arc knows where it is throwing before the record does. It walks BACKWARD from the script base for the crater, best-effort: that walk cannot cross the jump that makes the script loop, so a record on a later pass launches from the crater the pass learned from an earlier one. Tying the descent to the crater's success once left a fountain of thirty fireballs with six arcs. <br><br>**The clock is FRAMES TO LANDING, and it is the only quantity that spans all three phases.** The record's own position freezes for the countdown -- 76 frames of a 93-frame flight -- so an arc driven by position has to hide the fireball there and bring it back at the apex, which is the disappear-and-reappear this replaced. The frame count comes from the live `+$22` (a wait already running is not on the walk at all: `$01:CE5F` advances the cursor past the `$09` before counting down) plus one frame per remaining command plus the full operand of every wait not yet reached. It is monotone, it is authored, and it reaches zero on the landing frame -- so the arc reaches zero offset and zero height there by construction. Driving one curve from two different quantities (a whole-script frame count for progress, the descent alone for length) is what pinned every fireball at the crater and then snapped it onto its cell. <br><br>**The throw is sized by its flight time, not its distance.** With gravity fixed, `apex = T^2 / k` -- so a fireball with a long countdown ahead of it is lobbed higher and travels slower, and the countdown is spent climbing instead of parked. Distance plays no part; horizontal speed covers that, exactly as in the real thing. Measured over the captured eruption: 44-93 frame flights, 33-149 pixel apexes. <br><br>**The launch is the volcano's MOUTH, the renderer is asked where that is, and it is read LIVE.** The script's crater is the flat cell `(144,128)`; the volcano the player sees is the six-cell `kAitosVolcano` mountain stamp standing on it, whose relief raises the summit, pushes it down-map and leans it toward the camera. `AppendVolcanoEffects` already computes that point to place the crater glow and the smoke plume, so it publishes it (`SimBackgroundVoxelRenderer_CraterAnchor`, leaned) and `PublishSimCraterAnchor` hands it to the producer. Read on every build rather than snapshotted with the rest of the throw, because the mouth moves whenever the camera does — a frozen launch point would leave every fireball already in the air trailing back to where the crater used to be the instant the player panned. `kSimEruptionCraterLift/Drop` remain the fallback for a frame that drew no mountains, and the drop there is measured from the SCRIPT's crater row, not from the mountain baseline: taking it from the baseline put the launch ten pixels low, visible from the default camera and not from a horizontal one, because pitch decides how much of a map-row error reaches the screen. <br><br>**A fireball and the fire it leaves are the SAME RECORD, so not every arc lights a flame.** The landing command hands the record over to the ground-fire script and the next throw takes it back, which caps `airborne + burning` at the eruption's eight slots — an instant with eight fireballs in the air has nothing burning, by construction. Measured over the captured eruption: every one of the 45 throws lands on a cell that catches fire, but the commonest states are 7 air/1 fire and 8 air/0 fire. Deliberately kept: it is the ROM's own behaviour, and the variety reads better than a town uniformly ablaze. `AR_ERUPTDBG=1` prints the split whenever it changes. <br><br>**Placement is decided by where the record IS, not by whether a plan came back.** A record parked out of play still resolves a full descent, because walking its script from the base counts the whole fall; it is withheld because its position is neither the crater column nor the landing column. Same for the staged wait and any script that does not resolve, via `SimRenderObject::hidden`. <br><br>**The ROM's fireball ROUTINE is replaced; its ART is reused at the arc head.** The billboard is always withheld (`kSimEruptionSuppressFireballArt`) — drawing the record where the ROM put it puts a second, wrong fireball on screen — but a flame trail with nothing leading it reads as smoke from nowhere. So `DrawSimEffectFireballHeads` takes the two apart: the atlas rectangle and the composition's local extents come from the suppressed OBJECT, the position comes from the EFFECT. That split is the point — position through the object path would go back through the virtual-height switch, the terrain shear, the height pop and the depth bands, any of which can move a billboard somewhere its own trail is not; through the effect there is one number and the art cannot disagree with the smoke it leads. Drawn after the particles, so a fireball is in front of its own plume. <br><br>**Two things had to give way for that to work.** The ROM's sprite window drops a record's parts when it is off screen, and the eruption parks its records one row above the map for the whole countdown — so the art existed for only a third of a flight. Eruption records are therefore exempt from the VERTICAL window in the producer, and only the vertical one: the widescreen composite is the authentic screen's height, so a part above its top edge produces no pixels in the flat view, while the horizontal window is a real question about the margin and is left alone. That lifts coverage from 32% of samples to 88%. The rest is covered by BORROWING: every fireball wears the same two authored compositions, so a record whose own art the horizontal window dropped takes a sibling's identical entry from the same frame, found through `Sim3D_VolcanoFireballPhase`. 98.7% of samples end up with a head; the remainder draw none and the trail carries the throw. A rectangle cannot simply be cached instead — `sim_render_atlas.c` repacks every frame, so last frame's coordinates point at someone else's art. <br><br>**The art is turned onto its own trajectory.** The arc publishes its tangent as `SimEffectInstance::travel_x/travel_y/travel_height` — on the effect, not the object, because that is where the art is drawn from and the only place the heading and the smoke behind it are guaranteed to agree. The renderer steps along it, projects through the live camera and reads the angle off the result: screen space, not map space, because the same throw leans differently under yaw and pitch. **The two authored frames point OPPOSITE ways**, so the heading constant is per-phase (`SimFireballArtHeadingDegrees`): the ROM drew a climbing fireball pointing up and a falling one pointing down, and swaps between them on the build the descent starts — measured on `$0FA4`, composition goes `$E7D0` → `$E7A6` on the same build the arc's height rate goes +568 to −547, on every cycle of every record. The art therefore already carries half the rotation, and one constant for both is wrong for one phase by exactly 180°. That 180 is why it hid: it reads as CORRECT for whichever half of the arc it suits, so the error presents as "wrong on the climb, right after the apex" and, once flipped, the reverse — neither is a bug at the apex. If a fireball looks 180° out, check WHICH PHASE it is in before touching a number; the zero-rotation heading is a free measurement, since some heading always turns the art by exactly nothing. Every fragment turns about the shared anchor, since rotating each about its own centre pulls a multi-tile composition apart. <br><br>The record itself is never moved, so gameplay position stays exactly what the ROM wrote. `AR_ERUPT_OFF` is a single kill switch over BOTH passes — a half-disabled stage answers no question — and `tools/erupt_ab.sh` toggles it to prove the game is untouched: same binary, same save, same replayed input, byte-identical WRAM trace. Worth knowing what that does and does not buy. The renderer and the metadata producer hold NO write path to the emulated bus, so nothing there can leak with or without the check; what it guards is the sprite hook in `actraiser_widescreen_sprites.c`, which runs INSIDE emulation with nine `cpu_write16` calls and which the eruption reaches into twice — the flight is resolved there, and eruption records are exempted from the vertical sprite window there. The traced range covers the OAM low table (`$0380..$0580`) that hook writes as well as the eruption records (`$0F0C..$1061`); the earlier records-only range would not have covered the write surface at all. It is manual — no ctest test needs the ROM or a recording — so run it when the sprite hook or the arc is touched. Every consumer of "where is this object" must use `SimObjectDrawnWorld` — the depth sort and the mountain-terrain filter included. Arcs, the learned crater and the reported mouth are dropped on leaving town and on a direct town change; retained trails are dropped whenever the publishing rule changes. Gated on the exact fireball compositions, not the height class, because the angel arrow shares `kSimHeightClass_FlyingProjectile`. `AR_ERUPTDBG=1` prints one line per throw with its endpoints and the air/fire census whenever it changes: 45 throws across the captured eruption, all from the crater, landing from x=32 to x=288 and y=96 to y=352. |
| Effect path retention | `SimEffectLifetime::trail` → `SimEffectInstance::trail` (`src/sim/sim_render_metadata.c`) | The only place the metadata layer remembers where an effect used to be. Bounded to `kSimEffectTrailSamples` published world positions plus the altitude each was taken at, newest first. <br><br>**Retention starts LATE, and how late is rolled per throw.** Every fireball in the fountain launches from the same crater mouth, and a path's oldest samples carry its biggest, most spread-out puffs — so eight throws bury the one pixel the volcano is supposed to be erupting out of. Nothing is retained for the first `kSimEffectTrailLaunchDelay` builds plus a per-throw jitter, which starts each path a little way along its own arc and at a different radius, so the eight do not draw a clean ring. Measured over the captured eruption: tails start 41–143 authentic pixels up and 4–107 along, across 79 distinct values. It moves only the smoke — the fireball is drawn from the effect's live position and still leaves the crater. The jitter is hashed from the effect's generation, which is one throw's identity: deterministic within a flight (a delay that moved under one would make the tail crawl through its own smoke) and touching no shared RNG, so the presentation still cannot perturb the game. <br><br>**The head is live every build; the array only shifts every `kSimEffectTrailStride` builds.** Those are different requirements and one sample a build cannot serve both: the flame end has to sit exactly on the thing it is trailing, while the smoke behind it has to reach back far enough to show the trajectory, and the longest eruption throw is about 130 producer builds. 32 samples at stride 4 spans 128 — measured over the captured eruption, a full trail covers 83–180 map pixels of ground, most of each throw. Index *n* is therefore about *n × stride* ticks old and index 0 is now; a repeated capture of one immutable build never lengthens it. <br><br>Only kinds that actually travel populate it (`EffectKindTravels`); everything else publishes `trail_count == 0` rather than a pile of identical points. The path lives beside the generation that validates it and is cleared in the same branch that starts a new one, so a reused record slot can never inherit a stranger's trajectory — the fireball landing changes kind, which breaks continuity and drops the flight path with it. <br><br>Renderer side: age is normalised against the CAPACITY, not against how much of it is filled, or a puff would slide back toward "young" as the trail lengthened behind it and the whole plume would brighten while the fireball flew. Opacity holds through `1 - t²` and spends its fade at the far end, because a linear fade over a path this long makes the middle of the trajectory — the part that shows where the fireball went — the faintest thing on screen. The particle batch is `static`, not automatic: a trail this long puts the worst case near a third of a megabyte, and sizing the visual to fit a render thread's stack frame is the wrong trade. |
| Object classification | `Sim3D_ClassifyObject` (`src/sim/sim_render_metadata.c`) | Pure function of `(tier, class +$0E, state +$12, record address, composition +$08)` → presentation plane, virtual height, anchor/shadow traits. **Record semantics first, composition override second.** Adding a rule here is the supported way to fix a mis-anchored object; never add a height test in the renderer |
| Height easing | `ApplyHeightSlew` inside `SimRenderMetadata_CaptureFrame` | Per-world-record ramp (4px/frame) toward the classified plane, folded into the immutable frame copy. Cleared whenever the SIM 3D master is off, a picker is active, or the frame falls back, so re-enabling never replays a stale ramp. Contact-exact classes bypass it entirely |
| Picker view policy | `AR_SIM3D_PICKER_TOPDOWN` (CMake option, default `OFF`) | Build-time choice between the authentic flat picker view and keeping the projected view. Guards exactly two sites: the view classification in `SimRenderMetadata_CaptureFrame` and the capture gate in `Sim3D_PrepareCapture`. Gameplay, `$7F:9215`, D-pad targeting, and the selected cell are identical either way |
| Promoted town-HUD handoff | `StandardTownHudCapture` / `PrepareHudHandoff` (`src/sim/sim3d.c`) | The only pre-existing captures enhanced SIM accepts are the standard BG3 HUD rectangle and the promoted four-sprite hourglass OBJ rectangle. The hourglass range is per-frame data: menus move it from OAM 0-3 to 11-14, so validation rescans the complete signature with `ActRaiser_FindSimulationHourglass` and requires `capture.oamFirst` to equal the discovered range. Never replace that with `kActRaiserHudObjOamFirst`; fixed screen position does not imply fixed allocator ownership (ledger §47) |
| Shadow caster selection | `Sim3D_ObjectCastsShadow` (`src/sim/sim_render_metadata.c`) | Pure predicate over the classified descriptor: world tier, valid atlas art, and neither `MapPlane` nor `NoShadow`. Height is deliberately **not** an input — grounded actors cast too. To stop something casting, mark it `NoShadow` in the classifier, never special-case it in the shadow pass |
| Ground shadow mask | `DrawSimShadowMask` (`src/present_sim3d_shadows.c`) | Builds silhouettes into one transparent working target, so overlapping casters cannot double-darken. Flat terrain composites it after the BG1-low ground draw; elevated terrain instead hands it to the shared D32 pass, whose terrain-top receiver depth-tests the mask against cliffs and solid models before writing colour. The working dimensions are halved together until they fit `kSimShadowMaxTargetPixels` (roughly 1440p), then linearly sampled at viewport size; 1080p/1440p remain native while 4K+ memory and fill cost stay bounded. Reads only the immutable `FrameSlot`; allocation failure drops and logs only the shadow stage |
| Shadow blur | `BlurSimShadowMask` (`src/present_sim3d_shadows.c`) | Separable seven-tap box blur over the mask target, two full-target passes ping-ponging through one lazily allocated scratch target. The primary path is the same generated fragment shader contract on Metal/SPIR-V/DXIL; a renderer without that GPU state falls back to ordinary blended taps with a custom add-alpha mode. Missing scratch or blend support degrades to the hard mask and never disables geometry or the base shadow pass |
| Rim light | `DrawSimRimLight` (`src/present.c`) | Two extra silhouette draws through the shared billboard loop — offset fill in the light colour, then a mask blend (`dstA *= srcA`) that **intersects** it with the sprite's own body — composited additively per priority band. The band must stay *inside* the silhouette; subtracting instead puts it outside, which reads as a glow, not light. Confinement to billboards is structural: the loop skips map-plane art and each band composites after its own sprites, so the rim cannot reach the ground or HUD |
| Billboard depth order | `SimObjectSortsAfter` + `DrawSimObjectPriority` (`src/present.c`) | Projected billboards sort **overhead art last, then** back-to-front by captured screen row, *within* a hardware priority band; the band still owns coarse layering and reverse OAM order is the stable tiebreak (the comparator is strict for exactly that reason). Applies only to the projected profile — the flat path keeps pure OAM order, where it is correct. `kSimObjectTrait_Overhead` exists because the row sort is right for actors standing on the map and wrong for the miracle clouds, whose art hangs above the row their record sits on: sorting those by row let a nearer tree draw over a cloud, an overlap the ROM's own OAM order had expressed correctly before D3b introduced the sort |
| Selection overlay order | `kSimObjectTrait_SelectionOverlay` + `DrawSimSelectionOverlays` (`src/present_sim3d.c`) | Direction/position cursors and the hollow path selector remain map-plane geometry, but projected presentation excludes them from the terrain/object passes and draws them after the completed world and cloud shroud. This prevents raised voxel models from cutting interaction feedback apart without misclassifying a cursor as physically overhead. Authentic OBJ priority and reverse-OAM order are retained among selectors; fixed menu planes remain last. |
| Overhead trait | `Sim3D_ClassifyObject` (`src/sim/sim_render_metadata.c`) | Set for `$D9E5-$DCD2` **except `$DA22`**, which is the family's own ground shadow ellipse drawn 40-72px below the shared anchor — it lies on the ground and anything standing there must occlude it, the exact opposite of the cloud above it. Deliberately a sort trait and **not** a height: the family keeps its record-origin ground anchor, because the bolt and rain compositions span cloud to ground and any lift detaches the strike from the terrain (D3c). Asserted in `sim_render_metadata_test` on both sides |
| Presentation tuning | `Sim3D_AnnotateFrame(frame, const Sim3DTuning *)` | One struct of resolved tuning values (camera, height scale, shadow opacity, height pop) copied into the immutable frame. Add a knob to the struct, never as another positional argument, and never read `g_settings` from a render stage — a frame must not mix values read at different times |
| View-drop reporting | `Sim3D_LogViewTransition` (`src/sim/sim3d.c`) | Prints one `[sim3d-view]` line per enhanced<->authentic transition in a town, with game frame, capture status, and integrity flags. Always on and transition-only, so a one-frame flat flicker is self-diagnosing. Called from the per-frame trace site, which also runs headless |
| World-map underlay / owned developed map | `src/sim/sim_world_map.c` + `src/sim/sim_world_map_compose.c` + `src/sim/sim_world_map_build.c`, drawn by `DrawSimWorldUnderlay` (`src/present_sim3d.c`) | Reads the flat ROM base tilemap `$06:B341`, ordinary translation `$02:8000`, special `$E3-$EF` expansions `$02:8100`, town destinations `$02:87A5`, 256 8bpp tiles `$0E:8000`, and palette `$1C:BF93`. On simulation-town **or `$18/$19=00/09` world-navigation entry**, and on construction-input changes, the pure HLE copies the base, applies `$7F:9101` bit 0's 8x8 clear policy, and composes enabled quadrant-paged town maps from `$7F:2000-$37FF` / `$7F:6B18-$6B23`. Production touches no CPU, WRAM, PPU, stack, or math-unit state and never adopts `$7E:C000`, which acts and towns both clobber. `AR_WORLDMAP_HLE_COMPARE=1` retains the bounded `$02:865C/$02:86D1/$02:8726` transaction only as a differential oracle. Captures gf9461/gf782/gf764 and a live replay all match 16384/16384 bytes; the developed output differs from ROM in exactly 447 bytes. One world tile = one town cell, so the town underlay draws at 2x |
| World-navigation immutable frame | `SimRenderMetadata_CaptureFrame` → `SimWorldNavigationCapture_Capture` → `SimFrameData.world_navigation_scene` | `AR_SIM3D_WORLD_NAV` is an independent off-by-default selector for `$18/$19=00/09`; it does **not** require the Simulation town 3D master. `town` remains zero. The game thread snapshots focus `$0300/$0302`, current/staged matrices `$0304-$0312`, rotation `$0314`, zoom `$0316/$0318`, active location `$0341`, and INIDISP brightness; `SimWorldNavigationScene_Build` inverts the signed 8.8 matrix, publishes the 1024x1024 developed plane, and resolves `$0341` through the seven 256x256 regions at ROM `$01:B73C`. `$01:B6CA` clears `$0341` before scanning, so zero means no clear-region cutout and the complete world remains hazed. Navigation OAM is classified separately: packed priority-3 UI plus the Palace's complete fixed-centre 3x3 grid, or an all-hidden action-entry composition. Partial brightness remains enhanced: full-intensity host world/effects receive one exact master-fade overlay, then the already-brightness-adjusted PPU Palace/UI captures are drawn. Unclassified layouts, forced blank, missing map data, or a singular matrix fail closed to authentic Mode 7. A replay holds enhanced ownership across brightness 0→15 and 15→0, removing both effects-pop gaps. Near zoom hides cloud bodies below the shared altitude but retains ground shadows. Navigation lighting defaults on and clouds off; the weather pass shares no town cull-hole/underlay-margin policy |
| Town window table | `kTownWindows` (`src/sim/sim_world_map.c`) | Six origins: Fillmore (80,48), Bloodpool (48,48), Kasandora (16,64), Aitos (16,32), Marahna (64,96), Northwall (32,0). Derived as world cathedral icon minus the town's own cathedral cell. Every origin is a multiple of 16 and the six 32x32 windows tile the map disjointly — no other assignment of towns to the map's six icons has that property, which is what pins it. Asserted structurally in `tests/sim_world_map_test.c`, because a wrong origin puts a town on someone else's terrain and still looks plausible |
| Full-town ground canvas | `src/sim/sim_town_canvas.c`, drawn by `DrawSimTownCanvas` (`src/present_sim3d.c`) | Renders the whole 512x512 town from the resident BG1 tilemap at **`$7F:0000`** plus VRAM `$0000` character data and CGRAM. The tilemap is **quadrant-paged**: `$03:9C43` writes each cell's 2x2 block at `quadrant*2048 + (cellY & 15)*128 + (cellX & 15)*4`, words at `+$00/+$02/+$40/+$42` — row stride 32 tiles, quadrant stride 32x32 tiles, four pages = 64x64 tiles. A row-major read of that range looks like an unrelated layer, which is exactly how it was mistaken for BG2 twice. One image serial and independent tilemap/character/palette/display revisions describe what changed; unused art can advance its source revision without forcing an image upload. Dirty rows remain a separate render-thread cursor. Marahna's earthquake is therefore a live water-to-land image transition on one immutable surface, not a second terrain mesh or a host event flag. `sim_town_canvas_test` pins both the complete 16x16 publication and the independent invalidation contract |
| Background-voxel publication split | `SimBackgroundVoxels_Build` + `SimBackgroundVoxelRenderer_Upload` | The game thread snapshots the exact scene inputs it consumes: active town cell map, each structure record's position/flags (`+0..+2`, never the ticking action byte at `+3`), terrain/structure definitions, displayed tile-layout revision, and wind-hold policy. Only a change to those inputs reclassifies mountains, structures, bridges, and foliage or rebuilds replacement masks/baselines. Character animation, palette cycling, brightness fades, and backdrop changes refresh pixels against the cached plan without rescanning topology. Scene, enhanced-ground, mountain-atlas, and aggregate publications own independent serials; unchanged buffers retain their serial after a refresh. The SDL ground texture drains coalesced spans covering changed pixels, while the direct-GPU mountain atlas is uploaded only when its own pixels change. Failed partial ground uploads recover with a complete upload rather than accepting a partially advanced cursor. `SimBackgroundVoxels_BuildStats` exposes calls, scene rebuilds, pixel refreshes, and actual changed-pixel totals; regressions pin pixel-only, quiet, action-byte-only, and record-layout transitions |
| Audited town terrain topology | `SimTownTerrain_*` (`src/sim/sim_town_terrain.c`) + visible mesh in `src/present_sim3d_terrain.c` + D32 mesh in `src/sim/sim_background_voxel_terrain_depth.c` | Six immutable 32x32 Q8 corner fields carry classifier-owned hard-edge and face masks. Non-hard shared vertices are welded exactly; hard skirts use `SimTownTerrain_ClipVisibleHigherEdge`, including height reversals where each side owns only its non-crossing interval. Visible colour, depth occlusion, shadow receiving, grounded placement, buried foundations, and bridge datums all sample this one field. Live town artwork never regenerates it: initially submerged Marahna cells already lie at their audited water datum and reveal on those same vertices, preventing both a pre-event ridge and an event-time height pop. Landscape height scales this base relief while mountain/volcano relief remains independently authored |
| Mountain stacked-relief boundary | `SimBackgroundMountainObjects_Build` + `SimBackgroundMountainRelief_Resolve` + `SimBackgroundMountainRender_BuildFaces` + `sim3d_camera_limits.h` | The accepted authentic-art facade remains the front layer. A validated town supplies complete per-mountain objects assembled from explicit terrain-metatile IDs, not rectangular crops of an already-overlapped range: Fillmore's clean 4x4 and 6x6 definitions reuse `$81-$9F`, while fused `$78-$87` overlap variants remain composition evidence and never leak a square shoulder into an object's outer edge. Bloodpool, Kasandora, Aitos, and Northwall have canonical exact-cover tables using the same semantic parts; Aitos adds distinct regular-large and lava-crown stamps, and Northwall resolves the ids through its native snow art/palette. The complete terrain map is the placement oracle (including legitimate clipped edge objects), and every object owns its ground contact and depth anchor. Exact union validation fails closed to the generic connected-art fallback if a town edit would add, omit, or fuse a cell. Quality adds a bounded 1/2/3/5-layer stack (Low through Ultra), displaced only toward canonical map north by 0/1.5/2.5/4 source pixels. Complete sticker copies retain the same silhouette orientation and converge at both their authentic ridge and ground contact; displacement swells only through the mountain body. Every layer submits real projected depth into the shared D32 target, so loop order is only a batching detail. Flipping a rear copy moves off-centre tip pixels and creates false double horns. There are no camera-relative offsets or stretched perimeter joins, so yaw cannot reverse the thickness or produce long quad streaks. SIM pitch stays asymmetric (-1350..-575 mrad), and Adaptive LOD can reduce the layer count. **The stack direction is camera-relative, not map north.** `Scene3D_GroundDepthDirection` resolves the unit ground direction leading away from the camera from the projection's clip-W rows; the relief module still states the displacement as a signed northward offset, and the renderer reads that as a magnitude along this axis. A zero-yaw camera resolves to (0,-1) and reproduces the original northward offset exactly, but the sim camera has a yaw setting, a reactive Dynamic lean and a manual orbit - with a fixed northward push the copies fan out sideways as soon as any of those moves, which reads as the rear copies sliding off the ground while the front one stays anchored. The bases themselves always converge: the taper is zero at both ground contact and ridge. **The mountain reaches down to the land; the land is never raised to meet it.** Each relief layer is a flat inclined plane - its cross-section is a straight line from the ground at the front up and back to the ridge - so the wedge beneath it is empty and open at the sides, and any camera away from the canonical pose shows a tilted board whose upper half hangs in the air. Extra stack layers cannot fix that: they are parallel copies of the same plane and thicken it along the ground rather than filling underneath it, and their bases already converge exactly (the taper is zero at both ground contact and ridge). `AddMountainSkirtTile` closes the silhouette instead, dropping a quad from the plane's edge to z=0 wherever a cell has no neighbour. It follows the ART in both axes, not the cell grid. `MountainSkirtProfileFor` resolves each tile/side once from the immutable silhouette: the wall's top edge is the outline itself, so it stands vertical against a wall tile like `$90` and slants with a shoulder tile's diagonal (`$88` runs x=15 at row 0 to x=0 at row 15) rather than hanging a rectangle beside it. The quad is textured by **stretching** the tile's art down the wall - outline column at the top edge for a seamless join, interior column at the ground - because these tiles carry a one-to-two pixel dither margin along their outline that is both grass-coloured and partly transparent, and a wall sampled from a single column near the edge showed green see-through streaks. The wall's position is a least-squares fit of the whole outline, biased outward until no row of art lies outside it, with trailing rows trimmed where the outline stops being straight. Taking the two end rows alone slants the line inward at a base tile - whose last row is the dither fringe several pixels in - and the mountain then overhangs its own wall by three to four pixels; biasing outward without trimming instead leaves the wall protruding four pixels past the art. Fitted, biased and trimmed, overhang is 0.00 px across all 26 stamp tiles and the worst protrusion is 2.13 px, at rows already within a pixel of the ground. A genuine diagonal deviates from its own fit by nothing, so it is never trimmed. The wall also stands at the OUTERMOST stack copy on its side, not the front one: the stack recedes along the camera's away axis, which under yaw has a sideways component of up to 2.8px at Ultra, so a wall pinned to the front copy is overhung by the rear ones - and only when the camera turns, which is why that overhang survives a fix made against the art alone. Zero when the stack fans the other way, since the front copy is then already outermost. Verified across every quality level and the full yaw range: no copy of the art lies outside the wall on either side. It also carries a half-pixel outward margin and starts a third of a pixel above the face it meets, because the outline is jagged row to row and a single quad can only be straight. **Its shading ramps** from 240 at the mountain to 150 at the ground rather than sitting at one darker value: the face is drawn fully lit, so a flat wall meets it in a hard tonal step and any pixel of misalignment there reads as a shelf instead of an edge. That needed per-vertex brightness on relief faces, which previously carried one value for the whole quad. Those fringe rows are dithered rather than solid runs, so the inward UV sample also backs off to the nearest opaque column; every sampled corner of every stamp tile is verified opaque. The ground plane and town canvas are untouched, which keeps the ground free to gain real height later. |
| Regional structure identity | `SimBackgroundVoxelRegion_HouseStyle` + `SimBackgroundVoxelModel_BuildStyled` + `SimBackgroundVoxelPalette_Resolve` | House progression is a semantic `(town, level)` choice, never a recolour of Fillmore's house. Kasandora resolves yurt -> white tent -> adobe, Marahna resolves yurt -> stilt hut -> log cabin, Northwall's developed tier resolves stone housing, and Aitos' developed masonry owns a low flat terrace roof rather than inheriting a shared gable. Shared geometry is used only where the source culture actually shares a stage (the two towns' starting yurt); every other stage owns its silhouette, authored height and palette while still using the common cache and LOD path. Tests hash every town/stage combination, and the Aitos model is additionally checked at every detail/style boundary for a horizontal centre roof with no pitched roof faces, so an optional trim path cannot silently restore the old chalet silhouette. |
| Story-landmark identity | `SimBackgroundVoxelLandmarks_Classify` + `SimBackgroundVoxels_Classify` | Each unique landmark owns a reserved 2x2 plot in its town's `$7F:2000` cell map, stamped with an `$E0-$EF` special-structure expansion metatile: Bloodpool's castle is `$EC` at (6,16), Kasandora's pyramid is `$EE` at (20,4), and Northwall's ancient tree is `$EB` at (26,14). Fillmore, Aitos and Marahna have none. The plot is both the position and the size, so every landmark model is authored in 32x32 town pixels; the earlier lair-record derivation put the castle nine cells away at 4x4 and had no entry for the pyramid at all. Marahna's "temple" is not a landmark of this class but the `$C0` variant of the same 2x2 sanctuary plot every other town stamps `$C2`, so it is classified with the cathedrals and only its model differs. Classification runs before forest extraction and reserves the landmark's complete source footprint, preventing generic trees from leaking through it. |
| Aitos volcano semantics | `SimBackgroundMountainObjects_Build` + `AppendVolcanoEffects` | The lava-crown mountain stamp marks exactly one semantic volcano object. Its relief is 12% taller than ordinary mountains at every quality level. Balanced/Trim and above add a game-frame-synchronised crater flash; Architectural/High adds two smoke puffs and Ultra adds four. The flash is an elliptical octagonal fan centred on the blob the `$70/$71` crown actually draws - source pixels x 138-149, y 136-141, so eleven pixels below the crown row's top edge - and its depth radius is taken through the same plane mapping as the mountain face it sits on. The earlier axis-aligned rectangles were centred on the row's top edge and overhung the peak into the grass behind it. These are fixed bounded face counts with no per-frame allocation. The effect never screen-scrapes red pixels, so palette animation, camera scale and neighbouring mountain art cannot accidentally turn another peak into a volcano. |
| Structure animation and progress identity | `ApplyStructureFrame` + `SimBackgroundVoxels_Classify` + `SimBackgroundVoxelObject::animation_phase` | A windmill's blade position and a structure's built-versus-scaffold state are read from the frame its plot is currently drawing — matching the live 2×2 tile entries against the structure metatile atlas at `$7E:3100`, the same atlas `$03:9C43` copies from. Class 6 draws `$04`/`$06`/`$14` while going up and cycles `$24`/`$26`/`$16` once finished, three blade positions 30° apart in the wheel's 90° period; class 8 is `$34` scaffold and `$36` finished. That single read gives the spin, its cadence, its pause during the "no wind" event and its restart afterwards without a host clock, because it is the ROM's own step program driving both views. It deliberately ignores the record's `$40` bit (see above), and an unmatched plot keeps the finished model — un-building something the town has already finished is the louder error. The one intentional divergence is the Extras toggle "Wind stops every windmill", which holds every mill in a town while any of its records carries the bit, because `$03:E2BB` stamps only the records that exist when it fires. |
| Foliage identity | `FoliageClassForTile` + `DisplayedCellMetatile` + `SimBackgroundVoxels_Classify` | Canopy extraction reads terrain metatile identity, not rendered chroma. The metatile atlas is an eight-wide grid whose entries mean the same thing in every town: `$02-$04/$0A-$0C/$12-$14/$1A-$1C` plus `$23/$24` are the pointed evergreen family, `$05-$07/$0D-$0F/$15-$17/$1D-$1F` plus `$26/$27` the broad round canopies (Marahna's mangroves, Kasandora's oasis stands), `$22` a mostly-ground forest fringe that only becomes foliage when it touches a complete canopy cell and inherits that block's family, and `$01`/`$09` the two clearable brush entries - the round bush and the palm. Brush is single-cell and never joins a forest component; the permanent family is the **cell's**, not the town's, because Marahna's palms are clearable while its mangroves are not, and Kasandora carries both permanent families at once. Chroma could not make these calls: Bloodpool's `$3D-$4F`/`$8D` marsh reads 37-52% "canopy green" and was becoming ~145 cells of forest, while Northwall's grey-white firs read as none. **The cell map is not the last word on what a cell shows.** Clearing a bush or a wood commits the cleared cell-map value as soon as the miracle resolves and repaints the BG1 tilemap only when the animation ends, so for those frames the semantic map says grass while the original art is still drawn; classifying from the cell map alone dropped the model and let the flat authentic sprite pop back mid-strike. `DisplayedCellMetatile` resolves what the cell is actually displaying by matching its four live tilemap entries against the `$7E:2100` terrain definitions (masked `$DDFF`: bit 9 is traversal metadata, bit 13 is tile priority), falling back to the recorded value for structure art and expansion marks, which are not in that atlas. |
| Voxel shading memoisation | `SimBackgroundVoxelModelCache_Get` + `SimBackgroundVoxelModelShadingKey` | Per-face lighting looked like per-frame work because it was issued per frame, but none of its inputs move with the camera: face geometry and corner occlusion are fixed when the model is compiled, and light direction, shading mode and biome are settings. It is resolved once per cached model per lighting state and stored beside the geometry, keyed on azimuth/elevation/shading/biome - detail and style are already in the model key. Measured on a developed Bloodpool at Ultra: 1.111 ms/frame to 0.009, against 0.643 ms for projection, which genuinely does depend on the camera and is left alone. Costs 1.9KB per entry, cache 14.3 -> 15.2 MB. A `relights` stat is exposed because it should spike for one frame after a light or quality change and read zero otherwise: a steady non-zero count is the signature of a key that is missing an input. |
| Voxel shadow batching | `SimBackgroundVoxelRenderer_DrawShadowMask` | The shadow mask is the one voxel path still batching through `SDL_RenderGeometry` rather than the depth pass. `AppendSolidQuad` drops silently when the batch is full and the only flush was after the whole object loop, so a developed town - 1160 objects at up to three volumes and six quads each, 20880 quads - lost every shadow past the cap with no warning. The loop now flushes ahead of any caster that would not fit, which is free: the mask is opaque black into an offscreen target, so overlap is idempotent and draw order does not matter, and the OBJ caster pass above it already issues one draw apiece. With a real flush path the batch no longer has to be sized for the worst case, so it drops from 8192 quads (1.2MB) to 2048 (304KB) - one draw for a real town, eleven for the theoretical maximum. A `_Static_assert` pins it at no smaller than one caster's volumes. |
| Terrain depth/shadow projection cache | `SimBackgroundVoxelTerrainDepth_Append` + `EmitTerrainDepthQuad` (`src/sim/sim_background_voxel_terrain_depth.c`) | Terrain tops enter the shared D32 target as colour-disabled occluders; the same tops enter a depth-tested, depth-write-disabled receiver layer for the filtered screen-space shadow mask, while cliff skirts occlude but never receive a shadow stretched down their face. The projected quad list is bounded by the geometric maximum of five quads per cell and keyed by town, camera, source/viewport, render scale, Landscape height, and the complete projection matrix. It is replayed between the shadow receiver and main composite and across still frames. The actor-band depth extrema use the same key in a separate tiny cache, avoiding 1,089 repeated corner transforms when no earlier shadow pass populated the quad cache. Canvas serial is deliberately absent: artwork changes do not change the immutable topology. Overflow invalidates reuse instead of caching an incomplete mesh |
| SIM3D GPU depth boundary | `Sim3DDepthPass_Require` + `Sim3DDepthPass_Begin` / `Sim3DDepthPass_Submit` | SDL's GPU renderer, generated backend shaders, and `D32_FLOAT` target support are mandatory at startup; there is deliberately no software/painter fallback. Mountains use a pass-owned immutable GPU atlas and solid faces use a pass-owned white texture rather than sampling renderer staging resources from a direct command buffer. Terrain, mountains, buildings, and trees share one transparent colour target and one depth attachment, with alpha-cutout fragments discarded before depth writes. CPU lists stage four unique vertices per quad; geometrically growing persistent vertex/transfer buffers pair with a persistent 32-bit `0,1,2,0,2,3` index pattern uploaded only when capacity grows, and every layer submits with `SDL_DrawGPUIndexedPrimitives`. The shadow receiver alone uses a linear sampler; pixel-art mountains retain nearest sampling. **Grounded actors are split by TERRAIN, not by depth**: `SimBackgroundVoxels_CellIsMountain` on an actor's foot cell asks the authentic 2D question -- is it standing on a mountain metatile? -- and actors that are not are drawn BEFORE the composite so the town's own geometry hides them, while actors that are are drawn after it and lifted onto the slope. The overhead camera is what makes the simple answer the right one: a model only occludes what is behind it on screen, so an actor merely beside a building still shows. Flying actors and selectors remain later authored priority bands.

**A mountain hides only what is BEHIND it**, and only within its own reach. The camera looks from the south, so a mass north of an actor cannot clip it however far the sheared art climbs the screen — that was a villager keeping his feet and losing the orb held above his head. `SimBackgroundVoxels_MountainInFrontOf` therefore scans SOUTH from the actor's cell, bounded to four cells: a mass H cells tall displaces its highest pixels south by `H * (1 - face_depth_scale)`, about `0.38 * H`, and town masses run to roughly ten. The bound is not decoration. Scanning the whole column instead put **598 of Aitos's 626** ground cells behind a mountain — the southern rim alone claimed the town, which is no split at all. `CheckMountainOcclusionReach` pins it.

An earlier attempt split the composite into two colour attachments so ground actors could be hidden by mountains but not by buildings. It was removed once building occlusion turned out to be wanted: it cost a second output-resolution colour target (tens of megabytes, doubled again at Smooth 2x) and a second full-viewport blit to express a distinction nobody needed. If it is ever revived, it must stay ONE render pass using per-layer colour write masks -- splitting into two passes would force the shared depth target off `STOREOP_DONT_CARE` and pay a store plus a load of that same buffer every frame.

**Terrain lift.** An actor the 2D map places on a mountain has to take the same shear the mountain art does, or it draws at the mass's foot with the slope rising behind it. `SimBackgroundVoxels_MountainSurface` applies the renderer's own transform -- altitude `(baseline - y) * face_height_scale`, with the art pulled toward its base by `face_depth_scale` -- against a per-cell baseline that is the bottom row of the four-connected mountain mass the cell belongs to. Measured on Aitos, that is a lift falling to ~2.4px at a mass's bottom row and reaching ~45.6px at the top of a ten-row mass, with a matching southward shift. It is deliberately NOT scaled by the player's object-height setting: it has to agree with the geometry, which converts its own pixels straight through `height / source.h`.

**Two height-class predicates, deliberately not one.** `Sim3D_HeightClassStandsOnTerrain` asks whether terrain RAISES a class; `Sim3D_HeightClassIsOccludable` asks whether terrain may HIDE it. They were one function once, and merging them caused two regressions in a row: the angel and its arrows snapped upward crossing a peak's cells, and the Napper's pluck and the dragon's building strike — which dip toward the ground but pass above the roofs they reach over — were swallowed by buildings. The middle rows are the whole point: `GroundEffect`/`SemiGrounded`/`GroundStrike` are raised by terrain but never hidden by it. Occludable implies raised; the converse must not hold. `TestTerrainHeightClassPredicates` pins every class in both, and fails if a newly added class inherits a default. The bias is toward visible: an effect that should have been hidden is a smaller error than one that vanishes mid-animation.

**Structure-anchored overlays.** The ROM hangs status and thought bubbles on the owning structure's record cell, which projects to the building's FOOT — so in 3D they end up inside the model. `kSimObjectTrait_StructureOverlay` marks them and the renderer adds `SimBackgroundVoxels_StructureHeight` at that cell. Two measured details make it stable: the compositions are 16x32 stacks anchored at the TOP, so the height must be sampled from the art's bottom edge (`world_y + local_y1`), two cells below the anchor; and structure height is a step function at each cell boundary (measured on Aitos: 0 at map_y 382, 9.5 at 384) while the ROM gives these bubbles a 3px bounce, so the query scans DOWN a cell rather than sampling one. Sampling a single cell flipped the lift between nothing and a whole storey on alternate frames, which read as the bubble leaping.

**The rim light is part of this ordering, not an afterthought.** It composites from its own full-viewport target, so a band built from every actor and added after the composite paints the outlines of actors the composite has just hidden -- sprite silhouettes glowing through a mountain. Each terrain band now builds and composites only its own actors, immediately after the sprites it belongs to. Translucent crater glow and smoke use the final `Effect` pipeline: depth testing remains enabled so opaque geometry can occlude them, but depth writes are disabled so transparent texels cannot punch holes in later effects or mountain art. Do **not** reintroduce whole-object or average-face sorting as a visibility mechanism. Do **not** add generic back-face culling: cathedral, factory and stepped-foliage meshes are not uniformly watertight. `sim_background_voxel_surface` remains the single outward-normal convention used by lighting and surface analysis. |
| Sim-mode sprite ranges | `ws_sim_emit_margins` / `ws_sim_extended_margins` / `ws_sim_lifetime_margins` (`src/actraiser/actraiser_widescreen_sprites.c`) | Three named policies share one finite-town clamp but receive their requested range independently. **Emit** is real-OAM reach and remains display-preset-controlled with an authentic vertical window; **extended** is exact host-part/cull-cue reach; **lifetime** feeds `ActRaiser_SimProjectileVisible`, whose false result destroys the record (`$B44B` branches to destruction) and therefore changes gameplay and world-record pressure. `sim_view_range` (default 0, max 256) raises extended and lifetime reach together while real OAM stays unchanged, including under 4:3 and Wide Raw. Fog and the cloud-shroud clear rectangle consume the extended four-axis margins. ROM samples at 0/64/128/192/256 peaked at 12 of 44 live world records; synthetic parts peaked at 15 with no overflow. |
| Action OBJ apron (resolve channel) | `src/action/action_obj_apron.{c,h}` + `ActRaiser_DioramaApronFinish` (`src/actraiser/actraiser_rtl.c`) | Diorama-only. Captured OBJ planes carry `kPpuObjApron = 64` columns per side BEYOND the displayed span; a part the emitter rejects from the display window rides a host part list into them instead of vanishing, rasterized at capture time after scanout. **Real OAM is never widened, structurally:** the sprite builder's own X cull is untouched, so a rejected part stays PARKED exactly as the ROM left it; only the object-level draw predicate widens (it gates whether the builder runs at all), and an object admitted solely by that widening has every part rejected and parks a slot rather than consuming one. Parts carry EXACT position because these coordinates can be ambiguous after 9-bit OAM encoding. Writes are clipped to the two apron bands by handing `PpuRasterizeParts` the band as its `bounds` — the display window is scanout's, and writing it would double-draw a straddling part with no z-test against the sprites it lost to. Ordering mirrors the hardware: OAM order decides ownership through ONE shared z-test and only the survivor's priority picks a plane, so parts draw one at a time and a pixel already opaque in ANY of the four OBJ planes is skipped. **The apron is never DISPLAYED** — the scanline buffer is 512px (`kPpuExtraLeftRight = 128`) and the live background cap is 120px/side, while the 64px apron remains a separate resolve-only surface band. The benefit is that the DOF/edge-AA/rim shaders, which sample the full UV window, stop blending the last real texel against nothing. With GPU shaders off it changes no pixel. `kPpuObjApron = 0` is the A/B lever. `AR_APRONLOG=1` reports per-frame count plus cumulative peak/overflow (measured peak 16 over ~2200 diorama frames against a capacity of 128). See rendering-engine.md §13j |
| Cull-cue boundary | `Sim3D_CullProximity` (`src/sim/sim_render_metadata.c`) | The single pure predicate behind every out-of-range cue — ground fade, focus falloff, cloud shroud, per-record cover. Stated in the **emitter's biased coordinates**, not screen or town space, so the cull test and the things that explain it are one piece of arithmetic; `_Static_assert`s in `actraiser_widescreen_sprites.c` tie the mirrored window constants to the emitter's own. Rounded-box distance (not `max(dx,dy)`, which is an axis-aligned box and was the visible squareness) with a smoothstepped ramp. Rounding and the bottom `lift_inset` may only ever **add** cover; both are asserted that way |
| Cull fade vs cull dim | `SimCullFade` (`src/present.c`) | Two independent terms on one proximity ramp. `fade` is structural (which layer is showing, via alpha); `dim` is photometric (how lit it is, multiplied into the vertex colour). Splitting them was forced by the graded sky: the underlay's distance haze blends toward `separated_backdrop_argb`, formerly flat black and now a blue gradient, so the single combined control washed the far field grey-blue instead of darkening it. The blurred underlay pass takes `dim` but **not** `fade` — it is the layer being revealed, and fading it makes the far field transparent rather than dark |
| Cull evidence capture | `SimRenderMetadata_RecordAnchor` / `_RecordClippedPart` (`src/sim/sim_render_metadata.c`) | The emitter reports its biased composition origin and every part the sprite window rejected, at the two branches in `ws_sim_build_sprites` that previously parked the OAM slot at `$E000` and dropped the fact. The anchor is **handed over, not re-derived**: the emitter reaches it through DP `$94/$96` with 16-bit wraparound and a second derivation is a second thing to keep in step. Purely observational — no OAM or game state is touched |
| Cull cover eligibility | `Sim3D_SourceCullCover` (`src/sim/sim_render_metadata.c`) | Only the sprite window may create cover. A record that emitted nothing **and** was never clipped is the game declining to draw it, and covering that asserts something false about the world; fixed-tier furniture is screen space and never qualifies. Widening this to "anything absent" would put clouds over destroyed projectiles and off-world records |
| Cover timing vs placement | `Sim3D_SourceCullCover` vs `Sim3D_SourceDrawLift` | Two different questions and they must not be merged. *When* cover arrives is the emitter's — it culls on the record's own y, so the unlifted anchor is correct. *Where* cover goes is the renderer's — a flying record is drawn up-screen and cover at the record lands under its feet. `Sim3D_SourceDrawLift` runs the pure classifier from the **source record's** fields precisely because a fully-culled record has no `objects[]` entry to read a height from |
| Lit-window lift inset | `Sim3D_MaxDrawLift` (`src/sim/sim_render_metadata.c`) | The lit ground can only express the height-zero boundary, so its **bottom** edge is inset by the classifier's lift ceiling. Derived from the classifier, never measured over the live record list — an inset that tracked whatever happens to be flying would drift the ground fade while nothing on screen moved. Top edge deliberately not inset: lift is toward negative y, so that side is already conservative. Lifting `dp $96` instead is not an option — one value for every record, trades bottom for top, and the vertical window cannot move at all (see "Sim-mode OAM emit margin") |
| Ground-mesh density | `kSimUnderlayColumns/Rows` (`src/present.c`) | 64x48, raised from 24x18. The old value served affine UV correctness only; once the cull fade began being **sampled at these vertices** it became a correctness constraint of its own — the mesh must be finer than the smallest feature the fade shows, or a rounded window interpolates back into a box. Vertex/index arrays are file-scope at this density (~140KB) and are present-thread only |
| Fixed-colour add (sun miracle) | `ApplyFixedColorAdd` (`src/sim/sim3d.c`) | Third accepted D2 colour-math state: `cgwsel == 0` (fixed colour as the math source, enabled screen-wide, no window, no main-screen-black region), add without half or subtract, non-zero fixed colour, full brightness. Measured from the sun miracle as `cgwsel=$00 cgadsub=$01 fixed=$0001 screen=$15/$00`, a ramping red add onto BG1 alone. **Baked into the captured plane pixels**, not made a compositing policy like the half-add: a fixed-colour add is a property of one layer, and one application then serves the authentic rebuild, the flat recomposition and the projected textures alike. The arithmetic inverts the PPU's `brightnessMult` table to recover the 5-bit component, adds with the hardware clamp at 31, and maps forward — adding the *expanded* colours instead differs on 168 of 1024 (component, add) pairs and the byte-exact gate rejects it. Brightness 15 only, conservatively |
| Atmospheric backdrop | `DrawSimBackdrop` (`src/present.c`) | Graded sky behind the finite ground, drawn after the flat clear so no pixel is ever undefined if it declines to draw. Endpoints are **authored** sky colours mixed *from* `separated_backdrop_argb`, not derived from it: a town's backdrop is black, and black lifted toward white is grey, which is how the first version shipped a greyscale sky. **Strength 0 is pixel-identical to the flat clear**, which is what D5a-2 compares against. Confined to "behind the finite ground" by draw order, not by a mask |
| Projected horizon | `Scene3D_GroundHorizonScreenY` (`src/scene3d_math.c`) | Solved as the limit of the projection as ground y runs to infinity — never by projecting a "far enough" point, since the ground extension already reaches thousands of captured pixels out and any finite stand-in would need re-tuning with the extent. **The horizon is off screen throughout the supported SIM pitch range** (-1350..-575 mrad), so the sky grades around a *synthetic* horizon at `backdrop_horizon_pct` and uses the real one only if it becomes visible. `scene3d_math_test` sweeps that shared range and **fails if any setting ever puts it on screen**, forcing the backdrop to be revisited rather than silently drawing sky below the horizon |
| Sim camera mode | `Sim3D_ActivePose` (`src/main.c`) | Free and Dynamic each own a pose — Free the player-authored `sim3d_tilt_*`/`distance`, Dynamic the dedicated `sim3d_dyncam_baseline_*`. Resolved **once** on the game thread and published through `sim.projection_*`, because two `Sim3DTuning` sites read it. A single shared pose would make Dynamic sway around wherever the last manual drag left the camera. The right-drag is gated to Free (in Dynamic it would edit a pose nothing is built from), "Reset camera" restores the active mode's pose, and a mode change snaps rather than eases |
| Sim dynamic camera | `ApplySimDynamicCamera` (`src/present.c`) + `CaptureSimDynamicCamera` (`src/main.c`) | Same split as the diorama reactive camera: the game thread owns the WRAM reads, the running averages and the edge detection; present.c owns the formula. Signal is the **angel record's `+$1A/+$1C` planar velocities**, not `PlayerVelocity` (an action-stage concept). Hit is an **HP decrease**, not the invuln flag, which lags damage by ~10 frames. Lean magnitudes are roughly half the diorama's because a near-overhead camera turns the same angle into ground-swim. Applied **before** `Scene3D_BuildViewProjection` so every stage shares one camera. Reset to neutral outside a town, and the first town frame only seeds the HP baseline so arrival is not read as a hit |
| Menu deferral | `SimPlaneIsMenu` + `SimObjectTierFilter` (`src/present.c`) | The sim menu is three layers: text (`Bg3Low/High`), box frame and fill (`Bg2High`), and icons/cursors (**fixed-tier OBJ**, which rank above `Bg2High`). All are held out of the painter-order loop and drawn after every atmospheric effect, walking the **full hardware rank** so order within the group is unchanged and only its depth moves. Deferring BG3 alone left clouds inside the panels; adding `Bg2High` without the OBJ buried the icons under its own fill. Fixed-tier OBJ are deferred **by tier, not by plane**, because they share OBJ ranks with world billboards that must stay under the shroud — hence each band drawing twice. **`Bg2Low` is excluded**: the painter order places it behind the projected ground as a background layer. Safe for BG3 **only because** the town HUD's BG3 pixels are already removed by the `sim3d.c` overlay handoff |
| Atlas overflow policy | `SimRenderAtlas_Build` + `SimRenderMetadata_CommitAtlas` | A per-object failure (bounds query, too large to fit, atlas full, raster failure) **purges that object** and the build continues; only a broken contract fails the whole build. Purged objects keep `atlas_valid` clear and are counted by the D1 census, so the condition still fails a checkpoint without costing the frame. Three places encode this — the builder loop, the foot-union pass (a purged fragment has zeroed local bounds and would drag the shared foot), and the commit validator, which required `atlas_valid` on every object (ledger §24) |
| Metadata-failure scope | `Sim3D_ResolveFeatureMask` (`src/sim/sim_render_metadata.c`) | `metadata_valid == false` clears **only** the object stages (billboards, height, shadows, soft shadows, rim). The separated composite, ground projection and world underlay come from captured plane pixels and the camera, none of which the semantic record pass supplies. Dropping the whole view instead paid for a sprite problem with a full-screen perspective change (ledger §24) |
| Deterministic evidence | `AR_SIM3D_D1_TRACE=<file>` | Per-frame JSONL carrying view, `picker_flag`, the compiled `picker_topdown`, feature masks, atlas rects, and per-object `height_class`/`classified_height`/`virtual_height`. `tools/sim3d_demo.py` validates against it and asserts the picker contract that the binary actually shipped |

Three traps worth knowing before touching the table:

- **`$A627-$A792` is not an angel signal.** Miracle effect records borrow the
  angel's pose frames; only the `$0AE4` record address and class `$0C` select
  the angel's flight plane.
- **Cursors are not one contiguous composition range.** `$D233-$D302` is the
  ROM's cursor family, but the `$D993` path/area selection square is a
  separate map-plane cursor on a class-`$09` record, adjacent to the
  `$D9E5-$DCD2` miracle cloud effects. See `docs/sim-object-catalog.md`.
- **A geometrically correct billboard shadow is the wrong shadow.** Shearing a
  camera-facing silhouette along the light and projecting it onto the ground
  collapses it to a smear, because the billboard has no depth. D4a lays the
  silhouette flat about the caster's foot (`kSimShadowFootprintDepth`) and
  shears the whole quad by the caster's height instead.

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
- The handlers are the town's **event logic**: they test and maintain the per-town
  **story-event bitmaps** at `DB:$9107+` (prereq, 4 bytes = 32 event ids per town) and
  `DB:$911F+` (fired) via the helpers `$03:F46E` (test) / `$F479` (set) / `$F487` (clear),
  which use `$03:F497` (bit compute, scratch `DB:$914F`) and the WRAM-pointer tables
  `$03:DCA2`/`$DCAE`/`$DCBA`. They also drive the spawn-list engine (see #3) and post events.
  (These were called "open lairs"/"spawned lairs" here until 2026-08-17; see the story-event
  VM chapter for the corrected model.)
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
- `bank_01_CFF2(A)` takes the packed value **`A = (list << 8) | variant`**: it stores A's low
  byte to `$033D`, `XBA`s, stores the high byte to `$033C`, then calls `AC36`. Thus `$0502`
  means list 5, variant 2 (the live miracle-lightning sequence), not list 2, variant 5.
  `bank_01_AC36(X=entry)` =
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

### 7. Structure records, the 128-structure cap, and miracle damage (bank $03, mapped 2026-07-17)

The persistent "what is built where" state behind §5's development cycle — mapped while
implementing the optional bridge-limit enhancement (`src/actraiser/actraiser_bugfixes.c`). Motivated by The Admiral's
Maximum Population Guide (GameFAQs 47431): each region tops out at **128 structures**, bridges
count against that cap while supporting the fewest people, and bridges are indestructible —
so accidental bridges permanently cost population in Fillmore/Bloodpool/Kasandora.

**The record arrays.** Every town owns a fixed array of 128 × 4-byte structure records at
`word[$03:DC74 + town*2]` → `$7F:6BE7/$6DE7/$6FE7/$71E7/$73E7/$75E7` (0x200 bytes per town,
contiguous `$6BE7-$77E6`). One record = one standing structure:

| Byte | Meaning |
|---|---|
| +0 / +1 | map-cell X / Y (0-31; the town map is 32×32 cells = 8×8 selector squares of 4×4 cells) |
| +2 | flags/type: bit7 = active/occupied, bit6 = **not-yet-contributing** (the census scores it as zero support) and a per-class visual variant selector — it is *not* a construction flag, see "Bit 6 is not a construction bit" below, bits 4-5 = subtype (house civ level; wheat-vs-corn bit `$10`; bridge orientation), bits 0-3 = **type class** |
| +3 | low nibble = pending action id (queued via `$03:9F05`: `rec[3] = (rec[3] & $70) \| action`), bits 4-6 = build-step progress |

Type classes: 0 = house, **1 = bridge**, 2 = field (corn; +`$10` = wheat), 3/4 = factory/
windmill tier, 5/6 = special/support. New-game init: `$03:AA1C` clears road+square state and
`$03:AA51+` copies each town's initial records from ROM (pointer pairs at `$03:AB6C`,
`$FF`-terminated) into the arrays.

**Allocator `$03:9D9F`.** First-free scan (bit7 clear) over the current town's 128 slots;
writes `{$7C9D, $7C9F, $7CA1|$80, 0}` (pending cell X/Y and type staged by the caller), leaves
the slot index in `$7C05` and the record pointer in X, carry clear. After 128 occupied slots:
carry set = **the** 128-structure cap. Callers: the four §5 development-mode handlers
(`$93CA/$9486/$9541/$95EE`) and the two bridge sites below. This is a hard physical limit —
the arrays are fixed 0x200-byte allocations, so "raising the cap" is not possible in place.

**Bridges.** Built only by the road-crossing checkers at `$03:9975/$99A4` (allocation JSRs at
`$9994/$99D9`): when road expansion crosses a river cell (terrain probe `$97B0`, crossing
flags `$0080/$0100` in the road-map word) they allocate type `$01` (one axis, cell X&3==?) or
`$11` (other axis) and set the corresponding road-map bit (`$0080`/`$0100`) in the
crossing square's `$7F:6800` road word so the crossing is never re-bridged. When the allocator fails, the bit stays clear and the FAQ-documented
behavior emerges: the road continues on the far side with no bridge, and a later record free
lets the crossing scanner build the bridge immediately. Northwall ORs `$08` into the visual
variant (ice bridges) at build/draw time (`$A38E/$A3DC`). Crossing/pathing state lives in the
`$6800` road map, but an exhaustive consumer census (all `$DC74` readers + a banks-00-02 scan
for the array bases, 2026-07-17) found that bridge records still feed the census, miracle
scan, construction-scene marks pass, and reconstruction renderer. The type-0/3/5
story-event searches (`$8B66` callers `$8BC5/$EF9E/$EFD3`) never request bridges.

**Population/support census `$03:C07F`.** Iterates the town's records and accumulates the
two sides of §5's growth comparison: the house-derived **population** (type-0 houses, people
per house by subtype/civ level) into `$7C05` → `$7E:021C + town*2` (+2, minus `$9F57,X` —
population in this game is *derived from standing house records*, which is why destroying
houses lowers it and why the record cap bounds it), and the **support capacity** into `$7C07`
→ `$7F:6B26 + town*2` — completed class 2 = 32 (48 with the wheat bit), class 3/4 = 72, every
other class including bridges = 32, under-construction (bit6) = 0. The FAQ's 32/48/72
supported-people numbers fall straight out of this routine.

**Miracle structure damage.** Bank-01 miracle effect actors post the hit: miracle kind →
`$7F:96E8`, aimed pixel coords → `$96EA/$96EC`; the master-loop block at `$03:820F` converts
to map cells (`>>4`) in `$90E1/$90E5` with the kind in `$90EB`. The effect driver `$01:9840`
(and the per-miracle appliers around `$01:9A07/9A2B/9A9D/9AF5`, plus the scripted event at
`$03:E7A6`) then `JSL $03:B274`: align the aimed cell to its 4×4-cell selector square (kind
≥ 4 = earthquake = the whole 32×32 map), and for every active record in the rectangle queue
action `kind+1` into `rec[+3]` (kind 0 is an internal silent record-clear), set the
`$7F:90F7` refresh flag, then run the §5 build-step scanner `$9E5A` twice to execute the
queued actions. **Lightning = kind 1 = action 2.**

**Per-type action dispatch = the immunity matrix.** The §5 scanner routes each record by
type class to an outer handler that RTS-dispatches `rec[+3]`'s action through a per-type
8-entry table (entries are pushed-address−1): house `$A017`, bridge `$A364`, class 2 `$A0D1`,
class 3 `$A1A1`, class 4 `$A23D`, class 5 `$A29C`, class 6 `$A2F5`. Action 0/1 = construction
start/step (bridge: `$A374`, staging visual step programs via class `$7D1F`/variant `$7D21` →
`$A4B8`); actions 2-6 = the damage flavors. House action 2 (`$A050`) queues teardown action 7
and `JSL $03:B4A6`; **bridge actions 2-6 all point to `$A435`, a reset-to-idle no-op — that
single table row is the entirety of bridge indestructibility.** (Wheat/factory earthquake
immunity lives in the same tables' higher action rows.)

**Bit 6 (`$40`) is not a construction bit.** The allocator only ever ORs `$80`, so a new
record never starts with it. It is set on a live record by exactly four sites, each meaning
something different: `$03:BD66` (house), `$03:B25B` and `$03:E27B` (fields), and `$03:E2BB`
— the scripted "the wind has died" event, which walks the current town's 128 records and ORs
`$40` into every class-3 (windmill) record, arming visual class 6 **variant 4**. That variant
is `$03:D74E`, a one-frame program drawing list `$DBF1`: a **finished** mill with parked
blades. The Wind miracle queues record action 6; `$03:A1F4` arms variant 2, clears the bit with
`AND #$BF` and re-queues action 1, and the blades turn again. Houses tell the same story from
the art side — variants `$00` and `$10` share both scaffold frames and differ only in the
finished metatile they land on (`$02` vs `$03`). Presentation must therefore read the frame the
step machine is drawing, never this bit; see ledger §61 for the enhanced-view bug that came of
reading it as construction.

For the record, the class-6 (windmill) frames are `$04`/`$06`/`$14` scaffold and
`$24`/`$26`/`$16` the three blade positions 30 degrees apart in the wheel's 90-degree period,
and class 8 (factory tier) is `$34` scaffold, `$36` finished — all top-left metatiles in the
structure atlas at `$7E:3100`. `SimBackgroundVoxels_Classify` matches a plot's live 2x2 tile
entries against that atlas to resolve both the construction flag and `animation_phase`. It
additionally holds every mill in a town whose records carry the bit, because `$03:E2BB` stamps
only the records that exist when the event fires and the ROM leaves a later-built mill turning
through it.

**Visual step programs.** `$A4B8` (construction table family `$03:D4D2`) and `$A4A8`
(rebuild family `$03:D4E2`) share the body at `$A4C6`. When gate `$7BE9` is clear, it adds
class `$7D1F` and variant `$7D21` indirections to resolve a step-program pointer and arms the
record's slot in the 128 × 8-byte step-machine pool at `$7F:77E7 + record_index*8`
(`$77E7-$7BE6`, ending exactly at the `$7BE7` tick variable). Both entry points are now
whole-body HLEs backed by a shared resolver/armer that the bridge sidecar also uses; focused
tests pin both ROM families, stack bytes, decimal-mode ADC edges, DB-relative writes, and
the complete CPU return contract. The §5 8-frame step walker (`$89F7` + step table
`$03:8A7E`) then draws the tile changes. The programs themselves are interpreted by `$A4F7`,
which every per-class action handler tails into: `$FF` begins a loop, `$FE` ends one, `$FD`
stops, and any other entry is `{duration, draw list}`. Full program and slot layouts are in
[rom-map](rom-map.md) and [ram-map](ram-map.md); the practical consequence is that **a
structure's animation is its step program rewriting its own cells in the BG1 tilemap**, not
tile animation — a town has exactly one animated CHR page and it is water (rendering §7). This is why records and visuals can diverge: tiles
persist in the town map even if a record is freed afterwards.

**Auxiliary per-town state mapped along the way:** `$7F:9250 + town*0x80` = 64-entry
built-square dedup list (2-byte **square** coords x,y ≤ 7, `$FFFF` empty; appender `$8EC1`,
staged pair `$9550/$9552`, scan cursors `$9554,X`); `$7F:3800 + town*0x400` = per-cell flag
map (bit0 set near road/build commit `$9623`, bit1 near `$8E48`, transient pathfinder
visited bit2 set at `$9A50`; init cleared, not saved);
`$7F:6800 + town*0x80` road-map words, one per selector square, initialised from ROM
`$03:DCFA` (bit `$40` = obstructs the build-direction selector, `$0080/$0100` =
river-crossing/bridge state); `$7F:7BF9` = current town id, `$7F:7BFB` = town id ×2 (the
DC74 index); scratch: `$7C05/$7C07` accumulators, `$7C1D` loop counter,
`$7C11/$7C13/$7C15/$7C17` record-scan rectangle, `$7C9D/$7C9F/$7CA1` pending allocation
request. **SRAM persistence (validated against real saves 2026-07-17):** road maps at
`0x0000+town*0x80`, built-square lists at `0x0300+town*0x80`, record arrays at
`0x0600+town*0x200` (ending exactly at the `0x1200` region-progress block) — see
save-format.md §3.4. Cell-flag and tile maps are regenerated on town entry, not saved.

**Host bug-fix seam (`hle_func`, recomp/bank03.cfg + src/actraiser/actraiser_bugfixes.c).** Four narrow
routines are hle-replaced with faithful C contracts plus sidecar post-passes and
`AR_BRIDGEFIX_DEBUG` observability. The v1 toggled extensions (full-table allocations reusing
a completed bridge record; lightning freeing bridge records) were **withdrawn 2026-07-17
after play-testing**, which established a critical system fact: construction events
regenerate structure state from the record table, and a vanished crossing can strand the
build-direction cursor across the river. **v2 (`fix_bridge_limit`):** completed bridges
migrate to a per-town extension area in free checksummed SRAM (save-format §3.4) via the
allocator hle (`$9D9F`); the census hle (`$C07E`) adds their 32-person support; the
construction-scene marks hle (`$9CFB`) appends their structure identities; and the scene
finish hle (`$89F0`) appends their visible metatiles after native reconstruction.

The renderer has two distinct record-presence consumers, not one. `$9CFB` writes per-cell
**structure marks** through `$9FCD/$9FE4` into `$7F:2000` (bridge `$E1/$E2` by orientation,
gated on `$919E+town`); `$9710` maps `(x,y)` to
`town*$400 + quadrant*$100 + (y&15)*$10 + (x&15)`. `$9710` is now a whole-body
HLE backed by the same `ActRaiser_CellMarkIndex` function used by the bridge sidecar. A
whole-ROM scan found 21 direct JSR sites and no external branch into its body; the wrapper
preserves its pre-quadrant `$7C05` scratch value, A/X result, final flags, decimal-mode edge,
and RTS stack effect. An exhaustive 6-town × 32 × 32 test pins the canonical map and separate
raw-word cases pin the instruction-level ABI. `$9FCD/$9FE4` are whole-body HLEs for the
one-cell/2×2 structure writers; their wrappers and the sidecar-extended `$9CFB` pass use the
same `ActRaiser_WriteTownStructureMark` primitive instead of maintaining three copies of the
map geometry. The build-direction pathfinder's five call sites
then use `$96EF` to apply two rejection rules to that index: bit `$0200` in the metatile's
top-left word at `$7E:2100 + terrain_id*8` means impassable terrain, while bit `$04` in the
cell's `$7F:3800` flag means this traversal already visited it. `$96EF` is also a whole-body
HLE; it folds the nested `$9710` call into the shared calculation and preserves the original
Z-result convention (`Z=1` means available), accumulator high byte, X/Y, flags, `$7C05`,
nested-JSR stack bytes, and outer RTS. Predicate coverage plus an instruction-level ABI suite
and a deterministic simulation replay pin the conversion; the replay's final CPU state, WRAM,
and SRAM are byte-identical to the native-body baseline. Later, `$9D4D` dispatches active records
through `$9F6B/$9F8D` and `$A4A8/$A4F7`: the shared structure-step resolver selects the
bridge rebuild program from the `$03:D4E2` family, arms the record's `$77E7+slot*8` step
entry, and executes its initial draw
through `$A591` → `$9C43`. `$A591` interprets a count plus `{dx,dy,metatile}` triples;
`$9C43` copies four tilemap words from `$7E:3100 + metatile*8` into the quadrant-paged live
town tilemap. Its sibling `$9B5A` performs the same copy from the terrain definitions at
`$7E:2100`. Both bounded leaves are whole-body HLEs backed by the same named
`ActRaiser_CopyTownMetatile` primitive that the bridge sidecar now calls directly, removing
all three production copies of the quadrant/address/write logic. The CPU wrappers retain the
ROM routines' decimal-mode address edge, A/X/Y/P results, two temporary word pushes in stack
RAM, DB-relative writes, and RTS behavior. An exhaustive 32×32 grid for both atlases plus raw
coordinate and instruction-level ABI cases pins the conversion. For the observed settled
bridge states, record actions `$11/$31` select
programs `$D5C5/$D5D1`, lists `$DC1C/$DC24`, and metatiles `$44/$45`.

`$A591` is now a whole-body HLE as well. Its two native callers and the bridge-side restamp
share `ActRaiser_ExecuteTownDrawList`, the single count + `{dx,dy,metatile}` decoder. The CPU
wrapper retains the reconstruction-step/list pointer chain, `$7C23` countdown, decimal-mode
coordinate additions, nested `$9C43` return frame and temporary stack writes, final
register/flag state, and outer RTS. Host bridge restoration supplies a named command ceiling
and therefore fails closed on a malformed list without maintaining a second parser.

A direct visual capture disproved the earlier marks-only assumption: the sidecar-only bridge
had the correct `$E2` mark but remained a solid black map cell because `$9D4D` could not see
it. The `$89F0` post-pass now decodes the same rebuild program and performs the same metatile
copy for each validated sidecar bridge. Settled bridge programs contain one timed initial
draw followed by `$FD`, so the post-pass needs no persistent step-machine entry and does not
consume a structure slot. The four hooks preserve their native register/flag/scratch
contracts; all unrelated consumers retain the authentic 128-slot view.

---

### 8. Ambient scenery actors — the "town scene" spawner (bank $03/$01/$0A, mapped 2026-08-17)

The decorative live actors attached to standing structures — farmers in fields, horses and
sheep in ranch pens, boats, dogs, burning-house flames — are **not** part of the §3 spawn-list
engine or the §6 actor-script tier. They come from a separate, fully data-driven "scene"
system whose selection key is the *structure class* under a *fixed absolute map cell*. Mapped
while explaining an empty Aitos ranch pen (see the closing note — the observed behavior is
authentic).

**The per-frame driver `$03:FCE8`** (single caller: `$03:82AD`, the town master-loop chain
`$82DB/$84B9/$C07E/$8E10/$E414/$8612/$BA24` → here):

```text
LDX $7BFB                 ; town*2
LDA $9222,X ; BEQ done    ; per-town ACTIVE SCENE INDEX — 0 = no ambient actors
LDA $9F6B,X ; BEQ done    ; per-town development gate
LDA $9222,X ; CMP $922E ; BEQ done   ; $922E is never written by any decoded code — dead compare
ASL A ; TAX ; LDA $03:FD0E,X ; TAX   ; index the scene-script record table
JSR $CA84
```

`$7F:9222 + town*2` is **session state**, written only by town story-event handlers (§ story-event
VM). It is not in SRAM and is not rebuilt on load, so a freshly loaded save has no ambient
actors until an event re-arms it. One town holds exactly **one** active scene index at a time.

**Scene-script record table `$03:FD0E`** — 25 word pointers (index 0 unused, since 0 means
"disabled") to 10-byte records `{op, x, y, class, scene}`:

| Entries | op | class | scenes | Meaning |
|---:|---:|---:|---|---|
| 1–4 | 3/1 | 3,1,4,3 | `$03`,`$04`,`$1A`,`$1B` | early-town sets (incl. scene `$1B` = ranch horses on class-3 structures) |
| 5–12 | 1 | 2 | `$1C`–`$23` | field workers on class-2 (crop) structures |
| 13–24 | 1 | 5 | `$24`–`$2F` | **pen sheep on class-5 structures** |

**Interpreter `$03:CA84` / `$03:CA93`** (`$CA84` stages `$7F:9220 = $CA79`, `$CA93` stages
`$CA7E`; both fall into `$CAA0`) dispatches record `op` 0–3 to `$CAC7`/`$CAF1`/`$CB38`/`$CB67`.
Every op stages the same four variables and then calls one of the two spawn loops:

| WRAM | Meaning |
|---|---|
| `$7F:8F6F` | structure **class filter** (low nibble of structure record `+2`) |
| `$7F:8F70` | **scene id** (index into `$03:CE5B`) |
| `$7F:8F71` / `$8F73` | scene **base offset in pixels**; from record `+2`/`+4` via `XBA; LSR; LSR` = value × 64 px = value × 4 map cells. Zero for every `$FD0E` record, so those scenes use **absolute** cells |
| `$7F:9220` | which pool allocator the spawn trampoline calls (below) |

**The two spawn loops:**

- `$03:CD39` — unfiltered: spawn every object in the scene list.
- `$03:CDB0` — **structure-filtered** (`SEP #$20; STA $8F6F` on entry, called only from
  `$03:CB19`, the op-1 handler). For each list item: compute the cell, `JSR $03:BD84`
  (exact-cell structure lookup — walks the town's 128 records, carry clear = found, record
  pointer in X→Y), `BCS` skip, then `LDA $0002,Y; AND #$0F; CMP $8F6F; BNE` skip. Only a
  matching class spawns.

Both end with the spawn trampoline
`LDA #<continuation>; PHA; LDA $9220; PHA; RTS` — see the `$9220` correction in the
story-event VM section below.

**Scene-id table `$03:CE5B`** — 52 word pointers to `$FF`-terminated byte lists; each byte
indexes **`$0A:C800`**, a 190-entry word table of offsets (relative to `$0A:C800`) to scenery
object records `{cell X, cell Y, kind, …script}`. `kind` is always **even**.

**Kind → actor.** `$01:CF0A` turns a spawned record into a visual: `record+$0F` (= the high
byte of the pending-type word `$7F:7CA1`) indexes the 9-row × 12-byte table `$01:CF2B`
**by byte**, so the row is `kind/2`; the selected byte is the variant, `ORA #$0600` packs it as
spawn-list 6, and `JSR $01:CFF2` stages `$7F:033C/$033D`.

| kind | `$01:CF2B` row | variants (`$01:A91C`) | Actor |
|---:|---:|---|---|
| 0 | `$CF3D` | 1–5, 9–11 | town people |
| 2 | `$CF49` | `$0C`/`$0D` (+`$25`/`$26`) | horse (+ people/horse metatiles) |
| 4 | `$CF55` | `$0E`/`$0F` (+`$27`/`$28`) | dog |
| 6 | `$CF61` | `$10`/`$11` | **sheep** |
| 8 | `$CF6D` | `$12`–`$15` | sailboat |
| 10 | `$CF79` | `$16` | burning-house flame |
| 12 | `$CF85` | `$17` | (`$DD3F`/`$DD45` family) |
| 14 / 16 | `$CF91` / `$CF9D` | — | unclassified |

**Pool allocators `$01:B778`–`$B7A8`.** Seven entry points, each `LDX #base; LDY #count`
falling into the shared allocator at `$B7AE`: a free slot is one whose `record+$10` has bit 15
set; the allocator seeds `+$0A/+$0C/+$0E/+$14` from `$7F:7C9D/$7C9F/$7CA1/$7CA3` and clears
`+$10/$12/$16/$18/$20/$22/$24`.

| Entry | Base | Slots | Notes |
|---|---:|---:|---|
| `$01:B778` | `$0A00` | 6 | world effects / miracles |
| `$01:B780` | `$0CF8` | 6 | |
| `$01:B788` | `$0DDC` | 8 | **superset** of the next pool |
| `$01:B790` | `$0E02` | 7 | scenery/townspeople — the `$CA84` default |
| `$01:B798` | `$0F0C` | 8 | the `$CA93` default |
| `$01:B7A0` / `$B7A8` | `$103C` / `$1062` | 1 / 1 | |

`$B788` and `$B790` deliberately overlap (`$0DDC` + 8 × `$26` = `$0F0C`), so a full people
sweep can starve the scenery spawner: with all seven `$0E02` slots taken, a matching pen
silently spawns nothing. Observed in `runs/20260817-184251/snapshots/snap_01_gf25962`.

**⚠️ Authentic behavior — an empty pen is usually correct.** Pens (class-5 structures) are
addressed by **fixed absolute cell**, by both the story events and the ambient scenes:

- Aitos event 7 (`$03:EFD3`, the "our ranch has some horses" beat) spawns via script record
  `$03:E628` → base (3,5) × 64 px → **cell (12,20)**, scene 6 = one horse.
- Aitos event 8 (`$03:F00F`, Sheep's Fleece) spawns via `$03:E63C` → base (4,4) → **cells
  (16,16)/(17,17)**, scene 5 = two sheep — and sets `$7F:9228 = $15`.
- `$9228 = $15` selects `$FD0E` entry 21 → scene `$2C`, which checks exactly
  (12,16) (13,17) (16,16) (17,17) (12,20) (13,21).

The full pen lattice is {4,5,8,9,12,13,16,17,20,21,24,25}², partitioned into the twelve
scenes `$24`–`$2F` (3 pens × 2 sheep each), but a town only ever has ONE active index — so a
pen built outside the active group has no animals. Confirmed against a reference emulator
(2026-08-17): the Aitos pen is empty for most of a normal playthrough. **Do not chase this as
a dispatch-miss bug.** A registration audit of the whole chain came back clean: sim record-class
state handlers, the `$01:CD12`/`$01:CD6F` actor tables, all 6 × 32 town event handlers, the
7 × 8 per-class action tables (covered by `rts_dispatch 9EF3`), and every `$9220` trampoline
target are all emitted.

## Sim-mode town-map GRAPHICS pipeline (VRAM seam, mapped 2026-07-05)

The town map reaches the PPU through two WRAM→VRAM DMAs — **the seam an HD/replacement tile
backend hooks**:

| What | WRAM source | VRAM dest | DMA'd by | Notes |
|---|---|---|---|---|
| BG tilemap (32×32 tile-index grid) | `$7F:1000` | VRAM `$6800` | `bank_02_AEBB` | full-map upload, size $800 words; the map's tile *layout* |
| Animated tile GRAPHICS (bitplanes) | `$7F:B800 + phase*$E1` | VRAM `$0000` (or action-configured `$1000`) | `bank_02_AF30`→`AF3D` | full-word DMA of `$E1` bytes; the tile *pixels* for the selected animation phase |

The `$7F:1000` tilemap buffer is built from town map data; `$7F:B800-$BFFF` holds one
contiguous 4 KiB snapshot of already-loaded tile character data (written by
`bank_02_BAF5`), subdivided by the active profile's stride/count. A modern backend can
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
readback of exactly `$1000` bytes beginning at character-VRAM word `$DA` (`$0000/$1000`)
through `$2139` into `$7F:B800-$BFFF`, later sliced and DMA'd back. The same routine and
contract serve action rooms; raw cadence bit 7 marks a continuation that retains the prior
snapshot. Offline cumulative room reconstruction makes every continuation self-contained.

For a sim town (`$18=0`, `$19!=0,9`), NMI schedules animation with `$02:BC56` and consumes its
`$D7-$DC` descriptor through the generic full-word `$02:AF30` uploader; `$02:AF86` is only the
separate `$19=0 or 9` ROM-bank-$0A/high-byte upload branch. A recomp-only timing race was captured
at `runs/20260717-223857/snapshots/snap_08_gf1567`: scene setup armed four `$100`-byte frames while
the main coroutine remained in `$02:B63B` waiting for the SPC `$F0` acknowledgement. NMI then
uploaded the still-empty `$7F:B800` phase over freshly loaded VRAM `$0000`; `$BAF5` captured that
blank phase, making the town water flash black every fourth frame. The `$02:BC56` HLE now defers
invisible animation ticks while INIDISP force-blank is active, so `$BAF5` completes the single
4 KiB capture before animation can overwrite the loader's output. The reference emulator's captured
phase 0 matches ROM file offset `$060000`; it is not an intentional blank frame.

World navigation is a separate animation contract. `$02:AF86` copies one
64-byte ROM frame from `$0A:B000/$B040/$B080/$B0C0` into the high bytes of
both Mode-7 tile `$00` and tile `$AA`, advancing every eight game frames.
`SimWorldMap` mirrors those two tile replacements from ROM and never reads live
VRAM: `$09` consumes the source retained at `$7E:00D7`, while town outer
underlays continue the same cycle from `$88` because town `$D7` belongs to the
WRAM-buffered animation above. This keeps both the full-world navigation plane
and the half-resolution terrain outside a 3D town animated.

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
as trap stubs (listed by `go -C snesrecomp-go run ./cmd/v2regen stub-census --gen-dir ../src/gen`).
No event exercised them yet in play;
whichever event first walks into one will `[dispatch-oob]` loudly. Closing this needs the vector
WRITERS traced once (who stores to `$6E20`/`$7920`), then a cfg/indirect-vector authorization —
it cannot be closed statically.

**Resolved thread (2026-07-07):** the one-of-N cutscene actor sprites (DEBUG.md §7.17 —
lair-seal attackers, Bloodpool lightning pair) were fixed by registering the **`$9220`
trampoline family**, NOT the `$9FCD` dispatcher family (which remains statically
censused but symptom-free). A trampoline does
`LDA #<continuation-1>; PHA; LDA $9220; PHA; RTS` and a caller stores the callee-1 into
`$9220`. CE57 was found by tools/resolve_miss.py's first run, closing bug-ledger #13's
untraced `$CE56`.

**Corrected 2026-08-17 — `$9220` holds the CALLEE, not a resume address.** The two halves
were previously conflated. `$7F:9220` is a *late-bound subroutine pointer*: it selects which
world-record **pool allocator** the scenery/cutscene spawner calls, and only three values are
ever stored (`$03:CA8E` → `$CA79`, `$03:CA9D` → `$CA7E`, `$03:B617` → `$B67C`):

| `$9220` value | Callee | Body |
|---|---|---|
| `$CA79` | `$03:CA7A` | `JSL $01:B790` (7-slot `$0E02` pool); `RTS` |
| `$CA7E` | `$03:CA7F` | `JSL $01:B798` (8-slot `$0F0C` pool); `RTS` |
| `$B67C` | `$03:B67D` | town-people spawn variant |

`$03:CDAD` and `$03:CE57` are the *call-site continuations* pushed by the two spawn loops
(`$03:CDA4` pushes `$CDAC`, `$03:CE4E` pushes `$CE56`) — they are return points, not `$9220`
members. All six addresses still need registering; the distinction matters when reading
`resolve_miss` output, because a miss at `$CB1C`/`$CB85`/`$CAEE` is the spawn loop *returning*
(benign) and proves the loop ran, whereas a miss at a `$9220` value would mean it never
allocated. See the town architecture chapter §8 for the full spawner.

**Event selection state machine `$03:DFFB` (mapped 2026-08-17).** The town event handler
tables (`$03:E66E` → 6 towns × 32 entries, dispatcher `$03:E1D2`) are indexed by a per-town
32-bit event id chosen here:

```text
$920E & $80 ?  -> yes: return ($920E & $7F)   ; explicit pending-event latch
               -> no : scan $9202 = 0..$1F for the first id with
                       prereq($DCA2) set AND fired($DCAE) clear;
                       mark it in $DCBA, return it; $FF if none
$E045: STA $920E                              ; latched index (bit 7 stripped)
```

The three "bitmask" pointer tables are **story-event bitmaps, not lair state** (see the
correction in the symbol map): `$03:DCA2`/`$DCAE`/`$DCBA` → per-town 4-byte arrays at
`$7F:9107`/`$911F`/`$9137` (+ town × 4) = 32 event ids per town. Bit order is **MSB-first**:
id `k` → byte `k >> 3`, mask `$80 >> (k & 7)`, from the mask table `$03:F4D7`
(`80 40 20 10 08 04 02 01`) via `$03:F497` (scratch `$7F:914F`).

| Table | WRAM | Meaning | Persisted |
|---|---|---|---|
| `$03:DCA2` | `$7F:9107` | **prereq/enabled** — the event may be selected | SRAM `0x120E` |
| `$03:DCAE` | `$7F:911F` | **fired** — already run; never selected again | SRAM `0x1226` |
| `$03:DCBA` | `$7F:9137` | **dispatched this session** (set by `$03:E02B` and `$03:E0B0`) | no |

Dialogue is posted by `$03:E09B`: `JSL $01:B1C7`, `LDY #$8217; JSL $01:9314`, then
`LDX #$0008; STX $1A; LDA #$01; STA $00:033E` (the COP event request). Handlers that need a
follow-up beat self-latch with `LDA #$80|id; STA $7F:920E` (e.g. `$03:EFE5` = `$87`,
`$03:FAE1` = `$88`).

Several town events are **gated on a structure existing**: `$03:8B66` (callers `$8BC5`,
`$03:EF9E` type 3, `$03:EFD3` type 5) scans the town's 128 structure records for the first
active record of a given class, carry clear = found. `$03:EFD3` (`LDA #$0005; JSR $8B66;
BCS $EFFA`) therefore skips the whole horse cutscene — dialogue, spawn and
`$9228 = 4` — and just marks itself fired if no pen exists when the event comes up.

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

> **Action-object field widths are not uniform — read `+18` and `+29` as BYTES.** `+16..+18`
> is a 24-bit animation pointer (addr16 + bank8) and `+19` is a separate field, so a 16-bit
> read of `+18` returns `bank | next<<8`. A host observer that got this wrong rejected every
> spell it was written to decorate and drew nothing at all, silently, for as long as the
> feature existed (bug-ledger.md §32). `$00:95F0` copies spawn-record bytes into `$18`/`$28`
> byte-wise and is the corroborating reference. When adding a reader, grep for existing
> consumers of the field before choosing a width.
>
> **Widescreen reveals ROM spawn-hiding.** Magical Stardust's right-edge launch births a star
> at screen x 256 — one pixel outside the authentic window — and as low as the player's own
> screen line. With margin objects enabled at 446 wide that birth is on screen, which reads as
> a star spawning in the ground. Authentic, accepted, documented in bug-ledger.md §33. Expect
> other actors parked "just offscreen" to surface the same way.

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
6. **Cast acceptance**: `DEC $21`; `$08D0 |= $0010`; `INC $F9`; `STZ $F4`.
7. **Controller and lifetime**: `$00:9E89` creates controller slot `$0860`, copies `$02AC` to
   controller `+38`, stores the player backlink, and increments the player's cast reference.
   `$00:9F13` dispatches IDs 1-4 to `$9F25/$9F71/$9FBB/$9FFA`. `$00:A035` waits until every
   cohort slot `$06A0-$0820` is free; `$A054` clears the player reference and frees the
   controller. The player remains input-locked at `$9EAB` until that reference clears.
8. **Per-instance presentation**: the four spells are not one uniform projectile shape. Fire is
   four mirrored sweeping objects; Stardust is four staggered, four-launch actors; Aura is four
   moving mirrored orbs; Light is one centre flare plus two full-height beam columns. Exact
   slots, timings, animation states, composition geometry, miracle lifecycles, and enemy attack
   reuse are mapped in [effects-hook-investigation.md](effects-hook-investigation.md).

---

## Frame / timing  (mostly already HLE'd — the model is understood)

| Seam | Routine / address | Hardware | Intent | Notes | Status |
|---|---|---|---|---|---|
| VBlank wait | `$00:8418`, `$02:A85E` (HLE → `ActRaiser_WaitForVblank`); inline 3-read spins (`$01:9284` et al) | RDNMI `$4210` | "wait one frame" | Three-tier model (`snes.c`): HLE'd routines yield; the 7 statically-whitelisted inline spin blocks (`kSpinBlocks`, from `find_yield_points.py`) yield once per read in the coroutine; in NON-yieldable contexts (NMI/IRQ — e.g. the mode-`$85` story-event wait chain `$01:9270→8C43→9284`) whitelisted spins FAST-EXIT bit7=1, unpaced (a hang there is otherwise unbreakable — bug-ledger §7.16); `[4210-wedge]` tripwire names the refusing gate if a spin ever sticks 4096 reads | 🟢 |
| NMI handler | `$8520` (`NmiHandler`) | NMI | "per-frame vblank service" | game frame `$0088` bumped here | 🟢 |
| Frame coroutine | `RunOneFrameOfGame` (`actraiser_rtl.c`) | — | host frame ↔ game frame mapping | Normally resumes the coroutine to its vblank wait and then runs NMI. The action-load pacing seam starts at the exact `$00843E` force-blank write, before the collapsed loader's APU polls, and can consume up to 315 host frames with display/audio advancing while NMI is disabled and `$0088` remains fixed. While a live consumer drains audio, redundant main-CPU touch catch-up is suppressed until the matching `$F0`; no-consumer/headless runs retain accelerated handshake progress. That oracle value is the authentic upper bound; the exact enhanced one-shot latched at the transition may release it early only after natural completion. | 🟢 |

---

## Frame-rate decoupling — high-refresh presentation WITHOUT changing pacing

**The hard constraint first.** ActRaiser's entire notion of time is the **60 Hz (NTSC) logic tick**:
every timer, event trigger, physics step, animation-script advance, and the `$0088` game-frame
counter is keyed to one tick == one `NmiHandler`. A normal `RunOneFrameOfGame` call supplies that
tick. The intentional exception is a hardware-faithful load interval with `$4200` NMI disabled:
the host still advances display/audio frames while `RunOneFrameOfGame` consumes its calibrated
hold without running logic or changing `$0088`. You therefore **cannot speed up the logic** to get
smoothness — that *is* the pacing. The only correct way to a higher refresh rate is the classic
**fixed-timestep logic + independently paced presentation** split: keep ticking logic at exactly
60 Hz and present retained frames at the selected host cadence. Interpolation is a separate,
presentation-only refinement that can make those extra frames show intermediate motion; disabling
it must repeat the last completed tick without collapsing presentation back to the logic rate.

**The tick boundary (the seam you must preserve).** Outside an explicit NMI-disabled load hold,
`RunOneFrameOfGame` supplies one atomic 60 Hz logic tick: the host resumes the game coroutine,
which runs until its next vblank-wait, then NMI services the frame. The `AR_TRACE` **`frame`**
channel marks the edges (`vblank` = host frame boundary, `nmi` = logic tick serviced); a run of
`vblank` markers without `nmi` is expected only for the calibrated action load. Use the pairing to
*verify the tick cadence is clean* before building on it (a mode that yields N times per tick —
the 1/N-speed pacing-bug class, DEBUG.md §7.12/§7.13 — would break a naïve accumulator; those must
be fixed first).

**Implemented presentation architecture.** The earlier OAM/BG-register strategy was removed: it
could not cover arbitrary raster changes and depended on reconstructing SNES state that the current
HLE does not own. `DioramaFrameGeneration_Capture` instead retains consecutive completed ARGB plane
images, estimates bidirectional motion once per 60 Hz pair, and `Prepare` warps the nearer immutable
endpoint into a renderer-target texture for each host sub-tick phase. Drawing the farther endpoint
again is intentionally forbidden: on transparent sprite planes it retained old pixels and looked
like latched input. Coherent BG/residual planes use
one robust global vector (one generated quad); sparse OBJ priority planes use a 16-pixel block field.
This works after PPU rasterisation, so sprites, scrolling, HDMA detail, palette changes, and other
pixel filling share one contract without mutating `g_ppu` or rerunning the rasteriser.
The raw upload mirror propagates a per-plane changed mask into capture: unchanged planes reuse the
authoritative raw endpoint and perform no private copy, upload, analysis, or synthesis. Capture
regions also match upload regions exactly—OBJ priority planes retain their resolve apron, while
BG/backdrop planes exclude the known-zero apron columns. Source pitches are expressed in pixels at
the capture boundary and converted to bytes exactly once inside the row copier; treating that value
as bytes interleaves quarter-rows and corrupts both analysis and generated textures.

**Fail-closed boundaries.** Generation requires matching dimensions, BG mode, map/section, additive
plane mode, both endpoint planes, a normal non-turbo tick span, no forced blank, mutually consistent
forward/backward motion with a meaningful improvement over a stationary image, and a sub-50 ms
capture interval. Missing/failed planes keep their exact current uploaded texture. Disabling the
existing **Frame interpolation** setting performs no analysis or synthesis.

**Where the hooks live.** The implemented pipeline is:
1. **Present-rate decouple (implemented):** drive an accumulator so `RunOneFrameOfGame` fires at a
   fixed 60 Hz while `SDL_RenderPresent` follows Vsync, Unlimited, Limit, or Uncapped independently.
   With interpolation off, retained ticks are repeated exactly. `AR_PERF` and the
   `[present-cadence]` diagnostic verify that tick presents remain fixed while re-presents follow
   host cadence. Vsync has no software sleep in front of it: the renderer/swapchain is its clock.
   Limit alone contributes presentation-delay headroom to the emulation accumulator. Uncapped is
   the display-relative policy (2x the cached nominal rate, or 2x native as an unknown/VRR
   fallback), while Unlimited has no host deadline. Nominal refresh and actual-Vsync status live
   only in host presentation state. Refresh is cached by session-stable `SDL_DisplayID`, queried at
   boot and on display-mode events, and a failed/unspecified sample never erases a prior valid
   value; moving the window selects the destination monitor's cache without periodic polling.
   The explicit **Test 30 -> 60 Hz** diagnostic is the sole exception: only while interpolated
   Diorama is active, it doubles the source interval for observable slow-motion midpoint testing;
   presentation cadence remains independent.
2. **Captured-plane frame generation (implemented for 3D action/Diorama):** capture/analyse once in
   `PresentUpload`, synthesize at each retained present, and feed the resolved textures through the
   ordinary painter/depth/effect compositor.
3. **Future scope:** add equivalent endpoint ownership for render modes that do not use Diorama
   planes. Mode 7 and flat menus currently repeat their completed tick at the selected host cadence.

The whole scheme is **presentation-only**: `$0088`, timers, and logic never see the extra frames.

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
| `$03` Kasandora | `0301` | `0303` | `0302` is a valid natural-transition room but lacks required setup when used as a standalone warp |
| `$04` Aitos | `0401` | `0404` | |
| `$05` Marahna | `0501` | `0504` | |
| `$06` Northwall | `0601` | `0605` | `0608` selects the Act 2 boss map, but a direct non-action warp has invalid patterned CHR and self-exits; it is not a visual baseline |
| `$07` Death Heim | `0701` | — | boss-rush hub; verified end-to-end through every rematch and the final boss (2026-07-14) |

**Fidelity limit (2026-07-12):** the current test workflow reaches Fillmore
act 1 before pressing F6, producing an action→action request. Only the three
destination/request bytes are staged; this transition shape has not been
observed in natural progression and may inherit action timing/object state.
The warp now logs its source `$18/$19` and warns. It remains useful for visual
coverage, but timing anomalies require an authentic or pillarboxed A/B repro.
This limitation applies to the forced-warp workflow, not the natural game flow.
Death Heim was later verified end-to-end through all six rematches, the final
boss, and the return transition on 2026-07-14.

| Addr | Meaning |
|---|---|
| `$1D` | player HP |
| `$E6`/`$E7` | action-stage timer (BCD) |
| `$0088`/`$0089` | game-frame counter (16-bit) |
| `$06A0` +stride `$40` | object table (≥64 slots; fields in `DEBUG.md` §11) |
| `$08A0` | player object (slot 8) |
| `$7D1B` | saved stack ptr (act→sim transition stack relocation) |
| `$0014`-`$0017` | **SHARED DP SCRATCH — not a fixed-meaning anchor.** Used as a 16-bit ADD/XOR
checksum accumulator by `$00:84F3` (save-data validity check) AND as a message-type-ID parameter
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
| Player max HP / bar size | `$1E` | bigger/smaller health bar | 🅥/🅒 | **FOUND 2026-08-02.** `$1E` = max HP, `$1D` = current; stage entry does `LDA $1E; STA $1D` (`$00:83CF`). Authorities: new game `$02:BE5F` sets `$1E = 8`; **level-up `$03:B3DF` does `LDA $1E; CMP #$18; BCS skip; INC A; STA $1E` + mirrors into Angel max HP `$0287`** — so max HP is `8 + (level-1)`, hard-capped at 24. Professional mode starts at 24 (`$02:AB20`). Persisted to SRAM `$70:1246` (`$03:A7F3`), restored at `$03:A9FC`. |
| Invincibility frames (i-frames) | i-frame timer `$08C6` (+$26); **invuln flag = `$08D0` bit `0x2000`** (the gate) | **no-knockback / invuln** (speedrun "ignore hits") | 🅥 | **WIRED** — `AR_NO_KNOCKBACK=1` pins timer `$08C6`=0xFF AND sets flag `$08D0\|=0x2000` each frame -> invuln from frame ONE. (Hit-check gates on the FLAG; the game sets it on a hit and clears it when the timer hits 0 — so pin timer + set flag = permanent, no first-hit needed. `=26` alone only worked after one hit.) On hit: handler -> `$9C64` (hurt), knockback into `$08A6/$08A8`. **Confirmed 2026-07-12:** this pinned authentic invulnerability state also suppresses water drag; disable it for terrain/movement-physics validation and timing-sensitive recordings. |
| Player lives | `$1C` (BCD), persistent copy `$02AB` | infinite / set N lives | 🅥 | **FOUND 2026-08-02.** `$00:8850` = award-a-life (`$1C` BCD `+1`, capped `$99`; the 1UP item's effect); `$00:8861` = the paired decrement family. Act entry loads `$1C` from `$02AB` (`$02:84D7`); professional mode forces `$1C = 4` (`$02:AB13`). |
| Player sword damage (dealt) | player object `$08A0` field `+$2A` (`$08CA`), written by `$00:9DC8` | one-hit kills, weak sword | 🅒/🅥 | **FOUND 2026-08-02.** All melee/contact damage is one field: `$00:8AF9` does `victim.$2C -= attacker.$2A`. The player's `$2A` is (re)set every swing by `$00:9DC8`: **1 normally, 2 while the sword power-up `$E4` is nonzero**. Spawned magic/projectile objects carry their own `$2A` (e.g. `$00:9D00` sets 2). Pin `$08CA` for a one-hit-kill cheat. |
| Player sword length / reach | attack-frame composition record (`obj $20` +0..+3 → `$0A/$0C/$0E/$10`) | double reach | 🅒 | **FOUND 2026-08-02.** Hitboxes are **per animation frame**, not constants: `$00:8E2F` loads the current frame's composition and writes left/right extents to `$0A`/`$0E` and up/down to `$0C`/`$10` (mirrored when the object faces left). `$00:8A3C`'s AABB test reads only those four fields, so widening reach = scaling `$08AA/$08AE` after `$8E2F` runs, or editing the composition record in the per-act `$7E:4000` blob. |
| Player fly / moonjump | Y-**position** = `$08A4` (+$04) | moonjump / fly | 🅥 | **WIRED** — `AR_MOONJUMP=1`, speed from `AR_MOONJUMP_SPEED` (default 6 px/frame). Moves Y-pos up while the game's normal jump button (SNES B) is held (`ActRaiser_ApplyCheats`); no separate cheat binding. NOTE: uses Y-pos, NOT Y-vel `$08A8` — `$08A8` is "Y-velocity" only in the AIR state (polymorphic field); writing it while grounded did nothing. |
| Boss HP / health bar | boss object slot `+$2C`; initial value = spawn record byte `+8` | set boss HP, instant-kill | 🅥/🅒 | **FOUND 2026-08-02.** Same field as every other object. Every stock boss record ships `HP = 24, ATK = 1` and flag `$4000` (which routes death to `$00:A54A` instead of `$00:8892`). Several boss handlers **overwrite** `$2C` at runtime for later phases (`$00:F912` sets `$8032`; `$00:FC96` sets 2) — see the content-seams section: editing only the record does not cover those. |
| Enemy HP (general) | object slot `+$2C`; initial value = spawn record byte `+8` | global HP scale, one-hit kills | 🅥/🅒 | **FOUND 2026-08-02.** `$00:8AF9`: `victim.$2C -= attacker.$2A`; `≤ 0` → death (`$00:8892`, or `$00:A54A` when flag `$4000`), `> 0` → SFX `$02` + hit-flash `$26 = 8`. Initial HP comes from the per-region object record (`tools/act_content.py --tables`). Professional mode (`$0349`) already doubles `ATK`/`HP` at `$00:9679`/`$00:968C` when they are exactly 1 — that is the ready-made global-difficulty hook. |
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
> | +$12 | `$08B2` | handler ptr. Entry lifecycle is `$97A6` arrival approach → `$97C9` first transform → `$97E4` final transform → `$9832` first input-reading ground-control tick; ordinary states include `$98D9`/`$993F` jump, `$9884` walk, `$9A07`, and `$9C64` **hurt** |
> | +$1C | `$08BC` | **Crest** walking-cycle phase; increments while beginning movement (TAS terminology) |
> | +$24 | `$08C4` | **Boost** walking-speed countdown; cycles with Crest and can temporarily produce 3 px/frame movement |
> | +$26 | `$08C6` | **i-frame timer** (set 0x20 on hit, counts down) |
> | +$2A | `$08CA` | **ATK — damage this object deals** (`$00:8AF9`/`$00:8A24`). Player: 1, or 2 while `$E4` (sword power-up) is set |
> | +$2C | `$08CC` | **HP** for non-player objects. (The *player's* HP is DP `$1D`, not this field — the player is damaged by `$00:8A21`, a separate path from the object-vs-object one) |
> | +$2E | `$08CE` | **score awarded on death** (low byte → `$00:873C`, BCD add into `$1F`) |
> | +$30 | `$08D0` | flags — bit `0x2000` = invuln (set during i-frames) |
>
> **The HP field offset is now found: `+$2C`, with `+$2A` = ATK and `+$2E` = death score** — one
> shared layout for player, enemies and bosses. See "Content / randomizer seams" below for the ROM
> tables that seed them. Keep promoting these into `ram-map.md`.

---

## Content / randomizer seams (mapped 2026-08-02)

> **Provenance note.** Unlike the rest of this file, this section came from a *directed* static
> investigation (a randomizer feasibility question), not from chasing a bug. Everything below is
> ROM-static evidence — disassembly plus table decodes — cross-checked against one live WRAM
> snapshot (`saves/dump_wram.bin`, `$18=01 $19=01`). Nothing here has been exercised by writing to
> the tables and playing, so treat the *shapes* as established and the *behavioural consequences of
> editing them* as untested.

`tools/act_content.py` prints every table in this section from the stock ROM
(`--tables`, `--levels`, `--census`, `--lairs`), so a proposed edit can be diffed against the
original.

### The one fact that ties it all together

Every action-mode combat interaction is three bytes of one object record. In the `$06A0` table
(stride `$40`, 80 slots):

| Field | Meaning | Seeded from |
|---|---|---|
| `+$2A` | **ATK** — damage this object deals to whatever it hits | spawn record byte `+7` |
| `+$2C` | **HP** — remaining hit points | spawn record byte `+8` |
| `+$2E` | **score** awarded when this object dies | spawn record byte `+9` |

- **Object vs object** — `$00:8A3C` is the combat loop. Outer `Y` walks all 80 slots looking for
  `$30 & $0001` (an "attacker" — the player's swung sword, a projectile, a magic effect); inner `X`
  walks all 80 looking for anything whose AABB overlaps. On a hit, `$00:8AF9`:
  `victim.$2C = victim.$2C - attacker.$2A`. Result `> 0` → SFX `$02` and hit-flash `$26 = 8`;
  result `≤ 0` → `STZ $2C` and the death branch.
- **Object vs player** — a separate path (`$00:89B4`…`$00:8A2F`): `$1D = $1D - toucher.$2A`,
  clamped at 0. Same `$2A`, different victim field (the player's HP is DP `$1D`, not `+$2C`).
- **Death** — `$00:8892`: award `$2E` into the BCD score (`$00:873C`), install death-animation
  handler `$12 = $A382`, set `$1A = $FF`, `$30 |= $0038`. If `$30 & $1000`, repeat on the *next*
  slot (that is how multi-slot bosses die as a unit). Objects with flag `$4000` instead route to
  the boss-death handler `$00:A54A`.
- **AABB geometry** is `$0A/$0C/$0E/$10` (left/up/right/down extents), and those come from the
  **current animation frame's composition record**, refreshed every time `$00:8E2F` advances the
  animation. Hitboxes are therefore per-frame data, not per-object constants.

### 1. Enemy and boss stats — the per-region object-type tables

Region (`$18`) → the 8-pointer list at `$00:95DD` → that region's object-type table → a **12-byte
spawn record** at `B`, with the object's primary handler at `B+$0C`. The spawn dispatcher
`$00:9557`/`$00:95F0` copies:

| Record | → object field | Meaning |
|---|---|---|
| `+0` word | `$16` | animation/composition table base (see §5) |
| `+2` byte | `$18` lo | data bank of that table — `$7E`/`$7F` for act enemies, `$06` for player/common |
| `+3` byte | `$28` hi | spawn sub-parameter |
| `+4` word | `$30` | flags — `$0001` attacker, `$0200` pickup item, `$4000` boss, `$8000`/`$0030`/… behavioural |
| `+6` byte | `$1A` | initial animation index |
| **`+7` byte** | **`$2A`** | **ATK** |
| **`+8` byte** | **`$2C`** | **HP** |
| **`+9` byte** | **`$2E`** | **score on death** |
| `+A` word | `$14` | secondary/polymorphic field. **`$A3E1` here = "this enemy respawns"** |

Table bases, indexed by `$18`: `$96AF $A8F6 $B449 $C11E $CD9B $D928 $E722 $F39A`. A table value is
only decoded as a record when the spawning object's `$38 != $FF`; with `$38 == $FF` (`$00:9590`)
the value is installed as `$12` directly and **no stats are copied at all**.

Stock shape (full dump: `tools/act_content.py --tables`):

- Ordinary enemies are `ATK 1, HP 1-5`, score `$10`-`$40`. A few outliers: Fillmore `$13` = `ATK 2
  HP 6`, Kasandora `$11` = `ATK 1 HP 5`, Northwall `$19` = `ATK 2 HP 5`, Aitos `$13` = `ATK 1 HP 10`.
- **Every stock boss is `ATK 1, HP 24`** with flag `$4000` and animation base `$5000` (the boss
  blob, §5). Each kingdom has exactly two: score `$50` = the act-1 boss, score `$80` = the act-2
  boss. Death Heim's table holds the six rush bosses (all score `$80`, all with `$14 = $FE89`, the
  teleport-out sequencer) plus the final boss `$F80F` (score `$50`).
- `$14 = $A3E1` marks the respawning enemies. `$00:A382`'s tail is the mechanism: when the death
  animation ends and `$14 != 0`, it re-runs `$00:95ED` against the saved record base `$32`, sets
  `$26 = $0258` (600 frames), and installs `$12 = $14`. `$A3E1` then counts `$26` down **only while
  the object is offscreen** (`$30 & $0400`) and restarts the handler at `$32 + $0C`. Clearing `$14`
  makes an enemy type non-respawning; changing `$0258` retunes the delay globally.

**Caveat that matters for a randomizer:** the record byte is only the *initial* HP. These handlers
overwrite `$2C` at runtime and will ignore a table edit:

| Site | Writes HP | Context |
|---|---|---|
| `$00:F912` | `$8032` | Death Heim (bit 15 set — effectively unkillable by damage) |
| `$00:FC96` | `2` | Death Heim |
| `$00:C2C5` `$00:C932` `$00:D35E` `$00:D43B` `$00:DA4D` `$00:E026` `$00:EA7B` `$00:F045` `$00:F15D` `$00:FCE2` | `1` | per-handler sub-object / phase spawns |
| `$00:AD9E` | reads `$2C` → child's `$38` | Fillmore boss passes its own HP to a spawned part |

Runtime ATK overrides are the same story: `$00:9D03` = 2, `$00:EEC8` = 7, `$00:F80B` = 2,
`$00:FD31` = 3, `$00:EF1C` = `DEC $2A`.

**There is already a global difficulty modifier in the ROM: `$0349` = Professional Mode.**
`$02:AB05` starts it (`$0349 = 1`, lives 4, max HP 24, magic 0, score 0, stage `$1A/$1B = $01/$01`);
`$00:8781` advances it at act clear by stepping the 14-entry stage-order table at `$02:9013`
(the twelve acts in order, then `$18=$07` Death Heim, then `$18=$08` the ending). Its two content
effects are exactly the hooks a difficulty randomizer wants:

- `$00:9679` / `$00:968C`: if `$2A == 1` → 2, if `$2C == 1` → 2 (skipped when `$30 & $0201`, i.e.
  for attackers and pickups). Note it only promotes the value **1**, so it is a "weak enemies get
  twice as tough" rule, not a multiplier.
- `$00:962B`-`$00:9645`: rewrites statue contents — item `$00` (magic) → `$02` (screen clear),
  item `$03` (sword power-up) → `$01` (1UP).

### 2. Statues (item containers) — object type `$80`

The "gargoyle statue" is **object type `$80`**, and it is a single global type, not a per-region
one: `$00:9557` tests bit 7 of the requested type first (`BIT #$0080`) and forces region table
`$96AF` when set. So types `$80`-`$83` are the shared/common objects (`$80` statue, `$83` player)
and every region reaches the same records.

Record `$00:96B7`: animation base `$06:A800` anim `$08`, flags **`$0210`**, **ATK 0, HP 1**, score 0,
no respawn, handler `$00:96C3`. Rendering the composition confirms the object visually: a 16×40
two-column figure on a pedestal with arms raised holding a 2×2 orb (the orb is the palette-2 part
that shatters).

Life cycle:

1. **Intact** — `$30 & $0010` is set, which the player-contact scan (`$00:89C9`, `BIT #$0499`)
   rejects, so you cannot walk into a statue and collect it.
2. **Broken** — one sword hit takes `$2C` 1 → 0. Because `$30 & $0200` (pickup) is set, `$00:8B14`
   does **not** kill the object; it sets `$30 |= $0040` instead.
3. **Reveal** — `$00:96C3` sees `$0040`, allocates a second slot via `$00:8538` (which
   `$00:8581`-copies the whole 64-byte record, carrying `$38` along), DMAs the item graphic from
   ROM **`$06:A000 + itemId*$80`** (one 16×16 sprite each) to the reserved OBJ VRAM window `$2D80`,
   then hands the original slot `$30 = $0030` (shatter puff, anim `$09`) and the new slot
   `$30 = $0220` (collectible, anim `$0A`). Both get handler `$00:972A`, which animates once and
   frees the slot — that is the "grab it before it fades" timer.
4. **Collect** — the player-contact path `$00:8A00` sees `$30 & $0200`, calls `$00:879D` with the
   object's `$38`, and frees the slot.

### 3. Statue drops — the item-effect table

**`$00:879D` is the item dispatcher**, item id in `A`, sourced from the object's `$38`:

| id | Sprite (`$06:A000 + id*$80`) | Effect | Code |
|---|---|---|---|
| `$00` | scroll | **magic +1** — `INC $21`, capped `$FF` | `$00:87B9` → `$00:8875` |
| `$01` | heart / 1UP | **extra life** — `$1C` BCD `+1`, capped `$99` | `$00:87C3` → `$00:8850` |
| `$02` | winged figure | **screen clear** — walks the object table from the player slot up and `$00:8892`s every eligible object (objects with `$30 & $0200`, i.e. other statues, get flagged `$0040` = broken instead) | `$00:87CD` → `$00:87D1` |
| `$03` | sword | **sword power-up** — `$E4 = $80`; `$00:9DC8` then sets the player's ATK to 2 instead of 1. Never decremented: it lasts until the act ends (`$02:84E5`/`$02:BD43`/`$00:8784` clear it) | `$00:880B` |
| `$04` | urn | **heal ¼ max** — `$E3 = $1E >> 2` | `$00:8815` |
| `$05` | fruit | **full heal** — `$E3 = $1E - $1D` | `$00:8821` |
| `$06` | (text) | **+100** score (BCD) | `$00:882E` |
| `$07`+ | (text) | **+50** score (BCD) — this is the `else` arm, so any id ≥ 7 lands here | `$00:883E` |

`$E3` is a *heal queue*, not an instant grant: `$00:88D6` ticks it every 4th frame
(`$88 & 3 == 0`), `DEC $E3` + `INC $1D` while `$1D < $1E`. That is the visible refill animation.

Placement of the 69 statues and which id each holds lives entirely in the level stream (§4) as the
`$38` byte of a `type = $80` entry, so **adding, removing, moving and re-rolling statues is all one
edit to one 4-byte record** — no separate statue table exists. Stock distribution
(`tools/act_content.py --census`): full heal ×25, 1UP ×14, magic ×9, +100 ×9, +50 ×5, heal¼ ×4,
screen clear ×2, sword power-up ×1 (Aitos map 2 only).

### 4. Level layouts — the bank-`$0A` placement streams

`$00:92CB` is the level-entry loader: DB = `$0A`, zero the whole object table, reserve slots 0-7,
then look up the stage in the index at **`$0A:B100`** — `(key word, offset word)` pairs terminated
by `$FFFF`, where the key is compared against the 16-bit `$18` (so key = `$19 << 8 | $18`).

**50 entries are listed, and they are MAPS, not acts.** An act spans several consecutive `$19`
maps — Fillmore has 4, Bloodpool 8, Kasandora 6, Aitos 7, Marahna 8, Northwall 8, Death Heim 8,
plus one special (`$18=$00 $19=$09`, the ending montage) = 49 action maps + 1. The game has **12
acts** (6 kingdoms × 2); act 2 begins at `$19` = 2/2/3/4/4/5 for regions `$01-$06` (ram-map
`$7E:0019`), which is exactly what the professional-mode order table `$02:9013` enumerates — it
lists each act by its *entry* map. So Bloodpool act 2 is a seven-map run, not one level.

Each map blob is three parts, in order:

1. **Player start** (3 bytes): tile X, tile Y, param → `$34`, `$36`, `$38`, then spawned as type
   `$83`/`$82`/`$81` depending on `$FC`/`$0341`/`$032C` (`$00:932E`). Tile units are 16 px
   (`value << 4`).
2. **Terrain damage boxes** (`$00:93A9`): 5-byte records `x0, x1, y0, y1, damage`, `$FF`-terminated,
   expanded into 10-byte RAM records at `$1AE4` (`x`, `width`, `y`, `height`, `byte4`) with the
   count at `$1AE2`. `$00:8C44` tests the player against them each frame: when `byte4 & $0080` it
   sets the player's `$30 |= $8000` (a state flag, not a subtraction — the pit/instant-loss class,
   not chased further), otherwise `$1D -= byte4`. All stock boxes use `byte4 = $01`. These are the
   lava/spike/pit zones.
3. **Object placement list** (`$00:941C`), 4-byte entries, one per object slot. Slot allocation is
   fixed by the loader: slots 0-7 are marked free up front, **slot 8 is the player**, slots 9-16 are
   reserved free (`$00:9308`), and the placement list therefore fills **from slot 17 upward** —
   63 entries maximum before `$00:853D`'s `CPY #$1AA0` bound:

   | Byte | Meaning |
   |---|---|
   | 0 | tile X (`<< 4` → `$34`) — values `$FC`-`$FF` are opcodes, see below |
   | 1 | tile Y (`<< 4` → `$36`) |
   | 2 | spawn param → `$38`. **For `type = $80` this is the item id**; for ordinary enemies it is a behaviour variant |
   | 3 | object type → `$00:9557` (bit 7 set = common table `$96AF`) |

   Stream opcodes:

   | Byte 0 | Size | Meaning |
   |---|---|---|
   | `$FF` | 1 | end of list — the current slot gets status `$8000` |
   | `$FE` | 5 | **checkpoint / wave gate.** Installs handler `$00:A813` with `$34/$36` = the trip point and `$02/$04` = the respawn point, and stashes the *stream cursor* in `$38`. When the player passes the trip point, `$00:A813` despawns everything (`$00:874E`), moves the checkpoint into `$032E/$0330`, and `$00:94F2` spawns the rest of the list into free slots. This is how a level is cut into waves/rooms |
   | `$FD` | 2 | reserve N slots (status `$4000`) without spawning |
   | `$FC` | 3 | goto absolute bank-`$0A` address (word) |

   Only 8 stages use a `$FE` gate; the rest spawn their whole population at entry.

**So all five placement questions are the same edit.** Adding a statue = inserting a
`tileX, tileY, itemId, $80` record. Moving one = changing bytes 0-1. Re-rolling its drop = byte 2.
Swapping an enemy = byte 3. Two mechanical constraints:

- **The list is slot-allocating.** Every non-opcode entry consumes one of the 80 `$06A0` slots
  starting at 17. Inserting entries shifts every later object's slot. Handlers that hard-code slot
  numbers (Fillmore's bridge segments are slots 36-49, the Death Heim victory driver is slot 50 —
  see "Object & spawn-handler model") will break, and the whole list plus everything spawned at
  runtime has to fit in the 63 remaining slots.
- **Records are variable length in the stream but fixed-length in RAM**, so a rewrite has to keep
  `$FC` goto targets consistent. Prefer replacing entries in place over inserting.

### 5. Can enemies move between regions? (the loading constraint)

Three different asset layers have to line up, and only two of them are global:

| Layer | Scope | Where |
|---|---|---|
| Behaviour/handler | **per region** — the type index means something different in each of the 8 tables | `$00:95DD` tables; handlers all live in bank `$00`, so the *code* is reachable from anywhere |
| OBJ tile atlas | **global** — `$02:BC9E` copies the same ROM `$07:8000-$9FFF` (8 KB) into VRAM `$2000`-`$2FFF` and the same palettes `$07:D040-$D09F` into CGRAM `$C0-$EF`, at every act entry, unconditionally | `$02:BC9E`, called from `$00:8366` |
| Animation + composition data | **per act** — LZSS-compressed, loaded by one command of the per-map asset script (below). `$02:B69C` reads a flag + a 3-byte pointer, takes the decompressed size from the blob's own first word, and decompresses to `$7E:4000` (flag 0) or `$7E:5000` (flag nonzero) | `$02:B69C` → `$02:C5C9` |

Object records name their animation table by `(base $16, bank $18)`. Act enemies use
`($4000, $7E)`; bosses use `($5000, $7E)`; the player and the statue use `($8000/$A800, $06)` —
ROM-resident, which is why they work in every act. Verified against `saves/dump_wram.bin`
(Fillmore act 1): `$7E:4000` holds a live animation table (`92 03` = frame-pointer-table offset,
then per-animation script offsets), and its bytes appear **nowhere** in the ROM uncompressed.

Consequence: **transplanting an enemy across regions is not free.** Pointing Fillmore type `$1B`
at Bloodpool's record makes the handler run, but its animation indices resolve against whatever
`$7E:4000` blob the *current* act loaded, so it will draw the wrong frames (or garbage). The
practical options are, in increasing order of work: (a) shuffle types *within* a region — always
safe, since the same blob is loaded; (b) shuffle whole stages; (c) build a per-act allow-list of
importable enemies by checking that the source and destination blobs agree on the frame ids the
handler uses.

Formats for (c), both established:

- **Animation table** (`$00:8E2F`): word at `+0` = offset of the frame-pointer table; word at
  `(anim + 1) * 2` = offset of that animation's script. Script = 4-byte frames
  `[frameId, duration, dX, dY]`, `frameId = $FF` ends/loops.
- **Composition record** (`$00:8D68`): `+0` word = X extents (normal/flipped) → `$0A`/`$0E`;
  `+2` word = Y extents → `$0C`/`$10`; `+4` byte = part count; then N × 7-byte parts
  `[flags(bit0 = 16×16), Xnormal, Xflipped, Ynormal, Yflipped, tile+attr word]`. Tile ids ≥ `$100`
  resolve into the global `$07:8000` atlas (`ROM $07:8000 + (tile - $100) * 32`) — verified by
  rendering the statue.

#### The per-map asset script (mapped 2026-08-02) — this is what decides the allow-list

`$02:B1F7` is the **first call in the action level-entry sequence** (`$00:8325`). It walks a script
table at **`$05:8000`** (cursor in the long pointer `$A2`):

- `$02:B250` seeks: entries are `[$18 byte, $19 byte, command bytes…, $00]`, and non-matching
  entries are skipped by the operand-size chain at `$02:B264`. The table opens with a 3-byte
  `"SY\0"` header entry that matches nothing.
- Each command byte dispatches on its **highest set bit**, MSB first:

  | bit | handler | operand bytes |
  |---|---|---|
  | 7 | `$02:B28E` — VRAM char/tile upload | 6 |
  | 6 | `$02:B330` — CGRAM/palette load (writes `$2121` then streams `$2122`) | 6 |
  | 5 | `$02:B363` — **metatile definition table** (selector at operand 3: `$01` → `$7E:2100` BG1, `$02` → `$7E:2900` BG2). 2048 bytes, stored **byte-swapped** | 7 |
  | 4 | `$02:B3EB` — **generic WRAM data load** (selector at operand 0: `$01` → the BG1 metatile-id map at `$7E:8000`, i.e. the collision layer; `$02` → the BG2 map at `$7E:C000`). Blob header is `[widthChunks][heightChunks][size16]`, and the width/height bytes are what set the level dimensions `$2E`/`$30` | 4 |
  | 3 | `$02:B4E8` | 1 |
  | 2 | `$02:B631` | 3 |
  | 1 | `$02:B63B` — script-driven song change (already documented in the Audio section) | 5 |
  | 0 | `$02:B69C` — **OBJ animation/composition blob** | 6 |

- **Script pointers are 24-bit LINEAR ROM file offsets**, not SNES addresses. `$02:B4C0` converts
  in place: `bank = L >> 15`, `addr = $8000 | (L & $7FFF)`. Cross-check: the dialog-font command in
  every map's script carries `$0BECFB`, which converts to `$17:ECFB` — exactly the font address
  already documented in the Graphics section from an independent investigation.
- A bit-0 command's operands are `[destFlag, ignored, ignored, srcLo, srcHi, srcBank]`;
  `destFlag == 0` → `$7E:4000` (ordinary objects), nonzero → `$7E:5000` (boss). The blob's own
  first word is the decompressed size, stream starts at +2.

**The result (`tools/act_content.py --assets`): only 13 of the 49 action maps load an object blob.
Every other map inherits whatever is already in `$7E:4000`.** The 13 load points are Fillmore 1/2,
Bloodpool 1/2, Kasandora 1/3, Aitos 1/4, Marahna 1/4, Northwall 1/5, Death Heim 1 — i.e. **exactly
the twelve act-entry maps plus the Death Heim hub**, which independently reproduces the act
boundaries (`$19` = 2/2/3/4/4/5) derived from `$7E:0019` and the professional-mode order table.

So the enemy-animation allow-list is **per act**, and it is a partition with no sharing:

- **Within one act, enemies can be shuffled freely across all its maps** — Bloodpool act 2's seven
  maps share one blob, so anything legal in map 2 is legal in map 8.
- **Across acts**, a type's frame ids resolve against a different blob. Two routes: match frame ids
  by decompressing both blobs, or — much cheaper — **swap the 3-byte blob pointer in the script**
  so the destination act loads the source act's animation set wholesale.
- **Boss blobs are all distinct** (19 of them, one per boss-bearing map). That makes boss shuffling
  a two-part edit that is now fully specified: swap the object-table record *and* the boss blob
  pointer at its `$05:xxxx` command.

> **Open:** tile ids `< $100` were not chased. If enemy art below `$100` is per-act OBJ char, that
> is a fourth layer and tightens (c) further. `$02:BC9E` alone does not cover it.

> **Anomaly, unresolved:** the `$18=$00 $19=$09` stage entry spawns types `$05`/`$06`, but region
> table `$96AF` bounds at four entries. `$00:8340` only calls the level loader when `$18 != 0`, so
> this stage is probably reached through a different path; do not model region `$00` from this row
> without checking.

### 5b. Action terrain collision (mapped 2026-08-02)

This is the piece a placement randomizer cannot work without: given a tile, is it ground?

**`$00:91C3` is the collision oracle.** Inputs are 16px tile coordinates in DP `$14` (X) and `$16`
(Y) — **the same units as the placement stream** — and it returns a terrain attribute in `A`. It has
14 call sites, all inside the `$00:8F30-$91C2` terrain module. Two out-of-bounds behaviours matter:
`tileX >= $84` returns `$0F` (past the right edge reads as *wall*), `tileY >= $86` returns `$00`
(below the map reads as *empty*, i.e. you fall out).

Two WRAM structures feed it, both built at level entry:

| Where | What |
|---|---|
| `$7E:8000` | **metatile-id map**, one byte per 16px tile |
| `$7E:05A0` | **metatile id → attribute**, 256 bytes, built by `$02:BAC1` |

**Map indexing is chunked, not linear.** `$00:91C3` composes the index as

```
index = ( (tileY>>4) * $2F  +  (tileX>>4) ) * 256   +   (tileY & 15) * 16  +  (tileX & 15)
```

i.e. the map is cut into **16×16-tile chunks (256×256 px)**, chunks stored row-major with `$2F`
chunks per row, and each chunk stored row-major internally. `$2F` is the high byte of the level
pixel width `$2E`, so *chunk columns = width >> 8*. The `(tileY>>4)` term arrives by a subtlety
worth flagging for a decompilation: `$91D0`'s `ASL A ×4` leaves `tileY>>4` in the accumulator's
**hidden high byte**, and `$91D7`'s `SEP #$20; LDA $2F; JSR $846E` then multiplies `$2F` by it —
`$00:846E` is the 8×8→16 hardware multiply (`$4202/$4203` → `$4216/$4217`), and the `XBA` inside it
is what picks up that high byte. Verified against `saves/dump_wram.bin`: Fillmore act 1 is
4096×768 px = 256×48 tiles = 16×3 chunks, with `$2F = 16`.

**The attribute is a 4-bit quadrant-solidity mask** — one bit per 8×8 quadrant of the 16×16
metatile, `bit0 TL, bit1 TR, bit2 BL, bit3 BR`. `$02:BAC1` derives it at level entry by reading
**bit 1 of the high byte of each of the metatile's four sub-tile tilemap words** (the metatile
definitions live at `$7E:2100`, 8 bytes each) and packing them. So collision is carried in a spare
tile-number bit of the art itself — there is no separate collision layer.

How the terrain module reads it:

| Attribute | Meaning | Tested at |
|---|---|---|
| `$00` | empty | `$9038`, `$915A` |
| `$0F` | fully solid — walls, ceiling | `$8F4D` (wall probe), `$8FD0` (head bump), `$9150` |
| `& $0003 == $0003` | **top half solid** — the "stand on this" surface predicate | `$905B`, `$9076`, `$91AC` |
| `& $000C` | bottom half solid — underside / step | `$91A2` |
| `$06` | **slope `/`** — `$00:90A9` derives the Y snap from `objX & $0E` | `$901C`, `$902E` |
| `$09` | **slope `\`** — `$00:9084` uses `~objX & $0E`, the mirror | `$9024`, `$9033` |
| anything else nonzero | treated as solid ground | `$903D` |

The two slope values are exactly the two *diagonal* bit patterns (`$06` = TR+BL, `$09` = TL+BR), so
the mask encoding and the slope encoding are the same scheme, not two systems.

Confirmed empirically on the Fillmore act-1 dump: all 18 metatiles with attribute `$03` have
quadrant bits `[1,1,0,0]` (top half) and every one of the 59 tiles using them has empty space both
above and below — they are the level's thin floating platforms. Rendering the whole map produces a
coherent side-scrolling level (continuous ground line, hills, platforms), which is the check that
the index formula is right: a wrong formula produces noise.

**Placement Y is a ground line, not a body position.** `$00:96A4`, at the tail of the spawn-record
apply, does `objY = objY - obj.$10` after `$00:969B` has loaded frame 0 (which is what sets the
down-extent `$10`). So a placement's `tileY*16` is where the object's **feet** land, and the sprite
is lifted by its own height. A randomizer that writes placements without accounting for this will
sink every object into the floor by 8-24 px.

That gives a validation predicate, implemented as `tools/act_collision.py --check`: a well-formed
ground placement has `tileY` **standable** and the tiles above it clear. Run against stock Fillmore
act 1, **all three statues, the player start and every ground enemy pass**, with 16 objects flagged
"airborne" — types `$00/$01/$03/$06/$07/$08/$09`, i.e. the flying enemies. Stock data validating
clean under the predicate is the evidence that the predicate matches the game's own discipline.

**Offline as of 2026-08-02.** The whole chain is now resolved and the collision map for every
action map can be built from the ROM alone (`tools/act_collision.py --map MODE,SUB`):

1. the per-map asset script (§5) supplies two pointers — a **bit-4 command with selector `$01`**
   (the metatile-id map) and a **bit-5 command with selector `$01`** (the metatile definitions).
   Selector `$02` in each case is the BG2 layer's equivalent, not the collision layer;
2. the map blob's header is `[widthChunks][heightChunks][size16]` followed by the bit-packed
   stream. **`widthChunks` IS `$2F`** — the chunk-column count in the index formula comes straight
   out of the blob, which is why the level's pixel width is always a multiple of 256;
3. the metatile blob is 2048 bytes (256 metatiles × 8) stored **byte-swapped** relative to
   `$7E:2100`;
4. rebuild the `$05A0` attribute table exactly as `$02:BAC1` does.

This needed a faithful decompressor. The production implementation is now the shared
`src/quintet_lzss.c`, used by the ROM backdrop loader, settings font, and whole-body `$02:C5C9`
CPU HLE; `tools/quintet_lzss.py` remains an independent direct-port oracle for `$C5C9` / `$C639`
/ `$C66C`. Note for anyone tempted to reach for a stock Quintet LZSS routine:
this stream is **bit-packed rather than byte-aligned**, the ring is pre-filled with `$20` (not zero)
and its write cursor starts at `$EF`. A byte-aligned decoder consumes the stream and emits
plausible-looking output while being wrong, so validate any decoder against a known blob first —
that is what `--verify` exists for. (A guessed implementation lived at `tools/lzss_decompress.py`
until 2026-08-03; it was deleted rather than fixed, because wrong-but-plausible output is a trap.)

Evidence the port is exact:

- the two animation blobs named by Fillmore act 1's script (`$0CD695` objects, `$03EFC7` boss)
  decompress **byte-identical** to `$7E:4000` / `$7E:5000` in `saves/dump_wram.bin`
  (`tools/quintet_lzss.py --verify`);
- the map blob `$0AF131` decompresses byte-identical to `$7E:8000`, and the ROM-built attribute
  table plus all **12288** tile attributes match the WRAM-built ones exactly;
- the dialog font `$0BECFB` decompresses to exactly `$1000` bytes, the size documented
  independently in the Graphics section;
- `--selfcheck` cross-checks the uniform bit reader against a literal transcription of both
  `$02:C66C` spellings (the nibble read straddling a byte boundary is the easiest part to misread).

All **49 action maps** now build offline. The remaining softness is in `--check`'s heuristics, not
the map: the assumed 2-tile body height should become the animation frame's real down-extent `$10`,
which is itself now obtainable since the per-act blobs decompress.

### 6. Sim-mode monster lairs — positions, types and populations

The seed table is **ROM `$03:B825`** (file `0x1B825`), **24 records × 9 bytes**, installed by
`$03:B7C6` into eight parallel 24-entry word arrays in WRAM. (`ram-map.md`'s "up to 16 lairs" is
wrong — the loop counter at `$03:B7C9` is `$18` = 24, and `$03:B7A3`'s lookup uses `LDY #$0004`
against a base of `town * 8`, i.e. **4 lairs per town × 6 towns**.)

| Byte | WRAM array | Meaning |
|---|---|---|
| `+0` | `$7F:9568 + i*2` | **lair X** — 16 px town-map cell, 0..31 |
| `+1` | `$7F:9598 + i*2` | **lair Y** — same units |
| `+2` | `$7F:95C8 + i*2` | lair image id (also carries state bits at runtime: `$8000`, `$2000`, `$1000`) |
| `+3` | `$7F:95F8 + i*2` | **monster type** — this is the world-record class written to record `+$0E` |
| `+4` | `$7F:96B8 + i*2` | **remaining monster count** (the lair's population) |
| `+5` word | `$7F:9628` and `$7F:9658` | respawn delay, copied into the countdown at install |
| `+7` word | `$7F:9688 + i*2` | **world-record address** the lair's monster occupies |

Position semantics, from `$03:B99C` (the spawner) and `$03:B7A3` (the "is there a lair on this
square?" test):

- The spawned monster's world position is `(X * 16 + $18, Y * 16 + 8)` written to record `+$0A`/`+$0C`.
- A lair's **selector square** is `cell >> 2`, matching the 8×8 square grid the build UI and the
  `$7F:9250` built-square lists use. All 24 stock lairs sit on multiples of 4, i.e. exactly on
  square centres.
- Spawning is gated on the world record being free (`+$10 & $8000`) and on the countdown
  `$7F:9658` reaching 0, at which point it reloads from `$7F:9628`.

The four `+7` values are `$0B30 / $0B56 / $0B7C / $0BA2` — world-object records **8, 9, 10 and 11**
of the `$0A00` array (stride `$26`). That is also exactly the `$130`-byte block `$03:813F` stages
into `$0B30,Y`, tying this table to the sim-mode spawn section above. Lair slot *k* of the current
town always drives record `8+k`, so a randomizer can move a lair freely but **cannot add a fifth
lair to a town** without extending both the 4-per-town lookup stride (`town * 8`, `LDY #$0004`) and
the record allocation.

Stock table (`tools/act_content.py --lairs`); types are `$12` Blue Dragon, `$13` Napper Bat,
`$14` Red Demon, `$15` Skull Head:

| Town | lairs `(X, Y, image, type, count, respawn)` |
|---|---|
| Fillmore | `(00,10,03,Dragon,200,1) (18,08,05,Bat,100,1) (10,1C,05,Bat,100,1) (04,04,05,Bat,100,1)` |
| Bloodpool | `(04,18,04,Demon,100,1) (00,04,05,Bat,50,1) (10,1C,03,Dragon,90,1) (1C,10,03,Dragon,90,1)` |
| Kasandora | `(18,04,03,Dragon,110,1) (10,04,04,Demon,125,1) (18,18,04,Demon,125,1) (10,10,05,Bat,90,1)` |
| Aitos | `(04,04,03,Dragon,80,1) (08,10,06,Skull,80,$8D) (18,08,03,Dragon,80,1) (04,18,03,Dragon,80,1)` |
| Marahna | `(04,04,04,Demon,50,1) (00,18,05,Bat,60,1) (18,18,03,Dragon,50,1) (08,0C,03,Dragon,50,1)` |
| Northwall | `(18,10,0A,Skull,30,$64) (10,0C,09,Skull,30,$64) (10,18,07,Dragon,60,1) (04,08,08,Demon,60,1)` |

Population is also written at runtime: `$03:B4F3` (`INC`) and `$03:B52B` grow it, `$03:BB04`
(`DEC`) is the per-kill decrement (matched by comparing the lair's `$7F:9688` record address
against the dying actor), and `$03:BAC0`-`$BAD7` is the miracle/earthquake path — subtract 10,
clamp at 0, then award 10 growth points via `$03:B54E`.

> **Untested caveat for lair randomization:** nothing in `$03:B7C6` validates a lair position
> against terrain. The build/road map (`$7F:6800`, seeded from ROM `$03:DCFA`) and the water/obstacle
> layout are independent data, so a randomly placed lair could land in water or inside the town
> centre. Any lair shuffler should filter candidate squares against the road/obstruction bits
> documented in `ram-map.md` "Road Construction Encoding".

### 7. Persistent player economy (the values a randomizer would also want to touch)

| Value | Working copy | Persistent copy | Notes |
|---|---|---|---|
| HP | `$1D` | — | reset to `$1E` at every stage entry (`$00:83CF`) |
| Max HP | `$1E` | SRAM `$70:1246` | 8 at new game, `+1` per level, cap 24 (`$03:B3DF`) |
| Lives | `$1C` (BCD) | `$02AB` | loaded at act entry `$02:84D7` |
| Magic charges | `$21` | `$0295` | loaded at act entry `$02:84E0`; act pickups only touch `$21` |
| Score | `$1F` (BCD) | per-act records at `$02B3` | `$00:873C` adds, saturates at `$9999` |
| Sword power-up | `$E4` | — | `$80` = doubled sword ATK, cleared on act change |
| Heal queue | `$E3` | — | drained 1 HP per 4 frames by `$00:88D6` |
| Professional mode | `$0349` | — | see §1 |

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
| `$00:9557` | spawn dispatcher (reads `$18`, indexes per-act handler table at `$95DD`; **type bit 7 set forces the common table `$96AF`**) |
| `$00:95F0` | **spawn record → object field copier** — the 12-byte record decode (ATK `+7`→`$2A`, HP `+8`→`$2C`, score `+9`→`$2E`, flags, animation base/bank). Also the professional-mode (`$0349`) ATK/HP promotion and statue-drop rewrite. |
| `$00:92CB` | **action level loader** — DB=`$0A`, clears the object table, finds the stage in the `$0A:B100` index, then runs player start / terrain boxes / object list |
| `$00:93A9` | terrain damage-box loader — 5-byte ROM records → 10-byte RAM records at `$1AE4`, count `$1AE2` |
| `$00:941C` / `$00:94F2` | **object placement-list walker** (initial batch / wave-gate continuation). 4-byte entries + `$FC`/`$FD`/`$FE`/`$FF` opcodes |
| `$00:A813` / `$00:A82D` | **checkpoint + wave gate** — trips on player position, clears remaining objects (`$00:874E`), moves the respawn point into `$032E/$0330`, spawns the next batch from the stashed stream cursor |
| `$00:8A3C` | **object-vs-object combat loop** — AABB over all 80 slots; `$00:8AF9` is the single damage subtraction `victim.$2C -= attacker.$2A` |
| `$00:8892` | **enemy death** — award `$2E` score, install death animation `$A382`, follow `$30 & $1000` to the next slot for multi-slot bosses |
| `$00:A382` / `$00:A3E1` | **death animation + respawn**: when field `$14` is nonzero, `$A382` re-runs the spawn from record base `$32`, arms `$26 = $0258` and installs `$14` as the handler; `$A3E1` counts that down while offscreen and restarts the enemy |
| `$00:A54A` | **boss death** (selected by object flag `$4000` instead of `$00:8892`) |
| `$00:879D` | **item-effect dispatcher** — item id in `A` (from the pickup object's `$38`): 0 magic, 1 life, 2 screen clear, 3 sword power-up, 4 heal ¼, 5 full heal, 6 +100, ≥7 +50 |
| `$00:96C3` | **statue-break handler** (object type `$80`) — uploads `$06:A000 + id*$80` to VRAM `$2D80` and splits the record into a shatter effect plus the timed collectible |
| `$00:8E2F` | **animation stepper** — resolves `(base $16, bank $18, anim $1A, frame $1C)` to a composition record in `$20`, and refreshes the AABB extents `$0A/$0C/$0E/$10` from it. Hitboxes are per-frame data |
| `$00:91C3` | **terrain collision oracle** — tile coords in `$14`/`$16` → 4-bit quadrant-solidity attribute, via the chunked map at `$7E:8000` and the table at `$7E:05A0`. See "Content / randomizer seams" §5b |
| `$00:846E` | 8×8→16 hardware multiply (`$4202/$4203` → `$4216/$4217`); the `XBA` picks the multiplicand out of A's hidden high byte, which is how `$91C3` gets `tileY>>4` into the chunk-row term |
| `$02:BAC1` | **collision attribute-table builder** — packs bit 1 of each of a metatile's four sub-tile tilemap words (`$7E:2100`, 8 bytes/metatile) into the 4-bit mask at `$7E:05A0` |
| `$00:8FAD` / `$00:8FE7` | player head-bump probe (tests `$0F`) / ground probe (dispatches `$00`, `$06` slope, `$09` slope, else solid) |
| `$00:8F30-$8F56` | horizontal wall probe — 3 vertical samples at the leading edge (from `$00:916E`'s facing-aware hitbox pick), blocked on `$0F` |
| `$00:96A4` | spawn Y ground-anchor adjust — `objY -= obj.$10` after frame 0 loads, so a placement's `tileY` is the object's **feet**, not its body |
| `$00:8D68` | **sprite builder** — walks the composition's 7-byte parts into the OAM shadow (see the Graphics table row for the surrounding pipeline) |
| `$00:8781` | professional-mode act advance — `INC $0349`, step the stage-order table `$02:9013` into `$1A/$1B` |
| `$02:AB05` | **professional-mode start** — `$0349=1`, lives 4, max HP 24, magic 0, score 0, stage `$01/$01` |
| `$02:B1F7` / `$02:B250` / `$02:B264` | **per-map asset-script VM** — first call of action level entry; seeks the `($18,$19)` entry in the table at `$05:8000` and dispatches command bytes by highest set bit (skip chain `$B264` gives operand sizes) |
| `$02:B4C0` | asset-script pointer read + **linear→LoROM fixup** (`bank = L>>15`, `addr = $8000\|(L&$7FFF)`) into the decompressor source `$A5-$A7` |
| `$02:B69C` | per-act OBJ animation/composition blob decompress — flag + source pointer from the asset script, size from the blob's own first word, dest `$7E:4000` (objects) or `$7E:5000` (boss) |
| `$02:C5C9` / `$C639` / `$C66C` | **Quintet LZSS decompressor + whole-body HLE** — 256-byte ring at `$7E:2000` pre-filled with `$20`, write cursor starting at `$EF`, control bits in `$AE`. **Bit-packed, not byte-aligned**: `$C639` reads 8 bits and `$C66C` the 4-bit match length (len = n+2) by shifting `$B7/$B8` by the current bit phase. `src/quintet_lzss.c` is the shared production core; `tools/quintet_lzss.py` is the independent oracle. The HLE reconstructs caller-visible DP scratch, ring contents, registers, accumulator, and RTL stack behavior. |
| `$02:B3EB` | generic WRAM data load — selector, pointer, then `[widthChunks][heightChunks][size16]` from the blob; sets level dims `$2E`/`$30` and hands off to `$02:B446` (raw copy or decompress, dest from the `$0046,X` table) |
| `$02:B363` | metatile definition table load — 2048 bytes to `$7E:2100` (BG1) or `$7E:2900` (BG2), stored byte-swapped in ROM |
| `$02:B330` | CGRAM/palette load |
| `$03:B7C6` | **monster-lair installer** — ROM `$03:B825`, 24 × 9 bytes → the eight `$7F:95xx/96xx` arrays |
| `$03:B99C` | **lair monster spawner** — gates on population + free world record + respawn countdown, then writes `(cellX*16+$18, cellY*16+8)` and the monster type into the record |
| `$03:B3D4` | **level-up** — `$0291`+1, max HP `$1E`+1 capped at 24, Angel max/current HP `$0287`/`$0286`+1 |
| `$03:9156` | **act→sim transition handler dispatcher** (relocates stack to `$1FFF`, RTS-trick chain through `$9B22`/`$9B4A`/`$9195`) |
| `$03:8053` | **enter-sim SETUP** (runs on ANY entry to `$18=00`, incl. act→sim AND a warp to `$18=00`). Sequence of `JSR`s (`$9156`, `$AC8E`, …) → `$8193` → `$C147` → `$B20C`/`$B21F`. The 2026-06-26 act→sim hang in this cascade was a 1-byte SNES-stack leak per call in `$01:B898`'s jump-table RTS-trick, fixed via `indirect_dispatch B8C0 … ret:B8C2` (bank01.cfg) — see the `$01:B898` row below and bug-ledger §7.7. |
| `$01:B898` | **per-record per-type dispatcher, called once per active actor record every frame** (from the `$8193` master loop — see the sim-mode town architecture section above for the full chain into the spawn battery). `$B8C0` PHA-dispatches through the handler table at `$01:B8D0` keyed by object type, returns to `$B8C2`. History of THREE separate bugs found at this one site, in order: (1) a 1-byte SNES-stack leak that hung the act→sim transition (fixed 2026-06-26, see `$03:8053` above); (2) the table's `count` was left capped at 16 as a workaround for a since-fixed label-emission bug, but town actor types are 18/19 — above the cap, so their class handlers never dispatched (fixed 2026-07-02: bumped to the real bound, 26); (3) even at count=26, `idx:X` was wrong AT THIS SITE specifically — the ROM wraps the table read in `PHX($B8AE)/PLX($B8BB)`, so by the `PHA/RTS` dispatch X has been restored to the RECORD POINTER, not the type index (fixed 2026-07-02: switched to the value-keyed `idx:A` form, which reads the PHA'd table word instead of a register). This is the site responsible for the sim-mode actor-spawn corruption/freeze (`DEBUG.md` #18-25) — NOT the earlier stack-leak hang, a different bug at the same address. |
| `$03:AC8E` | transition state-machine step (counter loop, calls `$97B0`) |
| `$00:80E5` (label inside `ResetHandler`) | **sim-mode per-frame dispatch entry** — reached when `$18==0`; see "Sim-mode dispatch structure" above. |
| `$01:8000` | **sim-mode building/icon per-frame updater** — region-gated (`$19`), drives the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables; deep body at `$018170` does the actual per-building work via `JSL $1B1C7`. |
| `$00:8465` | writes a hardware-register-style immediate (`LDA #$A1`) — same pattern as the NMITIMEN setup at `$008051`; called from `ResetHandler`'s sim-dispatch tail (`$008122`). Its own native width (`M1X0`) runs correctly — confirmed NOT a misdecode (2026-07-01), just legitimate register churn that was mistaken for corruption early in that investigation. |
| `$00:8241` | called twice per main-loop iteration (`$008056` and `$00805C`, sandwiching the `$8418` vblank wait) — role not yet traced. |
| `$02:A622` | **title-screen continue/new-game state machine.** Calls the save checksum (`$02:A88D`) at `$02A70A`, branches on the result (`$02A70D: BCC $A72F`) to one of two dialog flows, and owns selection byte `$0336`. Phase-5 host evidence captured the interactive menu at gf821 with `$18/$19=00/00`, `$0300/$0302=0100/0110`, `$92=0C`, and `$0336<=2`. These remain useful renderer-state observations, but the settings overlay no longer depends on them: Escape/F1 is intercepted globally before emulated input dispatch — see "Save / persistence" below. |
| `$02:A88D` | **save-data validity checksum.** Computes a 16-bit ADD-sum and a 16-bit XOR-sum over SRAM `$700000-$701FEB` (calls `$00:84F3` to do the accumulation), compares against stored expected values at `$701FEC`/`$701FEE`. Returns pass/fail via carry (`CLC`=pass, `SEC`=fail). Confirmed correct 2026-07-01 (`AR_SAVECHECK`) — passes cleanly against a real mid-game save. |
| `$00:84F3` | **checksum accumulator loop + whole-body HLE** — native body: `LDX #0; loop: LDA $700000,X; ADC $14; STA $14; EOR $16; STA $16; INX INX; CPX #$1FEC; BNE loop`. The HLE reuses the tested `Save_ComputeChecksum` host core and reconstructs `$14/$16`, `A/X/Y`, `P`, and RTL behavior; its separate decimal-mode path preserves the native routine's lack of `CLD`. `$14`/`$16` are shared scratch — see the DP-scratch-reuse gotcha in `DEBUG.md` §0. |
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
| `$03:F46E`/`$F479`/`$F487`/`$F497` | **story-event bitmap helpers**: test / set / clear one per-town event bit; `$F497` resolves the WRAM byte + mask (scratch `DB:$914F`, mask table `$03:F4D7`, MSB-first) from the WRAM-pointer tables `$03:DCA2` (prereq, `DB:$9107+`) / `$DCAE` (fired, `DB:$911F+`) / `$DCBA` (dispatched, `DB:$9137+`). **Corrected 2026-08-17** — these were labelled "open-lair"/"spawned-lair" masks while chasing the 2026-07-02 corruption bug. They are 32 event ids per town, consumed by the `$03:DFFB` event selector and by every town-event handler in `$03:E6xx-$F3xx`; monster-lair state lives in the parallel arrays at `$7F:9568+` (ram-map "Monster Lair Data"). |
| `$01:AC36` | **process-script assigner**: `entry.+02/+06 = ROM[$01:A227[$033C*2] + $033D*2]`, `+04=0`, `+00=1`. The stride-12 tier's spawn primitive. |
| `$01:CFF2` | thin wrapper: packed `A=(list<<8)|variant` → high byte `$033C`, low byte `$033D`, then `JSR $AC36`. The sweep's per-entry call. |
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
