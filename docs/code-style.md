# Code style and structure

House rules for the hand-written runtime under `src/`. Generated code in
`src/gen/` and `recomp/` is exempt.

These are defaults, not laws. Break one when there is a specific reason and
document it. The examples come from this repository.

**Contents**
- [Size budgets](#size-budgets)
- [One concern per translation unit](#one-concern-per-translation-unit)
- [Where data lives](#where-data-lives)
- [Constants and numeric data](#constants-and-numeric-data)
- [Put the decision on the thing it describes](#put-the-decision-on-the-thing-it-describes)
- [Enforce invariants with the compiler](#enforce-invariants-with-the-compiler)
- [Includes and folders](#includes-and-folders)
- [Comments](#comments)
- [Verifying a change](#verifying-a-change)
- [Current debt register](#current-debt-register)

## Size budgets

Measure **logic lines**: total lines minus comments, blanks, and data tables. Raw
length can mislead: `settings.c` contains a large cohesive descriptor table,
while `settings_overlay.c` contains coupled navigation and rendering logic.

| Unit | Budget | Hard limit |
|---|---|---|
| File | ~1,500 logic lines | past ~2,500, splitting is the default answer |
| Function | ~150 lines | past 400, treat as a defect |

The cost is not raw length but how much context a change requires. A large data
table is searchable; a large function has no smaller reasoning unit. Prefer
small functions in a longer file to a few enormous functions in a shorter one.

Exempt from the file budget, but say so in the file's header comment:
- generated blobs (`src/shaders/*.h`)
- a single cohesive data table (see below)
- one state machine that genuinely does not decompose

## One concern per translation unit

When a file carries two subsystems that only meet at a few call sites, that seam
is a file boundary. `present.c` was 4,551 lines because the flat/diorama present
path and the SIM 3D renderer shared a file while barely sharing code.

**Derive the split from usage.** Move a definition only when it has no remaining
consumer on the original side after the obvious movers leave, then repeat to
closure. This found sim-only helpers in `present.c` whose names did not reveal
their ownership while retaining shared helpers.

**Name the seam.** A split translation-unit pair gets an explicit
`<name>_internal.h` containing only what crosses: shared types, helpers that lost
`static`, and callbacks in the other direction. See `src/present_internal.h`.

- The public API remains in `present.h`; growth in the internal header reveals
  erosion of the split.
- It carries no live game state, preserving the D6 invariant below.

**Watch for declarations vs definitions.** When splitting, a block of `extern`
declarations for globals owned by a *third* file is duplicated into both halves,
not moved. Getting this backwards is a link error, which is the cheap failure —
the expensive one is a `static` that should have been shared.

## Where data lives

**Do not move data definitions into headers.** In C this is a trap with two shapes:

- A non-`static` definition in a header is a duplicate symbol the moment a second
  TU includes it — a link error.
- A `static` definition in a header silently duplicates the storage into *every*
  including TU. No error, no warning; just bloat and divergent addresses.

`src/shaders/*.h` use the `static const` form and are fine **only** because each
is included by exactly one `.c`. That is a standing condition, not a license —
if a second TU ever includes one, it must convert first.

**The correct split, when data deserves its own file, is a `.c` plus an `extern`
declaration in the existing header.** Never a definition in a header.

**Measure coupling before splitting.** Count the file-private symbols the data
references. Data with none is self-contained; data with many is coupled logic,
and moving it inverts the dependency.

For example, the 1,190-line `g_setting_descs[]` table references 75 private
parsers, formatters, callbacks, predicates, and label arrays. Splitting it would
export those symbols only to move the easiest part of the file to skim. Leave it
in `settings.c`.

Split data out when it is bulky *and* self-contained. Otherwise let it sit next to
the code that owns it.

## Constants and numeric data

Name a number when the name explains a decision or prevents two consumers from
drifting. Search for an existing owner before adding one: game geometry, save
cardinalities, timing units, host path capacity, and cross-subsystem boundaries
live in [`src/constants.h`](../src/constants.h). That header contains only
preprocessor constants—no functions, types, storage, or runtime policy—so it is
safe for low-level and pure modules to include. Hardware constants shared by the
standalone SNES runtime stay at that runtime's lowest shared layer; for example,
`kSnesWramSize` and its derived mask live in the pure-defines
`snesrecomp-go/runtime/src/runtime_constants.h`.

Prefer the narrowest owner that has every real consumer:

- Put a file-local layout or algorithm limit beside the code that enforces it.
- Put a subsystem contract in that subsystem's public header.
- Promote a value to `src/constants.h` only when independent subsystems share
  the same semantic fact. Equal numeric values with different meanings are not
  the same constant.
- Derive related values (`mask = count - 1`, bytes from element counts, time
  units from smaller units) instead of maintaining both literals.

Do not manufacture names for loop origins, ordinary `+1` indexing, mathematical
coefficients, or one-off bit shifts when the expression already explains itself.
ROM signatures, palettes, coordinates, font bitmaps, and similar exact data
belong in named descriptor tables or narrowly named classifier functions; their
individual cells do not each need constants. Test fixtures may retain explicit
expected values because hiding them behind production constants weakens the
test.

Run `make check-constants` after moving a canonical value. The check is
deliberately narrow and guards high-risk regressions—raw WRAM indexing, duplicate
WRAM extents, authentic-dimension redeclarations, and the shared SPC bank
boundary—without pretending a text search can judge every numeric literal.

### In code, or in a file on disk?

> Externalize what a **user or modder** should be able to change without
> rebuilding, or what a **build step** consumes to generate code. Keep in code
> what only a programmer can meaningfully change — especially anything the
> compiler currently checks for you.

This repo already follows that line: `settings.ini`, `diorama-layers.ini`,
`game-assets/manifest.ini` and the music manifest are all end-user surface, and
`recomp/symbols.toml` is build-time input that *generates* cfg directives. Both
kinds earn their file.

The settings **descriptor table** is the instructive counter-example, because it
looks externalizable and is not. Five of its columns — `field`, `available`,
`on_change`, `parse`, `format` — are code references, which is exactly the 75
file-private symbols measured above. Moving the table to TOML/JSON would not
remove a table; it would add two (a name→function-pointer registry and a
name→struct-offset registry), hand-maintained, to resolve what the file names.

The decisive cost is losing compile-time checking. A bad callback or enum is a
build error today; in external data it becomes a boot-time user error.

If localization becomes a goal, externalize only `label` and `tooltip`. They are
pure strings and can fail safely by falling back to built-in English.

## Put the decision on the thing it describes

A per-row property belongs on the row, not in a predicate that enumerates rows
somewhere else.

`Settings_UsesLegacyEnvironmentSyntax` used to be a ~60-clause chain of
`desc->field != &g_settings.<row>` comparisons a thousand lines from the table it
classified. Nothing was wrong with it — until someone added a modern setting and
never knew this function existed, at which point their setting silently inherited
the legacy `AR_*` parse. It is now `return !desc->modern_env;`, with the answer
stated on each row.

If adding a case requires an unrelated edit elsewhere, encode the decision on
the record instead: a field, table flag, or named constructor such as
`BOOL_SETTING_MODERN`.

Two mechanics worth reusing:
- Put a new struct field **last** when rows use positional initializers — the
  existing rows zero-initialize it and need no edit. This is how 258 descriptors
  absorbed a new field while only 60 rows changed.
- Mark exceptional rows with a **designated** initializer (`.modern_env = true`)
  so the value cannot land in the wrong field regardless of how many positional
  values that row supplies.

## Enforce invariants with the compiler

Where an architectural rule exists, make violating it fail the build rather than
fail review.

`present.c` and `present_sim3d.c` may not read live game state — every
present-time decision arrives through `const FrameSlot *`. This is enforced by
simply never declaring `g_ppu`, `g_settings`, `g_snes_width`, `g_ws_extra`, or
`g_active_pixel_aspect` in those TUs, so a stray live read is an undeclared-symbol
error. That is worth more than any amount of documentation, and it survived a
3,300-line extraction unassisted.

Reach for this pattern when an invariant is (a) important and (b) easy to violate
by accident. Its cost is near zero and it never goes stale.

## Includes and folders

`src/` is organized as `src/<cluster>/` for subsystems — `sim/ diorama/ host/
dev/ action/ manual/ actraiser/` — with cross-cutting contracts and `main.c` at
the root. The root is "shared interfaces + entry point", not "everything else".

- `-Isrc` is on the include path, so **bare `#include "foo.h"` resolves from any
  subdirectory**. Root-level headers therefore need no path and no edits when
  files move around them.
- **Siblings inside a cluster include each other bare** (`#include "diorama.h"`).
  **External consumers use the pathed form** (`#include "sim/sim3d.h"`). This
  matches the runtime `snes/` house style.
- **There is no `src/common/`, deliberately.** Folders do not change the
  `#include` graph; moving the cross-cutting headers into one would rewrite ~120
  include sites for zero coupling change.

Adding a `.c` to the game means adding one `source =` line to `snesbuild.ini` —
the single source list both builds read. Header-only additions need no entry.
Tests link individual sources in `CMakeLists.txt`; a new `.c` that an existing
test's dependencies now call must be added to that target too.

## Comments

Preserve this codebase's high comment density. Much of the code must match a
1990 ROM, and the reason for a choice is often unrecoverable from the code alone.

- Explain **why**, and especially why the obvious alternative is wrong.
- Cite evidence: ledger entries (`ledger §24`), spec sections, ROM addresses,
  measured numbers. "Measured peak 16 against a capacity of 128 over ~2,200
  diorama frames" is worth more than "should be enough".
- Record the trap. Several comments here exist purely to stop the next person
  re-making a specific mistake; those are the highest-value lines in the file.
- Do not narrate the code. `/* increment i */` is noise; the above is not.

When you delete code, delete its comment. When you move code, move its comment —
verbatim, in the same commit, so `git blame` stays honest.

## Verifying a change

**Tiers, cheapest first.** The ROM-free tier needs no ROM and runs in seconds:

```bash
cmake -S . -B build-tests-only -G Ninja -DAR_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-tests-only && (cd build-tests-only && ctest)
```

A clean compile is necessary and nowhere near sufficient for render or codegen
work: a mis-extracted function body and a wrongly-shared `static` both compile
perfectly and render wrong.

**Render-affecting changes need a staged oracle.** `tools/sim3d_demo.py` and its
`tests/fixtures/sim3d/checkpoints.json` are the model: each checkpoint pins an
SRAM seed (`sram_base64` + `sram_sha256`), a settings fixture, a replay, and every
stage toggle. Pinning *all* the toggles matters — a checkpoint that names only
some silently inherits shipped defaults, so landing a new stage retroactively
changes what old checkpoints render.

**Three rules that exist because they were each learned the hard way:**

1. **Look at a frame before believing an identity result.** An unstaged replay
   sits on the title screen and compares byte-equal, so the A/B "passes" while
   testing nothing. This happened during the T2a verification and was caught only
   by converting a PPM and looking at it:
   `sips -s format png shot.ppm --out shot.png`.
2. **Run a positive control.** The same comparison with the feature *enabled*
   must differ. If both arms are equal, the harness is not measuring what you
   think.
3. **Compare against a rebuilt baseline, not a remembered one.** When two
   checkpoints diverged from a week-old run, building the pre-change binary
   showed it reproduced the divergence exactly — pre-existing drift, not the
   change under test.

Add `AR_HEADLESS_VIDEO=1` for anything present-side; without a renderer the shot
falls back to dumping `g_pixels` and every present-side change compares equal.
See `DEBUG.md` §4c for the full harness and its flag traps.

## Current debt register

Keep this list focused on reasoning cost rather than snapshot line counts. Use
the budgets above when touching one of these units.

| File / symbol | Why it remains debt |
|---|---|
| `src/settings_overlay.c` | Largest coupled logic unit. Useful seams include text/glyph measurement and the tab/section/row navigation model, but the remaining work is a long tail of helpers rather than one clean subsystem split. |
| `src/present_sim3d.c` | The world-map renderer has been extracted; effects, clouds, shadows, ground, billboards, and culling remain potential stage boundaries. |
| `src/actraiser/actraiser_rtl.c` | `ActRaiserDrawPpuFrame` and `ActRaiser_ApplyWidescreenPolicy` sit on the HLE seam against ROM behaviour, where mistakes are subtle and the oracles are weakest. Further decomposition is high risk and moderate reward. |
| `src/main.c` / `AppLoop_PumpEvents` | Boot and loop phases are named, but event dispatch remains one large flat switch. Its independent arms make it easier to review than its raw size suggests. |
| `src/diorama/diorama.c` | Near the file budget; watch for a new independent presentation stage before adding more responsibility. |

`src/settings.c` is deliberately absent: most of its size is the cohesive
descriptor registry. See [Where data lives](#where-data-lives) for why splitting
that table would make navigation worse.

Known coverage gap: the world-map navigation renderer has no automated render
coverage — no checkpoint sets `AR_SIM3D_WORLD_NAV` and no staged replay reaches
world-map travel, so an A/B of it compares equal whether or not it works. It now
lives alone in `src/present_world_nav.c` so the gap is visible in the file
listing rather than only here. Staging a world-map SRAM seed + replay and adding
a checkpoint for it is the missing regression asset.
