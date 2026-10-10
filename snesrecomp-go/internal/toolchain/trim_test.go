package toolchain

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// The license and notice files a real Zig 0.16 release ships inside lib/.
// The trim must keep every one, including those inside removed directories.
var fakeNotices = []string{
	"lib/libcxx/LICENSE.TXT", "lib/libcxxabi/LICENSE.TXT", "lib/libunwind/LICENSE.TXT",
	"lib/libc/glibc/LICENSES", "lib/libc/mingw/COPYING", "lib/libc/musl/COPYRIGHT",
	"lib/libc/freebsd/COPYRIGHT", "lib/libc/wasi/LICENSE-APACHE-LLVM",
}

// fakeZigLib lays out the directory shape of a real Zig 0.16 lib/ with one
// small file per directory, plus top-level files.
func fakeZigLib(t *testing.T) string {
	t.Helper()
	zig := t.TempDir()
	files := []string{
		"zig", "LICENSE",
		"lib/std/std.zig", "lib/std/os/windows.zig", "lib/compiler_rt.zig",
		"lib/compiler_rt/memcpy.zig", "lib/include/stddef.h", "lib/c.zig",
		"lib/c/string.zig", "lib/zig.h", "lib/ubsan_rt.zig", "lib/fuzzer.zig",
		"lib/compiler/build_runner.zig", "lib/docs/index.html",
		"lib/init/build.zig", "lib/build-web/main.js",
		"lib/libcxx/include/vector", "lib/libcxxabi/src/abort_message.cpp",
		"lib/libunwind/src/Unwind-seh.cpp", "lib/libtsan/tsan_rtl.cpp",
		"lib/libc/include/any-windows-any/windows.h",
		"lib/libc/include/any-darwin-any/stdio.h",
		"lib/libc/include/generic-glibc/stdio.h",
		"lib/libc/include/generic-musl/stdio.h",
		"lib/libc/include/x86-linux-gnu/bits/wordsize.h",
		"lib/libc/include/x86-linux-any/asm/unistd.h",
		"lib/libc/include/aarch64-linux-gnu/bits/wordsize.h",
		"lib/libc/include/aarch64-linux-any/asm/unistd.h",
		"lib/libc/include/any-linux-any/linux/limits.h",
		"lib/libc/include/generic-freebsd/stdio.h",
		"lib/libc/mingw/crt/crtexe.c", "lib/libc/glibc/abilists",
		"lib/libc/musl/src/string/memcpy.c", "lib/libc/wasi/libc-top-half/README",
		"lib/libc/darwin/libSystem.tbd", "lib/libc/freebsd/abilists",
		// A header that merely looks like a notice is still removed.
		"lib/libc/include/generic-freebsd/sys/copyright.h",
	}
	for _, name := range append(files, fakeNotices...) {
		path := filepath.Join(zig, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return zig
}

func libFiles(t *testing.T, zig string) []string {
	t.Helper()
	var names []string
	err := filepath.WalkDir(filepath.Join(zig, "lib"), func(path string, entry os.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		rel, err := filepath.Rel(zig, path)
		names = append(names, filepath.ToSlash(rel))
		return err
	})
	if err != nil {
		t.Fatal(err)
	}
	return names
}

func TestTrimLibKeepsOnlyTheTargetPlatform(t *testing.T) {
	for platform, want := range map[string][]string{
		"windows/amd64": {"lib/libc/include/any-windows-any/windows.h", "lib/libc/mingw/crt/crtexe.c"},
		"darwin/arm64":  {"lib/libc/include/any-darwin-any/stdio.h", "lib/libc/darwin/libSystem.tbd"},
		"linux/amd64": {"lib/libc/include/generic-glibc/stdio.h", "lib/libc/include/x86-linux-gnu/bits/wordsize.h",
			"lib/libc/include/x86-linux-any/asm/unistd.h", "lib/libc/include/any-linux-any/linux/limits.h", "lib/libc/glibc/abilists"},
		"linux/arm64": {"lib/libc/include/generic-glibc/stdio.h", "lib/libc/include/aarch64-linux-gnu/bits/wordsize.h",
			"lib/libc/include/aarch64-linux-any/asm/unistd.h", "lib/libc/include/any-linux-any/linux/limits.h", "lib/libc/glibc/abilists"},
	} {
		t.Run(platform, func(t *testing.T) {
			zig := fakeZigLib(t)
			before := len(libFiles(t, zig))
			goos, goarch, _ := strings.Cut(platform, "/")
			result, err := TrimLib(zig, goos, goarch)
			if err != nil {
				t.Fatal(err)
			}
			common := []string{"lib/std/std.zig", "lib/std/os/windows.zig", "lib/compiler_rt.zig",
				"lib/compiler_rt/memcpy.zig", "lib/include/stddef.h", "lib/c.zig", "lib/c/string.zig",
				"lib/zig.h", "lib/ubsan_rt.zig", "lib/fuzzer.zig", "lib/compiler/build_runner.zig"}
			remaining := libFiles(t, zig)
			slices.Sort(remaining)
			expected := append(append(append([]string(nil), common...), want...), fakeNotices...)
			slices.Sort(expected)
			expected = slices.Compact(expected)
			if !slices.Equal(remaining, expected) {
				t.Fatalf("kept:\n%s", strings.Join(remaining, "\n"))
			}
			if result.KeptFiles != len(remaining) || result.RemovedFiles != before-len(remaining) || result.RemovedBytes <= 0 {
				t.Fatalf("counts %+v for %d of %d files", result, len(remaining), before)
			}
			if err := filepath.WalkDir(filepath.Join(zig, "lib"), func(path string, entry os.DirEntry, err error) error {
				if err != nil || !entry.IsDir() {
					return err
				}
				if entries, err := os.ReadDir(path); err != nil || len(entries) == 0 {
					t.Errorf("left an empty directory %s (%v)", path, err)
				}
				return nil
			}); err != nil {
				t.Fatal(err)
			}
			for _, outside := range []string{"zig", "LICENSE"} {
				if _, err := os.Stat(filepath.Join(zig, outside)); err != nil {
					t.Fatalf("touched %s outside lib/: %v", outside, err)
				}
			}
			again, err := TrimLib(zig, goos, goarch)
			if err != nil || again.RemovedFiles != 0 || again.KeptFiles != result.KeptFiles {
				t.Fatalf("second trim was not a no-op: %+v %v", again, err)
			}
		})
	}
}

func TestTrimLibRefusesBeforeDeletingWhenAKeptPathIsMissing(t *testing.T) {
	zig := fakeZigLib(t)
	if err := os.RemoveAll(filepath.Join(zig, "lib", "libc", "mingw")); err != nil {
		t.Fatal(err)
	}
	before := libFiles(t, zig)
	if _, err := TrimLib(zig, "windows", "amd64"); err == nil || !strings.Contains(err.Error(), "libc/mingw") {
		t.Fatalf("expected a missing libc/mingw error, got %v", err)
	}
	if after := libFiles(t, zig); len(after) != len(before) {
		t.Fatalf("deleted %d files before refusing", len(before)-len(after))
	}
	if _, err := TrimLib(t.TempDir(), "windows", "amd64"); err == nil {
		t.Fatal("trimmed a directory without lib/")
	}
	if _, err := TrimLib(zig, "plan9", "amd64"); err == nil {
		t.Fatal("trimmed for a platform without a list")
	}
}

// Every platform with a pinned compiler must also have a trim list, so a new
// pin cannot ship the full multi-platform library by accident.
func TestTrimListCoversEveryPinnedPlatform(t *testing.T) {
	for platform := range pinnedZig {
		if _, ok := trimTarget[platform]; !ok {
			t.Errorf("no trim list for pinned platform %s", platform)
		}
	}
}
