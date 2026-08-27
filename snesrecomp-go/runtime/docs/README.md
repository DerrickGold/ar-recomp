# Runner documentation

These documents belong to the portable runner and travel with `runtime/` if it
becomes a standalone repository.

## Integration guides

- [`API_REFERENCE.md`](API_REFERENCE.md) is the operation, capability,
  ownership, lifetime, and result-code reference for the public C SDK.
- [`GAME_ENHANCEMENT_INTEGRATION.md`](GAME_ENHANCEMENT_INTEGRATION.md) explains
  how a game publishes widescreen graphics and enhanced audio without putting
  title-specific policy in the runner.
- [`RUNTIME.md`](RUNTIME.md) describes the runtime/game/host boundary and
  optional subsystems.

The recompiler, `snesbuild`, generated-project layout, and full hermetic build
pipeline are separate toolchain concerns and are not part of this runtime SDK.
