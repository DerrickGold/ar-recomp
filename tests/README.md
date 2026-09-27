# Tests

Run the ordinary local gate with `make check`. `make check-release` adds the
optimized suite and is required by `make release` before packaging.
The full C/Python suite is available through CTest in a testing build.
No GitHub CI is required.

## Finding and running a test

Most tests use the production module's name followed by `_test.c`. Start with the
feature visible in the game, then narrow to its behavior or presentation layer:

| Area | Test names / directory |
| --- | --- |
| Original game rules and CPU hooks | `actraiser_*_test.c` |
| Regional choices and save-session behavior | `regional_*_test.c`, `regional_session/` |
| Simulation menus | `sim_menu_*_test.c`, `present_sim_menu_test.c` |
| Dialogue and localized text | `dialogue_*`, `localized_text_*`, `text_*`, `language_*` |
| Simulation rendering | `sim3d_*`, `sim_background_*`, `present_sim3d_*`, `diorama_*` |
| Application, input and saves | `app_*`, `host_*`, `save_*` |
| Developer tools and generated-data contracts | Python `*_test.py`, `fixtures/` |

Use CTest's names to discover and run a focused slice after building:

```sh
ctest --test-dir build-check -N -R sim_menu
ctest --test-dir build-check --output-on-failure -R sim_menu
ctest --test-dir build-check --output-on-failure -L session
```

The regional-session executable has five independently runnable suites. Its
entry point in `regional_session_test.c` only selects a suite; the checks live
together under `regional_session/`:

| Suite | Responsibility |
| --- | --- |
| `activation.c` | Revision checks, pending choices and gameplay activation boundaries |
| `persistence.c` | Native/INI saves, interrupted writes and recovery |
| `codec.c` | Frozen bytes, schema compatibility and corrupt payload rejection |
| `profiles.c` | Profile expansion, descriptions and atomic application |
| `randomizer.c` | Deterministic, independent and compatible regional choices |

`fixture.c` owns only stateless helpers shared by these suites. Each suite owns
its failure count and can run in a separate process. Running
`build-check/actraiser_regional_session_test` without arguments runs all five;
pass a suite name (for example `codec`) to run just that group. CTest registers
each group as `actraiser_regional_session_<suite>`.

## Assertions and fixtures

Include `support/test_assert.h` when using `assert()`. It keeps assertions and
their setup calls active in optimized builds without changing production's
`NDEBUG` policy. The `actraiser_test_assertions` probe explicitly compiles with
`NDEBUG` and fails if an assertion's expression is skipped.

For checks that should report several failures before returning, use
`support/test_check.h` with a suite-owned counter. Keep specialized diagnostics
where a test needs pixel coordinates, CPU registers or similar context.

Share setup and stable input fixtures within a feature first. Keep expected
values, corruption cases and golden payloads independent of the implementation
being checked; deriving them from the production serializer would hide bugs.
Compare decoded struct fields, not whole object representations: padding bytes
are not saved data and can differ between Debug and Release builds.
`support/regional_test_values.h` shares these independent value checks for save
sessions, recipes, histories and actor/room snapshots. Raw-buffer comparisons
and failed-operation checks that require no bytes to change retain `memcmp`.
Authored test C follows the same layout rules as production and has no style
baseline exceptions.

To check optimized production code as well as the ordinary Debug gate:

```sh
make check-c-release CHECK_JOBS=3
make check-c-asan CHECK_JOBS=3
```

The matching CMake configure/build/test presets are `tests-debug`,
`tests-release` and `tests-asan`. Each uses its own build directory. The ASan +
UBSan preset instruments the harness and its implementation dependencies; it
requires GCC or Clang and stays separate from the release gate. To focus a
configured sanitizer build, use `ctest --preset tests-asan -R save_slots`.

## Shared implementation dependencies

`cmake/TestSupport.cmake` owns shared ROM-free implementation libraries for
performance metrics, rendering, text, scene math, ROM decoding, campaign/save
support and town models. Test executables list their harness and feature-specific
sources, then link these owners. Adding or moving a shared implementation belongs
in its library declaration rather than in every executable's source list.

The archives preserve focused stubs: only needed object files enter a test.
Before sharing another source, check both its code and included headers for
per-target definitions. Shader/reference, font and world-navigation variants
remain on their original targets. The world-navigation source pool deliberately
compiles its cache-testing and globe-testing variants separately; their common
render/math/town dependencies are shared. Threaded and serial voxel tests compile
their harness separately and link the same flag-independent town model.

The initial consolidation checked 111 source/flag combinations by comparing
preprocessed output with and without the tests' `AR_` definitions. It also keeps
CPU/HLE fatal support and the real save store in explicitly named libraries.
Do not obtain one executable's `SOURCES` to assemble another executable.

The menu harness supplies `host_clock_stub.c` instead of the platform clock.
All input timing, expiry and screenshot rendering therefore use the same
controlled time; production continues to use the real host clock.
