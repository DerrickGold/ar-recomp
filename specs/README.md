# specs/

Design specs and implementation plans. One flat folder on purpose: dozens of
source comments and doc references cite these by **bare filename**
(`SPEC-backdrop-clip.md`, `ar-recomp-threading-impl.md`), so a file must never
move again for a status change. Status lives in the table below.

`docs/` is the companion and is *not* the same thing: it holds living reference
that is continuously true (`ram-map.md`, `rom-map.md`, `rendering-engine.md`,
`bug-ledger.md`, `progress.md`). A spec here describes work at a point in time —
proposed, in progress, or done and kept for the reasoning behind the code.

**Status in the table is derived from the code, not from each document's own
header.** An in-document header may intentionally describe the implementation
snapshot when that spec was authored; where it differs, this index is current
and the notes call out the relationship.

Completed and historical specs preserve the reasoning and line numbers from
their implementation snapshot. Source paths are kept navigable, but search for
the named symbol before acting on an old line reference.

## Status

| Spec | Status | Notes |
| --- | --- | --- |
| [SPEC-wave4.md](SPEC-wave4.md) | **Implemented** | Combined Wave-4 spec (base `addd8f8`). All 15 wave-4 patches are integrated into `main`. |
| [SPEC-backdrop-clip.md](SPEC-backdrop-clip.md) | **Implemented — confirmed in play** | Fixes A, B and C all landed in `b9dc4f3`; black wedge at level bounds no longer observed (2026-07-28). Live A/B via `diorama_margin_fix`. |
| [SPEC-world-navigation-3d.md](SPEC-world-navigation-3d.md) | **Implemented — confirmed in play** | WN1's current forced-top-down presentation, movement, destination, fade, and action-entry behavior are accepted (2026-08-23). The optional 2048² reconstruction remains research, not current roadmap work. |
| [SPEC-interp-jitter.md](SPEC-interp-jitter.md) | **Resolved after audit; IJ2 deferred** | The proposed extrapolation→lerp change was not applied. `bfafe3e` fixed the measured IJ1 error: horizontal UV motion used the visible width instead of the allocated layer-texture width, producing an oversized shift and 60 Hz sawtooth. Revisit IJ2 only if on-device testing still shows direction-change jitter. |
| [SPEC-render-resolution.md](SPEC-render-resolution.md) | **Proposed** | RR1. Decouple render resolution from window/display resolution. Awaiting the audit in §9. |
| [SPEC-bg-hle.md](SPEC-bg-hle.md) | **Complete — BH1-BH8 accepted** | The bounded decoder/provider and exact `FrameSlot`/diorama handoff are default-on with `AR_ACTION_BG_HLE=0` authentic-center fallback; the PPU still owns VRAM/CGRAM, HDMA, priority, windows, transparency, mosaic and color math. BH8 removed the action-world ring-repair transaction, builder trampolines, 128 KiB snapshot, vertical band repair, and two unused PPU policy prototypes; `AR_WS_BGREFRESH` is now a hidden load-only alias. Unbound planned world layers clamp safely to the authentic viewport, while Wide Raw stays raw. The repair-removal matrices accept 612/612 artifacts under the explicit VRAM contract, and the final consumer cleanup is 612/612 byte-exact against that baseline. Native streamers/ring/oracle, live decorative policies, vertical layer clip, and the unrelated Sky Palace repair remain. |
| [SPEC-tile-extrusion.md](SPEC-tile-extrusion.md) | **Proposed** | WN2. Extruded 3D geometry for background tiles. Independent of WN1; not audited. Related: the `tiles` HD-replacement plane is parsed but reserved. |
| [ar-recomp-threading-impl.md](ar-recomp-threading-impl.md) | **Implemented, partly superseded** | M0–M8: diorama mode, present thread, fixed timestep, GPU shader effects. ⚠️ The **present thread it specifies was later removed** — SDL3's 2D render API is main-thread-only; all rendering is now synchronous on the main thread (`main.c` §"Phase 0"). Read M5/D5/D6 as history. |
| [ar-recomp-sim-rendering-plan.md](ar-recomp-sim-rendering-plan.md) | **Partly implemented** | Simulation-town 3D rendering. Phases 0–4 and the core Phase 5 presentation work have landed; current metadata checkpoints require zero accounting errors. Fillmore, Bloodpool, Kasandora, Aitos, and Marahna have confirmed event coverage; Northwall remains. Where the plan says "present thread," read "presentation path"—the FrameSlot/upload contract remains, but the thread was removed. |
| [ar-recomp-mod-support.md](ar-recomp-mod-support.md) | **Planning** | MS0–MS6. Symbol database → readable decomp → name-keyed mod surface. Nothing implemented. MS0 (the `name entry_mx` fix in snesrecomp-go) blocks everything else. |
| [ar-recomp-followup-improvements.md](ar-recomp-followup-improvements.md) | **Planning** | Consolidated next-steps after `cc0b042`. Successor to `ar-recomp-threading-impl.md`. ⚠️ Written while the **present thread was still live**, and several items (B1's F9 concurrency, B4's state-ownership split, the §357 cost accounting) are premised on it. That thread has since been removed — rendering is synchronous on the main thread — so re-check any concurrency rationale here against `main.c` before acting on it. |
| [ar-recomp-renderer-thread-map.md](ar-recomp-renderer-thread-map.md) | **Historical** | Thread-affinity audit of every SDL renderer call. Written to justify the present thread; kept because the affinity inventory is still accurate and was the evidence that led to removing that thread. |
| [ar-recomp-renderer-thread-migration.md](ar-recomp-renderer-thread-migration.md) | **Historical** | Draft migration plan companion to the map above. Overtaken by removing the present thread outright. |

## Conventions

- `SPEC-*.md` — a specification for a discrete change, usually with a short code
  (WN1, RR1, IJ1) that source comments cite.
- `ar-recomp-*.md` — broader multi-milestone implementation plans.
- Both are referenced from source by bare filename. Keep the names stable.
- When a spec lands, update its row here rather than moving the file.

Retired handover archives (wave patch series, A/B test writeups, handback notes)
live under `archive/`, which is untracked.
