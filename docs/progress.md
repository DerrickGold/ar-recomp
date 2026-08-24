# ActRaiser Recomp — Project Status and Roadmap

Last updated: 2026-08-23

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
| World-navigation 3D | ✅ | The current top-down world presentation is confirmed complete. No further work is planned for the current roadmap. |
| Build and platform targets | 🟡 | macOS arm64 and Steam Deck bundles are confirmed end to end. macOS x86_64, generic Linux, and Windows bundles build but still need representative launch testing; signing and notarization remain release work. |

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

- [Widescreen survey](widescreen-survey.md)
- [Action background coverage](bg-hle-census.md)
- [Rendering engine and Diorama architecture](rendering-engine.md)
- [Action room and editor architecture](action-room-loader-hle.md)
- [Simulation architecture and integration seams](SEAMS.md)
- [Effects implementation](effects-hook-investigation.md)
- [Native audio channel architecture](snes-native-audio-channels.md)
- [Settings system](settings-system.md)
- [Save format](save-format.md)
- [Build and packaging](BUILD_TOOLING.md)
- [ROM map](rom-map.md), [RAM map](ram-map.md), and
  [research symbols](research-symbol-map.md)
