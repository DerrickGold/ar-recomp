# Windows compiler launch regression

The Builder invokes `snesbuild` and Zig directly, without cmd.exe or PowerShell.
Zig's compiler driver resolves its own executable to a canonical path without
the extended-length Windows prefix, then spawns that path for Clang. A long
portable installation path can therefore pass `zig version` but fail `zig cc`
with `failed to spawn zig clang ... FileNotFound`. Merely launching Zig with a
prefix or through a directory junction does not address that internal spawn.

Each build owns a `Session`. Long compiler paths use a temporary executable
copy and `ZIG_LIB_DIR` pointing at the original SDK. A deep scratch directory
also needs a short working directory for compiler subprocesses. Environment
changes are confined to the build's child processes. The Builder owns cleanup
so it can remove the copy after cancelling the compiler process tree.

Run the real regression on **both native Windows x64 and ARM64**, using the
matching compiler from the packaged Builder. In PowerShell, from `installer`:

```powershell
$env:AR_TEST_ZIG = 'C:\path\to\bundled\zig.exe'
go test ./internal/compilerlaunch -run TestRealZigLongPaths -v
```

The test creates paths beyond 260 characters containing spaces and Unicode,
relocates the compiler, then compiles, archives, links and runs a small program.
It covers both short and deep build workspaces with the SDK, sources and output
at long paths. `AR_TEST_ZIG` is required explicitly; ordinary unit tests skip
this integration test when no compiler is selected. Running it on macOS/Linux
checks relocation and SDK selection, but does not prove Windows API behavior.

The unit tests cover environment isolation, temporary-directory cleanup,
concurrent build isolation, cancellation, collisions and UTF-16 path lengths.
