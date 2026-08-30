# Snesrecomp binary runtime SDK

This directory is the source-free runner SDK shipped by a hermetic game
builder. Generated and authored game translation units compile against
`include/snesrecomp` and link the static library under `lib/<zig-target>`.

The target directory is part of the compatibility contract. Do not use an
archive built for a different operating system, CPU architecture, object
format, or runner configuration. Unix-like targets use
`libsnesrecomp_runtime.a`; Windows targets use `snesrecomp_runtime.lib`.

`snesbuild build --hermetic` selects the matching archive automatically. The
library intentionally leaves generated-game callbacks unresolved until the
final game link. Runner implementation sources and private headers are not
needed and are deliberately absent from this distribution.

Public ABI behavior, ownership rules, and integration workflows are described
under `docs/`. Redistribution terms and provenance are recorded in `LICENSE`,
`licenses/Snaggletooth-LICENSE.txt`, and `PROVENANCE.md`. The private C++20 DSP
unit exposes only the runner's C ABI and is built without exceptions or RTTI;
game translation units remain C11.
