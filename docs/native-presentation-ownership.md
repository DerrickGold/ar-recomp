# Native presentation ownership

Presentation identity comes from native producers and becomes visible at the
native upload boundary. Regional artwork may change tile numbers, colours,
part counts and part ordering without changing which game object owns them.

## Tracing a sprite

Start with [sprite ownership](../src/actraiser/actraiser_sprite_ownership.c).
It maps native actor descriptors and SIM spawn families to presentation roles.
The [sprite emitters](../src/actraiser/actraiser_widescreen_sprites.c) record the
actual OAM byte range after culling and any regional composition substitution.
There are no fixed expected part counts.

| Presentation | Native owner |
| --- | --- |
| Action HUD icon | `$00:923A` call inside the action sprite build |
| Town hourglass | Fixed SIM record `$083E`, spawn families `$02..04` |
| Palace selected magic | Fixed SIM record `$083E`, spawn families `$25..28` |
| World Palace / plaque / label | SIM spawn families `$31 / $32 / $33` in scene `$00/$09` |
| Death Heim statue eyes and ornaments | Source descriptors `$F3FA, F408, F430, F458, F486, F494, F4B2` in scene `$07/$01` |

These are identities in the recompiled US engine. Donor addresses remain drawing
data; a regional choice does not select a different native hook.

The lifetime has three steps:

1. Begin a native sprite build and record each emitted range.
2. Complete the build, staging its metadata and the full 544-byte OAM shadow.
3. Publish only after the native `$02:ACA3` OAM upload completes.

The [native wrappers](../src/actraiser/actraiser_sprite_upload.c) observe the
SIM `$01:ACD9` build and OAM upload without replacing their work or stack
contracts. The action build already has an adapter and uses the same lifecycle.
An incomplete build leaves the previous uploaded ownership visible. An upload
with unknown or overwritten contents invalidates ownership. The shadow comparison
checks for an unobserved overwrite; its bytes never determine object identity.
Consumers also require the matching scene. Initialization and shutdown reset all
ownership, and malformed or disjoint ranges decline enhanced capture.

HUD promotion in `actraiser_rtl.c` and `sim3d.c` consumes the uploaded icon role.
[World composition](../src/sim/sim_world_navigation_scene.c) consumes the three
world roles, including the location captured with the label's sprite build.
An entirely empty completed world build represents a hidden animation frame.
Unknown emitted actors or incomplete plaque/Palace ownership reject composition.

## Overlap and Death Heim

Ownership says which slots belong to the statues; PPU arbitration says which
slot actually won each pixel. The runtime's `SR_PPU_OBJ_CAPTURE_WINNERS` surface
records only winning pixels from the selected range, including native OAM
priority rotation. Death Heim uses this surface to move the eyes into the face
layer. Equal RGB values on an overlapping actor cannot impersonate an eye.

The existing `SR_PPU_OBJ_CAPTURE_RANGE` surface has a different purpose: it
renders the selected slots independently, including parts hidden by other slots.
HUD restoration still uses that behavior. The two surfaces can coexist through
separate requests; one request cannot select both meanings. Frame reset clears
both. A runtime that cannot provide winner capture leaves native OBJ rendering.

## Terrain and displayed animation

[World-map decoding](../src/sim/sim_world_map.c) derives material coverage from
native source indices: `$10/$11` for water, `$01..09` for vegetation and `$40..45`
for mountain shading. Water coverage is stable across its wave frames.
[Navigation terrain](../src/sim/sim_world_navigation_terrain.c) consumes these
material masks instead of classifying rendered blue or green pixels. Palette
changes and black fades cannot change the inferred coastline or terrain height.

[Town replacement ground](../src/sim/sim_background_voxels.c) selects a displayed
plain metatile (`$08`, then `$00`), excluding occupied and cliff cells. Its live
tile entries must match a defined native metatile. The town palette supplies the
actual grass, sand or snow. If no source is known, original pixels remain.

Displayed metatile matching remains intentional for building and terrain
animations: logical terrain may already have changed while the previous native
animation is still on screen. Replacing it with logical state alone would make
enhanced objects change too early.

## Verification

- `actraiser_sprite_ownership_test`: staged versus uploaded lifetime, scene
  changes, overwritten shadows, native wrapper contracts and variable counts.
- `sim_render_metadata_test`: world roles, changed ordering and rejected gaps.
- Runtime PPU and ABI tests: overlapping equal-colour sprites, native priorities,
  rotation, cached/reference agreement and independent capture lifetimes.
- Terrain and voxel tests: palette-invariant materials, black fades, snow,
  water/cliff exclusion and displayed terrain definitions.

Headless native replay exercises town menus, the temple, world navigation and
Death Heim. It validates producer/upload behavior, including moving eye ranges;
it does not replace a GPU presentation check. Regional art and missing-donor
fallback use the same ownership path, independently of requested gameplay rules.
