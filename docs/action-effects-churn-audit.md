# Action environmental effects: code churn audit

Snapshot: `746cbc0f`, 2026-09-28.

The [2026-10-03 lifecycle audit](#projectile-lifecycle-audit-2026-10-03) below
records the later projectile ownership fixes and the complete capture inventory.

The recent growth mostly represents new effects and their validation. The main
maintenance risk is that stage capture, effect dispatch and presentation policy
continue accumulating in shared files. A focused consolidation pass is warranted
before expanding the environment work. This audit does not establish a rendering
performance regression, and does not recommend replacing the effects engine.

## Scope and measurement

The skybox compatibility changes were committed as `746cbc0f` before this audit.
The three focused projection, geometry and presenter test suites passed before
the commit. Unrelated working-tree changes were excluded and left untouched.

Counts below sum `git diff --numstat COMMIT^ COMMIT` for the six listed commits.
Intervening save, SIM and Builder commits are excluded. Production includes both
`src/` and `snesrecomp-go/runtime/`; tests and docs have their own categories.
Counts include comments and blank lines. Churn means additions plus deletions,
not net file growth. Git snapshots do not preserve the many uncommitted visual
tuning iterations, so these numbers cannot measure all trial-and-error rework.
Some commits also contain extended-view audits and support work.

| Commit / scope | Files touched | Added | Deleted | Production net growth |
| --- | ---: | ---: | ---: | ---: |
| `8f816754` Settings, forest and environmental foundation | 30 | 2,543 | 73 | 848 |
| `c37bee61` Fillmore cave, temple and tower | 36 | 3,438 | 102 | 1,407 |
| `ad620549` Bloodpool marsh moonlight and reflections | 28 | 1,819 | 42 | 1,081 |
| `a1d2b947` Marsh details and red water | 16 | 760 | 58 | 509 |
| `d2b6a9ee` Bloodpool castle and window lighting | 17 | 1,913 | 39 | 810 |
| `746cbc0f` Shared compatibility without BG2 plane | 13 | 428 | 32 | 180 |

Across 66 distinct files: **10,901 additions, 346 deletions, 11,247 changed lines**.

| Category | Added | Deleted | Net growth |
| --- | ---: | ---: | ---: |
| Production C, headers and runtime | 5,108 | 273 | 4,835 |
| Tests and fixtures | 3,570 | 45 | 3,525 |
| Documentation | 2,197 | 27 | 2,170 |
| Build, configuration and tools | 26 | 1 | 25 |
| Total | 10,901 | 346 | 10,555 |

About **52% of churn is tests and documentation**. Runtime growth is substantial,
but substantially smaller than the total diff suggests. The low deletion count
shows an additive development phase; it is not itself evidence of poor code.

## Where the growth is concentrated

File lengths compare `8f816754^` with `746cbc0f`. Touches count selected commits,
not individual edits within those commits.

| Production file | Before | Now | Net growth | Touches |
| --- | ---: | ---: | ---: | ---: |
| `src/action/action_effects.c` | 2,716 | 3,482 | 766 | 5 |
| `src/action/action_scene_effect_render.c` | 1,943 | 2,620 | 677 | 5 |
| `src/action/present_action_effects.c` | 734 | 1,007 | 273 | 6 |
| `src/action/action_bloodpool_effect_render.c` | 0 | 692 | 692 | 3 |
| `src/action/action_cave_effect_render.c` | 0 | 576 | 576 | 1 |
| `src/action/action_castle_effect_render.c` | 0 | 511 | 511 | 1 |
| `src/action/action_bloodpool_detail_render.c` | 0 | 248 | 248 | 1 |

The four dedicated renderers account for 2,027 lines, approximately 42% of
production growth: useful separation already exists. The three shared files
above account for another 1,716 lines, approximately 35%. Those shared files are
the better targets for reducing future change scope.

The three large effects test files are now 3,495, 3,546 and 1,079 lines. Their
clipping, depth, world-motion and failure-path assertions are valuable. Organizing
fixtures by effect family would help navigation; deleting tests to lower the line
count would not improve the implementation.

## Actionable findings

### 1. Give render-pass policy one owner

`PresentActionEffects_DrawDioramaPlane` (`present_action_effects.c:609`),
`DrawActionPlaneEffectFlat` (around line 807), and `PresentActionEffects_DrawFlatPlanes`
(line 933) separately encode layer selection, blend mode, alpha-mask readiness and
submission order. New effect families repeatedly require changes to these paths.
`PresentActionEffects_Bg1Dimming` (line 94) also interprets stage and effect identity
inside the presenter.

Introduce a small immutable pass description for shared blend, attachment and
mask policy, with explicit mode-specific handling. Keep source-camera selection
separate from draw order: shoreline mist follows BG1 coordinates but draws at a
different attachment slot. Flat winner masks and depth-ordered composition are
intentionally different; combining their complete implementations would be wrong.
Keep the direct masked-geometry path and the necessary backend fallback.

This is the highest-value consolidation for preventing future plane/skybox/flat
compatibility drift. It is an ownership problem, not a demonstrated bug in the
committed skybox fix.

### 2. Separate stage capture and simplify family dispatch

`action_effects.c` contains map-specific capture for forest, cave, temple, marsh
and castle alongside actor recognition and observer lifecycle. The environmental
entry point at line 2501 also owns forest generation directly. Move stage capture
beside the corresponding stage renderer, using a small validated capture context
and bounded append interface. Keep actor identity and lifecycle central.

`BuildSceneEffectList` (`action_scene_effect_render.c:2421`) has grown to about
176 lines of validation, counters, kind ranges and dispatch. Enum ordering now
implicitly defines several effect families. The same file still owns the forest
renderer. Extract forest geometry, then use explicit family metadata for the
common validation/dispatch rules. Preserve duplicate detection and capacity
checks; these are protective contracts, not disposable boilerplate.

The Bloodpool branch also searches the whole bounded effect list for the moon
for each applicable effect. Resolve that dependency once per build while retaining
the duplicate-moon rejection. This is avoidable repeated work, but with the current
16-instance limit it is not evidence of a major bottleneck.

### 3. Centralize existing shared geometry; keep artistic differences

`AppendSceneClippedTriangle` lives inside `action_scene_effect_render.c:1807`,
among forest implementation details, but several stage renderers depend on it.
`AppendBloodpoolSoftPatch` is implemented in the marsh renderer (line 30) and
called by the castle renderer (lines 431, 450, 480 and 485). Its name and home no
longer describe its ownership.

Move already-shared clipping and soft-patch primitives to a neutral geometry
module. Small ripple/diamond writers and falloff functions are further candidates,
but they have different clipping and visual behavior. For example, cave ambient
falloff uses a different exponent, and castle mesh diagonal choices affect fan
symmetry. Preserve those choices explicitly rather than forcing one visual style.
There is no need for a generalized particle framework or dynamic plugin system.

### 4. Make environmental data meanings explicit

`ActionEffectInstance.visual` means native actor identity, room/style identifier,
or landing-dust strength depending on kind. `source_mask` is documented as cave
anchors but also represents water strips and castle sources. These conventions
are followed today, but make new code and reviews harder to reason about.

Use named construction/access helpers or a bounded environmental payload to make
these meanings explicit without gratuitously enlarging every instance. Replace
magic family range assumptions with named metadata. Improve the dense positional
rows in `action_castle_sources.h` with named source types and clearer grouped
initializers. Retain authored coordinates and metatile witnesses: they correctly
tie effects to the actual art and reject incompatible maps.

### 5. Publish the resolved skybox capture origin

`PresentDiorama_Draw` (`present_diorama.c:281`) reconstructs finite-world clamping
to match `ActRaiser_PrepareSkyboxView` and `PpuRenderBackgroundViewLine`. Its comment
explicitly records that synchronization requirement. Future margin or capture
policy changes could update one side without the other.

Prefer publishing the resolved capture origin in frame metadata, or sharing the
pure coordinate-policy calculation across those boundaries. Preserve row-band
and frame-generation offsets. Address this before the duplicate-moon treatment
adds more skybox-specific policy. Current smoke checks do not demonstrate an
alignment failure; this is a concrete source of future drift.

## Runtime cost: measured bounds and remaining questions

The same native arm64 C compiler measured the baseline and current public
structures with `sizeof`. These are CPU storage sizes on this ABI, not GPU memory
measurements or timing results.

| Item | Before | Now |
| --- | ---: | ---: |
| Effect instance | 72 bytes | 88 bytes |
| Scene frame snapshot | 2,316 bytes | 20,180 bytes |
| Observer | 2,800 bytes | 4,096 bytes |
| Reusable scene render batch | 503,448 bytes | 708,964 bytes |
| Scene batch vertex capacity | 10,610 | 10,610 |
| Scene batch index capacity | 40,980 | 40,980 |
| Effect kind enum entries, including None | 28 | 53 |
| Available render-layer categories | 5 | 11 |

The scene batch increase is the 205,516-byte moonlight workspace, including its
800-by-224 CPU coverage mask. It is static retained scratch, not a fresh stack
allocation per draw. The snapshot increase is largely bounded occlusion data.
Those are reasonable costs at this scale, but adding a similar unconditional
payload for every future level would scale poorly. Track snapshot bytes and keep
mutually exclusive stage scratch reusable where practical.

More layer categories do not mean every frame performs eleven draws. Unchanged
geometry capacity also does not mean unchanged CPU/GPU time: active vertex counts,
coverage-mask work, repeated UV-band builds, target switches and transparent
overdraw still matter. Do not infer a bottleneck from line counts or buffer limits.
Before optimizing those paths, compare actual per-stage CPU/GPU times, draws,
target switches and populated geometry at the same viewport and settings.

This audit did not run a new runtime benchmark or validate Steam Deck/Vulkan or
Windows/D3D12 hardware. Existing direct geometry submission, cached resources,
bounded storage and portable renderer interfaces should be preserved. The latest
skybox fix adds no GPU surfaces, shaders or readbacks; its prior validation and
all-region smoke-check limits are recorded in `action-skybox-effects.md`.

## Recommended cleanup order and acceptance criteria

1. Extract stage capture and forest geometry in mechanical changes, preserving
   output and leaving native actor logic alone.
2. Consolidate pass metadata, shared geometry ownership and skybox source mapping.
   Make field meanings explicit at the affected boundaries.
3. Split test fixtures along those same boundaries and retain behavioral coverage.
   Add compositor-level coverage for plane, skybox-only, both, and hidden/alpha-zero
   plane fallback, including banded capture and failed uploads.

Use existing deterministic fixtures and small fixed-camera captures to verify
unchanged geometry, depth order, occlusion and scrolling. Run focused tests and
sanitizers after each meaningful change. For performance-affecting changes, compare
the same scenes and backend, especially masked flat draws and skybox band builds;
native Metal results alone do not close Steam Deck or D3D12 validation.

The target is fewer shared files requiring edits for the next stage, clearer data
contracts and stable performance. A large net line reduction is not the goal.
No production refactor or new visual behavior was implemented during this audit.

Local reproducibility artifacts (ignored): `runs/effects-churn-audit/measure.py`,
`metrics.json`, `sizes.c`, extracted baseline headers, and `sizes.txt`.

## Consolidation implemented after the audit

The follow-up preserves the authored effects and moves their implementation to
clearer owners:

- Forest, cave/temple and Bloodpool environmental capture now live in dedicated
  modules, with a private bounded read/append interface. Actor recognition and
  observer lifecycle remain in `action_effects.c`.
- Forest rendering has its own module. Shared triangle clipping and soft patches
  live in `action_environment_geometry.c`; the small per-vertex falloff operation
  stays inline. Castle source kinds are named, with all 32 authored rows unchanged.
- A single immutable pass table supplies attachment, blend, mask and ordering
  policy to flat and Diorama submission. Flat winner masks, intermediate-target
  fallback and depth-ordered composition retain their respective behavior.
- Explicit family membership replaces enum-range dispatch. Moon lookup happens
  once per relevant build and still rejects duplicate sources. A regression test
  also checks that unrelated passes do not reject a moon they never consume.
- Same-sized `environment_room` and `dust_strength` aliases clarify the captured
  payload. Source masks now document their meaning across effect kinds.
- Capture publishes the canonical skybox world origin in `FrameSlot`. A shared
  pure helper handles finite bounds for both PPU scanout and metadata publication;
  presentation no longer reconstructs that policy. Runner ABI records are unchanged.

| Shared file | Before cleanup | After cleanup |
| --- | ---: | ---: |
| `action_effects.c` | 3,482 | 2,765 |
| `action_scene_effect_render.c` | 2,620 | 2,176 |
| `present_action_effects.c` | 1,007 | 914 |

These shared files shrink by 1,254 lines. Including the new modules and headers,
total production size is effectively unchanged. Effect instance, scene snapshot,
observer and geometry workspace sizes remain unchanged; frame metadata adds one
32-bit skybox origin. No GPU resources, shaders, readbacks or per-frame heap
allocations are added.

Validation includes the focused capture/render/presenter/projection tests,
frame-generation and PPU pipeline tests, runtime PPU/ABI tests, sanitizers,
source/private-header/render-boundary checks, and arm64/x86_64 compilation.
Temporary geometry fingerprint instrumentation was removed after comparison;
the extraction matched all 6,695 baseline fixture builds. Subsequent pass cleanup
skips a previously constructed but unusable batch after mask-upload failure.
Native comparisons and final fingerprint results are recorded under
`runs/effects-consolidation/` (ignored).

Final checks found exact matches for 4,786 nonempty renderer batches and the 46
nonempty presenter builds retained after the failed-mask early exit. Native Metal
comparisons matched all 16 screenshots and 16 WRAM snapshots across eight cases:
forest skybox-only, forest plane-plus-skybox, hidden-plane fallback, cave skybox,
castle entry flat and plane-plus-skybox, marsh skybox, and Aitos lava skybox.
These are fixed-frame comparisons with isolated saves/settings, not complete
playthroughs. Direct warps into uninitialized castle/waterfall interior rooms were
rejected by the baseline, so those interiors retain fixture-based validation here.

This is a bounded consolidation pass. The large test files and the presenter's
small stage-dimming policy remain further organization opportunities. Their
existing coverage is retained. It does not add the duplicate-moon art treatment,
change lighting style, or establish Steam Deck/D3D12 hardware performance.

## Projectile lifecycle audit, 2026-10-03

Scope: every native object matcher in `ActionSceneEffects_CaptureFrameFiltered`,
the authored actor binding path, spell cohorts, and detached/environmental
emitters. The inventory is of **implemented enhancements**, not every projectile
in the original game. Parent-only WRAM mutations established capture failures;
they do not prove that every original enemy reaches each mutation during play.
Bloodpool Act 1 boss death also has a native replay reproduction.

### Complete native scene inventory

The source of truth is
[`action_scene_effect_rules.inc`](../src/action/action_scene_effect_rules.inc).
Its 24 entries declare recognition, room, ownership, dependency, and clock.
Original and Death Heim sources remain separately restricted to their rooms.

**Record** means the record's own identity defines the entire lifetime.
**Spawn** adds dependency validation on first admission to a new generation;
subsequent captures depend only on the child's identity and continuity.
**Attached** revalidates a continuing physical relationship every capture.
**Motion** freezes phase/age when position stops; **Gameplay** permits deliberate
stationary animation. These clock choices are explicit, independent of ownership.

| Registry family | Implemented effect / source | Previous dependency risk | Ownership / clock |
| --- | --- | --- | --- |
| Centaur | Fillmore Act 1 staff charge, lightning strike and floor impact (`AD45`) | Child signatures already independent; staff charge is on the boss record | Record / Gameplay |
| NorthwallMagic | Act 1 charge, falling magic and floor expansion (`E7C6`) | Already independent, including impacts outliving their falling parent | Record / Gameplay |
| MinotaurAxe | Thrown axe, including small flight artwork (`AF5D`, rematch `F6CA`) | Parent composition clearing or reuse rejected child | Spawn / Motion |
| FlamingWheelBody | Boss body emitter (`D838`, rematch `F712`) | Own boss flag intentionally gates the body | Record / Gameplay |
| FlamingWheelShot | Five launched directions from that boss | Parent boss flag, composition, or slot reuse rejected child | Spawn / Motion |
| IceDragonBall | Northwall Act 2 ice balls (`F161`, rematch `F760`) | Parent composition clearing or reuse rejected child | Spawn / Motion |
| Tanzara | Final boss projectile/burst tuples (`F80F`) | Already independent; multiple stationary burst phases | Record / Gameplay |
| BloodpoolBossFireball | Act 1 boss fireballs (`B786`) and their smoke source | Previous fix tolerated death flags; complete parent reuse could still reject child | Spawn / Motion |
| BloodpoolFireball | Act 2 ordinary/statue fireballs (`BD76`, `BD84`) | Already independent | Record / Motion |
| FillmoreOrb | Act 2 statue red orbs, maps 2–3; map 3 is room 2 (`B3BF`) | Parent retirement, state change, or reuse rejected child; merely scrolling the statue out did not | Spawn / Motion |
| MarahnaOrb | Unsplit orb (`E047`), including intentional hold poses | Already independent | Record / Gameplay |
| MarahnaSplit | Four cardinal children of that orb | Revalidated an exact **retired** parent pose; reuse rejected child | Spawn / Motion |
| MarahnaSnake | Snake-launched fireballs (`DE96`) | Parent death handler, retirement, composition change, or reuse rejected child | Spawn / Motion |
| MarahnaLink | Horizontal/vertical lightning between endpoints (`E18E`, partner `E254`) | Relationship is intentional: both endpoints and midpoint must remain valid | Attached / Gameplay |
| ViperBody | Marahna boss charge and orb on the boss itself (`E483`, rematch `F72A`) | Body phases correctly end with their own record/pose | Record / Gameplay |
| ViperBolt | Launched diagonal lightning from that boss | Mutable parent attack state/handler and liveness rejected child | Spawn / Motion |
| ViperGround | Moving floor charge from that boss | Required parent to remain in its exact post-impact state | Spawn / Motion |
| AitosLava | Rising, held, then falling lava fireball (`CF9E`) | Already independent; zero-speed hold is intentional | Record / Gameplay |
| AitosStatue | Statue flame growth and breath (`D5B1`, `D5C0`) | Effect belongs to the stationary flame record itself | Record / Gameplay |
| AitosRock | Molten rock launch (`CEEC`) | Already independent; native launch reuses the mouth's own slot | Record / Motion |
| AitosSword | Act 1 dragon's two diagonal sword crescents (`D646`) | Revalidated a retired launch controller **and** a live boss root | Spawn / Motion |
| PlayerSword | Player extended sword beam (`979A` measured player family) | Revalidated the player's current liveness/source/composition | Spawn / Motion |
| BloodpoolTrap | Act 2 lightning trap (`BD2A`) | Already independent | Record / Gameplay |
| WizardLightning | Bloodpool Act 2 strike and floor impact (`BDFF`, rematch `F6E2`) | Parent retirement/clearing/reuse rejected child; impact's parent is the strike | Spawn / Gameplay |

The 12 Spawn entries are the paths that previously consulted mutable parent
contents on every capture. The change preserves their initial identity checks
without allowing the parent to revoke an admitted child's lifetime.

### Other capture and emission contracts

| Path | Lifetime owner and handling |
| --- | --- |
| Authored actor selectors / recipe instances | Actor generation owns age and phase. `actor-parent` is retained spawn provenance, not a lookup of the parent's current slot occupant. Parent death/reuse no longer resets the child or changes its selector. Child source, animation, bank, backlink, retirement, room change, or a large position discontinuity starts a new generation. Stationary authored body effects still animate. |
| Magical Fire, Stardust, Aura, Light | Explicit native magic controller and its cohort slots own the cast. These intentionally retire when that controller ends; they are not enemy-parent relationships. Existing spell lifecycle and pulse tests remain in place. |
| Bloodpool fireball smoke | Bounded detached puffs retain source address **and generation**. Moving-source smoke dissipates after detachment. A frozen trail follows its source freeze and disappears when that source disappears; slot reuse cannot inherit it. |
| Landing dust | Bounded contact events have a finite independent lifetime; depletion patches and room reset govern further emission. No live parent dependency. |
| Room/metatile decorations and environmental fields | Torches, lava surfaces, waterfalls, forest/cave/marsh/castle particles, water, moonlight, atmosphere, rays and other placed fields belong to the captured room/map or authored field. Their room clock and filters are intentional; there is no projectile-parent liveness lookup to latch. Actor-bound variants use the authored actor contract above. |

### Shared admission and enforcement

1. Every scene family enters through the registry. A matcher receives only a
   `const ActionObjectSnapshot *`, so it cannot read mutable parent WRAM through
   its API. Parent/endpoint validators are separate callbacks.
2. `AdmitSceneObject` applies the ownership rule centrally. Spawn proof runs only
   for a new generation; attached proof runs on every capture. A missing required
   dependency validator rejects admission.
3. Continuation requires the same family, own source/animation/backlink key, and
   plausible position continuity. Native inactive/empty records, rejected own
   signatures, room transitions, and observer resets retire history. Another
   projectile in the same renderer kind cannot inherit admission across families.
4. Motion clocks retain the last heading when a previously admitted, unchanged
   projectile clears velocity. This does not weaken admission of new stationary
   lookalikes. Gameplay clocks retain intentional stationary phases. Zero elapsed
   gameplay ticks never advance either clock.
5. Visibility remains separate from lifetime. A hidden native sprite emits no
   visible light; its identity can remain tracked for reappearance. The existing
   Bloodpool boss render-budget exclusion and independent smoke budget remain.
6. `TestSceneLifecycleContracts` iterates **every registry ID** and demands an
   independently seeded WRAM fixture. Adding a registry entry without that fixture
   fails the test. Fixture expectations for ownership and clock are not generated
   from the production policy, so declaring the wrong policy also fails.

The matrix checks unchanged-child survival across parent flag, handler, state,
inactive status, composition and slot changes; stable generation and clocks;
stop/clear-velocity/resume; child retirement and invalid identity; room change;
visibility; fresh admission; and WRAM immutability. It repeats the applicable
families in all five Death Heim rematches and includes separate Wizard impact
children. Authored-selector tests cover retained and initially unknown provenance.
Existing detailed artwork, direction, phase transition, scope, smoke, renderer,
presenter, capture and recipe tests complement the matrix.

### Boundaries of the guarantee

Spawn validation remains conservative on a cold observer. If a savestate is
loaded after the parent evidence has already vanished, the observer cannot prove
that an otherwise plausible child was previously admitted. It rejects that new
admission rather than guessing. Ordinary observer continuity survives parent
death and reuse; observer reset deliberately does not preserve those old proofs.

The native pool has no allocation generation counter available to this observer.
A slot that is destroyed and recreated between captures with identical observed
identity and position cannot be distinguished from uninterrupted existence.
Detected retirement, identity/backlink changes, family changes, room/reset
boundaries and implausible motion do prevent inheritance. Stronger guarantees
would require native allocation events, beyond this read-only capture layer.

For a new effect, choose its owner and clock in the registry, keep its signature
object-local, provide any birth/attachment validator, and add a measured fixture
with the intended lifetime expectations. A new phase that changes ownership must
have a distinct family entry; sharing a renderer does not imply sharing a lifetime.

### Validation of this change

The release game builds successfully. All seven focused ASan/UBSan tests pass:
native scene effects (including the complete lifecycle matrix), frame capture,
recipes, authored actor binding, immediate and deferred rendering, and presentation.
Compared with HEAD, changed C/header files introduce no style violations; new
registry/test includes have none. The repository-wide style command still fails
on pre-existing violations across the tree (including unchanged counts in touched
files); this change does not refresh that baseline. `git diff --check` is clean.

The native Metal Bloodpool death replay is saved under the ignored directory
`runs/bloodpool-act1-boss-fire/lifecycle-diorama-after/runs/20261003-204727`.
Against the previously fixed `death-diorama-after` replay, all **24 screenshots**
and **57 full WRAM snapshots** are byte-identical. This confirms preservation of
the working boss-death presentation and native behavior. Other families' parent
death/reuse scenarios are validated with captured-layout fixtures, not claims of
complete native playthrough coverage.
