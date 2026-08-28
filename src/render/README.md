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
targets, viewport and clip state, clears, textured quads, indexed geometry,
and present. Backends translate formats, filtering, and blend modes once per
resource or batch. Capability flags cover enhanced facilities such as depth,
custom shaders, texture wrapping, and optional blend modes.

Texture and geometry submissions may carry an `ArRenderDrawState`. Tint and
blend overrides apply to that submission only; an adapter must restore any
native resource state before returning. A draw without state takes the direct
backend path with no state queries or mutations. Geometry uses per-vertex
colour, so texture tint is intentionally limited to textured quads.
`ArRenderDevice_DrawSolidRect` is a convenience geometry batch rather than an
additional backend callback; it gives UI, backdrop, and overlay code explicit
blend behavior without expanding the minimum adapter surface.

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

The diorama compositor implementation and the custom two-pass rim-light,
shadow-blur, heat-haze, and CRT targets still use the SDL bridge internally.
Those paths need render-target/custom-effect extension contracts before a
non-SDL backend can implement them without carrying SDL. The bridge is a
transition aid, not part of a game-facing contract. New renderer-facing code
must not add another borrowed native resource.

Temporary target composition now has a portable scoped-target extension. It
captures and restores the caller's target, viewport, and clip, and reports a
distinct state-loss result when restoration fails. The ROM skybox fill pass is
the first consumer, followed by the diorama crisp-AA supersample pass; backends
without the optional capability omit these enhancements and retain their
established fallbacks.

The remaining migration should proceed in independently testable slices:

1. Extend the scoped render-target contract with backend custom-effect passes
   for the diorama compositor, rim light, shadow blur, heat haze, and CRT
   postprocess.
2. Move the remaining diorama compositor implementation and transient effect
   resource ownership under `src/platform/<backend>` while retaining a
   capability-gated baseline path.
3. Port the top-level presentation viewport/output contract and host UI/manual
   draws, then remove native types from the complete presentation directory.
4. Remove the SDL borrow/unwrap bridge after its final internal consumer is
   gone.

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
supports.

The SDL-free `actraiser_render_device` test is the reference contract test.
Run it together with the dependency guard and upload test:

```sh
ctest --test-dir build --output-on-failure \
  -R 'actraiser_(render_backend_boundary|render_device|presentation_upload_mirror)'
```
