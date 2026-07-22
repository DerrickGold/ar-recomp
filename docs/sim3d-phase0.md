# SIM 3D Phase 0 evidence

This is the live implementation companion to
`ar-recomp-sim-rendering-plan.md`. It records what has been proven by executable
fixtures; planned checkpoints are not marked complete until their runner passes.

## Implemented foundation

- `kActRaiserWram_SimMapPickerFlag` names `$7F:9215` in the full 128-KiB WRAM
  mirror. `ActRaiser_SimMapPickerActiveForState` applies the six-town scope and
  full-word nonzero test.
- `kActRaiserWram_SimPendingWorldType` names `$7F:7CA1`. It corroborates picker
  activity but is not a complete operation enum: the still-unclassified
  `$01:93DC` path stages `$000B`; Direct the People's on-screen Building
  Direction picker and targeted miracles both stage `$0009`.
- `AR_SIM3D_TRACE=<path>` writes one JSON object per distinct rendered SIM
  state. It includes picker/view state, pending type, live SIM target
  (`$0AEE/$0AF0`), confirmed aimed cell (`$7F:90E1/$90E5`), camera,
  Mode-1/color-window registers, scroll state, and overlay captures. It is
  read-only and disabled by default.
- `AR_SETTINGS_PATH=<path>` permits a replay to use a named settings fixture
  instead of the live `settings.ini`.
- `tools/sim3d_demo.py` runs bounded checkpoints from isolated working
  directories. Each run copies or decodes its exact SRAM seed, verifies its
  SHA-256, and uses a separate narrow function-trace pass to identify picker
  routines without perturbing the visual-state pass.

Run one checkpoint or the complete suite:

```sh
python3 tools/sim3d_demo.py --checkpoint D0-fillmore-actions
python3 tools/sim3d_demo.py --all
```

Artifacts are written beneath `runs/sim3d-checkpoints/`. The suite writes a
single `coverage.json` linking every individual report.

## Passing coverage

| Checkpoint | Proven coverage |
|---|---|
| `D0-fillmore-idle` | ordinary Fillmore town frames with no picker |
| `D0-fillmore-picker` | Direct the People / Building Direction via ROM entry `$01:972F` |
| `D0-fillmore-repeat-picker` | Direct the People / Building Direction followed by two targeted miracles via `$01:9754` |
| `D0-fillmore-actions` | Direct the People / Building Direction, all five targeted-miracle picker calls and resolved kinds `1-5`, Napper ground-pluck, Blue Demon lightning, grounded people, and ground fire |
| `D0-fillmore-wide-full-edge` | widescreen map-edge behaviour |

Phase 0 is the scope of *this* document. Later phases add their own checkpoints
to the same manifest and runner; their results are recorded in
`ar-recomp-sim-rendering-plan.md` rather than duplicated here:

| Checkpoint | Phase |
|---|---|
| `D1-metadata-actions` | semantic record metadata and OBJ atlas |
| `D2-flat-actions` | pitch-zero separated recomposition |
| `D3a-ground-projection` | ground projection |
| `D3b-object-billboards`, `D3b-wide-hud-handoff` | world billboards, HUD handoff |
| `D3c-virtual-height`, `D3c-height-variations` | classified heights and easing |
| `D4a-hard-shadows` | ground-only hard shadows |
| `D4b-soft-shadows` | shadow-mask blur and the directional light |
| `D4c-rim-light` | lit edge on billboard silhouettes |
| `D2-margin-object-exit` | D2 byte-exactness when an object exits into a collapsed widescreen margin (ledger §23) |
| `D5a-world-underlay` | world-map underlay + full-town canvas; asserts the D2 gate stays at zero mismatch, so the extension is present-side only |

The checkpoint asserts the five ROM routine entries at `$01:9754` and the exact
resolved miracle-kind set `1-5` from `$7F:90EB`. The current `sim-actions.rec`
trace also contains these stable special-object facts:

- Napper ground-pluck: `$E71B/$E73A/$E75E`, record type `$13`;
- Blue Demon building lightning: `$E1BD/$E209/$E255`, record type `$12`;
- grounded people observed across `$E676-$E6B5`, record type `$19`;
- ground fire: `$E6CA/$E6D0/$E6D6`, observed on `$10/$12/$13` records.

Run `python3 tools/sim3d_demo.py --all` for the current suite result; it writes
one `coverage.json` linking every individual report. Pinning a specific past
run here goes stale on every added checkpoint, so the command is the reference.

## Coverage model and remaining gap

Town commands, pickers, camera behavior, layer machinery, and miracle systems
are shared. They are proven once in a canonical town. Enemy/effect validation
is variation-based and can come from whichever town naturally spawns the
required family. The remaining five towns need lightweight artwork,
palette/layer-role, and final-composite smoke frames—not duplicate recordings of
the same mechanics.

Direct the People is covered by the first picker in `sim-actions.rec`. A visual
frame confirms the game's on-screen command is named `Building Direction`, and
its ROM entry is `$01:972F`; treating that command and Direct the People as two
separate operations was an earlier classification error.

The `$01:93DC`/pending-type-`$000B` picker is a distinct, currently unclassified
position-placement path and was not observed in this recording. It uses the same
semantic `$7F:9215` authentic-view switch, so it does not block the rendering
contract, but its gameplay command should be identified before calling the full
picker catalogue complete. No additional Direct the People, targeted-miracle,
Napper, Blue Demon, or fire recording is required.

Every future `.rec` fixture must preserve its boot SRAM and gameplay-affecting
settings. A recording without those inputs is not deterministic: live save
editor changes can make the same controller stream enter different menus while
still appearing superficially valid.
