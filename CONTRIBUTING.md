# Contributing

The same ownership and quality expectations apply to the game, Builder,
Workshop, debug utilities, benchmarks, and build scripts. Start with the
[repository layout](README.md#layout-and-testing), the
[Builder entry points](installer/README.md), or the
[recompiler and runtime](snesrecomp-go/README.md) for the area you are changing.

## Developer checks

Install the [native build dependencies](README.md#build-from-source), Ninja,
Python 3.10+, Node.js 24+, and Go 1.25+ for the full repository checks. Native
desktop-shell tests also need the host libraries described in
[desktop packaging](docs/desktop-packaging.md). Set up Python tooling once:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/requirements-quality.txt
npm ci
brew install shellcheck  # Debian/Ubuntu: apt install shellcheck
make check-quality
```

`make check-quality` checks C/C++ layout, private-header boundaries, global
declarations, the shipped source manifest, repeated constants, Go formatting
and `go vet`, Python correctness with Ruff, JavaScript correctness with ESLint, and
shell correctness with ShellCheck plus syntax with the declared interpreter.
It includes authored tools and installer code. Required tools fail loudly when missing. Ruff is pinned in
[`tools/requirements-quality.txt`](tools/requirements-quality.txt); ESLint and its
browser/Node globals are pinned in `package-lock.json`.

Run `make check` before committing changes that cross subsystems. It adds the
ROM-free C/Python tests, tests for all three Go modules, and shader regeneration
checks when the shader tools are installed. Shader checks explicitly report
when they skip. To require the complete shader check, run
`DXC=/path/to/dxc python tools/build_shaders.py --check`. This requires `glslc`,
`spirv-cross` and DXC. MSL and SPIR-V must match byte-for-byte; DXIL may differ
only in the compiler build identifier and its dependent hash, with matching
instructions, bindings, signatures and validation metadata. Shipped shader
headers are never rewritten by a check. Individual targets are `check-c`,
`check-go`, and `check-shaders`; `CHECK_JOBS=3` limits CTest parallelism. CMake requires Python
when tests are enabled so the ownership and generator checks cannot disappear.

`make check-release` runs `make check`, then the full optimized C/Python suite
in `build-tests-release/`. `make release` and `make release-<platform>` require
this local gate before packaging. A failed check stops packaging, and multiple
release targets in one invocation share a single check run, including with
`make -j`. Use the activated Python environment above, or pass
`PYTHON=/path/to/venv/bin/python` to Make.
The lower-level CMake packaging workflow only packages; run `make check-release` first
when invoking it directly. No GitHub Actions setup is required.

Use `make check-c-release` for optimized tests alone, or `make check-c-asan` for
the separate ROM-free AddressSanitizer/UndefinedBehaviorSanitizer gate. The
sanitizer preset instruments test harnesses and implementation libraries with
GCC or Clang; undefined-behavior reports fail tests. It uses `build-tests-asan/`
and does not change the normal development or play build. `CHECK_JOBS` limits
both compilation and test parallelism for these test targets.

These checks do not substitute for a game build, GPU acceptance, regional-ROM
tests, or platform packaging checks. For gameplay or
rendering changes, build the game and use `make check-render CONTROL=/path/to/old
CANDIDATE=/path/to/new` with your own ROM and a GPU. See the root Makefile for
the separate cross-platform and regional-ROM gates.

## Code conventions

- Keep behavior, its data, and private helpers near the feature that owns them.
  Name files after what a player or developer recognizes: save slots, world
  navigation, action rooms, or a specific tool. Shared mechanisms belong below
  their callers; avoid a general utility module for unrelated feature policy.
- Declare public variables and functions in the owning header. Keep private
  headers inside their subsystem. File-private mutable C state uses `s_`;
  cross-file state uses `g_` and an owner header. Prefer explicit inputs and
  lifecycle functions when a new dependency would otherwise reach into another
  feature's state.
- C/C++ uses two spaces and the root `.clang-format`. Format the files or
  sections you edit; do not sweep unrelated tests. `tools/check_style.py` is a
  ratchet for existing debt, not an exact formatter check. New files must be
  clean, and `--update-baseline` may only reduce existing violations. Large
  cohesive static definitions are fine; extract distinct behavior, not lines.
- Go uses `gofmt` and `go vet`. Python uses four spaces and the root Ruff
  correctness rules (`E9`, `F`). JavaScript uses two spaces and must pass
  ESLint. The file-based action editor is checked in its real shared script
  scope, using its page template as the load-order manifest. Shell scripts
  should declare POSIX `sh` or Bash accurately; use the interpreter's syntax and quote paths. `.editorconfig` supplies the
  shared whitespace defaults.
- Reuse `tools/ar_lib.py` for ROM addressing, endian reads, and other existing
  debug-tool primitives. Keep tool outputs in ignored build or evidence
  directories. Generated and vendored sources are excluded from authored-code
  gates; change their generators or upstream source instead.
- Test observable behavior and meaningful failure paths. Keep ROM-free fixtures
  synthetic. For mechanical moves, preserve behavior and use existing tests
  plus pixel/state comparisons where applicable. Add shared test dependencies
  as libraries instead of repeating implementation source lists.

The historical C style baseline remains an incremental cleanup task. JavaScript
and shell correctness checks are gates, not substitutes for runtime tests.
