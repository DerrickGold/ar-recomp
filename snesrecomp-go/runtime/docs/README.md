# Runner documentation

These guides describe the public runner SDK for game ports, mods, and tools.

## Integration guides

- [`INPUT_AND_BOOT.md`](INPUT_AND_BOOT.md) covers serial pad/mouse input,
  native SPC uploads, and snapshot compatibility.
- [`API_REFERENCE.md`](API_REFERENCE.md) is the operation, capability,
  ownership, lifetime, and result-code reference for the public C SDK.
- [`GAME_ENHANCEMENT_INTEGRATION.md`](GAME_ENHANCEMENT_INTEGRATION.md) explains
  how a game publishes widescreen graphics and enhanced audio without putting
  title-specific policy in the runner.

The recompiler, `snesbuild`, generated-project layout, and full hermetic build
pipeline are separate toolchain concerns and are not part of this runtime SDK.
