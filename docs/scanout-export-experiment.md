# Line-local overlay export experiment — 2026-09-12

Status: **not enabled or retained in production**. The correctness tests remain.
The prototype is preserved as a patch and binary in
`/private/tmp/actraiser-scanout-followup.yIG0Bq/` for a future targeted experiment.

The Deck profile identified native scanout and packed overlay export among the
remaining CPU costs. This trial resolved priority-plane destinations, primary
fallback and content-bit selection once per scanline, then accumulated content
bits in the line-local plan. It did not alter the runner ABI, palette policy,
pixel sampling, GPU submission or worker count. It passed the independent PPU
pixel-renderer oracle, including newly added per-line content-mask comparisons
and live priority-surface alias/unbind/rebind coverage.

## Local measurements

Three serial eight-run ABBAABBA batches on the Mac, three helpers for both
variants, no simultaneous game runs/profilers/captures/builds. The control
includes the fixed hidden-window lifecycle (`f5b25a57`), so this comparison does
not conflate that correction with PPU work. Inputs and final WRAM are checked by
`tools/compare_pipeline_performance.py`; all accepted runs completed their
requested presentation count. Values are CPU wall time, not GPU time or FPS.

| Scene | Control render CPU median [range], ms | Trial median [range], ms |
| --- | --- | --- |
| Sky Palace | 3.271 [3.246–3.339] | 3.287 [3.166–3.334] |
| Aitos SIM town | 2.521 [2.487–2.573] | 2.517 [2.451–2.638] |
| Aitos Action 3D, interpolation on | 3.364 [3.250–3.381] | 3.301 [3.226–3.383] |

Palace is neutral (0.5% higher median); town is neutral (0.1% lower). Action has
a 1.9% lower median but substantially overlapping ranges. Its nested scanout
median is 2.094 → 2.041 ms, again with overlapping ranges. This is an interesting
action-only hint, not a demonstrated general or Deck gain. It does not justify
changing the shared scanout path given the current globe/town priority. The
prototype was removed and the normal game rebuilt; no speculative new toggle
or dormant production branch was added.

Evidence directories are `timing-palace-complete`, `timing-town-complete`, and
`timing-action` beneath the directory above. Early Palace/town attempts ended
at legacy replay EOF before the requested tick limit, were rejected by the
completion guard, and are not counted. The accepted held Palace fixture uses
`AR_REPLAY_NOSTOP=1` (its final input is idle) for 1,800 ticks. The town run ends
at 1,700 ticks inside its recording; action uses 3,000 ticks and room `04/04`.
No full-game image-parity claim is made for this discarded trial.

## Follow-through

Keep the stronger PPU regression coverage for future producer/scanout changes.
After restoring production scanout, all 36 portable-runtime tests and all 165
application tests pass, including the independent PPU oracle and Metal tests.
The normal Release game was rebuilt without the experiment.
Do not convert the handover's host-only motion-search observation or these
whole-replay profile shares into a projected speedup. Split and measure the
remaining capture work before changing its input sampling or contracts.

For navigation/Palace, source-surface GPU preparation for ground, ocean, cliffs
and moving weather receivers remains the larger architectural path. The short
fixed-build Deck Palace probe showed active model GPU reuse and no rejected
optional path, while CPU surface preparation and per-frame geometry upload
remain. See [GPU offload audit](gpu-offload-audit.md) for those boundaries and
[Deck lifecycle evidence](steam-deck-hidden-present.md) for the two bounded
verification probes. The main iteration loop stays local; the Deck verifies
promising mechanisms instead of running every exploratory matrix.
