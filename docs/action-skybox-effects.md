# Action effects with Skybox only

The shared compositor supports backdrop effects without a BG2-low depth plane.
This applies to all stages; it does not introduce new artistic effects in stages
that have not received an environment pass yet.

## Projection and composition

- Captured skyboxes publish the source rectangle of each successfully drawn UV
  band, including blur insets, finite-world camera clamping and frame-generation
  translation. Backdrop lights, particles and leaves use those same rectangles.
- The completed capture publishes its canonical world X origin with the surface.
  Scanout and capture metadata share the finite-view mapping helper; presentation
  adds frame-generation translation without reconstructing the margin policy.
- The skybox covers the output while preserving the selected pixel aspect
  (square or 7:6 CRT). It centre-crops excess source width; a narrower source
  instead crops the camera's vertical window. All visible raster bands share
  one vertical scale and fitted source width, retaining their horizontal source
  offsets. The published effect projection includes that exact crop, so attached
  moonlight does not use a separate stretch or centre correction. Blur-safe
  insets and the finite source bounds remain in force.
- Each UV band clips its own geometry, so different row policies cannot stretch
  a beam across a boundary. The number of bands has the existing fixed bound.
- Named ROM skyboxes use the full displayed capture for ambient fields. These
  replacement pages have no live capture transform; this is suitable for the
  stock ROM-backed Aitos waterfall atmosphere. Selecting unrelated custom art
  does not semantically relocate a stage's authored light sources onto that art.
- Skybox effects draw immediately after the skybox and before foreground scenery
  and actors. Later moonlit receivers can still query the published source.
  Plane + skybox uses the ordinary plane mapping while the plane is visible;
  hiding it or setting its alpha to zero allows the skybox mapping instead.
- Skybox only removes the distant BG2-low/far and residual backdrop planes.
  BG2-high remains at its authored depth, so foreground water is not lost.
  Visibility, alpha and failed-upload gates still apply to that high band.
- The special Aitos mist that hides the finite BG2 plane's lower seam is omitted
  when that plane is replaced. Native splash accents and the waterfall flow
  field remain. Cave splash mist is separate and follows the water high band.

## Effect attachment audit

| Effects | Attachment without the distant plane |
| --- | --- |
| Fillmore forest rays, leaves and motes | Exact skybox footprint; midpoint parallax retained |
| Fillmore cave water, glints and splash mist | Authored BG2-high water geometry |
| Fillmore cave/temple dust, drips, bounce light, tower windows | Existing BG1 geometry |
| Bloodpool moon rays, reflections and distant clouds | Exact skybox footprint |
| Bloodpool shoreline mist | Existing BG1 geometry, callback after BG2-high |
| Bloodpool castle windows, gallery, moat and surface lighting | Existing foreground geometry; skybox moon available to receivers |
| Aitos waterfall flow | Captured skybox mapping or normalized ROM backdrop field |
| Aitos lava, torches, enemy and boss accents in every region | Existing BG1-high/OBJ attachment; independent of the backdrop plane |

## Validation (2026-09-28)

- Native Metal captures of all 49 action rooms across seven regions with Skybox
  only and 64 requested extra rows. Room identities and capture counts matched;
  no fatal, missing-width variant or background preflight mismatch was reported.
  These are sequential debug-warp smoke checks, not complete playthroughs.
- Additional native sequences cover the Fillmore cave/waterfall and Bloodpool
  marsh, castle gallery and boss. All four paired cave WRAM snapshots are
  byte-identical with environmental effects enabled and disabled.
- Regression tests cover row-band clipping, viewport offsets, source alignment,
  invalid inputs, forest midpoint parallax, foreground water depth, additive/
  alpha batching and retained foreground receivers. Presenter fixtures also
  verify that skybox effects require no additional texture or render target.
- Focused renderer/presenter/projection tests pass, including ASan/UBSan.
  Frame-generation tests pass on native Metal and the software fallback.
  Changed portable C renderer units compile for arm64 and x86_64.

No new shaders, GPU readbacks, intermediate surfaces, per-particle submissions
or per-frame heap allocations are introduced. Geometry uses the existing bounded
batches; multi-band backdrops rebuild those batches for each distinct UV band.
Steam Deck/Vulkan and Windows/D3D12 were not exercised on hardware in this pass.

## Room defaults and duplicate moons

Direction updated 2026-09-30: establish which rooms can use Skybox only and prefer
that compatible presentation to duplicating backdrop artwork. The user confirms
Bloodpool works in this mode. The room audit in the local release polish backlog
still covers subsections, bosses, transitions, Marahna's additive colour base,
Death Heim and the ending before promoting defaults. Disabling BG2 entirely is
different from removing only its distant plane; foreground water must survive.

Moon-free replacement art is deferred to any proven case that must retain both
representations. No new room defaults or replacement artwork are introduced by
the aspect correction. The source moon keeps its authored proportions under the
selected pixel aspect; choosing square pixels need not make art designed for CRT
pixels geometrically circular.

### Aspect correction validation, 2026-09-30

The former mapping independently fitted the captured width and camera's vertical
window to the output, stretching the Bloodpool moon. The shared crop now keeps
their scales in the selected pixel-aspect ratio. It adds no texture, upload,
shader or draw pass, and does not change the native camera or game state.

The game rebuild and seven focused skybox, projection, effect, camera, layer-order
and frame-generation tests pass. Aspect tests cover square/CRT pixels, wide and
narrow valid spans, landscape/portrait output, invalid inputs and capture-row
redistribution at a camera stop. The pure C mapping compiles for arm64 and x86_64;
the edited C/header files gain no style violations. The repository style gate
still reports pre-existing failures in other files.

Paired native Metal Bloodpool Act 1 captures match all eight WRAM snapshots
across square and CRT pixels. The CRT comparison contains 99 frames per version,
at normal speed with scripted movement and health/jump assists. Additional
four-frame smoke captures cover the castle entrance and Fillmore forest. These
are representative checks, not a completed all-room/default compatibility audit
or a Deck/D3D12 performance result. Evidence is in `runs/skybox-aspect/`, with a
480x300, 20 fps, 9.9-second before/after clip of about 294 KB.

Local evidence and small review video: `runs/bloodpool-skybox/` (ignored).
