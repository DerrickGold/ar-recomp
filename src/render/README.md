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

## Current migration state

The base and authentic SNES framebuffers, HUD planes and composite target,
Mode 7 replacement surface, manifest-driven screen replacements, persistent
separated-SIM atlas/layers, and persistent diorama planes are device-owned
handles. Their creation, destruction, zero-fill, and exact dirty-region
uploads use the neutral device. The SDL adapter preserves their existing
formats, nearest filtering, blend/tint behavior, render-target semantics, and
texture updates. A fake backend runs the same dirty-upload flow in tests
without SDL.

The SIM and diorama compositors still unwrap those opaque resources through a
small bridge in `src/platform/sdl/render_sdl.h` while their geometry and
per-draw state are migrated. Diorama frame generation also retains private SDL
endpoint/target textures. The bridge is a transition aid, not part of the
portable contract. New renderer-facing code should not add another borrowed
native resource.

The remaining migration should proceed in independently testable slices:

1. Express the remaining enhanced composites as portable texture and geometry
   batches, with explicit tint/blend state.
2. Move render-target effects, diorama frame generation, and enhanced
   SIM/diorama geometry behind device
   capabilities while retaining a baseline path.
3. Isolate custom shader/depth setup in optional backend extensions.
4. Remove the SDL borrow/unwrap bridge, then expand the boundary check to the
   complete presentation directory.

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
