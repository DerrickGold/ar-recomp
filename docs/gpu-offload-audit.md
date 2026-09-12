# GPU offload and fallback audit — 2026-09-11

Scope: world navigation, SIM towns and Sky Palace. Action-mode expansion is
deferred. This is a source audit and implementation priority list, not a claim
that every listed path has already moved to the GPU or has a measured gain.

## Policy

Keep cheap conservative object/chunk culling and view-driven LOD on the CPU.
Clearly invisible objects should never need detailed per-vertex work. Partial
visibility is not a reason to return projection or polygon clipping to the CPU.
Visible/uncertain objects can remain GPU-resident, with hardware clipping and
depth resolving their visible fragments. CPU caches/workers remain useful for
scene updates, source geometry/shading generation and compatibility paths.

Distinguish three cases in code and diagnostics:

1. A genuine GPU failure/unsupported capability, invalid resource or explicit
   diagnostic/quality opt-out. Keep a safe, bounded alternative.
2. A CPU implementation which is still the normal path, despite a GPU shader
   drawing the resulting triangles. This is an offload opportunity, not a
   runtime GPU failure.
3. Cheap culling which avoids work altogether. Preserve it unless measurement
   shows its cost exceeds what it saves. Do not confuse it with polygon clipping.

## Concrete findings

The table records the starting audit; completed changes and measurements are
listed under Follow-through below. Moving-globe resident-source model integration
is now enabled by default for offline testing; terrain and moving SIM integration remain.

| Priority | Current CPU work | GPU-oriented change and constraints |
| --- | --- | --- |
| High: moving globe models | `ProjectWorldNavigationModelRange` performs per-vertex placement, radial deformation and projection; `FlushWorldNavigationModels` submits/clips the faces. The key in `DrawWorldNavigationTowns` includes the camera, so held-view retention cannot help a moving view. | Retain camera-independent source geometry; deform/project/clip on the GPU. Preserve authored models, revision ownership, LOD, animation and draw order. The new hardware-clipping prototype removes the partial-frustum restriction, but is not integrated into game views yet. |
| High: globe ground/ocean/cliffs | `ProjectWorldNavigationGroundRange` rotates normals, computes raised positions and lighting, then projects them. `DrawWorldNavigationGroundLayers` retains already-projected geometry only for repeated views. | Retain source surface geometry and use the same frame transform/depth mapping as models. Include textures/UVs and terrain lighting in the contract; the current untextured Solid prototype cannot simply be reused unchanged. |
| High, lower integration risk: held SIM geometry | `CollectDepthGeometry` re-appends the CPU projected-solid cache every presentation. `SimBackgroundMountainRender_SubmitFaces` reconstructs and appends cached mountain vertices. Both cause ordinary buffer collection/upload even when the view is unchanged. | Extend GPU retention to town solids and mountain batches, using their existing exact cache keys. Keep live atlas contents, scene/light/viewport invalidation and resource budgets. This can avoid copies/uploads without changing perspective rules. |
| High: moving SIM models | `DrawModel` projects every vertex on cache misses, including model-facing lean, grounded placement and separate bridge depth placement. | Express those visual operations in a project-private GPU contract rather than treating them as permanent CPU exceptions. Pixel-clean snapping and bridge/actor depth relationships require explicit coverage. Cheap `ObjectMayBeVisible` and LOD checks remain useful. |
| Medium: town terrain depth/shadow submission | `SimBackgroundVoxelTerrainDepth_Append` loops the projected terrain cache and repacks depth-only and shadow-receiver vertices. The surface is reused across separate shadow/model passes, but is submitted again. | Retain/reuse GPU geometry across the existing passes first. Do not merge or remove passes without preserving authentic actor bands and shadow/depth ownership. DepthOccluder and ShadowReceiver are not currently accepted by the opaque retained-mesh API; this needs an intentional contract extension. |
| Medium: moving globe weather receivers | GPU spherical UV mapping is already used by `AppendWorldNavigationReceivers`, but `PrepareWorldNavigationReceivers` and `PrepareWorldNavigationSphericalMesh` rebuild camera-dependent clipped receiver geometry. | Share GPU-resident surface positions/transforms with terrain, and let hardware handle clipping. Preserve longitude seams, affine texture policy and the independent weather sample budget. Moving UV work to the GPU alone does not eliminate CPU geometry preparation. |
| Lower: foreground Palace cloud slices | `PresentWorldNavSky_DrawClouds` creates camera-facing world corners, projects them, and CPU-clips the moving slices every frame. | GPU billboarding/instanced slices can use bank offsets and a frame transform. Preserve the already-cached global back-to-front order. Measure against the small bounded slice count before spending substantial effort here. |

## Actual fallbacks and avoidable preparation

- `SimCloudEffectBackend_IsAvailable` enables town GPU cloud composition by
  default. It rejects missing custom-shader capability, incompatible renderer,
  shader/state creation failure or `AR_SIM3D_CLOUD_GPU=0`. These are not
  partial-visibility fallbacks. `Bind` additionally validates values and reports
  state/uniform binding failure. A failed draw is not silently retried as an
  expensive full CPU render.
- `PrepareSimCloudMesh` formerly prepared three fallback vertex/UV arrays on a
  camera-key change even when GPU composition was available. The implementation
  below now initializes them lazily. This was bounded setup work, not the
  principal moving-globe bottleneck.
- Globe spherical shadow mapping falls back to CPU UV generation when its
  optional mesh preparation/publication fails. Resource limits, allocation and
  shader availability must remain explicit. Nothing here establishes that
  these failures commonly occur on the Deck; collect reason counters before
  attributing low FPS to them.
- The globe's 16-static-span retention guard limits draw-call amplification
  around animated windmills. It uses the existing CPU **projected cache**, not
  a full model reprojection. A prior 72-windmill fixture demonstrated why simply
  deleting this guard is harmful. GPU-side animation/batching is the way to
  remove that restriction while preserving ordering.
- Palace cloud density/light atlas baking occurs on key changes; cloud sorting
  is cached. They are not per-frame CPU fallbacks in a settled view. Leave
  one-time/cached work on the CPU unless measured cost justifies another path.

## Follow-through and evidence

### Shared surface boundary and Vulkan clipping correction

The [shared surface prototype](gpu-surface-prototype.md) now retains source
quads for a common GPU Ground/CloudShadow transform. An opt-in live land grid
now includes blur/haze in that same transform; ocean/cliffs/mountains remain
on their current path. Repeated local timing and upload/draw tradeoffs are
recorded in the prototype document; this is not yet a default-path promotion.
Validation also found and fixed pre-existing Vulkan
behind-eye affine attribute loss in the current model shaders, without a CPU
fallback. Existing default paths and multicore support remain available.

### Implemented: grouped SIM retention and lazy cloud fallback

Town solid models, mountain relief, invisible terrain depth and shadow
receivers now reuse GPU-resident batches in stable views. One grouped handle
belongs to the model pass and one to the separate ground-shadow pass, keeping
the existing four-opaque-handle limit alongside the globe's two handles.
No per-material resource budget increase, shader change, model reduction or
actor-band/pass merge. The two optional town handles together reserve at most
40 MiB of vertex/transfer storage, excluding driver cycling, within the
existing four-handle maximum. `AR_SIM3D_TOWN_RETAINED=0` disables the path.

The complete projection/scene/light/style/viewport key is copied; shadow
presence and opacity are included, but mask/atlas contents remain live.
Continuous camera changes do not generate extra never-reused publications.
Allocation/capture failure is latched for the key; reset permits republishing.
Dynamic volcano effects and actor callbacks still run on every presentation.
Grouped range appends validate the entire draw budget before queuing anything,
so fallback cannot draw only half of a retained pass twice.

SIM cloud fallback arrays are now constructed lazily from the shared unscaled
UV/coverage vertices, only when the GPU composition path cannot be used. The
original float order and fallback output are preserved; a later binding failure
does not repeat camera projection or coverage evaluation.

The Detailed overlay and `[pipeline-path]` log distinguish CPU projection,
CPU staging, GPU reuse/publication, explicit opt-out, known draw-limit guards
and optional API/resource rejection. `[sim3d-path]` adds stage attribution when
the focused SIM profiler is enabled. Counts are coarse events, not frame
percentages or a claim that every path is offloaded. CPU source projection in
moving globe/town views is still normal work and is not mislabeled a GPU failure.

Mac/Metal evidence: `/private/tmp/actraiser-town-retained.IBEHhJ/`. The populated
Aitos eruption replay measured these final eight-run ABBAABBA comparisons
(CPU wall time, median [min–max] ms; separate captures, isolated saves/settings):

| Requested helpers | Control render CPU | Retained render CPU |
| --- | --- | --- |
| 3 | 2.805 [2.783–2.825] | 2.703 [2.656–2.722] |
| 0 | 2.875 [2.787–2.883] | 2.810 [2.778–2.828] |

The earlier eight-run batch measured 2.783 → 2.715 ms with three helpers.
Final reductions are 3.6% / 2.3% in total render CPU, not Deck FPS. Zero-helper
ranges overlap, so its whole-pipeline gain is less certain. With three helpers,
the targeted model-stage median fell 0.167 → 0.081 ms, depth submission
0.066 → 0.041 ms, and shadow work 0.176 → 0.147 ms. Settled geometry uploads
fell from about 1.66 to 0.41–0.44 MiB/present with unchanged geometry/draw counts.
All 162 tests passed, including grouped material/depth ordering, atomic range
rejection, resource limits, warm uploads, reset and lazy cloud fallback tests.
Nine full-game checkpoints are byte-identical with clouds off, with GPU clouds
on (three helpers), and with CPU fallback clouds on (zero helpers): 27 paired
captures, with identical final WRAM in each comparison. GPU geometry tests
also verify that live published shadow textures still affect retained receivers.

### Additional finding: SDL renderer/custom-GPU command ordering

The new live-shadow test exposed a pre-existing boundary assumption in the
depth adapter: `SDL_FlushRenderer` does not submit the SDL GPU renderer's
command buffer. In the inspected SDL 3.4.12 backend, `GPU_RunCommandQueue`
records and ends a render pass, whereas `GPU_RenderPresent` submits the
command buffer. A separately submitted custom depth pass can therefore read
the previous shadow mask. Readback can mask this by forcing submission.
The grouped-retention test explicitly publishes its mask producer to isolate
cache/material correctness; it is not evidence of correct same-frame interop.

Source: [SDL 3.4.12 GPU renderer](https://github.com/libsdl-org/SDL/blob/release-3.4.12/src/render/gpu/SDL_render_gpu.c).
The default platform-adapter implementation now uses SDL's public offscreen GPU
renderer. It submits preceding 2D producers/consumers before
each custom pass; the adapter owns the one final window blit/present. No private
SDL structures, CPU readback, added fence waits, runner ABI changes or game-side
native handles. `AR_SDL_GPU_ORDERED=0` explicitly selects the former adapter for
diagnosis; correctness takes priority over the performance-neutral cases.
See [ordered-gpu-submission.md](ordered-gpu-submission.md) for lifecycle,
same-frame correctness, compatibility and performance evidence.

### Remaining integration work

Held globe ground and native mountain cutouts now share the existing retained
surface handle. This removes repeated mountain staging/uploads without changing
shaders, art, depth or resource budgets; moving camera projection is unchanged.
See [gpu-world-surfaces.md](gpu-world-surfaces.md) for exact-pixel tests, repeated
Mac results and short guarded Vulkan verification.

The whole-model work now has a default-on radial source/ordered-index path
in navigation and Sky Palace. See [gpu-world-models.md](gpu-world-models.md) for
its ownership, batching, validation and measurement record. It retains cheap
CPU culling and authored LODs while moving per-vertex placement/projection and
partial clipping to the GPU. `AR_SIM3D_WORLD_GPU_MODELS=0` restores the CPU path;
capability/resource failures also preserve the existing CPU/multicore fallback.
Native Deck validation and small legacy raster differences remain open.

Finish its full-game visual/performance acceptance, then extend the explicit
source-surface contract for ground/depth/weather. Those changes attack CPU preparation and
uploads together rather than adding more small draws or hiding work on a
different thread. Keep existing multicore support for remaining work.

The authentic-view fallback log is a different layer and must not be
treated as proof of a geometry-cache or GPU-shader fallback.

The clipping experiment and repeated measurements are recorded in
[gpu-model-prototype.md](gpu-model-prototype.md). Integration should be judged
with repeated full-game CPU timings, upload/draw counts, visual/motion checks
and zero/three-helper references. Compile all shipped shader formats, and
validate the native Vulkan path on Deck before claiming Deck-specific gains.
