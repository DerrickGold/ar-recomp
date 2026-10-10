package toolchain

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// A Zig release carries every target's libc headers and sources, but a
// distribution bundle only ever compiles C for the platform it runs on and
// links a prebuilt runtime archive, so most of lib/ is never read. Trimming at
// packaging time shrinks every download and, on Windows, the files unpacked
// on first launch.

// trimCommon is what every C compile and link reads: the standard library and
// compiler_rt sources (built for the target on first link), clang's built-in
// headers, Zig's own small C runtime pieces, and the compiler support code.
var trimCommon = []string{
	"std", "compiler_rt", "compiler_rt.zig", "include", "c", "c.zig",
	"zig.h", "ubsan_rt.zig", "fuzzer.zig", "compiler",
}

// trimTarget adds each platform's libc headers, exactly the directories
// `zig cc -target <triple> -v` searches, and the libc sources or stubs its
// link step reads. Native Linux builds use glibc; the vended runtime archive
// is built for glibc as well.
var trimTarget = map[string][]string{
	"darwin/arm64":  {"libc/include/any-darwin-any", "libc/darwin"},
	"darwin/amd64":  {"libc/include/any-darwin-any", "libc/darwin"},
	"windows/amd64": {"libc/include/any-windows-any", "libc/mingw"},
	"windows/arm64": {"libc/include/any-windows-any", "libc/mingw"},
	"linux/amd64": {
		"libc/include/generic-glibc", "libc/include/x86-linux-gnu",
		"libc/include/x86-linux-any", "libc/include/any-linux-any", "libc/glibc",
	},
	"linux/arm64": {
		"libc/include/generic-glibc", "libc/include/aarch64-linux-gnu",
		"libc/include/aarch64-linux-any", "libc/include/any-linux-any", "libc/glibc",
	},
}

// TrimResult counts what TrimLib removed and kept.
type TrimResult struct {
	RemovedFiles, KeptFiles int
	RemovedBytes, KeptBytes int64
}

// TrimLib deletes everything under zigDir/lib that the goos/goarch platform's
// builds never read, apart from license and notice files. Every kept path must
// exist before anything is deleted, so
// a Zig update that moves a directory fails packaging instead of shipping an
// incomplete compiler. Running it again on a trimmed tree removes nothing.
func TrimLib(zigDir, goos, goarch string) (TrimResult, error) {
	var result TrimResult
	platform, ok := trimTarget[goos+"/"+goarch]
	if !ok {
		return result, fmt.Errorf("no Zig trim list for platform %s/%s", goos, goarch)
	}
	keep := append(append([]string(nil), trimCommon...), platform...)
	lib := filepath.Join(zigDir, "lib")
	if info, err := os.Lstat(lib); err != nil || !info.IsDir() {
		return result, fmt.Errorf("not a Zig installation (no lib directory): %s", zigDir)
	}
	for _, path := range keep {
		if _, err := os.Lstat(filepath.Join(lib, filepath.FromSlash(path))); err != nil {
			return result, fmt.Errorf("Zig lib/%s is missing; review the trim list for this Zig release: %w", path, err)
		}
	}
	kept := func(path string) bool {
		for _, k := range keep {
			if path == k || strings.HasPrefix(path, k+"/") {
				return true
			}
		}
		return false
	}
	var remove []string
	err := filepath.WalkDir(lib, func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		rel, err := filepath.Rel(lib, name)
		if err != nil {
			return err
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if kept(filepath.ToSlash(rel)) || isNotice(entry.Name()) {
			result.KeptFiles++
			result.KeptBytes += info.Size()
			return nil
		}
		remove = append(remove, name)
		result.RemovedFiles++
		result.RemovedBytes += info.Size()
		return nil
	})
	if err != nil {
		return TrimResult{}, err
	}
	for _, name := range remove {
		if err := os.Remove(name); err != nil {
			return TrimResult{}, err
		}
	}
	return result, removeEmptyDirs(lib)
}

// Trimming removes code, never notices: every license, copying or copyright
// file in the release stays where it was. The LLVM license, which also covers
// clang's built-in headers and the LLVM inside the zig executable, ships only
// beside libc++, libc++abi and libunwind.
func isNotice(name string) bool {
	for _, prefix := range []string{"LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE"} {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	return false
}

// removeEmptyDirs deletes the directories the trim emptied, deepest first.
func removeEmptyDirs(root string) error {
	var dirs []string
	err := filepath.WalkDir(root, func(name string, entry fs.DirEntry, err error) error {
		if err == nil && entry.IsDir() && name != root {
			dirs = append(dirs, name)
		}
		return err
	})
	if err != nil {
		return err
	}
	for i := len(dirs) - 1; i >= 0; i-- {
		entries, err := os.ReadDir(dirs[i])
		if err != nil {
			return err
		}
		if len(entries) == 0 {
			if err := os.Remove(dirs[i]); err != nil {
				return err
			}
		}
	}
	return nil
}
