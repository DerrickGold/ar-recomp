# Bundled SNES runtime

This directory contains the C runtime and SNES hardware model consumed by
`snesrecomp-go` output. It originated as the `runner/` tree in
[`mstan/snesrecomp`](https://github.com/mstan/snesrecomp) and was carried into
the ActRaiser development fork before being bundled with the Go port. The
snapshot and full acknowledgement chain are recorded in
[`../ATTRIBUTION.md`](../ATTRIBUTION.md).

The copied runtime has since received integration-only changes for this module:
CMake paths now use `runtime/`, and ROM-specific RDNMI/dispatch policy is
selected through `RtlGameInfo` callbacks supplied by each game project. The
runtime remains C because it links directly with generated C; Go is a build-
time recompiler dependency, not a game-runtime dependency.

Use [`runner.cmake`](runner.cmake) from a per-game `CMakeLists.txt`. The game
must also supply its frontend, `RtlGameInfo`, generated sources, and HLE
functions. See
[`../docs/PROJECT_INTEGRATION.md`](../docs/PROJECT_INTEGRATION.md) and
[`../docs/RUNTIME.md`](../docs/RUNTIME.md).

The historical upstream repository did not declare an overall license at the
snapshot used for this port. The ActRaiserRecomp root MIT license therefore
excludes this directory; attribution alone does not grant redistribution
rights. The SNES core and individual files may also carry inherited or inline
terms that downstream distributors must review.
