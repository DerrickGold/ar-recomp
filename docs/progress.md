# ActRaiser Recomp — Project Status and Roadmap

Last updated: 2026-09-08

This document is the authoritative summary of project status, play-test
coverage, and remaining release work. Implementation details, investigation
history, and diagnostic procedures belong in the technical documents linked at
the end.

Status reflects observed results for the scope named in each row:

- ✅ Confirmed working
- 🟡 Implemented or playable, with acceptance work still remaining
- 🔴 Broken or blocking
- ⬜ Not yet tested

## Current snapshot

- Every ordinary action stage and the complete Death Heim boss rush are
  playable end to end in widescreen.
- Fillmore, Bloodpool, Kasandora, Aitos, Marahna, and Death Heim have confirmed
  Diorama action playthroughs.
- Fillmore, Bloodpool, Kasandora, Aitos, and Marahna have confirmed
  simulation-mode event coverage with Diorama effects.
- Northwall simulation mode and its two Diorama action stages are the remaining
  gameplay-coverage gaps.

## Next milestones

1. **Finish gameplay acceptance**
   - Capture Northwall's complete simulation-mode baseline.
   - Complete Diorama playthroughs for both Northwall action stages.
   - Finish the remaining visual acceptance for recent action lighting and
     particle effects.
2. **Close subsystem acceptance**
   - Complete the remaining simulation-3D and replacement-music acceptance
     items listed below.
3. **Complete release validation**
   - Run the full-game matrix with enhanced presentation enabled and disabled.
   - Launch-test the generic Linux and Windows distribution bundles.

## Gameplay coverage

### Action stages

The widescreen column covers end-to-end gameplay and presentation. The Diorama
column records the additional 3D-presentation playthrough.

| Region | Widescreen gameplay | Diorama route | Remaining |
|---|---|---|---|
| Fillmore | ✅ Acts 1 and 2 (2026-07-12) | ✅ Acts 1 and 2 (2026-08-11) | — |
| Bloodpool | ✅ Acts 1 and 2 (2026-07-12) | ✅ Acts 1 and 2 (2026-08-11) | Final visual acceptance for recent lighting effects |
| Kasandora | ✅ Acts 1 and 2 (2026-07-12) | ✅ Acts 1 and 2 (2026-08-11) | — |
| Aitos | ✅ Acts 1 and 2 (2026-07-12) | ✅ Acts 1 and 2 (2026-08-23) | — |
| Marahna | ✅ Acts 1 and 2 (2026-07-12) | ✅ Acts 1 and 2 (2026-08-11) | Final visual acceptance for recent lighting effects |
| Northwall | ✅ Acts 1 and 2 (2026-07-12) | ⬜ Not yet recorded | Diorama playthrough of both acts |
| Death Heim | ✅ Complete boss rush and final boss (2026-07-14) | ✅ Complete boss rush and final boss (2026-08-23) | — |

### Simulation mode

| Town | Status | Confirmed scope |
|---|---|---|
| Fillmore | ✅ | Complete round from Act 1 through Act 2, including development, events, lairs, rewards, and Diorama presentation (2026-08-11) |
| Bloodpool | ✅ | Complete development, event, lair, transition, and Diorama coverage (2026-08-11) |
| Kasandora | ✅ | Complete development, event, lair, transition, and Diorama coverage (2026-08-11) |
| Aitos | ✅ | All simulation development, authored events, and lair flows confirmed supported and working with Diorama effects (2026-08-23) |
| Marahna | ✅ | Complete development, event, lair, transition, and Diorama coverage (2026-08-11) |
| Northwall | ⬜ | Complete authentic and enhanced-presentation baseline still required |

## System roadmap

| System | Status | Current state and exit criterion |
|---|---|---|
| Boot, title, and core save/load | ✅ | Normal startup, continue, and in-game persistence are confirmed. |
| Action-stage gameplay | ✅ | Every ordinary action stage and the complete Death Heim route are playable end to end. |
| Action widescreen presentation | ✅ | Wide backgrounds, sprites, camera limits, transitions, and finite stage edges are confirmed across all action stages. |
| Action Diorama presentation | 🟡 | Every action route except Northwall is confirmed. Complete the two Northwall acts. Optional interpolation and vertical extension remain disabled by default until their visual sweeps are complete. |
| Action lighting and particles | 🟡 | Environment, weapon, and boss effects are implemented in their original rooms and applicable Death Heim rematches. Death Heim also has sharp hub faces, face-anchored eyes, reused Viper-room torches, and correct torch/boss occlusion. Aitos and Death Heim are accepted; complete the remaining visual sweep for Bloodpool, the sword beam, and Marahna/Viper. |
| Simulation gameplay | 🟡 | Five towns have confirmed event coverage; Northwall still needs a complete baseline. |
| Simulation 3D presentation | 🟡 | Town terrain, structures, actors, atmosphere, and event effects are implemented. Complete the remaining event-effect and full-town visual acceptance passes. |
| Magic | ✅ | All four spells and their enhanced effects are confirmed working. |
| Input | ✅ | Keyboard and gamepad rebinding, hotplug, controller-only menu access, and Steam Deck input are supported. |
| Runtime settings | ✅ | The in-game settings overlay and persistent configuration are implemented and working. |
| Battery save codec and editor | ✅ | Native save import, export, editing, backup, round-trip preservation, and in-game use are confirmed working. |
| Replacement music | 🟡 | Manifest-driven OGG playback and looping are implemented. Complete an in-game listening pass, identify the remaining unnamed tracks, and validate fades. |
| World-navigation 3D | 🟡 | Full spherical navigation embeds actual town models with shared palettes/LOD/depth, live native ground/water animation, raised ranges, aligned plains, protected mountain backs and native cliff geometry. Full-globe clouds, atmosphere, space, ten effect switches and visit-local orbital inspection are implemented. Aitos uses a single overhead crater cap with native lava animation; Advent clearance includes terrain/model heights. Existing town-entry/return menus and loading fades are retained and replay-verified. The code-quality follow-up adds boundary regression guards, complete major-stage CPU attribution, exact lighting/ocean optimizations and failed-atlas-upload recovery. Full CTest: 147 tests; sanitized Metal sweep: 246 exact-reference images. Repeated optimized full-quality flights average about 8.02 ms CPU presentation at 1792×1344; this is not a cross-platform FPS guarantee. Custom seamless entry/exit and globe-under-SIM are deferred. Physical camera/controller and non-Metal GPU acceptance remain open. Stable checkpoint: `4f1741b`; earlier fallback `380772a`. Evidence: `rendering-engine.md` §13h and `world-navigation-code-audit.md`. |
| Build and platform targets | 🟡 | macOS arm64 and Steam Deck bundles are confirmed end to end. macOS x86_64, generic Linux, and Windows bundles build but still need representative launch testing; signing and notarization remain release work. |

World-navigation follow-up (2026-09-08): the separate enhanced Sky Palace
setting now uses the shared developed globe behind the native foreground,
with a Palace-only blue daylight gradient and moving sky clouds. Inter-town
navigation retains space. Public observational capture, bounded homogeneous
clipping and same-frame native fallback preserve layer/ABI ownership. The
final suite passes **148 tests**; sanitized Metal retains all **246** prior
navigation images exactly and verifies six additional Palace viewpoints,
cloud motion/freeze, shadow toggles and reset/return parity. Native 4:3/16:9
on/off restores original Palace images exactly. Existing cloud shadows were
audited separately with four on/four off flights; their measured ~2.65 ms
CPU presentation cost is a quality-toggle tradeoff, not a claimed optimization.
Three additional eight-flight navigation series retained a small fast-path
cleanup and rejected a more complex cache layout; the Palace-capable renderer
still adds approximately .14 ms / 1.8% CPU presentation time in the retained
comparison. All 19 flight images remain exact; noisy runs are preserved.
See the code-audit document for scope, evidence and remaining platform checks.

Palace visual revision (2026-09-08): the stretched sky-noise deck is replaced
by optional lit density-volume clouds, with a cheap single-layer fallback.
The gradient reaches pale blue at the visible horizon; an atmosphere-gated
mist band and correct premultiplied cloud composition soften the view.
Existing portable shader/depth contracts are reused, without a runner ABI
change. All 148 tests pass; the sanitized Metal sweep retains the 246
navigation references exactly. Three serial runs per cloud quality mode
measure about 6.6 ms CPU presentation here; the .02 ms median difference is
smaller than run noise, not a GPU/FPS guarantee. Native screenshot:
`runs/20260908-151754/sky-palace-volumetric.png`.
The passing-cloud follow-up fills the middle/lower view with two staggered
drifting decks in front of the globe and behind the angel/palace. The small
color-corrected motion preview is
`runs/20260908-153916/passing-clouds-corrected.gif` (575 KiB).

Palace framing/color follow-up (2026-09-08): a Palace-only camera adjustment
lowers the globe, leaves more sky around the angel and retains authored scale
and safety bounds. Rich indigo-blue now reaches the visible windows; a narrower
horizon veil reduces color wash. The native daylight comparison is
`runs/20260908-155946/daylight-comparison.gif` (571 KiB, full-frame palette),
with true 16:9 inspected separately. All 148 CTests and the sanitized Metal
sweep pass; all 246 navigation references remain exact. Six counterbalanced
checkpoint/refined runs measure a small repeatable +0.239 ms median CPU
presentation cost (3.6%), retained for the revised framing. Details and all
run evidence are in `world-navigation-code-audit.md`. The later ActRaiser
2–inspired orbital A/B remains an isolated prototype in `runs/20260908-162052/`;
the user chose the daylight base.

Selected-town framing (2026-09-08): the globe now rotates the captured selected
region's raised centre near the visible horizon above the Palace menus. The
daylight camera/colors, authored relief and native travel state are unchanged.
Three repeating upper banks add high-window cloud coverage through existing
atlas, depth and quality controls. All 148 CTests pass, plus final focused and
sanitized native checks; all 246 navigation references remain exact. Six serial
checkpoint/current runs measure +0.781 ms median CPU presentation (11.2%) for
the extra visible content, retained as an explicit visual-cost tradeoff.
Native 4:3/16:9 and all six towns were inspected. Preview:
`runs/20260908-164237/selected-town-daylight.gif` (601 KiB).

### Quality-of-life enhancements

These items improve or extend the original game and are tracked separately from
core compatibility.

| Enhancement | Status | Current state and next step |
|---|---|---|
| Native audio improvements | 🟡 | Native audio is confirmed working. Further work is investigating ways to reduce music or sound-effect drops caused by the SNES channel limits. |
| Bridge capacity extension | 🟡 | The save-compatible bridge restoration path is implemented. Capture the remaining visual acceptance result. |
| Aitos windmill behavior | ✅ | The no-wind event stops all windmills, the Wind miracle restarts them, and the 3D presentation follows the intended state. |

## Future enhancements

- Use the standalone action editor for another Diorama authoring pass, with the
  goal of further improving room composition and effects beyond the currently
  accepted presentation.

## Release completion criteria

The project is ready for release when:

1. every action route and simulation town has a recorded result with the
   intended presentation modes;
2. every release-scoped 🟡 subsystem above has either passed its acceptance
   gate or has a clearly documented release decision;
3. the full-game enabled/disabled presentation matrix passes without gameplay,
   save, audio, or transition regressions; and
4. each advertised platform has a launch-tested distribution bundle.

## Detailed technical references

- [Rendering engine and Diorama architecture](rendering-engine.md)
- [Simulation architecture and integration seams](SEAMS.md)
- [Player settings and asset replacement](manual.md)
- [Save format](save-format.md)
- [Game documentation index](README.md)
- [ROM map](rom-map.md), [RAM map](ram-map.md), and
  [research symbols](research-symbol-map.md)
