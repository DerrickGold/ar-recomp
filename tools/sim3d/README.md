# SIM checkpoint tooling

`../sim3d_demo.py` is the command-line entry point. It coordinates isolated game
runs and writes evidence; it no longer implements frame validation.

- `checkpoints.py`: manifest format and complete stage/camera policy pinning.
- `trace.py`: game-state traces, picker transitions and routine/object evidence.
- `metadata.py`: streaming JSONL parsing with file/line diagnostics.
- `metadata_checks.py`: capture readiness, picker transitions, counts, atlas
  packing, effect lifecycle, OAM ownership, height/shadows and world suffixes.
- `metadata_report.py`: counters, bounded diagnostics, summaries and expected
  checkpoint results. The D1/D2 labels in reports remain compatible with saved
  checkpoint manifests.
- `artifacts.py`: framebuffer comparisons, screenshots and voxel references.

Each metadata run owns a fresh validator. Cross-frame effect/height state stays
with the checks; counters stay in the report model. No module imports the CLI.
`tests/sim3d_metadata_tool_test.py` checks synthetic streams against complete
summary/diagnostic fixtures captured before the extraction, including errors,
allowed atlas exceptions, picker handoff, lifecycle gaps and ballistic motion.

Run `python3 tools/sim3d_demo.py --list` to discover the existing checkpoints.
Use `--dry-run` to inspect a launch without running the game. A real checkpoint
needs the user's ROM; visual checkpoints also need the GPU. The harness forces
per-run artifact directories in both development and release builds.
