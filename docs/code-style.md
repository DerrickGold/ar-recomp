# Code style and structure

House rules for the hand-written runtime under `src/`. The generated code in
`src/gen/` and `recomp/` is machine output and is exempt from everything here.

These are budgets and defaults, not laws. Every one of them can be broken with a
stated reason — but "it grew that way" is not a reason. Each rule below is
followed by the concrete file in this repo that earned it.

**Contents**
- [Size budgets](#size-budgets)
- [One concern per translation unit](#one-concern-per-translation-unit)
- [Where data lives](#where-data-lives)
- [Put the decision on the thing it describes](#put-the-decision-on-the-thing-it-describes)
- [Enforce invariants with the compiler](#enforce-invariants-with-the-compiler)
- [Includes and folders](#includes-and-folders)
- [Comments](#comments)
- [Verifying a change](#verifying-a-change)
- [Current debt register](#current-debt-register)

## Size budgets

Measure **logic lines** — total minus comments, blanks, and data tables. Raw line
count is misleading: `settings.c` is 3,069 lines and perfectly workable because
1,286 of them are a descriptor table you skim in seconds, while `settings_overlay.c`
is 3,651 lines of which 2,479 are logic, and that one genuinely hurts.

| Unit | Budget | Hard limit |
|---|---|---|
| File | ~1,500 logic lines | past ~2,500, splitting is the default answer |
| Function | ~150 lines | past 400, treat as a defect |

**Why this matters here specifically.** Both humans and coding agents pay for
size the same way: the cost is not the file's length, it is *whether you must
read the whole thing to change part of it*. A 1,200-line data table costs
nothing — you jump to a row. A 1,200-line function costs everything, because
there is no smaller unit that can be reasoned about on its own. Prefer many small
functions in one longer file over few enormous functions in a short one.

Exempt from the file budget, but say so in the file's header comment:
- generated blobs (`src/shaders/*.h`)
- a single cohesive data table (see below)
- one state machine that genuinely does not decompose

## One concern per translation unit

When a file carries two subsystems that only meet at a few call sites, that seam
is a file boundary. `present.c` was 4,551 lines because the flat/diorama present
path and the SIM 3D renderer shared a file while barely sharing code.

**Deriving the split.** Do not partition by name — names lie. Partition by usage:
*a definition moves iff it has no remaining user on the original side once the
obvious movers leave*, applied to closure. When this was run on `present.c` it
independently found the sim-only helpers whose names gave no hint (`kEffectCircle32`,
`kPi`, `CloudHash`/`CloudSmooth`/`CloudNoise`) and independently kept every shared
helper behind.

**Name the seam.** A split TU pair gets an explicit `<name>_internal.h` holding
exactly what crosses: shared type definitions, the helpers that lost `static`, and
the callbacks going the other way. See `src/present_internal.h`. Two properties
make it work:

- It is *not* the public API. That stays in `present.h`. An internal header is a
  reviewable list of deliberate coupling — if it grows, the split is eroding.
- It carries no live game state, which is what lets the D6 invariant survive the
  split (see below).

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

**But measure coupling before splitting at all.** The test: *how many file-private
symbols does the data reference?* Data that references none is genuinely data;
data that references many is coupled logic wearing a data costume, and hoisting it
inverts the dependency.

Worked example — `g_setting_descs[]` in `settings.c` looks like a prime candidate
at 1,190 lines, and `settings.h` already declares it `extern`, so the move looks
free. It is not: the table references **75** file-private symbols (parsers,
formatters, `on_change` callbacks, availability predicates, label arrays).
Splitting it would export 75 symbols to relocate the most skimmable part of the
file. **Correct answer: leave it.** The 81 lines of pure label arrays are likewise
too small to be worth a file. `settings.c` is a false positive for the size rule
and should be left alone.

Split data out when it is bulky *and* self-contained. Otherwise let it sit next to
the code that owns it.

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

The decisive cost is losing the compiler. Today a typo'd callback or a bad enum
is a build error. Externalized, it is a runtime error on a user's machine at
boot — and settings is the worst subsystem for that, since a failure there can
lock someone out of the very graphics options they need in order to launch.

The version that *would* be worth it, if localization ever becomes a goal, is
externalizing `label` and `tooltip` alone: genuinely pure strings, genuinely
worth non-programmer editing, and able to fail soft (missing key falls back to
the built-in English) rather than failing the boot.

## Put the decision on the thing it describes

A per-row property belongs on the row, not in a predicate that enumerates rows
somewhere else.

`Settings_UsesLegacyEnvironmentSyntax` used to be a ~60-clause chain of
`desc->field != &g_settings.<row>` comparisons a thousand lines from the table it
classified. Nothing was wrong with it — until someone added a modern setting and
never knew this function existed, at which point their setting silently inherited
the legacy `AR_*` parse. It is now `return !desc->modern_env;`, with the answer
stated on each row.

Generalize: **if adding a case in one place requires remembering to edit another
place, the design is the bug.** Prefer a field on the record, a flag in the table,
or a variant constructor (`BOOL_SETTING_MODERN`) whose *name* states the intent at
the call site.

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

This codebase has an unusually high comment density — `actraiser_rtl.c` is 31%
comment — and that is an **asset to preserve, not cleanup debt**. The reason is
the domain: much of this code exists to match the observable behaviour of a 1990
ROM, and the *why* is frequently unrecoverable from the *what*.

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

Measured on `cleanup-crusade`. Update these numbers when they move; the point is
that the list is short, named, and honest.

| File / symbol | Logic lines | Note |
|---|---|---|
| `main()` in `src/main.c:612` | **1,311** | worst single artifact in the tree — boot, config, and loop tangled in one function |
| `settings_overlay.c` | 2,479 | largest logic file; resisted two extraction passes |
| `present_sim3d.c` | 2,443 | product of the T2a split; has its own stage seams (ground / billboards / shadows / rim / clouds / underlay / effects) not yet cut |
| `actraiser_rtl.c` | 1,815 | contains `ActRaiserDrawPpuFrame` (455) and `ActRaiser_ApplyWidescreenPolicy` (418) |
| `diorama.c` | 1,175 | at budget, watch it |

`settings.c` is deliberately **not** on this list: 3,069 total but 1,250 logic,
and see [Where data lives](#where-data-lives) for why splitting its table is the
wrong move.

Known coverage gap: `PresentWorldNavigation3D` has no automated render coverage —
no checkpoint sets `AR_SIM3D_WORLD_NAV` and no staged replay reaches world-map
travel. Staging that seed/replay pair is the missing regression asset.
