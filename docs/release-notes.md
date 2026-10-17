# v0803 — draft

Changes since **v0802** (October 10, 2026).

This update focuses on smoother diorama presentation and startup feedback,
with an early Android preview for testers.

## Presentation

- Spread the diorama skybox across the level's camera travel, so the sky
  stops exactly when the foreground does instead of running ahead of it.
- Smooth layered background scrolling in Aitos, Northwall and Death Heim,
  fixing jumps and drift when the camera stops or the foreground reaches
  a level's edge.
- Fix gaps where diorama framing extended beyond the captured terrain,
  including at 4:3 and in very wide windows using **Screen ratio → Auto**.
  Refine Death Heim boss-room framing to show less floor.
- Restore the missing statue heads on Death Heim's hub platform.
- Let 3D church interiors fill widescreen displays and move the church HUD
  to the screen edges. Keep the original 2D artwork centered without repeating
  background tiles.
- Fix blue borders around 3D church scenes, including seams visible with CRT
  effects at 16:10 and wider aspect ratios.

Room-by-room widescreen and diorama presentation touch-ups remain ongoing.

## Startup and stability

- Prepare diorama action-stage shaders at startup or when the mode is enabled,
  reducing pauses when entering action stages.
- Show an animated loading indicator during lengthy GPU preparation.
- Prevent gameplay from briefly fast-forwarding after a host stall.
- Log shader and pipeline compilation times to help diagnose slow startup
  and graphics-driver issues.

## Android preview

- Add an early native Android build for arm64 devices running Android 10 or
  newer with Vulkan support.
- Add on-screen controls for gameplay and menus. They follow existing keyboard
  bindings and hide when a gamepad is connected.
- Fix effect-shader compilation failures encountered on an Adreno GPU.

Android is currently a source-build preview, with no Android target in the
Builder. Use `make android ROM=/path/to/your-usa-rom.sfc`; see the
[Android guide](https://github.com/DerrickGold/ar-recomp/blob/main/docs/android.md)
for setup and installation. The generated APK includes your ROM and is for
your own device.

## Action editor and tooling

- Add editable light boxes with brightness, soft edges, and separate effects
  on scenery, the player and enemies. Matching brightness regions merge
  without applying their lighting twice where they overlap.
- Place native effect guides over their actual source regions and refresh them
  after edits. Preview stays focused on the region being edited.
- Support Emscripten 6.0.12 alongside 5.0.7 for action-editor builds.
- Add project metadata for recomp.fyi tracking.

For desktop installations, rebuild your game with the updated Builder. See
[Updating an installation](https://github.com/DerrickGold/ar-recomp#4-update-or-import-an-existing-installation).
