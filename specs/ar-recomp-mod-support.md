# Plan — MS: symbol database → readable decomp → mod support

**Status:** planning. Nothing implemented. Stage codes `MS0`–`MS6` are citable
from source comments.

## 1. Intent

Turn the *labeling* layer from prose into a **database that drives codegen**, so
that gameplay mods can be authored against stable names while staying
address-backed underneath.

Today the project has three layers, and only the middle one is missing:

| Layer | State |
| --- | --- |
| **Discover** — find the address | Well tooled. `dis65.py`, `romxref.py`, `find_handler_chain.py`, `resolve_miss.py`, `AR_TRACE`, `AR_MXCHECK`. Not the bottleneck. |
| **Label** — record what it is, once | **Prose only.** 213 rows in `docs/research-symbol-map.md` that no program reads. |
| **Manipulate** — hook it | Partly built, three incompatible ways (§2). |

Because Label is not executable, every mod-shaped task re-collapses all three
steps: rediscover the address, re-identify the routine, then patch it. The fix
is making step 2 cumulative. Everything in this plan follows from that.

**The end state:** a mod author writes `at = "ActionCombat_ResolveHits"`, not
`0x008AF9`, and the toolchain resolves it — while every doc, bug-ledger entry,
and trace log keeps citing `$00:8AF9` as it always has.

## 2. Where we are starting from (measured, 2026-08-04)

**Naming.** 1748 `func` declarations across `recomp/bank*.cfg`; **1745 are still
`bank_XX_YYYY`**. The only semantic names in generated C are `ResetHandler`,
`NmiHandler`, `IrqHandler` — the three `auto_vectors` produces for free.

**The map.** `docs/research-symbol-map.md` holds 213 rows over 264 distinct
addresses: 121 Verified, 54 Mapped, 13 Observed, 3 Stable. Promotion rate from
Verified+Stable into code is **3 of 124 (~2%)**.

Only **43 of the 264 documented addresses have any cfg line at all**. The rest
are auto-discovered callees. That is fine in itself — but nothing verifies they
still resolve to a real emitted function, so map rot is currently invisible.

**Generated C readability.** No mnemonic comments, no memory names, `_vN` temps,
`L_9812_M0X0` labels:

```c
uint16 _v1 = cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x0009));
uint16 _v2 = (uint16)((_v1 & 0xFFFF) << 1);
cpu_write16(cpu, 0x7E, (uint16)(cpu->D + 0x0009), _v2);
```

**Existing mod surface — three mechanisms, no registry.**

| Mechanism | Where | Count | Keyed by |
| --- | --- | --- | --- |
| `hle_func` code hooks | `recomp/*.cfg` → `src/*.c` | 13 | name (already!) |
| `AR_*` value hooks | `ActRaiser_ApplyCheats`, `actraiser_rtl.c` | ~a dozen | env var |
| Static ROM table edits | `tools/act_content.py` | n/a | address |

`hle_func` is already name-keyed and already works. `MS6` generalizes it rather
than replacing it.

**Blast radius of a rename pass: 5 symbols.** Handwritten code references only
five distinct `bank_XX_YYYY` names, in `main.c`, `sim_world_map_build.c`,
`actraiser_widescreen_bg.c`, and `actraiser_widescreen_sprites.c`.

## 3. Design principles

These are constraints discovered in the code, not preferences. Violating any of
them is what makes this kind of effort thrash.

1. **The address is the archival key; names regenerate.** LoROM, no overlays,
   no relocation — `$00:9DE1` is `$00:9DE1` forever. Addresses stay primary not
   because names drift against a moving target, but because *every piece of
   evidence we own cites the address*: `DEBUG.md`, `bug-ledger.md`, `SEAMS.md`,
   every `AR_TRACE` log. No rebase step ever needs building.

2. **One name is a *variant set*, not one function.** Every entry is emitted per
   M/X width variant: `Magic_CastGate_M0X0`, `_M0X1`, `_M1X0`, `_M1X1`. A hook
   on a name must resolve to all live variants of that address. There is no
   single C symbol to alias.

3. **Three generation layers, hand-edit only the first.**
   `recomp/symbols.toml` (source) → generated `name` block in `recomp/bank*.cfg`
   → `src/gen/*.c`. The existing "never hand-edit `src/gen`" rule extends up one
   level.

4. **Confidence and heat are orthogonal.** `status` (Observed/Mapped/Verified/
   Stable) is hand-authored judgement. `heat` (execution frequency) is derived
   from `AR_TRACE` and regenerated. Never merge them into one enum.

5. **The DB holds only what a program must read.** Contracts, evidence, and
   reasoning stay in `SEAMS.md` / `rendering-engine.md` / `ram-map.md`. DB rows
   carry an `evidence` anchor pointing at them. Duplicating prose into TOML
   guarantees the two drift.

6. **The recompiler stays dependency-free.** Go has zero third-party modules
   (`go.mod`, Go 1.24). The DB is TOML consumed by Python 3.14's stdlib
   `tomllib`; it *emits* cfg directives Go already parses. Across this whole
   plan the Go side changes exactly twice: `MS0` and `MS4`.

7. **Every stage is independently useful, and behavior-changing stages ship
   default-off.** No stage may leave the tree in a state where the previous
   stage's value is unavailable.

## 4. Stages

### MS0 — Unblock: `name` must carry `entry_mx` *(blocker, small)*

**Problem.** Both `config.go:263` and `regen.go promoteCrossBankNames` synthesize
the entry with `EntryMX{M:1, X:1}` when the named address is not already
declared. For an m=0-only routine like `$00:9DE1` that materializes a bogus
M1X1 variant decoded from misaligned bytes — dead code at best, an
`ar_garbage_variant_trap` at worst, and it inflates the stub census.

**Fix.** Teach `name` to accept `entry_mx:M,X` (same parser as `func`), and/or
suppress entry synthesis when demand discovery will supply the entry anyway.
`regen.go:501` already makes demand-discovered variants inherit the base entry's
name, so one correct declaration covers all four variants.

**Exit:** unit test in `internal/config`; a `name` on an m=0-only address
produces no M1X1 variant; stub census unchanged from baseline
(164 stubs / 68 sites).

---

### MS1 — Symbol source of truth + linter *(no codegen yet)*

Create `recomp/symbols.toml` and `tools/symbols.py`. **This stage generates no
code and changes no behavior** — it exists to make the map machine-checkable
before anything depends on it.

```toml
[[func]]
addr    = 0x009DE1
name    = "Magic_CastGate"
status  = "verified"          # observed | mapped | verified | stable
entry_mx = [0, 0]             # optional; omit when discovery supplies it
evidence = "SEAMS.md#magic-system--full-wiring-map"
note    = "requires $F8==0, $02AC!=0, $08D0 BIT #$2008 clear, MP $21>0"
```

**Seeding.** A one-time importer parses the 213 rows out of
`research-symbol-map.md`. Multi-address rows (e.g. the seven
`WaitForVblank_SpinSites`) expand to one entry per address with a shared
`group` field — a name must be unique per address.

**Lint checks** (`tools/symbols.py --lint`):

- address parses, lies in a code bank, and is not inside a `data_region`
- name is a legal, unique C identifier; no collision with an existing cfg name
- `status` is in the ladder; `evidence` anchor resolves to a real heading
- **the address resolves to an actually emitted function** — this is the one
  that converts today's 221 undeclared addresses from *unknown* into
  *confirmed live* or *stale, investigate*
- `entry_mx`, where given, matches a variant regen actually emits

**Exit:** `tools/symbols.py --lint` is clean and wired into `make dev` /
`snesbuild regen` as a non-fatal warning; `--report` prints coverage by bank and
status. `research-symbol-map.md` gains a header line pointing at the TOML as the
machine-readable form, and keeps its prose contracts.

---

### MS2 — Function-name promotion

Generate `name` lines from every `verified` and `stable` row (~124) into a
delimited block inside each bank cfg, then regen.

```
# >>> GENERATED BY tools/symbols.py — DO NOT EDIT BELOW <<<
name $00:9DE1 Magic_CastGate entry_mx:0,0
...
# >>> END GENERATED <<<
```

**Why a delimited block and not a separate file:** there is no cfg include
mechanism. `includes` is parsed into `cfg.Includes` and consumed by nothing, and
bank identity comes from the *filename* regex with `repo.byBank[bank] = state`,
so a second `bank = 00` file silently overwrites the first. The bank cfgs also
carry real hand-written evidence (see the `$8966` comment in `bank00.cfg`) that
must not be clobbered. Wiring `includes` up properly is a viable alternative if
separate files are preferred later.

**Do not promote `mapped` or `observed`.** The confidence ladder exists to be
obeyed; a confident wrong name is worse than `bank_00_8465`.

**The payoff is not `src/gen/*.c`.** Nobody reads 102 MB of generated C. The win
is that `g_last_recomp_func`, `RecompStackPush`, `cpu_trace_func_entry`, and
every `AR_TRACE` log become legible — which pays back within a day of debugging.

**Risk control.** Bank at a time; regen + visual-regression A/B between each;
update the 5 handwritten references in the same commit.

**Exit:** ≥120 semantic names in generated C; stub census unchanged; a headless
A/B run is pixel-identical to the pre-promotion build; a stack trace from a
deliberate crash reads in names.

---

### MS3 — Heat: rank by what actually executes

`cpu_trace_func_entry` is already emitted in every generated function, so an
`AR_TRACE` playthrough yields an execution-frequency ranking over all 1748
functions for free.

`tools/symbols.py --heat runs/<trace>` writes a **generated, never hand-edited**
heat field back into the DB and reports the top unnamed functions by call count.

**This is the prioritization oracle for all later work** — it answers "which of
the 1500 unnamed routines are worth a research session" with data instead of
address order. It can run before or during `MS2`; it is listed after only
because it is not a blocker.

**Exit:** a ranked list of the top ~50 unnamed-but-hot functions, filed as the
standing worklist for map growth.

---

### MS4 — Data symbols, annotation-only

There is currently **no data-symbol directive at all** — no `data_name`,
nothing. This stage adds one, used for *comments only*.

```toml
[[data]]
addr = 0x7E0021
name = "magic_points"
type = "u8"
note = "act working copy; loaded from persistent $0295 at $02:84E0"
```

New cfg directive `data_name $7E:0021 magic_points`, and an emitter change that
appends a trailing comment to absolute and provably-constant-DP accesses:

```c
uint16 _v1 = cpu_read16(cpu, 0x7E, 0x0021);  /* magic_points */
```

**Highest readability-per-unit-risk in the plan.** Every memory-touching line
becomes self-documenting, and a comment cannot change behavior. `ram-map.md`
already holds hundreds of curated addresses ready to feed it.

**Where D is not provably constant**, annotate as `DP+$09` rather than guessing
an absolute — silently resolving a variable direct page would produce confident
lies.

**Explicitly out of scope here:** named accessor macros in generated C. That is
`MS5`, and it must not be smuggled in early.

**Exit:** annotation is behind a regen flag, default on; a diff of generated C
before/after shows comment-only changes; build is byte-identical with the flag
off.

---

### MS5 — Structs, arrays, and host accessors

The half that actually matters for mods. Function names alone do not help a mod
author; field layouts do.

```toml
[[object]]
name   = "action_objects"
addr   = 0x7E06A0
struct = "ActionObject"
count  = 80
stride = 0x40

[[field]]
struct = "ActionObject"
offset = 0x2A
name   = "atk"
type   = "u16"
note   = "damage dealt; $00:8AF9 does victim.hp -= attacker.atk"
```

**Two schema extensions the game forces.** A flat offset→name→type table will
encode falsehoods here, and `SEAMS.md` documents both cases:

- **Polymorphic fields.** `+$08` (`$08A8`) is Y-velocity *only in the air
  state*; writing it while grounded does nothing. That is why `AR_MOONJUMP` uses
  Y-position instead. Needs a `when = "state:air"` qualifier, or at minimum a
  `polymorphic = true` flag that suppresses accessor generation.
- **Per-instance exceptions.** Every object carries HP at `+$2C` — *except the
  player* (slot 8, `$08A0`), whose HP is DP `$1D` on a completely separate
  damage path (`$00:8A21` vs `$00:8AF9`). Needs an `override` on the instance.

Arrays also need a second dimension: the town structure records are
`128 × 4B @ $7F:6BE7 + town * 0x200`, already correctly encoded in
`tools/town_structs.py` — that file is the reference shape for the schema.

**Deliverable:** generated `src/ar_symbols.h` with an offsets enum and thin
host-side accessors over guest RAM (`ar_obj_atk(i)`, `ar_obj_hp(i)`), plus a
generated struct comment block. Host helpers only — generated C is untouched.

**Exit:** `town_structs.py` and `act_content.py` can be re-expressed against the
generated header without behavior change; the polymorphic and override cases are
represented and covered by a lint check.

---

### MS6 — Name-keyed, address-backed mod surface

Unify the three existing mechanisms behind one registry.

```toml
[[hook]]
at     = "ActionCombat_ResolveHits"   # resolved to a VARIANT SET, per §3.2
kind   = "code"
plugin = "double_enemy_hp"

[[hook]]
at     = "action_objects[*].hp"
kind   = "value"
plugin = "god_mode"
```

**Sequence within the stage — value hooks first.** Value hooks are pure RAM
clamps; `ActRaiser_ApplyCheats` already proves the pattern with `AR_INF_HP`,
`AR_MOONJUMP`, `AR_NO_KNOCKBACK`, `AR_FREEZE_TIMER`. Migrating those from env
vars to registry entries is a refactor with a known-good reference behavior, and
it validates name resolution end to end before any control flow is intercepted.

**Code hooks second**, generalizing `hle_func` — which is already name-keyed and
already works for 13 sites. The new work is resolving one name to its live
variant set and installing across all of them.

**Explicitly deferred beyond this plan:** a plugin ABI, dynamic loading, a
scripting runtime, and any user-facing mod manager. `MS6` delivers a registry
and in-tree plugins compiled with the game. Shipping third-party mod loading is
a separate project and should not be designed here.

**Exit:** every current `AR_*` cheat is expressed as a registry entry with
identical behavior; at least one code hook is installed by name across its full
variant set; removing all hooks yields a byte-identical build.

## 5. Dependency order

```
MS0 ──► MS1 ──► MS2 ──► MS4 ──► MS5 ──► MS6
          └────► MS3 (informs MS2 ordering and all later map growth)
```

`MS3` is the only stage that can run in parallel.

**Why function names (MS2) come before the data model, despite mods being the
goal:** `MS2` is the only step that exercises the whole
source → cfg → regen → C pipeline against 124 rows that are *already Verified*,
with a 5-symbol blast radius. If the pipeline is wrong, it fails cheaply and
visibly. Going straight at `MS4`/`MS5` means building new emitter surface on an
unproven generation path, against exactly the data model that has the
polymorphism problem.

## 6. Non-goals

- Renaming `mapped` or `observed` rows. The ladder is the safeguard.
- Reproducing doc prose inside the TOML (principle §3.5).
- Named accessor macros inside generated C. Host-side accessors only.
- Decompiling to idiomatic C. This plan makes generated code *legible and
  addressable*; it does not restructure it.
- A plugin ABI, dynamic mod loading, or a mod manager.

## 7. Open decisions

1. **Where the DB lives.** `recomp/symbols.toml` (next to the cfgs it feeds) vs
   `docs/`. Leaning `recomp/` — it is toolchain input, not reference prose.
2. **Whether `research-symbol-map.md` stays hand-authored** after `MS1`, or
   becomes a generated view over the TOML. Generating it removes drift but costs
   the freeform contract prose that is currently its main value. Leaning: keep
   it hand-authored, lint the two against each other.
3. **`includes` — wire it up or leave it dead.** Affects whether `MS2` writes a
   delimited block or a separate generated cfg per bank.
4. **Whether `MS4` annotation is default-on in release builds** or dev-only.
   Comments cost nothing at runtime but do inflate `src/gen` (currently 102 MB).

## 8. First concrete step

`MS0` + `MS1`: the `name entry_mx` fix with its test, then `symbols.toml`, the
importer seeded from the 213 existing rows, and `tools/symbols.py --lint`.
No generated output, no behavior change — and it immediately tells us how many
of the 264 documented addresses are still real.
