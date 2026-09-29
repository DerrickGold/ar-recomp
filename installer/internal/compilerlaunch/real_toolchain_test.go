package compilerlaunch

import (
	"context"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Run on Windows amd64 and arm64 with AR_TEST_ZIG pointing to the matching
// bundled compiler. This exercises real Clang child launches, SDK headers,
// archiving, linking and execution with paths beyond MAX_PATH. It also runs
// on other hosts to check executable relocation and ZIG_LIB_DIR handling.
func TestRealZigLongPaths(t *testing.T) {
	zig := os.Getenv("AR_TEST_ZIG")
	if zig == "" {
		t.Skip("set AR_TEST_ZIG to run the native long-path compiler regression")
	}
	zig, err := exec.LookPath(zig)
	if err != nil {
		t.Fatal(err)
	}
	zig, err = physicalPath(zig)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	base := t.TempDir()
	deep := deepDirectory(t, base)
	compilerDir := filepath.Join(deep, "toolchain")
	if err := os.Mkdir(compilerDir, 0700); err != nil {
		t.Fatal(err)
	}
	compiler := filepath.Join(compilerDir, "zig.exe")
	if err := copyCompiler(ctx, zig, compiler); err != nil {
		t.Fatal(err)
	}
	// Hard links keep the fixture cheap on the same volume. Copies support
	// a compiler on another drive. Neither path changes the installed SDK.
	lib := filepath.Join(filepath.Dir(zig), "lib")
	err = filepath.WalkDir(lib, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		rel, err := filepath.Rel(lib, path)
		if err != nil {
			return err
		}
		target := filepath.Join(compilerDir, "lib", rel)
		if entry.IsDir() {
			return os.MkdirAll(target, 0700)
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("test SDK contains a symlink: %s", path)
		}
		if err := os.Link(path, target); err == nil {
			return nil
		}
		return copyCompiler(ctx, path, target)
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, deepScratch := range []bool{false, true} {
		t.Run(fmt.Sprintf("deep-workspace-%v", deepScratch), func(t *testing.T) {
			scratch := t.TempDir()
			if deepScratch {
				scratch = deepDirectory(t, scratch)
			}
			env := replaceEnvironment(os.Environ(), "ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "global-cache"))
			env = replaceEnvironment(env, "ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "local-cache"))
			env = replaceEnvironment(env, "ZIG_LIB_DIR", "")
			s, err := newSession(scratch, env, true, []string{base, os.TempDir()})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() {
				if err := s.Close(); err != nil {
					t.Error(err)
				}
			})
			if staged, err := s.UseToolchain(ctx, compiler); err != nil || !staged {
				t.Fatalf("stage compiler: %v, %v", staged, err)
			}
			run := func(executable string, args ...string) string {
				t.Helper()
				cmd := exec.Command(executable, args...)
				cmd.Dir, cmd.Env = s.Directory, s.Environment
				out, err := cmd.CombinedOutput()
				if err != nil {
					t.Fatalf("%s %v: %v\n%s", executable, args, err, out)
				}
				return string(out)
			}
			source, object := filepath.Join(deep, "hello.c"), filepath.Join(deep, "hello.o")
			if err := os.WriteFile(source, []byte("#include <stdio.h>\nint main(void) { puts(\"long paths work\"); return 0; }\n"), 0600); err != nil {
				t.Fatal(err)
			}
			compiler := environmentValue(s.Environment, "SNESBUILD_ZIG")
			run(compiler, "cc", "-c", source, "-o", object)
			archive := filepath.Join(deep, "hello.a")
			run(compiler, "ar", "rcs", archive, object)
			binary := filepath.Join(deep, "hello.exe")
			run(compiler, "cc", archive, "-o", binary)
			if got := strings.TrimSpace(run(binary)); got != "long paths work" {
				t.Fatalf("program output: %q", got)
			}
		})
	}
}
