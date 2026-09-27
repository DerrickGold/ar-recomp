# Regional HLE integration audit — September 27, 2026

The preceding sprite-allocation and Fillmore boss-clear fixes were committed as
`1c402445` (`Fix extended-view sprite allocation and action-clear returns`).
This audit repaired native callers bypassing registered HLE entries, stale packed
CPU flags in six HLE owners, and missing exact-width continuations at authored
function ends. The closeout checks below passed for these changes. Remaining
static coverage debt and a pre-existing last-town fixture failure are recorded
separately; this is not a claim that every possible game path has been proved.

## Scope and method

The inventory below covers all 220 game-owned `hle_func` / `hle_func_if`
registrations, including 170 entries in regional-policy owners and adjacent
integration modules. The two `hle_spc_upload` registrations are also checked
by the generator-boundary audit. Shared nonregional hooks are included because
the generator repair applies to them too.

The review followed each regional entry's predicate, policy capture, native
continuation, stack ownership, register/flag effects, bounded memory writes,
and failure/escape handling. It also covered the supporting policy helpers,
SIM-menu command calls, room-boundary snapshots, native save transactions,
asset loaders, and randomizer composition. Existing tests were run, including
optional comparisons with all five locally available regional ROMs. New tests
exercise previously missing CPU-flag and generated-dispatch conditions.

These are different levels of evidence: ROM signatures and differential tests
validate the cases they encode; helper tests often substitute native callees;
the ROM-free compiled fixture executes the real return dispatcher; the local-ROM
caller fixture runs nine actual hooks and their generated callers; and the game
fixtures cover clear, retry, room, SIM and save/Continue transitions. This is not an exhaustive
playthrough of every stage under every combination of regional settings.

## Findings and repairs

### 1. The original boss-clear freeze was a game-side return-ownership error

Commit `b2e644c814777ff2c5a63ac28c955f6805c7d10a`
(`feat(regional): add recoverable town conversion and arrival rules`) introduced
the `$00:A343` interception and its direct nested native call. At this point
the object's pushed RTS continuation still belongs to its enclosing native
activation. Adopting the current stack pointer as a fresh host return boundary
skipped the object-loop cleanup and its PLP. Execution resumed with M=0 where
M=1 was required; the logged missing-width variants were downstream symptoms.

The committed repair queues a native tail transfer with the inherited return
context. Japanese departure likewise reaches a native RTS instead of manually
popping and returning. The Palace sibling at `$01:861E` has a real JSR frame
(caller `$8594`, pushed return word `$8596`); its delegation does not have the
same ownership problem.

The original failure reproduced at vertical extensions 0 and 64 with only
the departure implementation changed. That isolates this freeze from the
sprite-allocation issue. The attribution uses source history and a controlled
implementation comparison, not a complete historical-build bisect.

### 2. Registered regional hooks could be bypassed inside generated functions

Before this audit, 96 emitted native blocks crossed one of nine HLE entry PCs
without entering its wrapper. This count includes alternative width/status
decodes, not 96 independently demonstrated gameplay paths.

| Hook | Entry | Emitted bypass copies |
| --- | --- | ---: |
| Collision birth | `00:969E` | 2 |
| Antlion volley | `00:C718` | 28 |
| Antlion decision | `00:C721` | 28 |
| Emitter cadence | `00:B3E4` | 1 |
| Fire bounce | `00:C40A` | 3 |
| Recovery drain | `01:B257` | 3 |
| Checkpoint retry | `00:981C` | 1 |
| Score/lives | `00:873C` | 2 |
| Wizard pause | `00:BE78` | 28 |

Authored function ends and ordinary sibling boundaries did not cover every
fall-through, conditional branch, backward branch, or BRK continuation. A
helper could therefore pass its unit tests while generated callers still ran
the original instruction sequence instead of the selected regional policy.

The decoder now treats configured HLE entry PCs as hard boundaries when reached
from another function root. The emitter supplies these boundaries both from
parsed configuration and from direct `EmitBank` options. A conditional HLE's
own root retains its native fallback; internal resume hints cannot override
an interception. BRK cleanup retains an external HLE continuation rather than
discarding it because its instructions were deliberately excluded.

Generated sources were regenerated through `snesbuild`, not edited by hand.
The post-regeneration scan finds all 222 wrapper roots and no cross-root
inline bypasses. The new decoder
test covers incoming-edge kinds and native fallback preservation. The emitter
tests cover all three configuration forms and compile synthetic code against
the real runtime, testing predicate on/off behavior and the PHP/REP/pushed-RTS/
PLP pattern involved in the original freeze.

This is a separate recompiler/HLE integration defect; it does not change the
controlled attribution of the reported Fillmore freeze to the departure hook.

Closing the boundary audit also exposed an older discovery/emission mismatch.
Variant discovery ignored authored function ends while emission honored them.
An alternative immediate width can finish an instruction **past** that byte
end, requiring a separate continuation at its actual next PC. Discovery now
uses the same end and records those exact PC/M/X exits, subject to the existing
ROM/data exclusions. It neither aliases widths nor treats a speculative decode
as proof of gameplay reachability. A regeneration regression checks both valid
continuations and data exclusions; compiled execution checks both widths.

The four initially added trap sites (`00:AFDB`, `00:B3E4`, `00:DADD`, `02:BC8A`)
are resolved by this change. See the static coverage accounting below.


### 3. Six modules could overwrite live flags from stale packed P

Generated arithmetic can update the CPU's flag mirrors while leaving packed
`P` stale. Editing `P` and calling `cpu_p_to_mirrors()` then overwrote flags
that the emulated instruction was supposed to preserve. Town census had the
same issue when saving `P` to emulate PHP and later restoring it with PLP.

| Module | Affected operation |
| --- | --- |
| Stage terrain | Terrain setup, initial position, checkpoint position |
| Stage hazards | Hazard-count load |
| Stage placements | Object initialization and placement-list exits |
| Scene music | Track selector load |
| Regional mosaic | Preserve V while updating N/Z/C |
| Town census | Preserve the live status across PHP/PLP |

Each now packs the live mirrors before modifying or saving `P`. Regression
tests deliberately disagree packed C/V with their live mirrors, then verify
the native flag contract. The original terrain, hazards, music, mosaic and
census implementations fail those regressions. Placement coverage also checks
the flags received by the native object initializer.

### 4. A regional test fixture had fallen behind its runtime dependencies

The regional runtime test failed to link after the world-resume integration:
its fixture lacked `g_ram`, `g_settings`, and `ActRaiserWorldResume_Begin`.
The fixture now supplies the globals through their owner headers and a checked
stub. World-resume behavior remains covered by its own tests. This was a test
integration failure, not evidence of another gameplay failure.

## Contract review results

| Area | Contract checked and result |
| --- | --- |
| Native returns | Branch-only prefixes retain the inherited return owner. Complete synchronous delegates keep their native JSR/JSL frame; helper calls that require a new frame use `ActRaiserNativeCall` or the bounded native-leaf helper. The A343 coroutine departure was the demonstrated exception and is repaired. |
| Transactions | Construction, town status, level awards, miracles, reports, earthquake, lair accounting and save operations capture policy at their owning boundary. Descendant hooks inherit that transaction. Native escape results propagate; postprocessing is guarded where required. |
| Live policy changes | Action settings latch at room entry; event-specific SIM settings latch at their event boundaries. Save/continue and population conversion use campaign/session ownership rather than independently changing part of a running operation. Existing mixed-policy and activation tests pass. |
| CPU ABI | Entry predicates check the required widths, bank/direct-page state and actor/room signatures. Loads preserve unrelated flags; comparisons and arithmetic replace only their specified flags. The stale-P defects above are repaired. |
| Memory and object ownership | Placement batches preflight capacity, including reserved controllers; actor overrides check descriptor/slot ownership; expanded animation programs use bounded native workspaces. Allocation-failure, malformed-data, reused-slot and rollback cases pass. |
| Rendering and assets | Terrain, hazard, room graphics/video, title/town artwork and inventory DMA retain their native continuation and loader contracts. Regional numerical animation/collision changes retain the expected US-resident compositions or use explicit compatible projections. The sprite builder prioritizes native-height objects and updates activation even after OAM fills. |
| Regional data | Optional ROM checks validate source instruction windows, tables, programs, compositions and selected CPU effects for US, Japan, English PAL, German and French releases. Host policy tests additionally exercise independent mixed settings. |

Direct native delegation is not intrinsically incorrect. The distinction is
whether the HLE intercepts a complete call or an interior continuation whose
stack belongs to an earlier activation. Prefix-only delegates such as the
population selector, compass rejection suffix and lives-display suffix were
also inspected for their surrounding stack cleanup; no second occurrence of
the demonstrated A343 failure was established. Stub-based tests alone are not
proof of every possible native return path.

## Closeout verification

- `make check-release` passes: **307/307 Debug and 307/307 Release tests**,
  including all five previously unavailable GPU/shader tests. All three Go
  modules and `go vet` pass. `make check-quality` passes using the existing
  pinned Python environment. Five unrelated packed-statement violations in the
  SIM-menu test were fixed in the separate formatting commit `74281667`.
- Shader-header regeneration now passes for **all 21 headers**, using the
  existing local DXC 1.9 build at `/tmp/actraiser-debt-tools/dxc-build/bin/dxc`.
  MSL/SPIR-V match exactly; DXIL passes the existing semantic comparison with
  only the compiler build identity differing. The initial audit skipped this
  check because DXC was not on PATH. Runtime shader/GPU tests also pass; this
  does not certify Windows packaging or presentation on a D3D12 device.
- The optimized game builds after regeneration. The final scan finds **222/222
  registered roots, zero missing roots and zero cross-root inline bypasses**.
- All 22 optional five-ROM suites and two Japan-specific arrival/SIM-AI suites
  pass. Nine focused ASan/UBSan tests pass for the affected game-side owners,
  arrival and sprite allocation. These are contract/reference checks, not five
  complete regional-ROM playthroughs.
- The ROM-free native-dispatch fixture covers fall-through, conditional/backward
  branches, BRK continuation, predicate fallback, exact-width end continuations,
  and 128 PHP/REP/pushed-RTS/PLP status combinations.
- The reusable local-ROM fixture passes **99 cases across all nine affected
  hooks**, with US/JP/PAL policy values and inactive-campaign fallback where
  applicable. It checks routing, A/X/Y/P/S, PB/DB/D, selected memory effects, the PAL
  extra-life sound request, and a bounded WRAM write footprint. It links the
  production generated functions and native callees; it does not edit generated
  C or execute substitute native helper bodies. The pre-audit generated-code
  control fails this fixture at the recovery-drain hook, confirming that it
  detects a real bypass. The PAL COP request is counted without a host audio
  device; unexpected BRK execution fails the fixture.

| Hook | Real generated caller used by the local-ROM fixture | Observable contract |
| --- | --- | --- |
| Collision birth | `00:95ED`, including native `8E2F` | JP collision top 31 versus native 32; initializer state, flags and stack retained |
| Emitter cadence | `00:B3D8`, including native facing helper | US `A=2402`; JP/PAL `A=2401`; balanced helper frame |
| Fire bounce | `00:C408` | Exact compare flags around thresholds 210 and 242 |
| Antlion decision | `00:C71E`, including native distance helper | Near/far boundary 64; JP far path owns its delay and resume fields |
| Antlion volley | Native `00:868F` RTS with prepared continuation | JP skips the post-volley animation; native fallback enters it |
| Wizard pause | Native `00:868F` RTS with prepared continuation | JP skips the wait; native fallback passes 30 to the wait helper |
| Checkpoint retry | Native `00:868F` RTS with prepared continuation | Marker consumption, JP score clearing, native/inactive fallback |
| Recovery drain | `01:B252`, including native motion and recovery callees | Queue cadence, JP eligible-call counter, width and saved PHX frame |
| Score/lives | `00:8892` and its native JSR | BCD 1999→2000, PAL life/sound award, original score in A and preserved P |

The prepared RTS cases reproduce the native scheduler's pushed continuation
and inherited return owner. Prefix probes terminate at their specified native
continuation; they are not full actor lifetimes. Some of the original 96 bypass
copies were speculative alternative decodes of data or otherwise unproved
entries. Testing a hook through a valid caller does not assert that every old
copy was reachable in gameplay.

The local-ROM test can be repeated after building the `play` preset:

```sh
python3 tests/regional_hle_generated_test.py --build build-release --rom ar.sfc \
  --output runs/regional-hle-callers
```

The fixture files are `tests/regional_hle_generated_test.py` and
`tests/fixtures/regional_hle_callers.c`. The ROM must match the generated game
build. This optional integration test is separate from the ROM-free CTest gate.

### Gameplay and persistence matrix

The isolated fixtures use copied saves and controller input, with explicit
triggers to reach the transitions under test. They select the audited rule
families through the game settings API before action entry or SIM events;
these are US/JP/PAL values for those families, not whole foreign-ROM execution
or every combination of all settings.

- **Fillmore clear:** six runs, three rule sets × vertical extensions 0 and 64.
  All enter native boss death, tally and departure and reach town without a
  missing-width warning or fatal session. JP uses an arena-position fixture
  because its changed terrain makes the US walk diverge; the boss death and
  departure code remain native. Earlier GPU acceptance shows the intact statue.
- **Death/retry:** all three rule sets return to live Fillmore gameplay after
  zero-HP death with remaining lives. JP changes score `4567→0000`; US/PAL
  retain `4567`. The native retry marker is consumed.
- **Room transitions:** all three rule sets run native loads into Fillmore rooms
  2 and 3 with extension 64 and finish in room 3 without traps.
- **SIM:** 22,000-frame runs exercise active-town and offscreen construction in
  all three rule sets. JP also executes the construction-payment override.
  Recovery policy activation, miracle commands and earthquake return normally.
- **Native save:** three private SIM runs invoke the native story writer through
  its JSL contract. All return normally with the original stack, produce SRAM
  plus a regional companion, and leave the host execution state restored.
  This is an injected save trigger, not a claim of testing every save-menu path.
- **Cold Continue:** fresh processes load those three saves, restore their
  recorded policy values and enter Fillmore. JP starts with time 200; US/PAL
  with 300. The saved score is retained and the source SRAM stays unchanged.
  Controller fixtures are used because legacy replay files correctly reject
  non-native policies without initial-state identity.
- The existing save-slot boot suite passes: prepared-slot boot, repeated soft
  reset, import, first-run setup, legacy adoption, Unicode paths and relocation.

### Visual comparison

Eight scenes pass the repository's strict pixel/WRAM comparison against a
control linked from the pre-audit generated output and pre-audit flag owners:
Aitos and Death Heim in flat/diorama modes, Aitos at vertical extensions 32 and
64, town actions, and voxel town. **All 92 composite captures and all eight
final WRAM dumps are byte-identical.** Other linked objects are shared, keeping
unrelated workspace changes out of this comparison.

The full corrected nine-scene command now exits successfully: **99 identical
composite captures and nine identical final WRAM dumps**.

The ninth `world-navigation` fixture initially failed on the **control**. Its
input at frame 391 left world navigation before the 1200–1800 capture window;
the later screenshots were Sky Palace. The revised replay holds navigation,
moves at frame 1300, and opens the Palace menu after capture, at frame 1900.
Both builds now pass the World 3D presentation prerequisite, **seven identical
composite captures and identical final WRAM**. A new `require_view` assertion
also checks the view at every capture, so a brief earlier World 3D visit cannot
satisfy this coverage requirement. The original failing replay is covered by
the ROM-free capture-view validator regression.

### Last-town restoration follow-up

This failure did **not** demonstrate a regression of `d3cc15b4` (last-town
restoration / Palace soft reset). The audit's completed Fillmore SIM seed had
saved regional states `[4,0,0,0,0,0]`: Northwall was still at Act 1. The test's
debug warp could visit Northwall and write a bookmark, but the production
restore correctly rejected a town unavailable in the durable campaign.

The boot test now rejects that precondition immediately, before launching the
game. It also checks that Continue actually completes. Raw legacy saves need
acknowledged regional history, because headless mode cannot accept the history
prompt. An optional campaign-test helper creates that metadata against a fresh
isolated copy, estimating stocks and reloads from the saved image. It never
changes the source save or unlocks towns to make the assertion pass.

With the developed Aitos fixture (Northwall state 2), **Northwall visit, cold
Continue, and Palace soft reset pass on both control and candidate**. Each
restart restores `town=6 focus=384,128`; the native save stays byte-identical.
The source fixture is `tests/fixtures/sim3d/aitos-voxel-seed.srm.b64`, decoded to
a temporary `.srm` file. Repeat with:

```sh
python3 tests/world_resume_boot_test.py build-release/ActRaiserRecomp ar.sfc \
  /path/to/decoded-aitos-seed.srm \
  --prepare-with build-tests-release/actraiser_regional_campaign_test
```

### Remaining limits

The static hard-stub census is **not clean**. The pre-audit generator reports
131 logical trap sites; the final generator reports **127**. Besides resolving
the four interim regressions, five former sites disappear (`00:983F`,
`00:D9DB`, `01:859C`, `01:85A2`, `01:89E5`). One newly visible site is
`03:E14F`, in the retained `E142 M0X1` body: the 8-bit-index decode consumes only
one LDY operand byte and then interprets the remaining `$DC` as an indirect
jump. Before this repair the dispatch registry had no `E13E M0X1` or
`E142 M0X1` body. The canonical X=0 code remains present. This is expanded
coverage of a previously absent speculative mode, not a demonstrated new
playable failure. Proving or eliminating the remaining speculative paths is
separate compiler-coverage work; no trap or width guard is suppressed here.

The follow-up census separates what those diagnostics mean; it does not remove
them or claim 127 demonstrated game bugs:

| Diagnostic | Sites | Reviewed interpretation / next proof needed |
| --- | ---: | --- |
| Dispatch domain guards | 7 | All have generated target tables. Explicit value tables are `01:B8C0` (26), `01:CD6A` (18), and `03:F5DF` (20); `01:E23D/E24C/E289/E298` have two-entry generated index tables. These guards diagnose values outside their table domains. A runtime hit requires checking the selector and table completeness, not blindly adding an HLE. |
| Direct call target traps | 50 | 49 target addresses are below `$8000`, outside the generator's LoROM code window. `22:9C21` maps beyond this 1 MiB ROM. Prove incoming decode widths / reachability; do not invent ROM bodies at these addresses. |
| Cross-function goto traps | 18 | All destination addresses are below `$8000`; 14 entries are the same `03:A237 → 03:6DA2` edge in different owning functions. These are 18 owner/site keys, not 18 unique instruction PCs. Incoming decode proof remains necessary. |
| Indirect missing-body guards | 52 | Emission first checks native return ownership and the live AOT registry. A guard's presence does not mean every execution traps. Prioritize observed missing targets; `03:E14F` is the reviewed speculative X=1 example described above. |

`tests/fixtures/regional-hle-trap-baseline.json` records every site and all
**502 emissions**, including owning function and decoded block-entry M/X
contexts (instructions within a block can subsequently change the live flags).
The census can export JSON while still failing its ordinary strict gate:

```sh
go -C snesrecomp-go run ./cmd/v2regen stub-census --gen-dir ../src/gen \
  --json /tmp/current-traps.json
```

An additional comparison checks for new sites, increased emissions, or newly
emitted CPU-width contexts, even if another site's removal hides the change in
the total count. It ignores source line movement and reports removed sites:

```sh
go -C snesrecomp-go run ./cmd/v2regen stub-census --gen-dir ../src/gen \
  --baseline ../tests/fixtures/regional-hle-trap-baseline.json
```

This reviewed baseline is **not a waiver of the strict regeneration gate**.
The ordinary census still exits unsuccessfully with 127 diagnostics, every
runtime guard remains intact, and no decode width is forced to another body.
Full reachability proof and any newly observed missing target remain compiler
coverage work. The baseline comparison is an opt-in local-ROM check; it is not
part of the ROM-free `make check` gate.

Local evidence is under ignored `runs/regional-hle-audit/` and
`runs/boss-clear-debug/`: inventories, boundary scans, trap comparisons, caller
results, isolated gameplay saves and logs, and GPU captures. ROM bytes and
generated C are not added to version control.

## Entry inventory

Addresses below are the US executable's interception sites. Regional source
addresses used for differential comparisons can differ. An inventory row
groups registrations by their implementation owner; supporting helpers and
policy/session tests are also part of the review described above.

### Regional owners and adjacent integration modules

| Implementation owner | Count | Entries |
| --- | ---: | --- |
| [actraiser_action_inventory](../src/actraiser/actraiser_action_inventory.c) | 6 | `00:879D`, `00:9EFC`, `00:96E3`, `02:AC20`, `02:AF3D`, `02:BCED` |
| [actraiser_action_motion](../src/actraiser/actraiser_action_motion.c) | 6 | `00:8E2F`, `00:B3CB`, `00:B3E4`, `00:C908`, `00:DADD`, `00:969E` |
| [actraiser_action_room_graphics](../src/actraiser/actraiser_action_room_graphics.c) | 2 | `02:B28E`, `02:B330` |
| [actraiser_action_start](../src/actraiser/actraiser_action_start.c) | 1 | `02:AB05` |
| [actraiser_action_video_config](../src/actraiser/actraiser_action_video_config.c) | 1 | `02:B4E8` |
| [actraiser_actor_stats](../src/actraiser/actraiser_actor_stats.c) | 4 | `00:FC96`, `00:FC99`, `00:FD2E`, `00:966C` |
| [actraiser_boss_rules](../src/actraiser/actraiser_boss_rules.c) | 15 | `00:A655`, `00:C67D`, `00:D980`, `00:D9DB`, `00:E878`, `00:E8B5`, `00:A65E`, `00:AFDB`, `00:C2CB`, `00:C2D1`, `00:C718`, `00:C721`, `00:BE78`, `00:E4DB`, `00:F8FC` |
| [actraiser_difficulty](../src/actraiser/actraiser_difficulty.c) | 4 | `00:966F`, `00:8A24`, `00:D766`, `02:BC8A` |
| [actraiser_fire_enemy](../src/actraiser/actraiser_fire_enemy.c) | 4 | `00:C3DD`, `00:C3EA`, `00:C405`, `00:C40A` |
| [actraiser_localization_schedule](../src/actraiser/actraiser_localization_schedule.c) | 5 | `01:8E29`, `01:8FC5`, `01:9278`, `01:9261`, `01:9099` |
| [actraiser_mode_entry](../src/actraiser/actraiser_mode_entry.c) | 8 | `02:A70D`, `02:A72D`, `02:A748`, `02:A751`, `02:A7E9`, `02:AAF9`, `02:AB00`, `02:AB03` |
| [actraiser_platform_skull](../src/actraiser/actraiser_platform_skull.c) | 2 | `00:D39A`, `00:D3A2` |
| [actraiser_save_transaction](../src/actraiser/actraiser_save_transaction.c) | 1 | `03:A656` |
| [actraiser_scene_music](../src/actraiser/actraiser_scene_music.c) | 1 | `02:B653` |
| [actraiser_score_lives](../src/actraiser/actraiser_score_lives.c) | 1 | `00:873C` |
| [actraiser_sim_menu](../src/actraiser/actraiser_sim_menu.c) | 5 | `01:8B7D`, `01:81D7`, `01:8CF0`, `01:8D92`, `01:8C43` |
| [actraiser_stage_hazards](../src/actraiser/actraiser_stage_hazards.c) | 1 | `00:940C` |
| [actraiser_stage_placements](../src/actraiser/actraiser_stage_placements.c) | 2 | `00:941C`, `00:9500` |
| [actraiser_stage_terrain](../src/actraiser/actraiser_stage_terrain.c) | 3 | `00:8329`, `00:933C`, `00:94B1` |
| [actraiser_statue_volley](../src/actraiser/actraiser_statue_volley.c) | 2 | `00:BD9F`, `00:BDA8` |
| [actraiser_town_census](../src/actraiser/actraiser_town_census.c) | 1 | `03:C07E` |
| [actraiser_tree_attack](../src/actraiser/actraiser_tree_attack.c) | 2 | `00:A9BF`, `00:A975` |
| [enhancements/actraiser_widescreen_sprites](../src/actraiser/enhancements/actraiser_widescreen_sprites.c) | 6 | `00:8D68`, `00:8C98`, `01:B4C6`, `01:ADAD`, `01:AE6F`, `01:B473` |
| [regional/actraiser_regional_arrival](../src/actraiser/regional/actraiser_regional_arrival.c) | 2 | `00:A343`, `01:861E` |
| [regional/actraiser_regional_construction](../src/actraiser/regional/actraiser_regional_construction.c) | 5 | `03:82DB`, `03:84B9`, `03:853B`, `03:8425`, `03:848E` |
| [regional/actraiser_regional_level_goals](../src/actraiser/regional/actraiser_regional_level_goals.c) | 5 | `03:E414`, `03:B3BA`, `03:B3C7`, `03:B3CD`, `03:B407` |
| [regional/actraiser_regional_mosaic](../src/actraiser/regional/actraiser_regional_mosaic.c) | 1 | `02:939C` |
| [regional/actraiser_regional_runtime](../src/actraiser/regional/actraiser_regional_runtime.c) | 49 | 49 entries expanded below |
| [regional/actraiser_regional_sim_ai](../src/actraiser/regional/actraiser_regional_sim_ai.c) | 7 | `01:BA67`, `01:BA6A`, `01:BB5C`, `01:BCBC`, `01:BD46`, `01:BEE3`, `01:BF72` |
| [regional/actraiser_regional_sim_combat](../src/actraiser/regional/actraiser_regional_sim_combat.c) | 5 | `01:B018`, `01:B0CC`, `03:813F`, `03:8168`, `03:B9EE` |
| [regional/actraiser_regional_town_status](../src/actraiser/regional/actraiser_regional_town_status.c) | 9 | `03:BF8C`, `03:91AE`, `03:91BC`, `03:8566`, `03:85A3`, `03:85C3`, `03:9271`, `03:BF9E`, `03:BFA2` |
| [regional/actraiser_title_art](../src/actraiser/regional/actraiser_title_art.c) | 3 | `02:B34D`, `02:B2D2`, `02:B4AB` |
| [regional/actraiser_town_art](../src/actraiser/regional/actraiser_town_art.c) | 1 | `02:B2C1` |

### Shared hooks covered by the generated-boundary scan

| Implementation owner | Count | Entries |
| --- | ---: | --- |
| [actraiser_action_metatile](../src/actraiser/actraiser_action_metatile.c) | 2 | `02:B90D`, `02:B95A` |
| [actraiser_action_room_loader](../src/actraiser/actraiser_action_room_loader.c) | 2 | `02:B363`, `02:B3EB` |
| [actraiser_bg3_upload](../src/actraiser/actraiser_bg3_upload.c) | 1 | `02:AEEB` |
| [actraiser_bugfixes](../src/actraiser/actraiser_bugfixes.c) | 3 | `03:9D9F`, `03:9CFB`, `03:89F0` |
| [actraiser_cell_map](../src/actraiser/actraiser_cell_map.c) | 4 | `03:96EF`, `03:9710`, `03:9FCD`, `03:9FE4` |
| [actraiser_localization_text](../src/actraiser/actraiser_localization_text.c) | 17 | `00:A4C3`, `00:88F7`, `01:9284`, `01:8CCE`, `01:8C79`, `01:9314`, `01:933C`, `01:935E`, `01:937D`, `01:9396`, `01:93A8`, `01:93B4`, `02:ABC4`, `02:BA41`, `02:AB30`, `02:BF60`, `02:C1B7` |
| [actraiser_lzss](../src/actraiser/actraiser_lzss.c) | 1 | `02:C5C9` |
| [actraiser_rtl](../src/actraiser/actraiser_rtl.c) | 3 | `00:8418`, `02:A85E`, `02:BC56` |
| [actraiser_save_checksum](../src/actraiser/actraiser_save_checksum.c) | 1 | `00:84F3` |
| [actraiser_sprite_upload](../src/actraiser/actraiser_sprite_upload.c) | 2 | `01:ACD9`, `02:ACA3` |
| [actraiser_town_lair_bits](../src/actraiser/actraiser_town_lair_bits.c) | 8 | `03:F46E`, `03:F479`, `03:F487`, `03:F497`, `03:F4DF`, `03:F4EA`, `03:F4F8`, `03:F508` |
| [actraiser_town_metatile](../src/actraiser/actraiser_town_metatile.c) | 3 | `03:9B5A`, `03:9C43`, `03:A591` |
| [actraiser_town_structure_steps](../src/actraiser/actraiser_town_structure_steps.c) | 2 | `03:A4A8`, `03:A4B8` |
| [enhancements/actraiser_action_camera](../src/actraiser/enhancements/actraiser_action_camera.c) | 1 | `02:B091` |

### Regional runtime registrations

| Entry | Adapter (`ActRaiser_Regional` prefix omitted) |
| --- | --- |
| `00:9DE1` | `ScrollCast` |
| `00:9843` | `MagicDedicated` |
| `00:9A6E` | `MagicAttack` |
| `00:981C` | `Retry` |
| `00:A754` | `ScoreCard` |
| `00:A30D` | `ScoreDeparture` |
| `01:85A2` | `Population` |
| `01:9EE7` | `SkullUse` |
| `01:9F80` | `SkullSkipWait` |
| `01:8916` | `SourceCollection` |
| `01:9CCE` | `SourceLifeKeep` |
| `01:9CF0` | `SourceMagicKeep` |
| `01:8AF5` | `Speed` |
| `01:8B18` | `SpeedPosition` |
| `01:8B59` | `SpeedRight` |
| `01:8C98` | `SpeedScale` |
| `01:899B` | `MasterReport` |
| `01:89EE` | `SkipScore` |
| `01:97E5` | `QuakePlayer` |
| `01:9840` | `QuakePosted` |
| `01:B257` | `RecoveryDrain` |
| `01:9C30` | `RecoveryMoving` |
| `01:9C34` | `RecoveryStopped` |
| `01:9460` | `Effect` |
| `01:948E` | `EffectVisuals` |
| `02:A622` | `Title` |
| `02:A79F` | `Continue` |
| `02:C280` | `LivesDisplay` |
| `03:E13E` | `StoryThreshold` |
| `03:EB35` | `StoryCompass` |
| `03:B7C6` | `LairSeed` |
| `03:B6BF` | `LairReduction` |
| `03:BADD` | `LairKill` |
| `03:BA42` | `LairMiracle` |
| `03:B4A6` | `LairHouse` |
| `03:D095` | `LairScore` |
| `03:B4B8` | `HouseUnits` |
| `03:D0B3` | `ScoreRoute` |
| `03:D0D4` | `ScoreConversion` |
| `03:B525` | `ScoreSubtract` |
| `03:A066` | `QuakeHouses` |
| `03:A144` | `QuakeFields` |
| `03:A1E8` | `QuakeClass3` |
| `03:A284` | `QuakeClass4` |
| `03:A2E3` | `QuakeClass5` |
| `03:8271` | `RecoveryCycle` |
| `03:872A` | `TownWait` |
| `03:E865` | `Fishing` |
| `03:8193` | `Development` |

The additional runtime SPC upload interceptions are `02:9964` and `02:9A56`.

### Optional regional-ROM suites run

`actraiser_action_inventory_test`, `actraiser_action_motion_test`, `actraiser_action_start_test`, `actraiser_actor_stats_test`, `actraiser_boss_rules_test`, `actraiser_development_test`, `actraiser_difficulty_test`, `actraiser_fire_enemy_test`, `actraiser_fishing_test`, `actraiser_mode_entry_test`, `actraiser_platform_skull_test`, `actraiser_quake_test`, `actraiser_recovery_test`, `actraiser_regional_mosaic_test`, `actraiser_scene_music_test`, `actraiser_score_lives_test`, `actraiser_stage_hazards_test`, `actraiser_stage_terrain_test`, `actraiser_statue_volley_test`, `actraiser_town_wait_test`, `actraiser_tree_attack_test`, `regional_placements_test`.

Japan-only: `actraiser_regional_arrival_test`, `actraiser_sim_ai_test`.
