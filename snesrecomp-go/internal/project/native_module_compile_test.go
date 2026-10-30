package project

import (
	"debug/pe"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile against the real Windows CRT/libc++ headers and inspect the unresolved
// call target. Both global and std::abort must keep using the module's C hook;
// dropping the remap just to make C++ compile would fail this check.
func TestPrivateModuleAbortWithWindowsCxxHeaders(t *testing.T) {
	zig := dependencyTestZig(t)
	scratch := t.TempDir()
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	include, err := filepath.Abs("../../runtime/include")
	if err != nil {
		t.Fatal(err)
	}
	for _, target := range []string{"x86_64-windows-gnu", "aarch64-windows-gnu"} {
		for _, language := range []string{"c", "cpp"} {
			for _, private := range []bool{false, true} {
				name := target + "/" + language + "/normal"
				if private {
					name = target + "/" + language + "/private"
				}
				t.Run(name, func(t *testing.T) {
					folder := t.TempDir()
					source := filepath.Join(folder, "abort."+language)
					writeTestFile(t, source, `
#ifdef __cplusplus
#include <array>
#include <cstdlib>
#include <span>
void call_std_abort(void) { std::abort(); }
#else
#include <stdlib.h>
#endif
void call_abort(void) { abort(); }
`)
					args := []string{"cc", "-target", target, "-std=gnu11", "-O0", "-I" + include}
					if private {
						args = append(args, privateModuleCompileArgs...)
					}
					args = runtimeSourceCompileArgs(args, source)
					object := filepath.Join(folder, "abort.obj")
					args = append(args, "-c", source, "-o", object)
					if output, err := exec.Command(zig, args...).CombinedOutput(); err != nil {
						t.Fatalf("compile failed: %v\n%s", err, output)
					}
					coff, err := pe.Open(object)
					if err != nil {
						t.Fatal(err)
					}
					defer coff.Close()
					want, forbidden := "abort", "sr_native_module_abort"
					if private {
						want, forbidden = forbidden, want
					}
					found := false
					for _, symbol := range coff.Symbols {
						if symbol.SectionNumber != 0 {
							continue
						}
						name := strings.TrimPrefix(symbol.Name, "__imp_")
						if name == forbidden {
							t.Fatalf("abort uses the wrong boundary: %s", symbol.Name)
						}
						found = found || name == want
					}
					if !found {
						t.Fatalf("compiled object has no call to %s", want)
					}
				})
			}
		}
	}
}
