package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

const (
	testCommandEnvironment = "SNESBUILD_TEST_COMMAND"
	testFakeZigEnvironment = "SNESBUILD_TEST_FAKE_ZIG"
)

// TestMain lets the contract test exercise the command as a real subprocess
// without rebuilding snesbuild from inside its own test suite. The same test
// executable also stands in for Zig: it creates the requested object, archive,
// and linked-output paths so this test verifies orchestration rather than the
// host compiler installation.
func TestMain(m *testing.M) {
	if os.Getenv(testFakeZigEnvironment) == "1" && len(os.Args) > 1 {
		os.Exit(runFakeZig(os.Args[1:]))
	}
	if encoded := os.Getenv(testCommandEnvironment); encoded != "" {
		var args []string
		if err := json.Unmarshal([]byte(encoded), &args); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		if err := run(args); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	os.Exit(m.Run())
}

func runFakeZig(args []string) int {
	var output string
	if len(args) >= 5 && args[0] == "ar" {
		output = args[4]
	} else {
		for index := 0; index+1 < len(args); index++ {
			if args[index] == "-o" {
				output = args[index+1]
				break
			}
		}
	}
	if output == "" {
		fmt.Fprintln(os.Stderr, "fake Zig invocation has no output path")
		return 2
	}
	if err := os.MkdirAll(filepath.Dir(output), 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if err := os.WriteFile(output, []byte("generic fixture output\n"), 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	return 0
}

func TestJSONLEventContractEscapesDiagnostics(t *testing.T) {
	var output bytes.Buffer
	sink, err := newEventSink("jsonl", &output)
	if err != nil {
		t.Fatal(err)
	}
	sink.phase("compile", "Compiling")
	sink.progress("compile", 42, 300)
	sink.artifact("game-binary", "/tmp/game")
	sink.diagnostic("error", "compiler said: \"bad\"\nnext line")

	scanner := bufio.NewScanner(&output)
	count := 0
	var diagnostic machineEvent
	for scanner.Scan() {
		count++
		var event machineEvent
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatalf("line %d is not one JSON object: %v", count, err)
		}
		if event.Schema != eventSchema || event.Version != eventVersion {
			t.Fatalf("line %d has wrong contract: %#v", count, event)
		}
		if event.Type == "diagnostic" {
			diagnostic = event
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if count != 4 {
		t.Fatalf("events = %d, want 4", count)
	}
	if diagnostic.Message != "compiler said: \"bad\"\nnext line" {
		t.Fatalf("diagnostic message = %q", diagnostic.Message)
	}
}

func TestMachineContractRejectsUnknownFormat(t *testing.T) {
	if _, err := newEventSink("xml", &bytes.Buffer{}); err == nil {
		t.Fatal("unknown event format accepted")
	}
}

func TestMachineProcessContractRunsGenericWorkflow(t *testing.T) {
	base := t.TempDir()
	root := filepath.Join(base, "utils")
	mustWrite := func(relative string, data []byte, mode os.FileMode) {
		t.Helper()
		path := filepath.Join(root, filepath.FromSlash(relative))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, mode); err != nil {
			t.Fatal(err)
		}
	}

	rom := make([]byte, 0x8000)
	rom[0] = 0x60 // RTS at the generic fixture's sole entry point.
	mustWrite("game.sfc", rom, 0o600)
	mustWrite("recomp/bank00.cfg", []byte(
		"bank = 00\nfunc fixture_entry 8000 entry_mx:1,1\n"), 0o644)
	mustWrite("snesbuild.ini", []byte(
		"[project]\nname = FixtureGame\nsource = src/main.c\n"), 0o644)
	mustWrite("CMakeLists.txt", []byte(
		"add_executable(FixtureGame\n    src/main.c\n)\n"), 0o644)
	mustWrite("src/main.c", []byte("int main(void) { return 0; }\n"), 0o644)
	mustWrite("snesrecomp-go/runtime/src/runner.c", []byte("int fixture_runner;\n"), 0o644)
	mustWrite("snesrecomp-go/runtime/include/runner.h", []byte("/* public */\n"), 0o644)
	mustWrite("snesrecomp-go/runtime/runner.cmake", []byte(`
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`), 0o644)

	regen := runMachineCommand(t, "regen", "--root", root, "--rom", "game.sfc",
		"--jobs", "1", "--event-format", "jsonl")
	requireEvent(t, regen, "phase", "regen", "")
	requireEvent(t, regen, "artifact", "", "generated-source-directory")

	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	build := runMachineCommand(t, "build", "--root", root, "--rom", "game.sfc",
		"--hermetic", "--zig", executable, "--jobs", "1", "--event-format", "jsonl")
	requireEvent(t, build, "phase", "compile", "")
	binary := requireEvent(t, build, "artifact", "", "game-binary").Path
	if !filepath.IsAbs(binary) {
		t.Fatalf("build artifact is not absolute: %q", binary)
	}

	install := runMachineCommand(t, "install", "--root", root, "--binary", binary,
		"--rom", filepath.Join(root, "game.sfc"), "--destination", base,
		"--event-format", "jsonl")
	requireEvent(t, install, "phase", "install", "")
	installed := requireEvent(t, install, "artifact", "", "game-binary")
	launcher := requireEvent(t, install, "artifact", "", "launcher")
	for _, path := range []string{installed.Path, launcher.Path} {
		if !filepath.IsAbs(path) {
			t.Fatalf("install artifact is not absolute: %q", path)
		}
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("missing install artifact %s: %v", path, err)
		}
	}
}

func runMachineCommand(t *testing.T, args ...string) []machineEvent {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable)
	command.Env = append(os.Environ(),
		testCommandEnvironment+"="+string(encoded),
		testFakeZigEnvironment+"=1")
	var stdout, stderr bytes.Buffer
	command.Stdout, command.Stderr = &stdout, &stderr
	if err := command.Run(); err != nil {
		t.Fatalf("snesbuild %s failed: %v\n%s", args[0], err, stderr.String())
	}

	var events []machineEvent
	scanner := bufio.NewScanner(&stdout)
	for scanner.Scan() {
		var event machineEvent
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatalf("invalid JSONL from snesbuild %s: %v\n%s", args[0], err, stdout.String())
		}
		if event.Schema != eventSchema || event.Version != eventVersion {
			t.Fatalf("wrong contract from snesbuild %s: %#v", args[0], event)
		}
		events = append(events, event)
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if len(events) == 0 {
		t.Fatalf("snesbuild %s emitted no events", args[0])
	}
	return events
}

func requireEvent(t *testing.T, events []machineEvent, eventType, phase, kind string) machineEvent {
	t.Helper()
	for _, event := range events {
		if event.Type == eventType && (phase == "" || event.Phase == phase) &&
			(kind == "" || event.Kind == kind) {
			return event
		}
	}
	t.Fatalf("missing %s event (phase=%q kind=%q): %#v", eventType, phase, kind, events)
	return machineEvent{}
}

func TestSnesbuildHelpHasNoGameOwnedCommands(t *testing.T) {
	for _, command := range []string{"language", "localization-extract", "localization-graphics", "gui", "audio-preview", "quintet-lzss"} {
		if strings.Contains(helpTextForTest(), command) {
			t.Errorf("generic help still contains %q", command)
		}
	}
}

func helpTextForTest() string {
	data, _ := os.ReadFile("main.go")
	return string(data)
}
