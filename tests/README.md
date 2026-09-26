# Local test dependencies

Run the existing local gate with `make check`; `make release` runs it before
packaging. The full C/Python suite is available through CTest in a testing build.
No GitHub CI is required.

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
