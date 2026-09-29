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

The separate duplicate-moon treatment for Plane + skybox remains follow-up work.
Its selection must depend on a visible BG2 plane, preserving the original moon
in Skybox only or when the plane is hidden/alpha-zero.

Local evidence and small review video: `runs/bloodpool-skybox/` (ignored).
