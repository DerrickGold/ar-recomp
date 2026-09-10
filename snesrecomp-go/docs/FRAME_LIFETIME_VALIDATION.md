# Conditional frame-lifetime audit validation

2026-09-10. Baseline: `e9f03eaf`, clean working tree. This milestone adds a
report-only check at calls and jumps to complement the existing return-site
audits. It does not fix the reported Mario Paint failure yet.

Follow-up: [native return-word relocation](RETURN_WORD_RELOCATION_VALIDATION.md)
documents the subsequent execution fix and validation. It uses a separate
local proof with runtime ownership checks, not the conditional facts here.

## Mario Paint finding

Without a new configuration entry or replay, the analyzer connects the
two-byte PLA at `$00:EFAB` to JSR `$00:F002 -> $00:9FC4`, with lexical
continuation `$00:F005`, in the `$00:EF6E M0X0` entry context.

At that call, under the explicitly conditional two-byte native entry-frame
contract, the report records:

- `S_minus_entry_S = -3`: the earlier PLA advanced S by two, PHP pushed one,
  and the later arguments pushed four bytes.
- Both incoming return bytes are no longer proven active in their original
  slots (`unprotected_entry_byte_mask = 3`). No copies remain in the modeled
  registers or active stack (`modeled_saved_copy_byte_mask = 0`).
- The originating pull site is `$00:EFAB`. Preceding calls must return
  normally with zero net stack effect, and memory/hardware writes must not
  alias the modeled stack. Neither assumption is promoted to a fact.

The first call after PLA/PHP still has modeled copies of the incoming bytes
in A; later register writes/calls lose that identity. This distinction prevents
the analysis from declaring a frame permanently consumed merely because it
was pulled into a register. `$00:F005` is already an internal generated label,
not a missing function that this finding should add to the registry.

The audit subsequently stops at an unknown PLP value at `$00:F022`: the
stack-neutral assumption does not establish the callee's actual argument
cleanup. This is expected incompleteness, not proof that PLP is invalid.

The prior isolated runtime contract probe showed why a stale enclosing entry
limit can reject the exact continuation after a moved callee-clean return.
This new static finding identifies the relevant caller shape; it does **not**
prove entry ownership, cleanup, alias safety, or a replacement runtime limit.
The reported 5,380-frame interaction was not replayed in this milestone.

## Four-game read-only comparison

Both analyzers used the same local ROM/config inputs. The final analyzer ran
with `--jobs 1` and `--jobs 8`. All outputs were isolated under
`/private/tmp/frame-lifetime.RNsTJm`; no command targeted a game's generated
directory or authored configuration. Commercial ROM bytes are not included in
the tests or this document.

| Game | Selected entry/frame hypotheses | Review contexts | Unique review PCs | Incomplete hypotheses | Budget-limited hypotheses |
| --- | ---: | ---: | ---: | ---: | ---: |
| ActRaiser | 412 | 18 | 10 | 111 | 3 |
| Mario Paint | 210 | 14 | 13 | 70 | 1 |
| Battletoads | 111 | 66 | 44 | 51 | 0 |
| Mighty Morphin Power Rangers | 126 | 0 | 0 | 30 | 0 |

These are conditional inventories, **not counts of bugs or executed traps**.
Multiple entry/frame hypotheses or paths can describe the same source PC.
Zero review sites in Power Rangers is not a completeness claim: 30 hypotheses
remain incomplete. A selected graph may be speculative or unobserved; the new
audit does not classify it as gameplay-reachable or garbage.

Checks passed for every game:

1. Full JSON reports and exported databases are byte-identical at one and
   eight workers.
2. Every pre-existing JSON report field matches the baseline after removing
   the new `return_frame_lifetimes` field and the top-level version.
3. Exported databases match after removing only `shadow_report_version`
   (18 → 19). Fact payloads, ordering, ROM identities, and templates did not
   change. Existing v17 and v18 databases remain accepted without rewriting.

Exported proven dispatch/entry facts remain respectively: ActRaiser 26/138,
Mario Paint 0/0, Battletoads 0/2, and Power Rangers 0/0. These numbers describe
the optional exported overlay, not all automatic discovery during generation.

## Synthetic contracts and checks

`internal/tooling/framelifetime_test.go` covers:

- Plain calls, local saves, exact pull/push restoration, cross-register saves,
  D/DB saves, hidden accumulator B, and X-width truncation.
- Extracted versus restored bytes, equal-height replacement, stack-relative
  copies/writes, and saved copies at relocated active stack positions.
- A branch with an early RTS and another path that consumes the incoming
  word, pushes arguments, and makes a later call.
- Invalidation of registers and inactive stack scratch across unknown calls.
- JSR/JSL hypotheses, bank-byte ownership, branch-specific states, PHP/PLP,
  HLE barriers, collapsed dispatches, interrupt-frame exclusion, unknown stack
  resets, and bounded state/stack exhaustion.
- PC-zero provenance, deterministic aggregation, unchanged decode graphs and
  exit summaries, read-only ROM/config behavior, and exclusion from both
  production dispatch-fact selectors.

Passed on the final code revision:

```sh
go test ./...
go test -race ./internal/tooling \
  -run 'Test(FrameLifetime|StaticAnalysisDatabase|ShadowDispatchInventory)' -count=1
git diff --check
```

No generation/runtime implementation changed. Accordingly this milestone did
not regenerate game C, run native replay/CTest suites, or benchmark gameplay.
Before a behavior-affecting follow-up, restore the full regeneration, replay,
state/dispatch hash, hard-diagnostic, runtime conformance, and performance gates.

## Next independently testable step

Track bounded stack-derived expressions through S/A/X/D and exact stack/DP
aliases, retaining incoming return-byte identity through callee cleanup. This
must establish the actual frame consumed by RTS/RTL, not infer correctness
from balanced height or a nearby ADC. Separately prove when the caller's old
frame is retired and which ancestor limit remains protected. Preserve explicit
unknown call, hardware, M/X, wrap, and alias obligations.

Only after those contracts are established should generated ownership change.
Do not globally weaken `cpu_accept_adjusted_return`, register the continuation
as a new normal function, or encode the Mario Paint addresses in shared code.
