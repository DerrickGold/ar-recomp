# Go tooling migration boundary

This inventory records which repository utilities belong in the portable
`snesrecomp-go` toolchain and why the remaining Python files do not. The rule is
semantic ownership, not file size: reusable compiler, ROM, trace, and bring-up
mechanics belong in Go; ActRaiser content knowledge and enhancement asset
authoring stay with the game project.

No Python interpreter is required to regenerate, analyze, configure, build, or
package a normal downstream game. The root CMake project has one optional
Python-backed ActRaiser background-HLE test, enabled only when Python is already
available.

## Migrated reusable tools

| Retired Python tool | Go command / implementation |
|---|---|
| `dis65.py` | `snesbuild disasm` / `v2regen disasm` |
| `romxref.py` | `xref`, including branch, access, raw-word, and target-minus-one evidence |
| `find_rts_webs.py` | `rts-webs` |
| `find_yield_helpers.py` | `rts-webs --yield-helpers` |
| `find_tailcall_past_end.py` | `link-audit --tailcalls` |
| `trace_slice.py`, `resolve_miss.py` | `trace-inspect` |
| `rom_info.py` | `rom-info` |
| `dis_spc700.py` | `spc-disasm` |
| `find_yield_points.py` | `poll-census` |
| `oracle/diff_trace.py`, `diff_seq.py`, `diff_aligned.py` | `trace-diff final`, `sequence`, and `aligned` |
| `wram.py` | `wram get`, `diff`, `scan`, and `symbols` with an optional game-owned Markdown symbol map |
| `dump_chr.py`, `dump_snapshot_chr.py`, `icon_picker_sheet.py` | `chr-render rom`, `snapshot`, and `icons` |
| `benchmark_runner_replays.py` | manifest-defined `snesbuild replay-bench` with isolated saves, artifact hashes, hard-diagnostic gates, adjacent A/B pairs, and performance thresholds |

The generic Quintet bit-packed LZSS implementation is also available as
`quintet-lzss`. `tools/quintet_lzss.py` remains deliberately: it is an
independent implementation used as an oracle by ActRaiser's collision/content
research rather than production tooling.

`mx-diff` provides the generic game-frame M/X comparison. The remaining
`tools/oracle/diff_mx.py` adds an ActRaiser-only boss-to-simulation anchor based
on `$18/$1A`; putting that policy in the shared command would make the compiler
tooling game-specific.

## Intentionally game-owned Python

These files encode ActRaiser addresses, data layouts, content meanings, frontend
environment variables, or enhancement policies. They are not candidates for a
literal `snesrecomp-go` port:

- ROM/content and save schemas: `act_collision.py`, `act_content.py`,
  `action_magic_catalog.py`, `find_handler_chain.py`, `srm.py`, and
  `town_structs.py`.
- Action-background and 3D enhancement research: `bg_hle.py`,
  `bg_hle_census.py`, `bg_hle_artifact_compare.py`, `bg_hle_matrix.py`,
  `bg_render_level.py`, `sim3d_demo.py`, `sim_bg_tile_catalog.py`,
  `sim_object_catalog.py`, and `make_sim_voxel_comparison_sheet.py`.
- ActRaiser native-audio validation: `analyze_native_audio_trace.py`,
  `audit_spc_effects.py`, `compare_native_audio_pcm.py`, and
  `verify_quickstate_pcm.py`. The generic SPC decoding used by the audit is now
  supplied by `snesbuild spc-disasm`.
- `build_shaders.py` is an optional renderer asset-authoring pipeline around
  external shader compilers. Checked-in generated headers are consumed by
  normal builds, so it is not a host/build dependency and is outside the
  recompiler boundary.
- `ar_lib.py` is now a small game-side helper containing only ROM mapping,
  hashes, endian reads, and dependency-free PNG output used by the scripts
  above. Its duplicate 65816 decoder, metadata loader, symbol parser, and cfg
  hazard logic were removed after their consumers moved to Go.

`tests/bg_hle_census_test.py` remains an optional ActRaiser enhancement
conformance test. `tools/action_editor/build.sh` also contains a small Python
export step for that game-specific editor; neither participates in compiler or
downstream game bring-up.

## Future boundary

`find_handler_chain.py` demonstrates useful provenance shapes—handler values in
spawn records and object fields—but its current seeds, field offsets, table
layout, and coherence filters are all ActRaiser-specific. Those concepts should
move only as part of the game-agnostic value-provenance and code/data ownership
analysis, with synthetic fixtures and evidence classifications, rather than as
a renamed copy of this script.
