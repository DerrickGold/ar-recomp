# GPU world-animation snapshots — 2026-09-12

The default world GPU path reuses completed Ground atlas versions instead of
rebuilding and uploading the same water/town animation combinations every
cycle. This is independent of camera movement and applies to both navigation
and the Sky Palace. No resolution, art, animation rate, lighting or effect
setting is reduced. The full compatibility opt-out remains
`AR_SIM3D_WORLD_GPU_GRID=0`; no new experimental switch or saved setting is added.

## Ownership and failure behavior

The simulation's world-art owner exposes the current **validated** water frame,
not a presentation-side approximation from the clock or raw WRAM. Unknown
initial ROM water is deliberately not identified as frame zero. Four water
frames and four town-art phases give at most sixteen combinations.

The presenter owns the content identity: geography, detailed town sources,
model cleanup, cliff style and mountain transitions. Existing full-publication
invalidation also destroys every cached version. A live-owner/slot serial
mismatch bypasses reuse. A hit never advances the CPU publication serial or
phase: those certify the retained pixels and mutable base atlas, not the last
image displayed. An uncached combination can therefore use the existing exact
incremental update from the real CPU image, even after many cached frames.

The private depth adapter owns one optional opaque cache with sixteen slots,
each limited to 2048x2048 RGBA8: at most 256 MiB of live snapshot payload.
Snapshots are lazy, not preallocated or prewarmed on scene entry. A new version
is copied from the successfully published mutable atlas entirely on the GPU.
Repeat visits only select its texture. Source uploads and copies use normal
ordered GPU submissions, with no CPU readback, fence, compute queue or wait.
Driver allocation overhead and in-flight retirement can exceed live payload;
this is not a total VRAM ceiling.

Capture allocates a replacement before releasing a valid old version; invalid
inputs/allocation/submission failure do not overwrite that version. A failed
optional capture releases the scene cache and latches the mutable path until
resource reset; it does not force authentic graphics. Reset releases GPU
payloads while preserving opaque handles for republishing. Scene reset destroys
the handle. Every depth-pass Begin restores the normal Ground texture, so a
world selection cannot leak into town rendering. Changes after ordinary or
retained Ground geometry is queued reject; destruction of a queued selection
invalidates the pass safely.

No runner ABI or render-device vtable changes. Backend APIs know neither game
phases nor source descriptors. Caller data remains on the owner thread. The
bounded helper pool is retained for cold/revised animation, terrain and models;
warm animation jobs disappear because their work is unnecessary, not because
multicore execution was disabled.

`Atlas hit / copy` in the detailed overlay and `[pipeline-atlas]` in logs report
hits and GPU-only copy bytes/calls per completed present. These are distinct
from geometry copies and CPU texture uploads, and are not GPU timing/bandwidth
measurements. Cold copies remain visible in the log.

## Validation

All 165 local CTests pass. The depth fixture checks sixteen immutable versions,
partial source updates, source replacement/resize, invalid indices, a rejected
oversize capture preserving the old version, repeated out-of-order selection,
pass isolation, ordinary/retained queued guards, destruction and reset/rebuild.
The world fixture checks all sixteen visibly animated water/town combinations
against fresh full renders, incremental publication, revisits and reset. Its
21-state geography/style/mountain/viewport matrix also runs with valid water
identities so it exercises snapshot invalidation, with zero and three helpers.
World-owner tests cover unknown, invalid, valid and unavailable phase queries.

Sixteen local full-game navigation composites are byte-identical to the control
and final WRAM matches. Two Palace composites are byte-identical as well,
including the final build's shortened overlay label. Repeated timing cohorts use pinned `-O2` release binaries,
Quality, three helpers and serial ABBAABBA, with no own overlapping build/test/
capture work. Values are settled frame-weighted windows, then run medians.

Evidence directory: `/private/tmp/actraiser-gpu-atlas-versions.37OKpW/`.
Control Mac SHA-256:
`2896c94b3cd9837d8e6f775b3b8009be58daed52900956bb8221006a62f50911`.
Candidate Mac SHA-256:
`97850c82618b81bb2a92e5eb6fd47cdf128ec0c8e8f468dee4dd6edc4d622286`.
Final Mac (only overlay-label shortening):
`9d9e36553433c23df0522c567df7f8a46262684943b72cbbdf37c48e718c20b7`.
Final Linux:
`1543433a92fd086f130ace6041d5f22f07b79ad3d165c110d4b8ff2a9ec9091b`.

## Measurements

Local navigation, 2,000 ticks per run: render CPU 1.806 [1.579–1.877] →
1.465 [1.189–1.539] ms, **18.9% lower**. SIM upload .498 → .026 ms;
world animation .297 → zero. Total instrumented texture uploads 1.867 → .353
MiB/present (**81.1% lower**), with one atlas hit per warm frame and no further
snapshot copies. Remaining world-transfer time includes the independent
animated mountain atlas. Cadence varied between runs; these CPU savings are
not an equal visible-FPS claim. Every timed replay's final WRAM matches.

Two local Palace ABBAABBA cohorts are deliberately retained, including noisy
runs. The first control/candidate medians are 2.312 / 2.323 ms; the repeat is
3.001 / 2.549 ms. Individual runs span 1.950–6.836 ms with large unrelated PPU
variation. **No precise local Palace CPU percentage is established.** Both
cohorts consistently eliminate warm animation work and reduce uploads from
approximately 1.988 to .473 MiB/present (76.2%). No third local repeat was used
to select a favorable headline.

### Steam Deck directional check

Isolated Vulkan, Quality, three helpers, 1080x672 hidden output, 2,000 ticks,
both binaries warmed before ABBA. Two runs per variant; settled-window weighting
and run medians. All full replays and WRAM checks pass:

| Scene | Control CPU, ms [range] | Candidate CPU, ms [range] | CPU reduction | Cadence, ms | Texture upload MiB/present |
| --- | --- | --- | --- | --- | --- |
| Palace | 3.878 [3.862–3.894] | 3.285 [3.273–3.298] | 15.3% | 5.142 → 4.469 | 1.992 → .473 |
| Navigation | 2.974 [2.948–2.999] | 2.340 [2.340–2.341] | 21.3% | 4.240 → 3.545 | 1.923 → .427 |

Palace SIM-upload scope .725 → .093 ms; navigation .775 → .073 ms. Warm atlas
hits are 1/present, with no subsequent atlas copies, animation jobs or helper
joins. The same independent PPU work is approximately unchanged on Deck.
Moving-navigation settled time windows cover slightly different trajectory
segments at different throughput; geometry-byte differences are not attributed
to the atlas cache. These are short directional checks, not visible-FPS,
GPU-execution-time, energy or battery-life claims.

No RAM/GPU/time guard trips or optional-source rejections. Maximum observed GPU
allocation growth: Palace control 242 MiB / candidate 329 MiB; navigation 269 /
411 MiB. Available system RAM stays above 11.4 GiB. This trades additional
resident textures for CPU/transfer work as intended. The focused Vulkan atlas
fixture passes, and two Deck Palace composites are byte-identical, with matching
final WRAM. D3D12 remains untested at runtime; texture copies use the existing
portable SDL GPU API and no new shader format.

Remote evidence: `/home/deck/argame/atlas-probe-20260912.ehqLsL/`. Local copies
are `direction/`, `deck-navigation/`, `visual/` in the scratch directory above.
The initial 2,000-tick Palace warmup stopped at its 1,790-present replay end;
its strict completion assertion rejected it. It remains in `warmup/`, is not
part of the measurements, and was rerun with `AR_REPLAY_NOSTOP=1` in
`warmup-complete/`. Installed game/settings/saves were untouched.

During evidence retrieval the Deck navigation JSON accidentally replaced the
local navigation summary, not its eight original logs or run bundles. Remote
files were moved into `deck-navigation/`; `restore-navigation-report.py`
reconstructed the local summary from all eight untouched logs and the preceding
visual cohort's verified identical input pins, rechecking completion, WRAM and
the previously reported medians. The reconstruction is marked in that JSON.
