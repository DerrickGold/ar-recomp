package project

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A real zig compile, because the thing under test is whether the COMPILER's
// own output reaches the build log -- a fake tool would only test the plumbing
// around it.
func TestHermeticBuildForwardsCompilerDiagnostics(t *testing.T) {
	zig, err := filepath.Abs("../../../build/toolchain/zig-aarch64-macos-0.16.0/zig")
	if err != nil {
		t.Fatal(err)
	}
	if _, statErr := os.Stat(zig); statErr != nil {
		t.Skip("pinned zig toolchain is not present")
	}
	build := func(t *testing.T, body string) (string, error) {
		t.Helper()
		root := t.TempDir()
		writeTestFile(t, filepath.Join(root, "snesbuild.ini"),
			"[project]\nname = MyGame\nsource = src/main.c\n")
		writeTestFile(t, filepath.Join(root, "CMakeLists.txt"),
			"add_executable(MyGame\n    src/main.c\n)\n")
		writeTestFile(t, filepath.Join(root, "src", "main.c"), body)
		// The build refuses to run against an empty generated-source tree.
		writeTestFile(t, filepath.Join(root, "src", "gen", "gen_unit.c"), "int generated_symbol;\n")
		runtimeDir := filepath.Join(root, "snesrecomp-go", "runtime")
		writeTestFile(t, filepath.Join(runtimeDir, "src", "runner.c"), "int runner_symbol;\n")
		writeTestFile(t, filepath.Join(runtimeDir, "include", "runner.h"), "/* public */\n")
		writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`)
		var log bytes.Buffer
		_, buildErr := HermeticBuild(HermeticOptions{
			Paths: DefaultPaths(root), ZigPath: zig, Jobs: 1,
			Stdout: &log, Stderr: &log, Verbose: true,
		})
		return log.String(), buildErr
	}

	// A call to nothing: the compiler warns, and the linker then fails on the
	// missing symbol. Both of those are output the reader could not see before.
	log, err := build(t, "int main(void){ return not_a_function(); }\n")
	if err == nil {
		t.Fatal("a broken translation unit built without complaint")
	}
	if !strings.Contains(log, "not_a_function") {
		t.Errorf("the tool's diagnostic never reached the log:\n%s", log)
	}
	if !strings.Contains(log, "  -- ") || !strings.Contains(log, "  | ") {
		t.Errorf("the diagnostic is not emitted as an attributed block:\n%s", log)
	}
	if strings.Contains(err.Error(), "undefined symbol") {
		t.Error("the error still carries the full tool output; it belongs in the log")
	}
	if !strings.Contains(err.Error(), "build log") {
		t.Errorf("the error does not point at the log: %v", err)
	}

	// A healthy build stays quiet (everything compiles with -w) but must still
	// name each unit, which is both the reader's sense of progress and what the
	// GUI's progress bar counts to measure the compile phase.
	log, err = build(t, "int main(void){ return 0; }\n")
	if err != nil {
		t.Fatalf("a clean build failed: %v\n%s", err, log)
	}
	if !strings.Contains(log, "cc ") || !strings.Contains(log, "src/main.c") {
		t.Errorf("verbose builds should name each translation unit:\n%s", log)
	}
	if strings.Contains(log, "  -- ") {
		t.Errorf("a clean build should emit no tool-output blocks:\n%s", log)
	}
}
