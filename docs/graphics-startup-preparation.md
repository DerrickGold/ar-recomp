# Graphics startup preparation

2026-09-12. Resolves the actionable findings in the enhanced-view fallback review
and prepares the hardware-dependent graphics features before gameplay.

## Startup contract

The SDL adapter logs the actual GPU backend, device/driver identity and shader
formats. Preparation reports the texture limit and each prepared pipeline group;
the host also records logical CPU count and system RAM. Capability decisions use
actual resource/pipeline operations, not GPU model-name heuristics or assumed
performance tiers.

`RenderPreparation_Prepare` operates on scratch resources through portable
contracts. It prepares depth, model, radial model, retained surface and spherical
body pipelines, plus blur, rim, DOF, CRT, cloud and shadow shaders. SDL custom
fragment states are exercised against small offscreen targets in both RGBA/BGRA
byte orders and the ordinary blend variants to materialize their draw pipelines.
No window frame, readback or game-state mutation is required for preparation.

Settings receives an application-owned feature mask, not SDL handles. Unsupported
masters/effects are disabled for this session and greyed out with an explanation.
Their saved requested values survive automatic settings saves, allowing the same
configuration to retain its intent on capable hardware. Explicitly turning a
blocked setting off clears that saved intent. Numerical CRT controls are also
unavailable without its shader; dependent SIM/world controls follow their masters.

Equivalent ordinary cloud/shadow draw implementations remain usable when only
their acceleration shader is unavailable. This is not a visual-quality reduction.
The minimum SDL GPU renderer remains required. A scratch-target/state failure
that prevents safe preparation stops startup; an unsupported individual feature
does not disable unrelated features.

On graphics-resource reset, preparation runs again. It must restore the original
feature set; it cannot silently shrink that set or change the user's settings
mid-session. Device loss still reports an explicit session failure.

## Shader ownership

The warm-up regression test exposed an SDL custom-pipeline lifetime hazard:
warming one effect, releasing its shader, and allocating another shader could
reuse the pointer used as the renderer's pipeline-cache key. Cloud pixels then
matched an earlier effect's pipeline instead of the cloud shader. Reordering the
test hid the problem; retaining the correct lifetime fixes both orders.

The SDL backend now owns a small cache of compiled fragment shaders, keyed by
the immutable shader-blob descriptor and its resource signature. Effects borrow
these shaders and own only their render states. Ordinary effect resets recreate
states without recycling shader identities or recompiling the same source.
Backend teardown releases the shaders after effect-state teardown. This stays
entirely within the SDL adapter; no renderer or runner ABI changes are needed.

## Build/install versus first launch

`tools/build_shaders.py` already generates checked-in SPIR-V, DXIL and Metal
source headers. Ordinary builds/installations ship those headers' contents in the
executable and need no GLSL compiler on the player's system.

Driver-specific shader/pipeline preparation runs on the target GPU at boot.
A Mac builder cannot produce a generally reusable Steam Deck pipeline cache:
Vulkan pipeline-cache compatibility includes device/vendor and implementation
identity. See the [Vulkan pipeline cache guide](https://docs.vulkan.org/guide/latest/pipeline_cache.html)
and [SDL shader formats](https://wiki.libsdl.org/SDL3/SDL_GPUShaderFormat).
No cross-device persistent cache or install-time game launch has been added.

This prepares known application pipelines, not a guarantee that every driver
will perform zero internal work on a later draw. Scene-sized texture/mesh
allocation, output resizing and device lifetime still require runtime checks.

## Fallback fixes

- Palace foreground upload/draw or globe core failure now reaches an explicit
  presentation error, never a native framebuffer substitution.
- Connected SIM globe failure is a core failure, never a latched flat underlay.
  Flat underlay remains an explicit setting. Failed optional image-cache copies
  still use the identical direct-rendered image, preserving the optimization's
  correctness and appearance.
- SIM and Palace accept unused fixed-colour state when no colour-math layer is
  designated. Real effects, colour windows and overlay ownership remain checked.
- Metrics resolve SIM capture validity, master/ground settings and authentic
  comparison routing. Invisible brightness-zero/forced-blank frames do not count
  as visible unexpected fallback. Failed enhanced presents remain failures.
- Palace producer diagnostics distinguish API/query, PPU profile, stale snapshot,
  forced blank and foreground ownership rejection, and log reason changes only.

Unsupported live PPU/overlay semantics are not hardware capabilities: their
ownership guards remain in place. The no-op case is proven safe; broad removal
of capture validation would risk losing native foreground/UI content.

## Verification

- Fifteen focused assertion-enabled portable suites pass, covering settings,
  fault-injected startup preparation, Palace failure routing, view telemetry,
  real PPU scanline/capture parity, metadata, world rendering and failure handling.
- Both real-Metal GPU suites pass: effect warm-up/reset plus cloud pixel parity
  on external and shipping ordered renderers,
  and prepared depth/model/surface pipelines with their existing rendering tests.
- Runner/render boundary checks and negative injection tests pass. The new
  portable source families are included in the boundary inventory.
- A private-save 1,400-tick Mac replay completes Palace → navigation → Palace →
  Aitos SIM, capturing the final enhanced composite. All nine feature bits are
  available; initial preparation took 327 ms in this correctness run. No Mac
  performance benchmark or claim of throughput improvement is made.
- The hermetic Linux build succeeds. A private Wayland/Vulkan Deck probe also
  completes 1,400 ticks through Palace/navigation/Aitos and captures the enhanced
  composite. All nine feature bits are available; preparation reports 26 ms
  on RADV VANGOGH (driver 25.99.99). These are individual launches, not cold-cache
  startup benchmarks. The installed game and live saves are unchanged.
- Mac and Deck final WRAM SHA-256 match:
  `0649f3b1cf152ec5d9d1f9744e7d5a66ee790af9fcd6a459835aa638732a6933`.
  Deck probe binary SHA-256:
  `b5f7fa47d9e356a651175369d453faf078b8225be2d6ef75846aa4cf46769c6c`.
  The final follow-up removes an unused private shader pointer array; the GPU
  suite is rerun after that ownership-only cleanup.

Local smoke artifacts: `/private/tmp/actraiser-startup-preparation.RqHWlD/`.
Deck smoke artifacts: `/home/deck/argame/graphics-startup.YfK3vo/`.

The unit Palace test covers the real selected-scene helper with injected failures;
the full-game smoke exercises successful routing. It does not inject a live GPU
failure into the complete host loop or substitute for campaign-wide release QA.
