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
- [`GAME_SDK_EVALUATION.md`](GAME_SDK_EVALUATION.md) records the cross-game
  Super Mario World and Zelda 3 integration exercise.

## Engineering records

- [`ABI_ROADMAP.md`](ABI_ROADMAP.md) is the chronological record of completed
  and deferred component-access work; it is not the normative API reference.
- [`PERFORMANCE_ROADMAP.md`](PERFORMANCE_ROADMAP.md) is the historical record
  of portable and architecture-specific performance investigations.

The recompiler, `snesbuild`, generated-project layout, and full hermetic build
pipeline are separate toolchain concerns. Their guide intentionally remains in
[`../../docs/PROJECT_INTEGRATION.md`](../../docs/PROJECT_INTEGRATION.md).
