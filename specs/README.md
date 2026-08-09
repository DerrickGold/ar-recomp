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
header.** Several headers are stale; where they disagree, the table is right and
the discrepancy is called out.

## Status

| Spec | Status | Notes |
| --- | --- | --- |
| [SPEC-wave4.md](SPEC-wave4.md) | **Implemented** | Combined Wave-4 spec (base `addd8f8`). All 15 wave-4 patches are integrated into `main`. |
| [SPEC-backdrop-clip.md](SPEC-backdrop-clip.md) | **Implemented — confirmed in play** | Fixes A, B and C all landed in `b9dc4f3`; black wedge at level bounds no longer observed (2026-07-28). Live A/B via `diorama_margin_fix`. |
| [SPEC-world-navigation-3d.md](SPEC-world-navigation-3d.md) | **Mostly implemented** | WN1. Steps 1–4d plus visual tuning 5a/5b/5d/5e landed in `48f2495` ("feat: add 3D world navigation"). Step 4e still open. |
| [SPEC-interp-jitter.md](SPEC-interp-jitter.md) | **Proposed** | IJ1. Scroll interpolation exists but ships **off by default** because of exactly this jitter. Primary hypothesis must be confirmed against data before any code changes (§7). |
| [SPEC-render-resolution.md](SPEC-render-resolution.md) | **Proposed** | RR1. Decouple render resolution from window/display resolution. Awaiting the audit in §9. |
| [SPEC-bg-hle.md](SPEC-bg-hle.md) | **In progress — BH7 complete** | The bounded decoder, observer, 49-map `ActionBgPlan`, full-world provider, and exact `FrameSlot`/diorama handoff are implemented. The provider is default-on with exact `AR_ACTION_BG_HLE=0` native fallback; the PPU still owns VRAM/CGRAM, HDMA, priority, windows, transparency, mosaic and color math. Five paired 12-entry presentation matrices, long Fillmore Full/Raw/diorama runs, a natural Death Heim transition soak, savestate/rebind/geometry lifecycle gates, a real compositor A/B, debug/release builds, and all 41 tests pass. The only framebuffer delta is an intended 30-pixel synthetic-margin finite-bound correction in Wide Full `0301`; every authentic center and all state artifacts are exact. Direct `0608` remains rejected because matching words coexist with corrupt CHR. BH8 behavior-neutral legacy cleanup remains. |
| [SPEC-tile-extrusion.md](SPEC-tile-extrusion.md) | **Proposed** | WN2. Extruded 3D geometry for background tiles. Independent of WN1; not audited. Related: the `tiles` HD-replacement plane is parsed but reserved. |
| [ar-recomp-threading-impl.md](ar-recomp-threading-impl.md) | **Implemented, partly superseded** | M0–M8: diorama mode, present thread, fixed timestep, GPU shader effects. ⚠️ The **present thread it specifies was later removed** — SDL3's 2D render API is main-thread-only; all rendering is now synchronous on the main thread (`main.c` §"Phase 0"). Read M5/D5/D6 as history. |
| [ar-recomp-sim-rendering-plan.md](ar-recomp-sim-rendering-plan.md) | **Partly implemented** | Simulation-town 3D rendering. Phase 0 evidence tooling plus phases 1–4 and the D5a ground extension are done; D5a's checkpoint is known-failing on 24 purged atlas objects. Other towns still need art/layer work. Where it says "the present thread" (§86, §364, §367) read "the present path" — the FrameSlot/upload contract it describes still holds, but that thread was later removed. |
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
