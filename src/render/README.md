# Rendering backend boundary

`src/render` is the platform-neutral rendering contract for the game host.
It must remain usable without SDL headers or libraries. Native adapters live
under `src/platform/<backend>`; the current desktop adapter is
`src/platform/sdl`.

The runner ABI already publishes CPU-rendered PPU surfaces without depending
on SDL. This layer owns the next boundary: uploading those surfaces and
compositing game presentation into a platform window or native framebuffer.

## Contract

- Game and presentation state stores `ArRenderTexture`, never a native texture
  pointer. A handle is meaningful only to the device that created it.
- Resource creation and destruction go through the same `ArRenderDevice`.
- Portable code submits one operation per resource action or draw batch. The
  device is not called from pixel, tile, or vertex-generation loops.
- Backend capabilities describe optional facilities. Game code must select a
  supported presentation path instead of including a native API to probe it.
- Native headers, constants, error types, and handles stay in the platform
  adapter. `actraiser_render_backend_boundary` enforces this for the portable
  renderer and upload path.
- The render device is main-thread-owned. Frame slots may borrow handles for a
  synchronous present, but do not extend resource lifetime.

The mandatory baseline operations cover texture lifetime and uploads, render
targets, physical output coordinates and extent, viewport and clip state,
clears, textured quads, indexed geometry, and present. Backends translate
formats, filtering, and blend modes once per resource or batch. Capability
flags cover enhanced facilities such as depth, custom shaders, texture
wrapping, and optional blend modes.

Texture and geometry submissions may carry an `ArRenderDrawState`. Tint and
blend overrides apply to that submission only; an adapter must restore any
native resource state before returning. A draw without state takes the direct
backend path with no state queries or mutations. Geometry uses per-vertex
colour, so texture tint is intentionally limited to textured quads.
`ArRenderDevice_DrawSolidRect` is a convenience geometry batch rather than an
additional backend callback; it gives UI, backdrop, and overlay code explicit
blend behavior without expanding the minimum adapter surface.
`ArRenderDevice_DrawLine` follows the same rule for host guides and debug
overlays, generating one screen-space quad so backends need no native line API.

`ArRenderOutputFrame` is the corresponding frame-orchestration scope. It
selects physical coordinates, validates the requested aspect-fit viewport,
clears margins and scene colours without leaking backend state, supports
temporary full-output callbacks, and restores the full viewport on finish or
best-effort abort. Presentation paths share this policy instead of duplicating
platform viewport choreography. `ArRenderOutput_ResolveAspectFit` exposes the
same calculation without beginning a frame for effects that must first size
and bind an intermediate target. `ArRenderOutput_UseFull` is the terminal
overlay counterpart: it selects unclipped physical output coordinates without
clearing or changing the current target.

`ArPresentationLayout_ResolveViewport` owns the platform-neutral aspect-fit
calculation and logical content axes. The SDL adapter owns only SDL logical
presentation plus its temporary full-output state; obsolete duplicate native
render-target scoping has been removed in favor of `ArRenderDevice_BeginTarget`.
The terminal frame orchestrator and its public HUD/viewport contracts now use
only render-device rectangles and extents; platform present remains owned by
the host display layer.
HUD projection is a pure `render/hud_layout` module shared by drawing and
inspector hit-testing. Its inputs state authentic width and CRT pixel-aspect
policy explicitly, so the reusable math has no game globals or SDL vocabulary.
Settings, manual, comparison-frame, and debug-panel rendering contracts also
use portable rectangles and points. Their SDL window and event handling stays
host-owned and is independent of the geometry handed to a renderer backend.
The central `present.c` compositor is now part of the enforced SDL-free source
boundary and consumes the overlay through `settings_overlay_render.h`; the
host-facing menu header retains only its window and input integration surface.
Host cadence submits completed frames and queries physical output extent through
`ArRenderDevice`; swapchain configuration, display events, and window scaling
remain responsibilities of the selected platform host.

## Current migration state

The base and authentic SNES framebuffers, HUD planes and composite target,
Mode 7 replacement surface, manifest-driven screen replacements, persistent
separated-SIM atlas/layers, persistent world-navigation resources, background-
voxel ground, persistent diorama planes, and the decoded/authored ROM skybox
cache and diorama supersample target are device-owned handles. Their
creation, destruction, zero-fill, and exact dirty-region uploads use the
neutral device. The SDL adapter preserves their existing formats, filtering,
blend/tint/address behavior, render-target semantics, and texture updates. A
fake backend runs the same dirty-upload flow in tests without SDL.

Ordinary separated-SIM flat, world-layer, and menu-layer composites now submit
through the device, as do the standard projected SIM ground mesh, ordinary
object billboards, half-add billboards, promoted map-plane geometry, clip
state, solid backdrops, additive lightning flashes, and shared SIM/action
effect batches. The shared SIM/world-navigation sky gradient now builds and
submits one portable geometry batch. The persistent world-map underlay, its
downsampled blur, the full-town canvas, navigation cloud, and town cloud are
device-owned textures whose CPU images upload without native texture mappings.
Their projected extension meshes submit portable geometry, as do world-
navigation ground, active-region haze, light treatment, master fade, terrain,
mountains, voxel models, and terrain-clipped shadows. SIM effects generate
portable vertices directly; action effects and the diorama projection contract
do as well.

The SIM D32 contract is SDL-free and its current custom-GPU implementation is
isolated in `src/platform/sdl/sim3d_depth_pass_sdl.c`. Diorama composite and
frame-generation callers likewise exchange only `ArRenderDevice`, portable
rectangles, and opaque texture handles. The current frame-generation
implementation lives in `src/platform/sdl/diorama_frame_generation_sdl.c`;
its private endpoint and target textures are deliberately backend-owned.

The diorama compositor is now SDL-free. It requests physical output
coordinates and extent through `ArRenderDevice`, owns viewport-local scene
orchestration through portable viewport/clear/solid-rectangle operations, and
restores the full output viewport before callbacks or return. Layer, skybox,
shoebox, stack, skirt, shadow, and overflow geometry use portable vertices plus
scoped per-batch blend/address state. Its blur, rim-light, and combined
depth-of-field/edge-AA effects cross an SDL-free semantic contract in
`diorama/diorama_effect_backend.h`. Their
shader formats, native state, binding, and lifetime live in
`platform/sdl/diorama_effect_backend_sdl.c`. SIM shadow blur follows the same
pattern through `sim/sim_shadow_effect_backend.h`; its separable seven-tap SDL
implementation is isolated in `platform/sdl/sim_shadow_effect_backend_sdl.c`.
Enhanced world navigation is also SDL-free. It uses the aspect-fit output
scope directly, submits its map, haze, weather, backdrop, fade, and captured
UI through portable textures and geometry, and reads monotonic animation time
through the host clock boundary. Its coordinates stay local to the selected
game viewport, so platform adapters do not need an SDL-style logical
presentation transform.
The enhanced SIM-town frame owner now uses that same viewport-local scope and
portable full-output master fade. Its stage contracts use portable rectangle
types, opaque texture handles, and the host clock; they do not expose SDL
headers or native geometry to effects, projection, terrain, cloud, shadow, or
frame-orchestration code. GPU-only depth and shader adapters remain isolated
behind platform operations.
The ordinary flat/action frame owner now uses the same scope for its base PPU
frame, Mode 7 replacement, action effects, and forced-blank clear. Heat
refraction sizes its intermediate target from the portable aspect-fit query
and restores it through the scoped target/output contracts without capturing
native draw state or relying on logical-presentation transforms.
The diorama's flat HUD reconstruction target is likewise scoped through the
device and restores the actual caller target instead of assuming a CRT target.
Scene-inspector markers and action background-authoring guides now submit
portable line geometry and do not capture or mutate native draw state.
The in-game manual owns portable linear-filtered page textures and submits its
settled sheets, turning leaf, shadow, backdrop, and fading hint plate through
the render device. Its remaining SDL vocabulary is input-only.
The settings menu and draggable diagnostic panel likewise own only portable
atlas handles and submit all frames, fills, glyphs, icons, palette previews,
and text batches through the device. SDL remains at that boundary solely for
host events and the optional text-input window; cursor, status, and manual-hint
timing use the monotonic host clock contract.
Action heat refraction is a portable mesh warp rather than a custom shader; its
viewport-sized texture is now device-owned and uses the scoped-target contract
plus portable geometry for both its warped and fallback resolves.
Flat action-plane decorations now use device-owned streaming winner masks and
a scoped effect target. The blend vocabulary carries their premultiplied-add
resolve explicitly, so unsupported backends can reject that optional effect at
resource creation without exposing a native texture or draw-state probe. SIM
3D rim lighting likewise owns a portable linear target and expresses its
silhouette intersection as a destination-alpha mask blend; its atlas tint,
fill, mask, and additive resolve no longer borrow native texture state. SIM
shadows now use the same device-owned target model for caster geometry,
separable blur ping-pong, terrain-depth handoff, and final opacity. The
portable alpha-accumulate blend preserves the shader-free blur fallback
without exposing atlas or mask state. The last native SIM sprite draw is also
gone: eruption fireball fragments keep their shared-anchor rotation in scene
geometry and submit textured quads through the device, while upright fallback
fragments use the ordinary portable texture path. CRT
still uses the SDL-free semantic contract in `crt_post.h`; frame orchestration
owns player policy while `platform/sdl/crt_post_sdl.c` owns the preferred-format
scene target, shader formats, render state, and target-local presentation
plumbing. The scene target is exposed to composition only as an opaque handle.
Native texture wrapping and renderer access are private to
`platform/sdl/render_sdl_internal.h`. SDL-owned CRT, depth, shader, and frame-
generation adapters may use that interop; game-side code and ordinary backend
clients include only `render_sdl.h` and cannot unwrap opaque resources.

Temporary target composition now has a portable scoped-target extension. It
captures and restores the caller's target, viewport, and clip, and reports a
distinct state-loss result when restoration fails. The ROM skybox fill pass is
the first consumer, followed by the diorama crisp-AA supersample and action
heat-refraction passes; backends without the optional capability omit these
enhancements and retain their established fallbacks.

All current game-side frame composition, layout, viewport, effect geometry,
resource ownership, present submission, and developer snapshot orchestration
now cross portable contracts. Developer output capture requests row-pitched
RGB24 data; `platform/sdl/dev_tools_readback_sdl.c` owns native readback,
format conversion, and temporary-surface lifetime. Optional SDL GPU effects
and frame generation intentionally retain internal native-resource interop.
Future backends can omit those enhancements or implement their corresponding
semantic adapter contracts without exposing native handles to the game.

Each slice should preserve the replay state hashes, pass synthetic and real-PPU
render tests, and be measured against the checked-in replay performance gate.
A backend call per batch is acceptable; per-pixel dispatch, implicit full-frame
copies, and backend-owned game state are not.

## Adding a backend

Implement `ArRenderBackendOps`, populate `ArRenderCapabilities`, and bind the
adapter with `ArRenderDevice_Init`. The adapter owns native objects created for
opaque handles but not the application window unless its platform boot layer
explicitly transfers that ownership. Validate the baseline format and blend
mapping first, then add only the optional capabilities the native API actually
supports. `use_output_coordinates` must select one render unit per physical
pixel (or succeed as a no-op when that is already the native convention), and
`get_output_size` reports the current output or render-target extent.

The SDL-free `actraiser_render_device` test is the reference contract test.
Run it together with the dependency guard and upload test:

```sh
ctest --test-dir build --output-on-failure \
  -R 'actraiser_(render_backend_boundary|render_device|presentation_upload_mirror)'
```
